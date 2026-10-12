"""Tests for the simulated mesh.

    python3 -m unittest discover -s sim -p 'test_*.py'

Two things are being checked here and they are worth naming separately.

The first is that the simulator behaves like the firmware: same frame format,
same deduplication, same hop limit, same refusals. A simulator that is more
generous than the real thing is worse than no simulator, because everything
passes and nothing has been learned.

The second is that the demonstration is honest. Invented signal readings must
stay labelled as invented, a frame the mesh drops must actually be dropped, and
the return path has to go through the same rules as the outbound one. The moment
the simulator quietly helps, it starts manufacturing exactly the false
confidence this product exists to prevent.
"""

import os
import random
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mesh  # noqa: E402

SOS = "SOS|1|A4F2|52.8734|-118.0814|12|1755000000|I|86"


def beacon_frame(seq=0, payload=SOS):
    """What the real beacon firmware puts on the air, at zero hops."""
    return mesh.build_link("A4F2", seq, 0, payload)


class LinkFormat(unittest.TestCase):
    """The simulator and the firmware have to agree byte for byte."""

    def test_a_frame_the_firmware_would_build(self):
        self.assertEqual(beacon_frame(41), "TS1|A4F2|41|0|" + SOS)

    def test_taking_one_apart(self):
        f = mesh.parse_link("TS1|A4F2|41|2|" + SOS)
        self.assertEqual(f["origin"], "A4F2")
        self.assertEqual(f["seq"], 41)
        self.assertEqual(f["hops"], 2)
        self.assertEqual(f["payload"], SOS, "the payload comes back whole, pipes and all")

    def test_a_round_trip(self):
        f = mesh.parse_link(beacon_frame(7))
        self.assertEqual((f["origin"], f["seq"], f["hops"], f["payload"]), ("A4F2", 7, 0, SOS))

    def test_what_is_refused_is_what_the_firmware_refuses(self):
        self.assertIsNone(mesh.parse_link(SOS), "a bare payload has no envelope")
        self.assertIsNone(mesh.parse_link("TS2|A4F2|1|0|" + SOS), "a link version we do not speak")
        self.assertIsNone(mesh.parse_link("TS1|ZZ!Z|1|0|" + SOS), "an origin that is not hex")
        self.assertIsNone(mesh.parse_link("TS1|A4F2|x|0|" + SOS), "a sequence that is not a number")
        self.assertIsNone(mesh.parse_link("TS1|A4F2|70000|0|" + SOS), "a sequence out of range")
        self.assertIsNone(mesh.parse_link("TS1|A4F2|1|0|"), "no payload")
        self.assertIsNone(mesh.parse_link("TS1|A4F2|1"), "too few fields")
        self.assertIsNone(mesh.parse_link(""), "nothing at all")

    def test_a_node_that_could_not_be_named_in_a_frame_is_refused_loudly(self):
        """The GW01 trap: G and W are not hex, so a node named that transmits
        nothing at all and looks perfectly healthy doing it."""
        self.assertIsNone(mesh.build_link("GW01", 1, 0, SOS))
        with self.assertRaises(ValueError):
            mesh.SimulatedRepeater("GW01")
        with self.assertRaises(ValueError):
            mesh.SimulatedGateway("GW01")


class Relaying(unittest.TestCase):
    def test_a_repeater_carries_a_call_and_counts_the_hop(self):
        r = mesh.SimulatedRepeater("FA01")
        out = r.hear(beacon_frame(1), 1000)
        self.assertEqual(out, "TS1|A4F2|1|1|" + SOS)
        self.assertEqual(r.relayed, 1)

    def test_the_payload_is_never_touched(self):
        r = mesh.SimulatedRepeater("FA01")
        out = mesh.parse_link(r.hear(beacon_frame(1), 1000))
        self.assertEqual(out["payload"], SOS,
                         "a repeater that edits a distress call is a repeater corrupting one")

    def test_the_same_message_is_not_carried_twice(self):
        """What stops a flood mesh eating itself."""
        r = mesh.SimulatedRepeater("FA01")
        self.assertIsNotNone(r.hear(beacon_frame(1), 1000))
        self.assertIsNone(r.hear(beacon_frame(1), 1100))
        self.assertIsNone(r.hear("TS1|A4F2|1|1|" + SOS, 1200), "nor when it comes back round")
        self.assertEqual(r.relayed, 1)
        self.assertEqual(r.dropped_duplicate, 2)

    def test_a_repeat_from_the_beacon_is_a_different_message(self):
        """The beacon repeats until acknowledged, and each repeat carries a new
        sequence number precisely so it is not deduplicated away."""
        r = mesh.SimulatedRepeater("FA01")
        self.assertIsNotNone(r.hear(beacon_frame(1), 1000))
        self.assertIsNotNone(r.hear(beacon_frame(2), 31000))
        self.assertEqual(r.relayed, 2)

    def test_a_frame_stops_when_it_has_come_far_enough(self):
        r = mesh.SimulatedRepeater("FA01")
        spent = mesh.build_link("A4F2", 1, mesh.MAX_HOPS, SOS)
        self.assertIsNone(r.hear(spent, 1000))
        self.assertEqual(r.dropped_out_of_hops, 1)

    def test_a_repeater_does_not_relay_its_own_frames(self):
        r = mesh.SimulatedRepeater("FA01")
        self.assertIsNone(r.hear(mesh.build_link("FA01", 1, 0, "TEL|1|FA01|91|4.1|0.0"), 1000))

    def test_a_lost_frame_is_not_recorded_as_having_been_heard(self):
        """Subtle and worth a test. If loss marked the frame as seen, a copy
        arriving later by another path would be dropped as a duplicate of
        something that never arrived, and one lost packet would poison every
        route."""
        r = mesh.SimulatedRepeater("FA01", loss=1.0)
        self.assertIsNone(r.hear(beacon_frame(1), 1000))
        self.assertEqual(r.dropped_lost, 1)

        r.loss = 0.0
        self.assertIsNotNone(r.hear(beacon_frame(1), 1100),
                             "the same message by another path must still get through")

    def test_a_beacon_that_rebooted_is_heard_again(self):
        """It restarts its sequence numbers. A cache that remembered forever
        would silently drop the first frames of a real new distress call."""
        r = mesh.SimulatedRepeater("FA01")
        r.seen = mesh.SeenCache(forget_after_ms=60000)
        self.assertIsNotNone(r.hear(beacon_frame(1), 1000))
        self.assertIsNone(r.hear(beacon_frame(1), 30000))
        self.assertIsNotNone(r.hear(beacon_frame(1), 100000))


