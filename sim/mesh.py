"""A LoRa mesh, simulated in software.

This is what lets the whole system be demonstrated with one radio. The Heltec
runs the real beacon firmware, built so that its transport is the USB cable
rather than the radio; this module plays everything on the other side of that
cable: the repeaters that relay the frame and the gateway that hands it to the
base station.

It is not a mock. It runs the same link format the firmware puts on the air,
parsed from the same field layout, and it makes the same decisions a repeater
makes: deduplicate, count the hop, drop it when it has gone far enough. What it
cannot do is radio, and it says so rather than pretending:

  - signal readings are invented from a model, and every one of them is labelled
    simulated all the way through to the console, so nothing a warden sees as a
    measurement came from here
  - packet loss is a coin flip against a fixed probability, not a propagation
    model
  - there is no timing, no collision, no duty cycle and no interference

What it does prove is the part above the antenna: that a real beacon's frames
are well formed, that a repeater's rules carry them, that the gateway's host
protocol is right, and that an acknowledgement typed by a human at the console
travels all the way back and stops the beacon repeating. That is genuinely most
of the system, and none of it was testable before.

Standard library only, like everything else here.
"""

import random

# Must match TREKSAFE_SEEN_CAPACITY and kMaxHops in the firmware. Stated as a
# number in both places rather than shared, because the firmware and this are
# separate programs that happen to agree; a shared constant would hide the day
# they stop agreeing.
MAX_HOPS = 3
SEEN_CAPACITY = 64

LINK_TAG = "TS1"


def parse_link(line):
    """Takes a link frame apart, or returns None.

    Deliberately a mirror of parseLink in firmware/src/link.cpp, down to
    rejecting the same things. Only the first four fields belong to the link
    layer; the payload is everything after the fourth pipe, kept whole, because
    it is itself pipe delimited and splitting it here would be this layer
    reading a payload it has no business understanding.
    """
    if not line:
        return None
    parts = line.split("|", 4)
    if len(parts) < 5:
        return None

    # Deliberately no stripping of either field.
    #
    # The firmware trims nothing here, so neither does this. A frame off a
    # radio has no business carrying stray whitespace, and accepting it in the
    # simulator while a real repeater drops it is the divergence that makes a
    # simulator worse than useless. Found by running the two link layers against
    # the same inputs and diffing, which is what test_link_layers_agree.py does.
    tag, origin, seq, hops, payload = parts
    if tag.upper() != LINK_TAG:
        return None

    origin = origin.upper()
    if not _is_dev_id(origin):
        return None

    seq = _strict_unsigned(seq, 0xFFFF)
    hops = _strict_unsigned(hops, 0xFF)
    if seq is None or hops is None:
        return None
    if not payload:
        return None

    return {"origin": origin, "seq": seq, "hops": hops, "payload": payload}


def _strict_unsigned(text, limit):
    """Digits only, nothing else, or None.

    Deliberately not int(), and this was found by testing the two link layers
    against each other rather than by reading either. Python's int() accepts a
    leading plus, surrounding whitespace, and underscores as digit separators,
    so "+1", " 1 " and "1_0" all parse. The firmware's parseUnsigned accepts
    none of them and drops the frame.

    That made the simulator more permissive than the real thing, which is the
    one way a simulator can be actively harmful: a demonstration would show the
    mesh carrying frames that a repeater in a field would silently drop, and the
    difference would only turn up on a ridge. sim/README.md states the rule and
    this is what enforcing it looks like.
    """
    if not text or not text.isascii() or not text.isdigit():
        return None
    value = int(text)
    return value if 0 <= value <= limit else None


def build_link(origin, seq, hops, payload):
    """The other half. Refuses what the firmware refuses."""
    if not _is_dev_id(origin) or not payload:
        return None
    return "%s|%s|%d|%d|%s" % (LINK_TAG, origin, seq, hops, payload)


