"""Reading and writing a USB serial port, with nothing installed.

pyserial would be the obvious answer and this deliberately does not use it. The
whole project has no dependencies: the console needs no build step, the bridge
needs nothing beyond the standard library, and the tests run on a clean machine
with a Python and a C++ compiler. That property is worth more than the fifty
lines below, because the person who has to get this running at a park office is
not necessarily going to have a working pip.

What a serial port needs is a baud rate and no line discipline getting in the
way, and termios in the standard library does both.

macOS and Linux only. Windows needs a different implementation, and this says so
rather than failing in a way that looks like a broken cable.
"""

import errno
import os
import select
import sys

try:
    import termios

    HAVE_TERMIOS = True
except ImportError:  # Windows
    HAVE_TERMIOS = False


class SerialPortUnavailable(Exception):
    """Raised with a sentence a person can act on, never a traceback.

    Everything that can go wrong here goes wrong for a physical reason: the
    board is not plugged in, it is plugged into a charge-only cable, something
    else already has the port. The person reading the message is holding the
    board.
    """


def list_candidate_ports():
    """Serial devices that look like a plugged-in dev board, best guess first.

    Sorted so that the USB CDC device an ESP32-S3 presents comes before the
    Bluetooth and debug ports macOS also exposes as /dev/cu.*, which are never
    what anybody means.
    """
    if not os.path.isdir("/dev"):
        return []

    names = []
    for name in sorted(os.listdir("/dev")):
        # cu.* rather than tty.* on macOS: opening a tty.* blocks waiting for
        # carrier detect, which a USB board never asserts, so the open hangs
        # forever and looks like the board is not responding.
        if name.startswith("cu.usbmodem") or name.startswith("cu.usbserial"):
            names.append("/dev/" + name)
        elif name.startswith("cu.wchusbserial") or name.startswith("cu.SLAB_USBtoUART"):
            names.append("/dev/" + name)
        elif name.startswith("ttyUSB") or name.startswith("ttyACM"):
            names.append("/dev/" + name)
    return names


def describe_no_port():
    """What to tell somebody when there is no board to talk to."""
    return ("No serial device found. Plug the board in with a cable that carries data, "
            "not a charge-only one, and check it appears: ls /dev/cu.*")


