"""A beacon, in software.

Only for the demonstration that needs no hardware at all. When the Heltec is
plugged in it is the real firmware doing this, and this module is not involved.

It mirrors BeaconRole in firmware/src/role_beacon.cpp: raise a call, repeat it
on a widening schedule, stop when an acknowledgement addressed to it arrives,
then check in occasionally. It is a second implementation of behaviour that
already exists in C++, which is a real cost and worth being honest about: the
two can drift, and if they do, this one is wrong by definition. The firmware is
the product.

It earns its place because the alternative is worse. Without it the console can
only be shown by replaying a file of canned frames, which demonstrates the
console and nothing else: no link layer, no hop counts, no deduplication, and
above all no return path, so the acknowledgement that D3 calls the core of the
product cannot be shown working at all.
"""

REPEAT_SCHEDULE_MS = [30000, 60000, 120000, 300000]
CHECKIN_MS = 600000


class SimulatedBeacon:
    """One beacon carried by one party.

    Driven by a clock the caller supplies, in milliseconds, so a demonstration
    can run faster than real time without any of the behaviour changing.
    """

    STATE_IDLE = "idle"
    STATE_AWAITING_ACK = "awaiting-ack"
    STATE_ACKNOWLEDGED = "acknowledged"

    def __init__(self, devid, lat=None, lon=None, start_seq=0, category="U", battery=86):
        self.devid = devid.upper()
        self.lat = lat
        self.lon = lon
        self.category = category
        self.battery = battery

        self.seq = start_seq
        self.state = self.STATE_IDLE
        self._step = 0
        self._last_sent_ms = None
        self._last_checkin_ms = None
        self._last_sos_had_fix = False

        self.sent = 0

    # -- what the beacon puts on the air ------------------------------------

    def _position_fields(self):
        """Latitude and longitude, or the contract's way of saying no fix.

        0|0 is a valid distress message and not an error: a beacon under heavy
        canopy that has not acquired a lock still raises the alarm, and the
        position follows when it arrives.
        """
        if self.lat is None or self.lon is None:
            return "0", "0"
        return "%.4f" % self.lat, "%.4f" % self.lon

    def _sos(self):
        lat, lon = self._position_fields()
        # The epoch field is left empty on purpose. A simulated beacon has no
        # GPS and therefore no valid clock, and the contract says an empty
        # timestamp means exactly that. Substituting this machine's clock would
        # put a fabricated send time on an incident record.
        return "SOS|1|%s|%s|%s|12||%s|%d" % (self.devid, lat, lon, self.category, self.battery)

    def _checkin(self):
        lat, lon = self._position_fields()
        return "OK|1|%s|%s|%s|12|" % (self.devid, lat, lon)

    def _frame(self, payload):
        """Wrapped for the mesh, at zero hops: this is its first time on air."""
        frame = "TS1|%s|%d|0|%s" % (self.devid, self.seq, payload)
        self.seq = (self.seq + 1) & 0xFFFF
        self.sent += 1
        return frame

    # -- behaviour -----------------------------------------------------------

    def press_button(self, now_ms, category=None):
        """Somebody held the button. Returns the frame to put on the air."""
        if category:
            self.category = category
        if self.state != self.STATE_AWAITING_ACK:
            self._step = 0
        self.state = self.STATE_AWAITING_ACK
        self._last_sent_ms = now_ms
        self._last_sos_had_fix = self.lat is not None
        return self._frame(self._sos())

    def acquire_fix(self, lat, lon):
        """A GPS lock arrives after the call has already gone out.

        Does not transmit by itself; the next tick does, immediately rather than
        on schedule, because the base station is holding an incident marked
        NO FIX and somebody is deciding where to send people.
        """
        self.lat = lat
        self.lon = lon

    def tick(self, now_ms):
        """Returns a frame to transmit, or None. Call as often as you like."""
        if self.state == self.STATE_AWAITING_ACK:
            if not self._last_sos_had_fix and self.lat is not None:
                self._last_sent_ms = now_ms
                self._last_sos_had_fix = True
                return self._frame(self._sos())

            wait = REPEAT_SCHEDULE_MS[min(self._step, len(REPEAT_SCHEDULE_MS) - 1)]
            if self._last_sent_ms is not None and now_ms - self._last_sent_ms >= wait:
                self._step = min(self._step + 1, len(REPEAT_SCHEDULE_MS) - 1)
                self._last_sent_ms = now_ms
                return self._frame(self._sos())

        elif self.state == self.STATE_ACKNOWLEDGED:
            if self._last_checkin_ms is None or now_ms - self._last_checkin_ms >= CHECKIN_MS:
                self._last_checkin_ms = now_ms
                return self._frame(self._checkin())

        return None

    def hear(self, payload, now_ms):
        """A payload off the mesh. Returns True if it was our acknowledgement.

        Everything else is somebody else's traffic. A beacon does not relay: its
        battery has to last until somebody reaches the person carrying it.
        """
        if self.state != self.STATE_AWAITING_ACK:
            return False

        fields = payload.split("|")
        if len(fields) < 3:
            return False
        if fields[0].strip().upper() != "ACK":
            return False
        if fields[1].strip() != "1":
            return False
        if fields[2].strip().upper() != self.devid:
            return False

        self.state = self.STATE_ACKNOWLEDGED
        self._last_checkin_ms = now_ms
        return True