def _is_dev_id(s):
    """2 to 8 uppercase hex characters, which is what the contract requires.

    The reason this is enforced here and not just documented: a node named
    something that is not hex, "GW01" being the obvious temptation, cannot be
    put in a frame at all, so every one of its transmissions fails and it sits
    there looking healthy. The firmware refuses to start in that state and this
    refuses to build the frame, which between them makes it impossible to do by
    accident.
    """
    if not s or not (2 <= len(s) <= 8):
        return False
    return all(c in "0123456789ABCDEF" for c in s)


class SeenCache:
    """Which messages have already been handled.

    Bounded, and entries are forgotten after a while, for the same two reasons
    the firmware's version is: a flood mesh that does not deduplicate saturates
    on one distress call, and a beacon that reboots restarts its sequence
    numbers, so a cache that remembered forever would silently drop the first
    frames of a genuine new call.
    """

    def __init__(self, capacity=SEEN_CAPACITY, forget_after_ms=15 * 60 * 1000):
        self.capacity = capacity
        self.forget_after_ms = forget_after_ms
        self._entries = {}  # (origin, seq) -> first heard, ms

    def is_new(self, origin, seq, now_ms):
        if not origin:
            # No key to remember it under. Never dropped; the caller is
            # responsible for not relaying it.
            return True

        key = (origin, seq)
        if self.forget_after_ms:
            stale = [k for k, at in self._entries.items()
                     if now_ms - at >= self.forget_after_ms]
            for k in stale:
                del self._entries[k]

        if key in self._entries:
            # Not refreshed, deliberately. The entry ages from when the message
            # was first heard, so a beacon repeating one sequence number cannot
            # hold a key alive and shadow a later reboot that lands on it.
            return False

        if len(self._entries) >= self.capacity:
            oldest = min(self._entries, key=self._entries.get)
            del self._entries[oldest]

        self._entries[key] = now_ms
        return True


class SimulatedRepeater:
    """One relay on a ridge.

    Holds the same rules as RepeaterRole in the firmware, and no others.
    """

    def __init__(self, devid, loss=0.0, snr_at_this_hop=None, rng=None):
        if not _is_dev_id(devid):
            raise ValueError(
                "a repeater id must be 2 to 8 hex characters, got %r. A node whose id "
                "is not hex cannot be named in a frame and would transmit nothing."
                % (devid,))
        self.devid = devid
        self.loss = loss
        self.snr_at_this_hop = snr_at_this_hop
        self.seen = SeenCache()
        self._rng = rng or random.Random()

        self.relayed = 0
        self.dropped_duplicate = 0
        self.dropped_out_of_hops = 0
        self.dropped_lost = 0

    def hear(self, frame, now_ms):
        """Returns the frame to put back on the air, or None.

        None covers every reason a repeater stays quiet, and the counters are
        what tell them apart afterwards. A simulator that could not say why a
        frame stopped would be no better than the silence it is meant to
        explain.
        """
        f = parse_link(frame)
        if f is None:
            return None

        if f["origin"] == self.devid:
            return None  # our own frame, heard back from a neighbour

        # Loss first, so a frame this repeater never heard is not recorded as
        # seen. Recording it would mean a later copy arriving by another path
        # got dropped as a duplicate of something that never arrived.
        if self.loss and self._rng.random() < self.loss:
            self.dropped_lost += 1
            return None

        if not self.seen.is_new(f["origin"], f["seq"], now_ms):
            self.dropped_duplicate += 1
            return None

        if f["hops"] >= MAX_HOPS:
            self.dropped_out_of_hops += 1
            return None

        self.relayed += 1
        return build_link(f["origin"], f["seq"], f["hops"] + 1, f["payload"])