class SerialPort:
    """One open serial port, read a line at a time without blocking.

    Non-blocking on purpose. The caller is a loop that also has a mesh to run
    and a bridge to feed, and a read that blocks would stop all of it whenever
    the board had nothing to say, which on a quiet mesh is almost always.
    """

    def __init__(self, path, baud=115200):
        if not HAVE_TERMIOS:
            raise SerialPortUnavailable(
                "reading a serial port this way needs termios, which Windows does not "
                "have. Run the simulator on macOS or Linux, or use the fully simulated "
                "source that needs no board at all.")

        self.path = path
        try:
            self._fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        except OSError as exc:
            if exc.errno == errno.ENOENT:
                raise SerialPortUnavailable("no such serial device: %s" % path)
            if exc.errno in (errno.EACCES, errno.EPERM):
                raise SerialPortUnavailable(
                    "not allowed to open %s. On Linux this usually means adding yourself "
                    "to the dialout group and logging back in." % path)
            if exc.errno in (errno.EBUSY, errno.EAGAIN):
                raise SerialPortUnavailable(
                    "%s is busy. Something else is holding it open: a serial monitor, "
                    "an editor's console, or another copy of this." % path)
            raise SerialPortUnavailable("could not open %s: %s" % (path, exc))

        try:
            self._configure(baud)
        except Exception:
            os.close(self._fd)
            raise

        self._buffer = b""

    def _configure(self, baud):
        """Raw mode at the given baud.

        Without this the kernel's line discipline is in the way: it would echo
        what we write back at us, translate carriage returns, and treat certain
        bytes as flow control. A frame containing one of those bytes would be
        silently altered on its way through, which for a distress call is the
        worst class of bug there is.
        """
        speed = getattr(termios, "B%d" % baud, None)
        if speed is None:
            raise SerialPortUnavailable("unsupported baud rate %d" % baud)

        attrs = termios.tcgetattr(self._fd)
        iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs

        # No input processing at all: no flow control, no CR/LF translation, no
        # parity stripping.
        iflag = 0
        # No output processing either, so a byte we write is the byte that goes.
        oflag = 0
        # No echo, no canonical mode, no signal generation from input bytes.
        lflag = 0

        cflag |= termios.CLOCAL | termios.CREAD  # ignore modem lines, enable receive
        cflag &= ~termios.PARENB  # no parity
        cflag &= ~termios.CSTOPB  # one stop bit
        cflag &= ~termios.CSIZE
        cflag |= termios.CS8  # eight data bits

        # Do not drop DTR when the port is closed.
        #
        # This is the reason the board kept rebooting. HUPCL lowers the modem
        # control lines on the last close, and a dev board wires that to its
        # reset circuit, so simply disconnecting from the port restarts the
        # firmware. It was measured: a beacon's link sequence numbers jumped
        # from 37973 to a fresh random 46174 between two runs, which is what a
        # reboot looks like, since the sequence is seeded from hardware at boot
        # precisely so that a reboot does not reuse numbers.
        #
        # Left set, every reconnection restarts the beacon mid-incident. The
        # bridge then correctly notices its source has gone and shuts itself
        # down, so one closed connection takes the whole base station with it.
        cflag &= ~termios.HUPCL

        cc = list(cc)
        cc[termios.VMIN] = 0  # never block waiting for a minimum byte count
        cc[termios.VTIME] = 0

        termios.tcsetattr(self._fd, termios.TCSANOW,
                          [iflag, oflag, cflag, lflag, speed, speed, cc])
        termios.tcflush(self._fd, termios.TCIOFLUSH)

    def read_lines(self):
        """Every complete line waiting, without their newlines.

        Partial lines are held until the rest arrives, which is the whole reason
        this buffers: serial delivers a few bytes at a time and a frame split
        across two reads must not become two half frames.
        """
        # A quiet port and a port that has gone away are different things, and
        # conflating them is the worst bug this file could have. An unplugged
        # board that looks like a quiet one means the bridge loops forever, keeps
        # telling every console the feed is healthy, and a distress call never
        # arrives with nobody any the wiser. That is the precise false confidence
        # D7 exists to prevent, and the bridge already has the right answer for
        # it, which is to die loudly, but only if something tells it.
        #
        # Telling them apart is not as simple as it looks, and this is worth
        # spelling out because the obvious version is wrong. Reading a character
        # device configured for polling returns zero bytes in BOTH cases. It was
        # measured on macOS, with this file's own termios settings:
        #
        #                        VMIN=0              VMIN=1
        #   quiet, far end alive read -> b''         EAGAIN
        #   far end gone         read -> b''         read -> b''
        #
        # So with VMIN=0, which is the conventional polling configuration used
        # here, read() alone cannot distinguish them at all. What does, in both
        # configurations, is whether the descriptor is readable:
        #
        #   not readable                 nothing waiting. Normal.
        #   readable and read gives b""  end of file. The board is gone.
        #
        # Hence the select, which is about correctness rather than efficiency
        # and must not be removed as an optimisation.
        readable, _, _ = select.select([self._fd], [], [], 0)
        if not readable:
            chunk = None  # nothing waiting, which is not the same as nothing left
        else:
            try:
                chunk = os.read(self._fd, 4096)
            except (BlockingIOError, InterruptedError):
                chunk = None
            except OSError as exc:
                if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK, errno.EINTR):
                    chunk = None
                elif exc.errno in (errno.ENXIO, errno.EIO, errno.EBADF):
                    # What a real USB serial device does when it is unplugged.
                    raise SerialPortUnavailable(
                        "%s disappeared. The board was unplugged or reset." % self.path)
                else:
                    raise SerialPortUnavailable("could not read %s: %s" % (self.path, exc))

            if chunk == b"":
                raise SerialPortUnavailable(
                    "%s stopped responding. The board was unplugged or reset." % self.path)

        if chunk:
            self._buffer += chunk

        lines = []
        while True:
            index = self._buffer.find(b"\n")
            if index < 0:
                break
            raw, self._buffer = self._buffer[:index], self._buffer[index + 1:]
            # errors="replace" rather than dropping: a frame with one corrupted
            # byte still says it is a distress call, and the base station is
            # where a malformed one gets reported with a stated reason.
            lines.append(raw.decode("utf-8", "replace").strip())

        # A line longer than any frame this contract can produce is noise, and
        # holding it would mean its tail glues onto the next real frame.
        if len(self._buffer) > 4096:
            self._buffer = b""

        return [ln for ln in lines if ln]

    def write_line(self, line):
        data = (line + "\n").encode("utf-8")
        try:
            while data:
                written = os.write(self._fd, data)
                data = data[written:]
        except OSError as exc:
            raise SerialPortUnavailable("could not write to %s: %s" % (self.path, exc))

    def close(self):
        try:
            os.close(self._fd)
        except OSError:
            pass


def open_first_available(path=None, baud=115200):
    """Opens the named port, or the only obvious candidate.

    Refuses to choose when there is more than one, because picking the wrong
    board and reporting nothing is worse than asking.
    """
    if path:
        return SerialPort(path, baud)

    candidates = list_candidate_ports()
    if not candidates:
        raise SerialPortUnavailable(describe_no_port())
    if len(candidates) > 1:
        raise SerialPortUnavailable(
            "more than one serial device is plugged in, so this will not guess. "
            "Name the one you want with --port:\n  " + "\n  ".join(candidates))
    return SerialPort(candidates[0], baud)


if __name__ == "__main__":
    # A way to see whether the cable works at all, before anything else is
    # blamed. Prints whatever the board says, forever.
    import time

    try:
        port = open_first_available(sys.argv[1] if len(sys.argv) > 1 else None)
    except SerialPortUnavailable as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(1)

    print("reading %s, ctrl-c to stop" % port.path, file=sys.stderr)
    try:
        while True:
            for line in port.read_lines():
                print(line, flush=True)
            time.sleep(0.02)
    except KeyboardInterrupt:
        pass
    finally:
        port.close()