class Gateway(unittest.TestCase):
    def test_a_frame_reaches_the_host_in_the_firmware_s_own_protocol(self):
        g = mesh.SimulatedGateway(rng=random.Random(1))
        line = g.hear("TS1|A4F2|1|2|" + SOS, 1000)

        verb, snr, rssi, hops, payload = line.split(" ", 4)
        self.assertEqual(verb, "RX")
        self.assertEqual(hops, "2", "how far it came, which the survey is built from")
        self.assertEqual(payload, SOS, "the payload, untouched")
        float(snr)  # readable as a number
        int(rssi)

    def test_the_same_call_by_two_paths_is_not_two_calls(self):
        """Otherwise the console counts the copies as a beacon calling again
        because nobody answered, which reads very differently to a warden."""
        g = mesh.SimulatedGateway()
        self.assertIsNotNone(g.hear("TS1|A4F2|1|1|" + SOS, 1000))
        self.assertIsNone(g.hear("TS1|A4F2|1|2|" + SOS, 1100))
        self.assertEqual(g.heard, 1)

    def test_an_unreadable_frame_is_reported_rather_than_dropped(self):
        g = mesh.SimulatedGateway()
        self.assertTrue(g.hear("not a frame", 1000).startswith("LOG "))

    def test_the_acknowledgement_is_wrapped_for_the_mesh(self):
        g = mesh.SimulatedGateway(devid="BA5E")
        self.assertEqual(g.transmit("ACK|1|A4F2|INC-001"), "TS1|BA5E|0|0|ACK|1|A4F2|INC-001")
        self.assertEqual(g.transmit("ACK|1|B117|INC-002"), "TS1|BA5E|1|0|ACK|1|B117|INC-002",
                         "each one is its own message, or the second is deduplicated away")

    def test_signal_weakens_with_every_relay(self):
        """The model is invented, and it is at least shaped like reality."""
        rng = random.Random(7)
        direct = [mesh.default_snr_model(0, rng)[0] for _ in range(200)]
        relayed = [mesh.default_snr_model(2, rng)[0] for _ in range(200)]
        self.assertGreater(sum(direct) / len(direct), sum(relayed) / len(relayed))


class EndToEnd(unittest.TestCase):
    """A distress call from a real beacon's frame to the base station, and the
    acknowledgement all the way back."""

    def test_a_call_crosses_the_mesh(self):
        m = mesh.SimulatedMesh(rng=random.Random(3))
        lines = m.from_beacon(beacon_frame(1), 1000)

        self.assertEqual(len(lines), 1)
        verb, snr, rssi, hops, payload = lines[0].split(" ", 4)
        self.assertEqual(verb, "RX")
        self.assertEqual(payload, SOS)
        self.assertEqual(hops, "2", "two repeaters between the beacon and the gateway")

    def test_the_acknowledgement_comes_back(self):
        m = mesh.SimulatedMesh(rng=random.Random(3))
        m.from_beacon(beacon_frame(1), 1000)

        frame = m.to_beacon("ACK|1|A4F2|INC-001", 2000)
        self.assertIsNotNone(frame, "the return path is the whole product, D3")

        f = mesh.parse_link(frame)
        self.assertEqual(f["payload"], "ACK|1|A4F2|INC-001")
        self.assertEqual(f["origin"], "BA5E")
        self.assertEqual(f["hops"], 2, "it came back through the same two repeaters")

    def test_a_call_that_does_not_get_through_does_not_get_through(self):
        """A simulator that quietly helped would be manufacturing the false
        confidence the whole product is against."""
        dead = mesh.SimulatedRepeater("FA01", loss=1.0)
        m = mesh.SimulatedMesh(repeaters=[dead], rng=random.Random(3))
        self.assertEqual(m.from_beacon(beacon_frame(1), 1000), [],
                         "a mesh with a dead relay in it carries nothing, and says nothing")

    def test_the_beacon_repeating_produces_one_call_per_repeat(self):
        m = mesh.SimulatedMesh(rng=random.Random(3))
        first = m.from_beacon(beacon_frame(1), 1000)
        again = m.from_beacon(beacon_frame(1), 1500)
        repeat = m.from_beacon(beacon_frame(2), 31000)

        self.assertEqual(len(first), 1)
        self.assertEqual(again, [], "the same frame heard twice is one call")
        self.assertEqual(len(repeat), 1, "a genuine repeat still reaches the console")


if __name__ == "__main__":
    unittest.main()
