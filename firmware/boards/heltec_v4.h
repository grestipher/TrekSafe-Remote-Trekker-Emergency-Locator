// Pin map and radio settings for the Heltec WiFi LoRa 32, ESP32-S3 + SX1262.
//
// WHERE THESE NUMBERS COME FROM, September 2026
//
// Measured on the board, not copied from a datasheet. Everything below was
// determined by running src/probe.cpp on this actual hardware:
//
//   chip        ESP32-S3 rev 2, 16 MB flash, no PSRAM
//   mac (low32) A51B5BF8, so this board's device id is 5BF8
//   VEXT        36. The display answered at 0x3C with 36 driven low, and not
//               at all with 40. That is what separates this revision from the
//               V4.3 / r8 one, where VEXT moved to 40.
//   battery     pin 37 enables the divider and is ACTIVE HIGH: 2 mV with it
//               low, 846 mV with it high.
//
// That identifies it as Meshtastic's `heltec_v4` variant, the V4.2 shape,
// rather than its `heltec_v4_r8` (V4.3) variant.
//
// WHY THIS WAS MEASURED RATHER THAN LOOKED UP
//
// Heltec state that the V4 keeps the V3's pinout. That is true of the pins
// along the header, and it hides four things that are not: an RF front end the
// V3 does not have, a battery enable whose polarity is inverted, a different
// display controller, and an onboard GPS. An earlier version of this file was
// written from the V3 map on the strength of that claim, and would have given a
// board that ran, reported success, and could not transmit a thing.
//
// So if you are holding a different board, do not edit this from a web page.
// Run the probe. It takes a minute and it answers definitively:
//
//     cd firmware && pio run -e boardprobe -t upload -t monitor
//
// Nothing else in the firmware knows a pin number exists. This is the only file
// that has to be right for a given board, and the only one that changes for a
// new one.

#pragma once

// ---------------------------------------------------------------------------
// SX1262 radio, on SPI
// ---------------------------------------------------------------------------
#define TS_LORA_NSS 8   // SPI chip select      (variant.h LORA_CS)
#define TS_LORA_SCK 9   // SPI clock            (LORA_SCK)
#define TS_LORA_MOSI 10 // SPI data to radio    (LORA_MOSI)
#define TS_LORA_MISO 11 // SPI data from radio  (LORA_MISO)
#define TS_LORA_RST 12  // radio reset          (LORA_RESET)
#define TS_LORA_BUSY 13 // busy line, SX126x specific (LORA_DIO2 on this board)
#define TS_LORA_DIO1 14 // radio interrupt      (LORA_DIO1)

// ---------------------------------------------------------------------------
// Two settings that are not pins, and without which the radio does not work
// ---------------------------------------------------------------------------
//
// These are the two things everybody gets wrong on this board, and both fail in
// the same confusing way: the firmware starts, reports no error worth noticing,
// and then simply never gets a packet through.

// DIO3 on the SX1262 powers the temperature-compensated oscillator, and the
// module expects 1.8 V on it. Without this the radio has no stable reference
// clock. It will initialise, report success, and then transmit and receive on a
// frequency that drifts, so nothing ever demodulates anything.
#define TS_LORA_TCXO_VOLTAGE 1.8

// DIO2 drives the antenna switch on this module. Without telling the radio
// that, transmit power is never routed to the antenna: the chip happily reports
// a successful transmission that went nowhere.
#define TS_LORA_DIO2_IS_RF_SWITCH 1

// ---------------------------------------------------------------------------
// The board's own peripherals
// ---------------------------------------------------------------------------

// The PRG button, the only button on the board.
//
// Wired to ground with a pull-up, so it reads LOW when pressed. It is also the
// boot-mode button: holding it through a reset puts the ESP32 into its
// bootloader instead of running this firmware. Fine for a demonstration, not
// fine for a beacon, which is why config.h says a real beacon has its own
// button.
#define TS_BUTTON_PIN 0
#define TS_BUTTON_ACTIVE_LOW 1

// The white user LED. (pins_arduino.h: LED = 35, LED_BUILTIN = 35)
#define TS_LED_PIN 35

// Powers the display and the LoRa antenna boost. ACTIVE LOW: drive it low to
// turn them on.
//
// Confirmed by probe: with 36 low the display answered on I2C, with 40 low
// nothing did. It is 40 on the V4.3 / r8 revision, which is the single most
// likely reason a screen stays dark on a board that is otherwise fine.
//
// Nothing on the display works until this is driven, which is why a blank
// screen is usually a power problem rather than a driver one.
#define TS_VEXT_PIN 36
#define TS_VEXT_ACTIVE_LOW 1

// ---------------------------------------------------------------------------
// Display: a 128x64 SSD1315, driven as an SSD1306
// ---------------------------------------------------------------------------
//
// Confirmed present at I2C address 0x3C once VEXT is on. The controller is an
// SSD1315, which is register compatible with the SSD1306 every library
// supports, so it is driven as one.
#define TS_HAS_DISPLAY 1
#define TS_OLED_SDA 17
#define TS_OLED_SCL 18
#define TS_OLED_RST 21
#define TS_OLED_ADDR 0x3C
#define TS_OLED_WIDTH 128
#define TS_OLED_HEIGHT 64

