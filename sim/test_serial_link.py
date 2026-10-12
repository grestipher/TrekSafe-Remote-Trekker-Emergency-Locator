"""Tests for the serial port reader.

    python3 -m unittest discover -s sim -p 'test_*.py'

This is the code the one-board demonstration runs on, and it was the only new
code in the project with no tests at all, which made it the riskiest thing here
by a wide margin.

It is tested against a pseudo-terminal rather than a board. A pty is a real
kernel character device with real termios semantics, so everything that can be
exercised without physical hardware is: opening, configuring, non-blocking
reads, reassembling lines from partial reads, writing, and every failure mode
that has a sentence attached to it.

What a pty cannot tell us is whether the cable works, whether the baud rate is
right, or whether this machine's USB stack behaves. Those need the board.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from serial_link import (SerialPort, SerialPortUnavailable, describe_no_port,  # noqa: E402
                         list_candidate_ports, open_first_available)


class PtyFixture(unittest.TestCase):
    """A pseudo-terminal standing in for the cable.

    The far end is the master; the SerialPort opens the slave by its path, which
    is exactly how it would open /dev/cu.usbmodemXXXX.
    """

    def setUp(self):
        self.master, slave = os.openpty()
        self.slave_path = os.ttyname(slave)
        # Closed here: SerialPort opens the path itself, and leaving this
        # descriptor open would keep the pty alive after the test and mask a
        # failure to clean up.
        os.close(slave)
        self.port = SerialPort(self.slave_path)

    def tearDown(self):
        self.port.close()
        os.close(self.master)

    def board_says(self, text):
        """Bytes arriving from the far end, as a board would send them."""
        os.write(self.master, text.encode())

    def board_hears(self):
        """Everything written towards the board, as one string."""
        os.set_blocking(self.master, False)
        try:
            return os.read(self.master, 65536).decode()
        except BlockingIOError:
            return ""


class Reading(PtyFixture):
    def test_a_complete_line_arrives(self):
        self.board_says("AIR TS1|A4F2|1|0|SOS|1|A4F2|52.8|-118.0|12||I|86\n")
        self.assertEqual(self.port.read_lines(),
                         ["AIR TS1|A4F2|1|0|SOS|1|A4F2|52.8|-118.0|12||I|86"])

    def test_nothing_waiting_is_not_an_error(self):
        """The ordinary case on a quiet mesh, and it must never block."""
        self.assertEqual(self.port.read_lines(), [])

    def test_several_lines_in_one_read(self):
        self.board_says("LOG one\nLOG two\nLOG three\n")
        self.assertEqual(self.port.read_lines(), ["LOG one", "LOG two", "LOG three"])

    def test_a_line_split_across_reads_is_reassembled(self):
        """Serial delivers a few bytes at a time. A frame split across two reads
        must not become two half frames, which would reach the console as two
        malformed distress calls instead of one real one."""
        self.board_says("AIR TS1|A4F2|1|0|SOS")
        self.assertEqual(self.port.read_lines(), [], "an incomplete line is held, not guessed at")

        self.board_says("|1|A4F2|52.8|-118.0|12||I|86")
        self.assertEqual(self.port.read_lines(), [], "still incomplete")

        self.board_says("\n")
        self.assertEqual(self.port.read_lines(),
                         ["AIR TS1|A4F2|1|0|SOS|1|A4F2|52.8|-118.0|12||I|86"],
                         "and arrives whole once the newline does")

    def test_carriage_returns_are_stripped(self):
        """Arduino's println sends \\r\\n. A trailing carriage return left on the
        end of a frame would be parsed as part of the last field."""
        self.board_says("AIR TS1|A4F2|1|0|OK|1|A4F2|0|0||\r\n")
        self.assertEqual(self.port.read_lines(), ["AIR TS1|A4F2|1|0|OK|1|A4F2|0|0||"])

    def test_blank_lines_are_dropped(self):
        self.board_says("\n\nLOG something\n\n")
        self.assertEqual(self.port.read_lines(), ["LOG something"])

    def test_a_partial_line_survives_until_the_rest_arrives(self):
        self.board_says("LOG complete\nLOG partial")
        self.assertEqual(self.port.read_lines(), ["LOG complete"])
        self.board_says(" now finished\n")
        self.assertEqual(self.port.read_lines(), ["LOG partial now finished"])

    def test_a_corrupted_byte_does_not_lose_the_frame(self):
        """A frame with one bad byte still says it is a distress call. The base
        station is where a malformed one gets reported, with a stated reason."""
        os.write(self.master, b"AIR TS1|A4F2|1|0|SOS|1|A4\xff F2\n")
        lines = self.port.read_lines()
        self.assertEqual(len(lines), 1, "the line arrives rather than being dropped")
        self.assertTrue(lines[0].startswith("AIR TS1|A4F2|"))

    def test_an_absurdly_long_line_does_not_grow_without_bound(self):
        """Noise on the cable, or a board in a strange state. Holding it would
        mean its tail eventually glues onto the front of a real frame.

        Written in chunks with a read between each, because a pty's kernel
        buffer is a few kilobytes and a single large write would block forever
        waiting for a reader that only runs after the write returns. A real
        serial port delivers in small pieces for the same reason, so this is
        also the more faithful shape.
        """
        for _ in range(40):
            self.board_says("X" * 500)
            self.assertEqual(self.port.read_lines(), [], "no newline yet, so no line")

        self.board_says("\nAIR TS1|A4F2|1|0|SOS|1|A4F2\n")
        lines = self.port.read_lines()
        self.assertIn("AIR TS1|A4F2|1|0|SOS|1|A4F2", lines,
                      "a real frame after the noise still gets through")


class PortConfiguration(PtyFixture):
    def test_closing_the_port_does_not_drop_dtr(self):
        """HUPCL must stay cleared, or closing the port reboots the board.

        A dev board wires the modem control lines to its reset circuit, so
        HUPCL, which lowers them on the last close, restarts the firmware every
        time anything disconnects. Measured on a real Heltec: a beacon's link
        sequence numbers jumped from 37973 to a fresh random 46174 between two
        runs, which is what a reboot looks like, and after clearing this flag
        they went 46174 to 46175 across a close and reopen.

        The consequence of regressing this is not cosmetic. The beacon restarts
        mid-incident, the bridge correctly notices its source has gone, and one
        closed connection takes the whole base station down with it.
        """
        import termios
        cflag = termios.tcgetattr(self.port._fd)[2]
        self.assertEqual(cflag & termios.HUPCL, 0,
                         "HUPCL is set, so closing this port will reset the board")

    def test_the_line_discipline_is_out_of_the_way(self):
        """Raw mode. With the kernel's line discipline in play a frame carrying
        certain bytes is silently altered on its way through, which for a
        distress call is the worst class of bug there is."""
        import termios
        iflag, oflag, cflag, lflag = termios.tcgetattr(self.port._fd)[:4]
        self.assertEqual(iflag, 0, "no input processing, no flow control, no CR translation")
        self.assertEqual(oflag, 0, "a byte written is the byte that goes")
        self.assertEqual(lflag, 0, "no echo, no canonical mode, no signals from input")
        self.assertTrue(cflag & termios.CLOCAL, "ignore modem lines")
        self.assertTrue(cflag & termios.CREAD, "and actually receive")


class Writing(PtyFixture):
    def test_a_line_is_written_with_a_newline(self):
        self.port.write_line("AIR TS1|BA5E|0|0|ACK|1|A4F2|INC-001")
        self.assertEqual(self.board_hears(), "AIR TS1|BA5E|0|0|ACK|1|A4F2|INC-001\n")

    def test_the_acknowledgement_round_trip(self):
        """The return path D3 calls the core of the product, over a cable."""
        self.port.write_line("AIR TS1|BA5E|0|0|ACK|1|A4F2|INC-001")
        heard = self.board_hears()
        self.assertIn("ACK|1|A4F2|INC-001", heard)
        self.assertTrue(heard.endswith("\n"), "the board reads a line at a time")


class Failures(unittest.TestCase):
    """Everything that can go wrong here goes wrong for a physical reason, and
    the person reading the message is holding the board. None of these may be a
    traceback."""

    def test_a_missing_device_says_so_plainly(self):
        with self.assertRaises(SerialPortUnavailable) as ctx:
            SerialPort("/dev/definitely-not-a-serial-port")
        self.assertIn("no such serial device", str(ctx.exception))

    def test_naming_a_port_that_is_not_there(self):
        with self.assertRaises(SerialPortUnavailable):
            open_first_available("/dev/nope-not-here")

    def test_an_unsupported_baud_rate_is_refused_by_name(self):
        master, slave = os.openpty()
        path = os.ttyname(slave)
        os.close(slave)
        try:
            with self.assertRaises(SerialPortUnavailable) as ctx:
                SerialPort(path, baud=12345)
            self.assertIn("baud", str(ctx.exception))
        finally:
            os.close(master)

    def test_a_quiet_port_is_not_mistaken_for_a_dead_one(self):
        """The other half of the pair below, and the reason both exist.

        A quiet mesh at three in the morning is normal and must not look like a
        failure. On a non-blocking descriptor that case is EAGAIN, which is not
        an error and not end of file.
        """
        master, slave = os.openpty()
        path = os.ttyname(slave)
        os.close(slave)
        port = SerialPort(path)
        try:
            for _ in range(20):
                self.assertEqual(port.read_lines(), [], "quiet is quiet, not an error")
        finally:
            port.close()
            os.close(master)

    def test_a_port_that_went_away_is_noticed_rather_than_looking_quiet(self):
        """The board was unplugged or reset mid-session, which happens
        constantly on a bench and eventually happens in a park office.

        This is the one that matters most in this file. Reading a character
        device that has gone returns end of file, not an error, so the naive
        handling treats it as "nothing waiting" and the reader loops forever.
        The bridge would then keep telling every console the feed was healthy
        while nothing at all was listening, which is exactly the false
        confidence the product exists to prevent. It has to raise.
        """
        master, slave = os.openpty()
        path = os.ttyname(slave)
        os.close(slave)
        port = SerialPort(path)
        os.close(master)  # the far end disappears

        with self.assertRaises(SerialPortUnavailable) as ctx:
            for _ in range(5):
                port.read_lines()
        self.assertIn("unplugged or reset", str(ctx.exception))
        port.close()

    def test_the_advice_when_nothing_is_plugged_in_is_actionable(self):
        advice = describe_no_port()
        self.assertIn("charge-only", advice, "the commonest actual cause")
        self.assertIn("/dev/cu.", advice, "and how to check")

    def test_listing_candidates_never_raises(self):
        """Called on a machine with no board attached, which is most of them."""
        self.assertIsInstance(list_candidate_ports(), list)

    def test_closing_twice_is_harmless(self):
        master, slave = os.openpty()
        path = os.ttyname(slave)
        os.close(slave)
        port = SerialPort(path)
        port.close()
        port.close()
        os.close(master)


if __name__ == "__main__":
    unittest.main()