class SimulatedGateway:
    """The radio beside the base station, in software.

    Produces exactly the host protocol lines the real gateway firmware produces,
    which is what lets the bridge read from either without knowing the
    difference. See the protocol note at the top of firmware/src/role_gateway.h.
    """

    def __init__(self, devid="BA5E", snr_model=None, rng=None):
        if not _is_dev_id(devid):
            raise ValueError("a gateway id must be 2 to 8 hex characters, got %r" % (devid,))
        self.devid = devid
        self.seq = 0
        self.seen = SeenCache()
        self._rng = rng or random.Random()
        self._snr_model = snr_model or default_snr_model

        self.heard = 0
        self.dropped = 0

    def hear(self, frame, now_ms):
        """One frame off the mesh. Returns the host line, or None."""
        f = parse_link(frame)
        if f is None:
            return "LOG heard a frame this gateway could not read"

        if f["origin"] == self.devid:
            self.dropped += 1
            return None

        if not self.seen.is_new(f["origin"], f["seq"], now_ms):
            # The same call arriving by a second path. Without this the console
            # counts the copies as a beacon calling again because nobody
            # answered, which is a materially different thing on a warden's
            # screen.
            self.dropped += 1
            return None

        snr, rssi = self._snr_model(f["hops"], self._rng)
        self.heard += 1
        return "RX %.1f %d %d %s" % (snr, rssi, f["hops"], f["payload"])

    def transmit(self, payload):
        """A payload from the base station, wrapped for the mesh."""
        frame = build_link(self.devid, self.seq, 0, payload)
        if frame is None:
            return None
        self.seq += 1
        return frame


def default_snr_model(hops, rng):
    """Invented signal readings, and they are invented.

    Loosely shaped like reality: a frame heard directly is strong, and each
    relay costs signal because the last leg was the marginal one. The numbers
    are a plausible spread and nothing more. Everything downstream of here
    labels them as simulated, because a coverage survey built from invented
    readings is the exact false confidence D7 exists to prevent, and the only
    thing that stops it is that the label never comes off.
    """
    base = 8.0 - 5.0 * hops
    snr = base + rng.uniform(-2.0, 2.0)
    rssi = int(-70 - 18 * hops + rng.uniform(-6, 6))
    return snr, rssi


class SimulatedMesh:
    """A beacon's frame, carried to the base station and back.

    Wires a chain of repeaters to a gateway. The topology is a line, which is
    the shape of a valley and the shape this product is designed for: a beacon
    at the far end, relays along the way, and the staffed facility at the road.
    """

    def __init__(self, repeaters=None, gateway=None, rng=None):
        self._rng = rng or random.Random()
        self.gateway = gateway or SimulatedGateway(rng=self._rng)
        if repeaters is None:
            repeaters = [SimulatedRepeater("FA01", rng=self._rng),
                         SimulatedRepeater("FA02", rng=self._rng)]
        self.repeaters = repeaters

    def from_beacon(self, frame, now_ms):
        """A frame the beacon put on the air. Returns host lines for the bridge.

        Walks the chain: each repeater in turn decides whether to carry it, and
        whatever comes out the far end reaches the gateway. A frame every
        repeater drops reaches nothing, which is a coverage gap and is exactly
        what should happen.
        """
        lines = []
        current = frame

        for repeater in self.repeaters:
            relayed = repeater.hear(current, now_ms)
            if relayed is None:
                return lines  # it stopped here
            current = relayed

        line = self.gateway.hear(current, now_ms)
        if line:
            lines.append(line)
        return lines

    def to_beacon(self, payload, now_ms):
        """A payload from the base station, carried back to the beacon.

        Returns the frame to write down the cable, or None if it did not get
        through. The return path goes through the same repeaters, in reverse,
        under the same rules, because an acknowledgement that only works because
        the simulator was generous is an acknowledgement that will not work on
        the air.
        """
        frame = self.gateway.transmit(payload)
        if frame is None:
            return None

        for repeater in reversed(self.repeaters):
            frame = repeater.hear(frame, now_ms)
            if frame is None:
                return None
        return frame
