"""Bridge sources that talk to TrekSafe's own firmware.

Three of them, and they differ only in how much of the system is real:

  node:<port>      the Heltec, plugged in, running the real beacon firmware
                   built so its transport is the USB cable. Repeaters and the
                   gateway are simulated on this machine. This is the
                   one-board demonstration.

  gateway:<port>   a real gateway board with a radio, speaking the host
                   protocol over USB. Nothing simulated. This is the
                   deployment shape, and it needs a second board.

  sim              nothing plugged in at all. A simulated beacon as well as a
                   simulated mesh, for showing the console with no hardware.

They all present the same thing to the bridge, which is the point: the bridge
and the console cannot tell which one is running, so what you see working in the
demonstration is the same code path that runs with a radio.

WHAT IS REAL AND WHAT IS NOT, in the one-board case:

  real       the beacon firmware, its state machine, its repeat schedule, the
             frames it builds, the link envelope, and its handling of the
             acknowledgement that comes back
  simulated  the repeaters, the gateway, the signal readings, and the air
             itself

Every invented signal reading is labelled, all the way to the console. See
`SIMULATED_NOTE` and where it is used.
"""

import os
import random
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "sim"))

import mesh  # noqa: E402
from beacon import SimulatedBeacon  # noqa: E402
from serial_link import SerialPortUnavailable, open_first_available  # noqa: E402


def parse_rx_line(line):
    """Reads a gateway's RX line into a frame and what the radio measured.

    The protocol is defined at the top of firmware/src/role_gateway.h:

        RX <snr> <rssi> <hops> <payload>

    A reading of "-" means the radio reported nothing, which is not the same as
    a reading of zero and must not become one. Zero dB is a real marginal link;
    silence about it is silence. The coverage survey is built from these and a
    survey that confuses the two is the false confidence D7 exists to prevent.

    Returns (payload, link) or (None, None).
    """
    if not line.startswith("RX "):
        return None, None

    parts = line.split(" ", 4)
    if len(parts) < 5:
        return None, None

    _, snr, rssi, hops, payload = parts
    link = {}

    if snr != "-":
        try:
            link["snr"] = float(snr)
        except ValueError:
            pass
    if rssi != "-":
        try:
            link["rssi"] = int(rssi)
        except ValueError:
            pass
    try:
        link["hops"] = int(hops)
    except ValueError:
        pass

    return (payload if payload else None), link


# Attached to every measurement that came out of the simulator rather than off a
# radio. It travels with the frame to the console, so nothing invented here can
# ever be presented to a warden as something that was measured.
SIMULATED_NOTE = "simulated"


class _MeshBackedSource:
    """Shared by the two sources that run a simulated mesh.

    Both carry a beacon's frames across simulated repeaters to a simulated
    gateway, and carry acknowledgements back. They differ only in where the
    beacon's frames come from, which is the one method each subclass supplies.
    """

    heartbeats = True

    def __init__(self, seed=None):
        self._rng = random.Random(seed)
        self.mesh = mesh.SimulatedMesh(rng=self._rng)
        self._on_frame = None
        self._started_ms = int(time.time() * 1000)

    def _now_ms(self):
        return int(time.time() * 1000)

    def _deliver(self, host_lines):
        """Turns gateway host lines into frames for the console."""
        for line in host_lines:
            if line.startswith("LOG "):
                print("[mesh] %s" % line[4:], flush=True)
                continue
            payload, link = parse_rx_line(line)
            if payload is None:
                continue
            link["source_note"] = SIMULATED_NOTE
            self._on_frame(payload, link)

    def send(self, payload):
        """An acknowledgement from the console, onto the simulated mesh."""
        frame = self.mesh.to_beacon(payload, self._now_ms())
        if frame is None:
            # The return path did not get through. Reported rather than
            # swallowed: the console is holding an incident it believes it has
            # replied to, and D3 is that a beacon pressed into silence is the
            # failure this product exists to prevent.
            return False, "the simulated mesh did not carry it to the beacon"
        return self._deliver_to_beacon(frame)

    def _deliver_to_beacon(self, frame):
        raise NotImplementedError


