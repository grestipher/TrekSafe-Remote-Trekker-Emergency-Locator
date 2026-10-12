"""ARCHIVED. Meshtastic bridge sources. Nothing imports this.

These read frames from a Meshtastic node, over a serial cable or over TCP to a
natively built node acting as a simulated radio. They were the bridge's link to
the outside world while TrekSafe ran as a Meshtastic module, which is decision
D1, and they stopped being used when that was dropped, which is D10.

Kept, not deleted, because this project is not under version control and the
code worked. It is not built, not tested and not imported, and it must stay that
way: an archive that something depends on stops being an archive and becomes a
directory nobody maintains but everything needs. See
firmware/archive-meshtastic/ARCHIVED.md for the full reasoning.

If Meshtastic is ever reintroduced as a transport, this is a working starting
point rather than a thing to copy in unread. It expects `meshtastic` and
`pubsub` installed, and the Source interface it implements is in
bridge/treksafe_bridge.py.
"""

import sys
from collections import deque

from treksafe_bridge import Source


def link_quality(packet):
    """What the radio reported about a received packet.

    Read from the packet layer, which is the only place it exists: the payload
    contract carries none of this and did not have to change to get it. Hops
    used is hopStart minus hopLimit, since hopLimit counts down as a packet is
    relayed. Anything absent stays absent rather than becoming a zero, because
    "no reading" and "a reading of zero" mean opposite things to a survey.
    """
    out = {}
    if not isinstance(packet, dict):
        return out

    snr = packet.get("rxSnr")
    if isinstance(snr, (int, float)):
        out["snr"] = float(snr)

    rssi = packet.get("rxRssi")
    if isinstance(rssi, (int, float)):
        out["rssi"] = int(rssi)

    start, limit = packet.get("hopStart"), packet.get("hopLimit")
    if isinstance(start, int) and isinstance(limit, int):
        hops = start - limit
        # A negative count means the two fields disagree. Report nothing rather
        # than a number that cannot be true.
        if hops >= 0:
            out["hops"] = hops
    return out

class MeshtasticTcpSource(Source):
    """A Meshtastic node reached over TCP rather than serial.

    This is what makes the whole path testable with no second board. Meshtastic
    builds natively for macOS and Linux and can simulate its LoRa chip over a
    local TCP port, so two native nodes on one laptop mesh with each other as
    though they were on the air. Point this at the one acting as the gateway.
    See firmware/README.md.

    Also the right source for a real node on the far side of a network link.
    """

    name = "meshtastic-tcp"

    def __init__(self, host="127.0.0.1", port=4403):
        self.host = host
        self.port = port
        self.iface = None
        self.seen = SeenPackets()

    def run(self, on_frame, stop):
        from pubsub import pub  # noqa: F401
        import meshtastic.tcp_interface

        def on_receive(packet, interface=None):
            handle_text_packet(packet, on_frame, self.seen)

        self.iface = meshtastic.tcp_interface.TCPInterface(hostname=self.host, portNumber=self.port)
        pub.subscribe(on_receive, "meshtastic.receive")
        try:
            while not stop.is_set():
                stop.wait(0.5)
        finally:
            try:
                self.iface.close()
            except Exception:
                pass

    def send(self, frame):
        if not self.iface:
            return False, "not connected to the node yet"
        self.iface.sendText(frame)
        return True, "sent to the mesh over tcp"

class SeenPackets:
    """Remembers packet ids so one transmission is not counted twice.

    The Meshtastic library publishes a received packet on more than one pubsub
    topic, and both propagate up to the parent topic this bridge subscribes to,
    so a single distress call arrived twice. Found by running against a real
    node, not by reading: the console counted one call as a repeat.

    Deduplicating on the Meshtastic packet id rather than on the frame text is
    what keeps a genuine repeat from the beacon intact, because the beacon's
    repeat is a new packet with a new id and must still be seen.
    """

    def __init__(self, remember=512):
        self._order = deque(maxlen=remember)
        self._ids = set()

    def is_new(self, packet_id):
        # A packet with no id cannot be deduplicated. Let it through: counting
        # one call twice is a nuisance, dropping one is not survivable.
        if packet_id is None:
            return True
        if packet_id in self._ids:
            return False
        if len(self._order) == self._order.maxlen and self._order:
            self._ids.discard(self._order[0])
        self._order.append(packet_id)
        self._ids.add(packet_id)
        return True


def handle_text_packet(packet, on_frame, seen=None):
    """Pulls a contract frame out of a Meshtastic packet, with what the radio
    reported about how it arrived. Never lets a bad packet kill the feed."""
    try:
        decoded = packet.get("decoded") or {}
        if decoded.get("portnum") != "TEXT_MESSAGE_APP":
            return
        text = decoded.get("text")
        if not text:
            return
        if seen is not None and not seen.is_new(packet.get("id")):
            return
        on_frame(text.strip(), link_quality(packet))
    except Exception as exc:
        print("[bridge] could not read a packet: %s" % exc, file=sys.stderr, flush=True)


class MeshtasticSource(Source):
    """The real thing. Imported lazily so the rest of the bridge runs, and is
    testable, on a machine with no Meshtastic library and no board."""

    name = "meshtastic"

    def __init__(self, port=None):
        self.port = port
        self.iface = None
        self.seen = SeenPackets()

    def run(self, on_frame, stop):
        from pubsub import pub  # noqa: F401  (ships with the meshtastic library)
        import meshtastic.serial_interface

        def on_receive(packet, interface=None):
            handle_text_packet(packet, on_frame, self.seen)

        self.iface = meshtastic.serial_interface.SerialInterface(devPath=self.port)
        pub.subscribe(on_receive, "meshtastic.receive")
        try:
            while not stop.is_set():
                stop.wait(0.5)
        finally:
            try:
                self.iface.close()
            except Exception:
                pass

    def send(self, frame):
        if not self.iface:
            return False, "the node is not connected yet"
        self.iface.sendText(frame)
        return True, "sent to the mesh"
