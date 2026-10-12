"""Tests for the sources that talk to TrekSafe's own firmware.

    python3 -m unittest discover -s bridge -p 'test_*.py'

Two properties are load-bearing here and the rest is plumbing.

The first is that a reading of "-" and a reading of zero stay different all the
way through. Zero dB of signal to noise is a real and marginal link; "-" is the
radio having said nothing. The coverage survey treats them completely
differently and can only do that if nothing along this path flattens one into
the other.

The second is that the simulated label never falls off. Everything the simulator
invents is marked, and that mark has to survive being parsed here, stored by the
bridge, replayed after a restart and rendered on screen, because the moment it
stops arriving the console starts presenting invented numbers as measurements
and nothing looks any different.
"""

import os
import sys
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import node_source  # noqa: E402

SOS = "SOS|1|A4F2|52.7211|-117.6413|12||I|86"


class ReadingTheGatewayProtocol(unittest.TestCase):
    """The host protocol defined at the top of firmware/src/role_gateway.h."""

    def test_a_full_reading(self):
        payload, link = node_source.parse_rx_line("RX 6.2 -95 2 " + SOS)
        self.assertEqual(payload, SOS)
        self.assertEqual(link, {"snr": 6.2, "rssi": -95, "hops": 2})

    def test_the_payload_keeps_its_pipes(self):
        """The payload is last in the line precisely so its own delimiters need
        no escaping and cannot shift a field."""
        payload, _ = node_source.parse_rx_line("RX - - 0 " + SOS)
        self.assertEqual(payload, SOS)

    def test_a_reading_of_zero_is_kept(self):
        """Zero dB is a real, marginal link and must reach the survey."""
        _, link = node_source.parse_rx_line("RX 0.0 0 0 " + SOS)
        self.assertEqual(link["snr"], 0.0)
        self.assertEqual(link["rssi"], 0)
        self.assertEqual(link["hops"], 0)

    def test_no_reading_is_absent_rather_than_zero(self):
        """The other half of the pair above, and the whole reason "-" exists."""
        _, link = node_source.parse_rx_line("RX - - 1 " + SOS)
        self.assertNotIn("snr", link)
        self.assertNotIn("rssi", link)
        self.assertEqual(link["hops"], 1, "hops is always known, it is counted not measured")

    def test_heard_directly_is_zero_hops_not_missing(self):
        _, link = node_source.parse_rx_line("RX 8.0 -70 0 " + SOS)
        self.assertEqual(link["hops"], 0)

    def test_lines_that_are_not_frames_are_not_frames(self):
        for line in ("BEAT 12345", "LOG something happened", "RX", "RX 6.2 -95", "", "noise"):
            payload, _ = node_source.parse_rx_line(line)
            self.assertIsNone(payload, "%r must not be read as a frame" % line)

    def test_an_unreadable_number_is_dropped_rather_than_guessed(self):
        """A reading we cannot parse is a reading we do not have. Substituting
        one would put an invented number into the coverage survey."""
        _, link = node_source.parse_rx_line("RX loud quiet 1 " + SOS)
        self.assertNotIn("snr", link)
        self.assertNotIn("rssi", link)


class TheSimulatedLabelSurvives(unittest.TestCase):
    def test_every_frame_from_the_simulator_is_marked(self):
        src = node_source.SimulatedMeshSource(seed=1)
        got = []
        src._on_frame = lambda frame, link: got.append((frame, link))

        src._deliver(["RX 6.2 -95 2 " + SOS])
        self.assertEqual(len(got), 1)
        frame, link = got[0]
        self.assertEqual(frame, SOS)
        self.assertEqual(link["source_note"], node_source.SIMULATED_NOTE,
                         "an unmarked simulated reading becomes a fake measurement")

    def test_the_note_is_the_exact_string_the_console_looks_for(self):
        """Three programs have to agree on this word: the source writes it, the
        store keeps it, the console tests for it. A typo in any one of them
        fails silently and invented readings start being surveyed."""
        self.assertEqual(node_source.SIMULATED_NOTE, "simulated")

    def test_a_log_line_is_printed_rather_than_delivered_as_a_frame(self):
        src = node_source.SimulatedMeshSource(seed=1)
        got = []
        src._on_frame = lambda frame, link: got.append(frame)
        src._deliver(["LOG heard a frame this gateway could not read"])
        self.assertEqual(got, [], "a note about the mesh is not a distress call")


class TheOneBoardDemonstration(unittest.TestCase):
    """NodeOverUsbSource, driven without a board.

    The serial port itself is tested against a pseudo-terminal in
    sim/test_serial_link.py. What is tested here is the layer above it: what the
    source does with each kind of line the board can send.
    """

    def setUp(self):
        self.src = node_source.NodeOverUsbSource(seed=5)
        self.got = []
        self.src._on_frame = lambda frame, link: self.got.append((frame, link))

    def test_a_frame_the_board_put_on_the_air_crosses_the_mesh(self):
        self.src._handle("AIR TS1|A4F2|1|0|" + SOS)
        self.assertEqual(len(self.got), 1, "it reached the console")
        frame, link = self.got[0]
        self.assertEqual(frame, SOS, "the payload arrives exactly as the beacon built it")
        self.assertEqual(link["hops"], 2, "relayed by both simulated repeaters")
        self.assertEqual(link["source_note"], "simulated")

    def test_the_board_talking_about_itself_is_not_a_frame(self):
        self.src._handle("LOG device id A4F2")
        self.src._handle("LOG role beacon")
        self.assertEqual(self.got, [])

    def test_noise_on_the_cable_is_not_a_frame(self):
        self.src._handle("not anything we recognise")
        self.src._handle("")
        self.assertEqual(self.got, [])

    def test_an_empty_air_line_is_ignored(self):
        self.src._handle("AIR ")
        self.assertEqual(self.got, [])

    def test_the_same_frame_twice_is_one_call(self):
        """The board's own repeat carries a new sequence number; an identical
        frame arriving twice is the cable or the mesh echoing."""
        self.src._handle("AIR TS1|A4F2|1|0|" + SOS)
        self.src._handle("AIR TS1|A4F2|1|0|" + SOS)
        self.assertEqual(len(self.got), 1)

    def test_a_genuine_repeat_still_reaches_the_console(self):
        self.src._handle("AIR TS1|A4F2|1|0|" + SOS)
        self.src._handle("AIR TS1|A4F2|2|0|" + SOS)
        self.assertEqual(len(self.got), 2, "the beacon repeating must not be swallowed")

    def test_an_acknowledgement_with_no_board_attached_is_refused_not_swallowed(self):
        """D3: the console is holding an incident it believes it has replied to,
        so a reply that did not go has to come back as a failure."""
        ok, detail = self.src.send("ACK|1|A4F2|INC-001")
        self.assertFalse(ok)
        self.assertIn("not connected", detail)