class NodeOverUsbSource(_MeshBackedSource):
    """The Heltec, running the real beacon firmware over the USB cable.

    The board believes it is transmitting. Every frame it would have put on the
    air comes up the cable prefixed AIR, crosses a simulated mesh here, and
    reaches the console. An acknowledgement goes back the same way and the real
    firmware stops repeating, which is the whole product demonstrated with one
    radio.
    """

    name = "node-over-usb"

    def __init__(self, port=None, seed=None):
        super().__init__(seed=seed)
        self.port_path = port
        self._port = None
        self._telemetry_seq = 0
        # Whether the board is reachable right now. The bridge asks before
        # sending a heartbeat, so a console is never told the feed is healthy
        # across a gap where nothing is listening to the board.
        self.connected = True

    def run(self, on_frame, stop):
        self._on_frame = on_frame

        # Opened here rather than in the constructor so a missing board is a
        # startup failure the bridge reports as one plain sentence, rather than
        # an exception escaping from somewhere the user cannot see.
        self._port = open_first_available(self.port_path)
        print("[bridge] reading the node on %s" % self._port.path, flush=True)
        print("[bridge] the beacon firmware is real; the repeaters, the gateway "
              "and every signal reading are simulated", flush=True)

        # Repeaters report in, so the console has infrastructure to show and a
        # coverage figure that means something rather than "none known".
        self._announce_repeaters()

        try:
            while not stop.is_set():
                try:
                    for line in self._port.read_lines():
                        self._handle(line)
                except SerialPortUnavailable as exc:
                    # The board went away. This is not fatal and must not be.
                    #
                    # A USB device disappears for entirely ordinary reasons: the
                    # laptop slept, the cable was nudged, the board reset. The
                    # first version of this let the exception end the source,
                    # which the bridge correctly treats as its radio dying and
                    # answers by shutting the whole base station down. That is
                    # the right response to a gateway that has genuinely failed
                    # and far too violent a response to a laptop waking up.
                    #
                    # So: wait for it to come back. The safety property is kept
                    # by not pretending in the meantime. This source stops
                    # heartbeating while it is disconnected, so the console
                    # shows the feed as lost, which is exactly what is true.
                    if not self._reconnect(exc, stop):
                        return
                stop.wait(0.02)
        finally:
            if self._port:
                self._port.close()
            self._port = None

    def _reconnect(self, why, stop):
        """Waits for the board to come back. False if asked to stop first."""
        self.connected = False
        try:
            self._port.close()
        except Exception:
            pass
        self._port = None

        print("[bridge] lost the node: %s" % why, file=sys.stderr, flush=True)
        print("[bridge] the console will show the feed as lost until it is back. "
              "Waiting for it.", flush=True)

        attempt = 0
        while not stop.is_set():
            stop.wait(1.0)
            if stop.is_set():
                return False
            attempt += 1
            try:
                self._port = open_first_available(self.port_path)
            except SerialPortUnavailable:
                # Said occasionally rather than every second, so a board left
                # unplugged overnight does not fill the log it would be found in.
                if attempt % 30 == 0:
                    print("[bridge] still waiting for the node, %d seconds" % attempt, flush=True)
                continue

            self.connected = True
            print("[bridge] the node is back on %s" % self._port.path, flush=True)
            # Fresh telemetry, so the repeater panel does not sit stale from
            # before the gap.
            self._announce_repeaters()
            return True
        return False

    def _announce_repeaters(self):
        """Lets the simulated repeaters introduce themselves.

        Without this the console reports "no repeaters heard, coverage is
        unknown", which is true of a mesh that has said nothing and misleading
        here, because the same simulated repeaters are relaying every frame the
        board sends. A panel that contradicts the hop count beside it is a panel
        an operator learns to distrust.
        """
        if not self._on_frame:
            return
        for repeater in self.mesh.repeaters:
            payload = "TEL|1|%s|%d|4.1|0.0" % (repeater.devid, self._rng.randint(55, 95))
            frame = mesh.build_link(repeater.devid, self._telemetry_seq, 0, payload)
            self._telemetry_seq = (self._telemetry_seq + 1) & 0xFFFF
            line = self.mesh.gateway.hear(frame, self._now_ms())
            if line:
                self._deliver([line])

    def _handle(self, line):
        if line.startswith("AIR "):
            frame = line[4:].strip()
            if frame:
                self._deliver(self.mesh.from_beacon(frame, self._now_ms()))
            return
        if line.startswith("LOG "):
            # The board talking about itself. Worth showing: it is where the
            # device id, the role and any startup refusal appear.
            print("[node] %s" % line[4:], flush=True)
            return
        # Anything else is noise on the cable. Printed rather than dropped,
        # because a board saying something unexpected is worth seeing.
        print("[node] ? %s" % line, flush=True)

    def _deliver_to_beacon(self, frame):
        if not self._port:
            return False, "the node is not connected"
        try:
            self._port.write_line("AIR " + frame)
        except SerialPortUnavailable as exc:
            return False, str(exc)
        return True, "sent to the node over usb"