// Battery measurement.
//
// Two pins, not one, and the polarity is the part that is easy to get wrong.
// The divider is deliberately not powered all the time, because leaving it
// connected would drain the battery it is measuring, so the enable pin has to
// be driven before a reading means anything.
//
// ACTIVE HIGH on this revision, and it is ACTIVE LOW on a V3. Getting it the
// wrong way round does not fail loudly: the divider simply stays off and the
// reading is whatever the pin happens to float at. Confirmed by probe on this
// board: 2 mV with pin 37 low, 846 mV with it high.
#define TS_BATTERY_ADC_PIN 1
#define TS_BATTERY_ENABLE_PIN 37
#define TS_BATTERY_ENABLE_ACTIVE_LOW 0

// What to multiply the voltage at the pin by to get the battery voltage.
//
// From Meshtastic's ADC_MULTIPLIER for this board, 4.9 * 1.045, where 4.9 is
// the nominal divider ratio and the rest is an empirical correction.
//
// UNCALIBRATED ON THIS BOARD. Treated as approximate everywhere downstream and
// the firmware says so in its log at startup. The console shows battery as a
// number a warden uses to judge how long somebody can wait, so before this is
// relied on it should be checked against a multimeter on a known cell. Being
// roughly right and honest about it beats being precisely wrong.
#define TS_BATTERY_MULTIPLIER (4.9 * 1.045)

// ---------------------------------------------------------------------------
// Radio settings
// ---------------------------------------------------------------------------

// 902 to 928 MHz is the licence-exempt ISM band in Canada and the United
// States, and 906.875 sits inside it. This is the frequency the whole fleet has
// to agree on: nodes on different frequencies do not form a degraded mesh, they
// form no mesh at all, and each one looks perfectly healthy on its own.
//
// REGULATORY NOTE, and not a formality for a park-owned system. Canadian use of
// this band falls under ISED RSS-247, which constrains transmit power, antenna
// gain and channel occupancy. Nothing in this firmware enforces a duty cycle.
// Before anything transmits outside a bench, somebody who has read the rules
// has to check this. It is written here rather than only in a document because
// this is the file somebody edits when they move the project to another
// country, and that is the moment they need telling.
#define TS_LORA_FREQ_MHZ 906.875

// Bandwidth, spreading factor and coding rate together decide how far a frame
// reaches and how long it occupies the air. These are the slow, long-range end,
// roughly the trade Meshtastic's "Long Fast" preset makes, which is a
// reasonable starting point precisely because a lot of people have used it in
// the field. Expect to revisit them after an actual coverage survey.
#define TS_LORA_BANDWIDTH_KHZ 250.0
#define TS_LORA_SPREADING_FACTOR 11
#define TS_LORA_CODING_RATE 5
#define TS_LORA_SYNC_WORD 0x2B // keeps this network off other LoRa traffic
#define TS_LORA_PREAMBLE_LEN 16

// ---------------------------------------------------------------------------
// The RF front end. Present on a V4, absent on a V3
// ---------------------------------------------------------------------------
//
// This board does not connect the SX1262 to the antenna directly. In between
// sits a GC1109: a power amplifier for transmit and a low noise amplifier for
// receive, on its own power and control pins.
//
// It has to be powered and enabled or the radio does not work. Nothing about
// that failure is loud; the chip reports successful transmissions that never
// reach the antenna, which is the same class of silent failure as the TCXO and
// the DIO2 switch above.
//
// TX and RX path switching is handled by the SX1262's DIO2 line, which is
// already configured as the RF switch, so the front end needs no per-packet
// attention beyond being switched on at startup.
#define TS_HAS_RF_FRONTEND 1
#define TS_FEM_POWER_PIN 7   // VFEM_Ctrl, the LDO feeding the front end
#define TS_FEM_ENABLE_PIN 2  // CSD, chip enable. HIGH is on
#define TS_FEM_TX_MODE_PIN 46 // CPS. HIGH is full amplification, LOW is bypass

// Transmit power at the SX1262, in dBm, BEFORE the front end amplifies it.
//
// Not 22. That is the chip's maximum and it would be right on a board where the
// chip drives the antenna, which this is not. Meshtastic measure this front end
// at roughly +11 dB net gain, so 22 dBm into it lands somewhere around 33 dBm
// at the antenna, which is far outside what ISED RSS-247 allows in this band
// and would also be a good way to damage the amplifier.
//
// 11 dBm in gives roughly 22 dBm out, which is a sane ceiling. It is an
// estimate from somebody else's measurements of a similar board and it has not
// been verified on this one with any instrument. Nothing here should transmit
// in the field until it has been.
#define TS_LORA_TX_POWER_DBM 11

// Overcurrent protection for the power amplifier, in mA. The SX1262 defaults to
// 60 mA, which is below what it draws at 22 dBm, so leaving it would have the
// radio limit itself mid-transmission. 140 is the datasheet figure for the
// high-power amplifier.
#define TS_LORA_CURRENT_LIMIT_MA 140.0
