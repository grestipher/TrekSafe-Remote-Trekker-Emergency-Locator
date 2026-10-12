#include "transport_lora.h"

#include "../boards/heltec_v4.h"
#include "link.h"

#include <SPI.h>
#include <string.h>

namespace treksafe {

// The SX1262's wiring, as RadioLib wants it: chip select, interrupt, reset and
// the SX126x specific busy line. These come from the board header, which is the
// one file that has to be right for a given board.
LoraTransport::LoraTransport()
    : radio_(new Module(TS_LORA_NSS, TS_LORA_DIO1, TS_LORA_RST, TS_LORA_BUSY)), up_(false),
      listening_(false)
{
}

bool LoraTransport::begin(char *reason, size_t reasonCap)
{
    // The Heltec puts the radio on pins that are not the ESP32's default SPI
    // bus, so the bus is started explicitly before RadioLib touches it.
    SPI.begin(TS_LORA_SCK, TS_LORA_MISO, TS_LORA_MOSI, TS_LORA_NSS);

    // The TCXO voltage is the last argument and it is not optional on this
    // board. RadioLib defaults it to 1.6 V; this module wants 1.8 V on DIO3,
    // and getting it wrong gives a radio that initialises, reports success, and
    // then drifts off frequency so that nothing ever demodulates anything. See
    // the note in the board header.
    int state = radio_.begin(TS_LORA_FREQ_MHZ, TS_LORA_BANDWIDTH_KHZ, TS_LORA_SPREADING_FACTOR,
                             TS_LORA_CODING_RATE, TS_LORA_SYNC_WORD, TS_LORA_TX_POWER_DBM,
                             TS_LORA_PREAMBLE_LEN, TS_LORA_TCXO_VOLTAGE);
    if (state != RADIOLIB_ERR_NONE) {
        // The overwhelmingly likely cause is a wrong pin, so the message says
        // so rather than only printing a number. Somebody reading this at a
        // bench needs the next action, not a code to look up.
        snprintf(reason, reasonCap,
                 "the radio did not start (RadioLib %d). Check the pin numbers in "
                 "firmware/boards/heltec_v4.h against your board.",
                 state);
        return false;
    }

#if TS_LORA_DIO2_IS_RF_SWITCH
    // DIO2 drives the antenna switch on this module. Without this, transmit
    // power is never routed to the antenna and the chip reports a successful
    // transmission that went nowhere. The other half of the pair of settings
    // that fail silently; see the board header.
    state = radio_.setDio2AsRfSwitch(true);
    if (state != RADIOLIB_ERR_NONE) {
        snprintf(reason, reasonCap,
                 "the radio refused to use DIO2 as its antenna switch (RadioLib %d), so "
                 "nothing it transmitted would reach the antenna",
                 state);
        return false;
    }
#endif

    // The power amplifier draws more than the chip's default 60 mA limit at
    // full power, so without raising it the radio throttles itself part way
    // through a transmission.
    state = radio_.setCurrentLimit(TS_LORA_CURRENT_LIMIT_MA);
    if (state != RADIOLIB_ERR_NONE) {
        snprintf(reason, reasonCap, "the radio refused a current limit (RadioLib %d)", state);
        return false;
    }

    // A two byte CRC, which is what LoRa normally carries. It means a corrupted
    // frame is rejected by the radio rather than handed up as a malformed
    // distress call, and that is the right place for the decision: this layer
    // can tell corruption from a frame that was never ours, and the layers
    // above it cannot.
    state = radio_.setCRC(2);
    if (state != RADIOLIB_ERR_NONE) {
        snprintf(reason, reasonCap, "the radio refused a CRC setting (RadioLib %d)", state);
        return false;
    }

    up_ = true;
    if (!listen()) {
        snprintf(reason, reasonCap, "the radio started but would not listen");
        up_ = false;
        return false;
    }
    return true;
}

bool LoraTransport::listen()
{
    if (!up_)
        return false;
    // RadioLib's no-argument startReceive puts the SX1262 into continuous
    // receive with an infinite timeout, so the chip stays in receive across
    // packets and this does not have to be called again for each one. It does
    // have to be called again after transmitting, which is what send() does.
    int state = radio_.startReceive();
    listening_ = (state == RADIOLIB_ERR_NONE);
    return listening_;
}

bool LoraTransport::send(const char *frame)
{
    if (!up_ || !frame || !*frame)
        return false;

    // Blocking, which is the right call here. A frame at these settings takes
    // well under a second of airtime, nothing else on this node has anything to
    // do while it goes out, and the alternative is a state machine tracking
    // transmit completion across ticks for no gain.
    int state = radio_.transmit((const uint8_t *)frame, strlen(frame));

    // Back to listening whatever happened. Transmitting leaves the chip out of
    // receive mode, and a beacon that stops listening after calling for help
    // will never hear the acknowledgement it is waiting for. This is the single
    // easiest thing to leave out and the hardest to notice missing, because
    // everything looks fine from the node's own side.
    listening_ = false;
    listen();

    return state == RADIOLIB_ERR_NONE;
}

bool LoraTransport::receive(char *out, size_t cap, LinkQuality &quality)
{
    quality = LinkQuality();
    if (!up_)
        return false;

    // A failed startReceive would otherwise leave this node permanently deaf,
    // so it is retried here as well as after transmitting.
    if (!listening_ && !listen())
        return false;

    // Ask the chip whether a packet actually arrived.
    //
    // This is the part that has to be an interrupt flag rather than anything
    // else. An earlier version polled getPacketLength(), which reports the
    // length of the last packet the modem handled and says nothing about
    // whether a new one has come in: it would have re-read one packet forever
    // and never noticed a second. SX126x::getIrqFlags returns the chip's raw
    // IRQ register, so the constant here is the chip's own, not RadioLib's
    // normalised enum.
    uint32_t irq = radio_.getIrqFlags();
    if (!(irq & RADIOLIB_SX126X_IRQ_RX_DONE))
        return false;

    char raw[kLinkFrameMax];
    // Read before the signal readings are taken. readData clears the IRQ flags
    // itself, which is what stops this seeing the same packet on every call.
    int state = radio_.readData((uint8_t *)raw, sizeof(raw) - 1);

    if (state == RADIOLIB_ERR_CRC_MISMATCH) {
        // An ordinary event on a marginal link, not an error worth reporting.
        // The frame is gone, and the beacon repeating is what recovers from it.
        // Nothing is logged here because a node on a ridge logs to nobody.
        return false;
    }
    if (state != RADIOLIB_ERR_NONE)
        return false;

    size_t len = radio_.getPacketLength();
    if (len == 0)
        return false;
    if (len >= sizeof(raw))
        len = sizeof(raw) - 1;
    raw[len] = '\0';

    // What the radio measured about this packet. This is the only place these
    // numbers exist, and everything the coverage survey knows comes from here.
    quality.hasSnr = true;
    quality.snr = radio_.getSNR();
    quality.hasRssi = true;
    quality.rssi = (int)radio_.getRSSI();

    snprintf(out, cap, "%s", raw);
    return true;
}

} // namespace treksafe