class SimulatedMeshSource(_MeshBackedSource):
    """Everything simulated, including the beacon. No hardware at all.

    For showing the console when there is no board to hand. It runs a scripted
    incident: a party checks in, presses the button under canopy with no GPS
    lock, the lock arrives, and the beacon repeats until somebody at the console
    acknowledges it, at which point it stops. All of it crosses the same link
    layer and the same mesh rules the firmware uses.
    """

    name = "simulated-mesh"

    def __init__(self, seed=None, speed=1.0):
        super().__init__(seed=seed)
        # A demonstration should not need thirty seconds of standing about to
        # show a repeat. Time is scaled, and nothing about the behaviour
        # changes: the beacon is driven by a clock, not by a sleep.
        self.speed = max(speed, 0.01)
        self.beacon = SimulatedBeacon("A4F2", category="I")
        self._script_start = None

    def run(self, on_frame, stop):
        self._on_frame = on_frame
        self._script_start = time.time()
        print("[bridge] everything is simulated: the beacon, the repeaters, the "
              "gateway and every signal reading", flush=True)

        # A repeater reporting in, so the console has infrastructure to show and
        # a coverage figure that means something.
        for repeater in self.mesh.repeaters:
            self._emit_repeater_telemetry(repeater)

        pressed = False
        fixed = False

        while not stop.is_set():
            elapsed_ms = int((time.time() - self._script_start) * 1000 * self.speed)

            if not pressed and elapsed_ms > 4000:
                pressed = True
                print("[sim] the button has been held", flush=True)
                self._put_on_air(self.beacon.press_button(elapsed_ms))

            # The lock arrives a while after the call went out, which is the
            # case the NO FIX rule exists for.
            if pressed and not fixed and elapsed_ms > 20000:
                fixed = True
                print("[sim] the beacon has acquired a position", flush=True)
                self.beacon.acquire_fix(52.7211, -117.6413)

            if pressed:
                frame = self.beacon.tick(elapsed_ms)
                if frame:
                    self._put_on_air(frame)

            stop.wait(0.05)

    def _emit_repeater_telemetry(self, repeater):
        payload = "TEL|1|%s|%d|4.1|0.0" % (repeater.devid, self._rng.randint(55, 95))
        frame = mesh.build_link(repeater.devid, 0, 0, payload)
        # Straight to the gateway: a repeater's own telemetry is not relayed by
        # the repeater that sent it, and the console only needs to have heard it
        # once to know the node exists.
        line = self.mesh.gateway.hear(frame, self._now_ms())
        if line:
            self._deliver([line])

    def _put_on_air(self, frame):
        self._deliver(self.mesh.from_beacon(frame, self._now_ms()))

    def _deliver_to_beacon(self, frame):
        parsed = mesh.parse_link(frame)
        if parsed is None:
            return False, "the acknowledgement was not a readable frame"
        if self.beacon.hear(parsed["payload"], self._now_ms()):
            print("[sim] the beacon heard the acknowledgement and has stopped calling",
                  flush=True)
        return True, "delivered to the simulated beacon"


class GatewayOverUsbSource:
    """A real gateway board with a real radio, over USB.

    Nothing is simulated here. This is the deployment shape and it needs a
    second board flashed as the gateway; the beacon is then a separate board on
    the air. It is included now because it is fifteen lines once the host
    protocol exists, and because having it written down is what makes the demo
    honest about what it is standing in for.
    """

    name = "gateway-over-usb"
    heartbeats = True

    def __init__(self, port=None):
        self.port_path = port
        self._port = None

    def run(self, on_frame, stop):
        self._port = open_first_available(self.port_path)
        print("[bridge] reading a real gateway on %s" % self._port.path, flush=True)

        try:
            while not stop.is_set():
                for line in self._port.read_lines():
                    if line.startswith("LOG "):
                        print("[gateway] %s" % line[4:], flush=True)
                        continue
                    if line.startswith("BEAT "):
                        # The gateway saying it is alive. The bridge sends the
                        # console's heartbeat on its own schedule, so this is
                        # not forwarded; it is here so that a gateway which
                        # stops beating can be noticed later.
                        continue
                    payload, link = parse_rx_line(line)
                    if payload is not None:
                        on_frame(payload, link)
                stop.wait(0.02)
        finally:
            self._port.close()
            self._port = None

    def send(self, payload):
        if not self._port:
            return False, "the gateway is not connected"
        try:
            self._port.write_line("TX " + payload)
        except SerialPortUnavailable as exc:
            return False, str(exc)
        # The gateway answers with a LOG line if the radio refused. That is
        # printed above rather than returned here, because the write itself
        # succeeded and the radio's answer arrives later.
        return True, "handed to the gateway"