class TheFullySimulatedDemonstration(unittest.TestCase):
    def test_it_runs_a_scripted_incident_and_can_be_stopped(self):
        src = node_source.SimulatedMeshSource(seed=3, speed=400)
        got = []
        stop = threading.Event()

        def on_frame(frame, link):
            got.append(frame)
            # Stop once the position has arrived, so the test does not depend on
            # wall-clock timing beyond the script reaching that point.
            if "52.7211" in frame:
                stop.set()

        thread = threading.Thread(target=src.run, args=(on_frame, stop), daemon=True)
        thread.start()
        self.assertTrue(stop.wait(20), "the scripted incident never reached a position")
        thread.join(timeout=5)

        self.assertTrue(any(f.startswith("TEL|") for f in got),
                        "the repeaters report in, so the console has infrastructure to show")
        self.assertTrue(any(f.startswith("SOS|") and "|0|0|" in f for f in got),
                        "the first call goes out with no fix, which the contract allows")
        self.assertTrue(any(f.startswith("SOS|") and "52.7211" in f for f in got),
                        "and the position follows when the lock arrives")

    def test_the_acknowledgement_reaches_the_simulated_beacon_and_stops_it(self):
        src = node_source.SimulatedMeshSource(seed=3)
        src._on_frame = lambda frame, link: None

        # Put the beacon into distress the way the script does.
        src._put_on_air(src.beacon.press_button(0))
        self.assertEqual(src.beacon.state, src.beacon.STATE_AWAITING_ACK)

        ok, detail = src.send("ACK|1|A4F2|INC-001")
        self.assertTrue(ok, detail)
        self.assertEqual(src.beacon.state, src.beacon.STATE_ACKNOWLEDGED,
                         "the return path is the whole product, D3")


if __name__ == "__main__":
    unittest.main()


class LosingTheBoardIsSurvivable(unittest.TestCase):
    """A board that goes away must not take the base station with it.

    USB devices disappear for entirely ordinary reasons: the laptop slept, the
    cable was nudged, the board reset. The first version of this let that end
    the source, which the bridge correctly treats as its radio dying and answers
    by shutting the whole base station down. That is the right response to a
    gateway that has genuinely failed and far too violent a response to a laptop
    waking up.

    The safety property is kept by not pretending: while disconnected the source
    reports so, the bridge stops heartbeating, and the console shows the feed as
    lost, which is exactly what is true.
    """

    def test_a_source_reports_whether_it_can_hear_the_mesh(self):
        src = node_source.NodeOverUsbSource(seed=1)
        self.assertTrue(src.connected, "assumed connected until something says otherwise")

    def test_a_source_that_cannot_lose_hardware_is_always_connected(self):
        """The fully simulated source has nothing to lose, and the bridge's
        default has to treat that as permanently fine."""
        src = node_source.SimulatedMeshSource(seed=1)
        self.assertTrue(getattr(src, "connected", True))

    def test_reconnecting_gives_up_when_asked_to_stop(self):
        """Ctrl-C during a reconnect wait must exit rather than hang."""
        src = node_source.NodeOverUsbSource(port="/dev/definitely-not-there", seed=1)
        src._port = None
        stop = threading.Event()
        stop.set()
        self.assertFalse(src._reconnect("test", stop))
        self.assertFalse(src.connected, "and it is honest about being down")

    def test_the_repeaters_introduce_themselves(self):
        """Otherwise the console reports "no repeaters heard, coverage unknown"
        while the same simulated repeaters relay every frame the board sends,
        and a panel that contradicts the hop count beside it is one an operator
        learns to distrust."""
        src = node_source.NodeOverUsbSource(seed=1)
        got = []
        src._on_frame = lambda frame, link: got.append(frame)
        src._announce_repeaters()

        self.assertTrue(got, "the repeaters said nothing at all")
        for frame in got:
            self.assertTrue(frame.startswith("TEL|1|"), frame)
        ids = {f.split("|")[2] for f in got}
        self.assertEqual(ids, {r.devid for r in src.mesh.repeaters})

    def test_each_repeater_announcement_is_its_own_message(self):
        """Sharing a sequence number would have the gateway deduplicate the
        second one away, and only one repeater would ever appear."""
        src = node_source.NodeOverUsbSource(seed=1)
        got = []
        src._on_frame = lambda frame, link: got.append(frame)
        src._announce_repeaters()
        self.assertEqual(len(got), len(src.mesh.repeaters),
                         "one repeater went missing to deduplication")
