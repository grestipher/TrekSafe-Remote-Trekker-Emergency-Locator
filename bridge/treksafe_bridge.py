#!/usr/bin/env python3
"""TrekSafe bridge. Carries frames between the mesh and the console.

Decision D5. This runs as its own process and talks to the console over a local
HTTP connection rather than being imported into it.

The original reason was licensing: the Meshtastic Python library is GPL-3.0-only
and linking it into the console would have made the console a derivative work.
That reason went away with D10, when TrekSafe stopped being a Meshtastic module
and got its own firmware, and the decision stands anyway on the strength of what
it turned out to buy. A separate process outlives the browser, which is what
makes persistence possible at all, D6. It survives the console being reloaded,
closed, or opened on a second screen. And it keeps the thing that talks to
hardware away from the thing a warden is looking at, so a wedged radio driver
cannot take the screen down with it.

Transport to the console is Server-Sent Events one way and an HTTP POST the
other. No dependency, no build step, no WebSocket library, and it works from a
console opened straight off the disk.

Nothing here has any opinion about what a frame means. Parsing lives in the
console and in the firmware, which are tested. This moves bytes and says so.

    python3 bridge/treksafe_bridge.py --source sim --open
    python3 bridge/treksafe_bridge.py --source node --open
    python3 bridge/treksafe_bridge.py --source replay:bridge/sample-feed.txt
"""

import argparse
import errno
import json
import os
import posixpath
import queue
import re
import sqlite3
import sys
import webbrowser
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from store import Store, now_ms

DEFAULT_LISTEN = 8730

# How often the console is told the gateway is alive. Must be comfortably under
# the console's warning threshold of 90 seconds, so a healthy link never looks
# overdue. See issue I3.
HEARTBEAT_SECONDS = 15

# How long a stream sits on an empty queue before writing a comment line to find
# out whether the console is still there. Nothing to do with the heartbeat: this
# is about noticing a console that has gone, not about telling one it is covered.
STREAM_PING_SECONDS = 10


# ---------------------------------------------------------------------------
# Pure helpers. Tested in bridge/test_bridge.py.
# ---------------------------------------------------------------------------

def sse_event(name, data):
    """Formats one Server-Sent Event.

    A frame is a single line by contract, but a corrupted one could carry a line
    break, and a raw one would end the event early and split one distress call
    into two malformed ones. Every line is prefixed.

    Server-Sent Events terminate a line on a carriage return as well as a
    newline, and on the pair. Splitting on newlines alone left a lone carriage
    return to truncate the event at the client, which is a distress call
    arriving mangled with nothing anywhere saying why.
    """
    lines = re.split(r"\r\n|\r|\n", str(data))
    body = "".join("data: %s\n" % line for line in lines)
    return "event: %s\n%s\n" % (name, body)


def parse_source_spec(spec):
    """Splits a source argument into (kind, argument).

    replay:path  read frames from a file
    stdin        read frames from standard input
    node         read frames from a board running the beacon firmware
    sim          everything simulated, no hardware at all
    """
    if not spec:
        raise ValueError("no source given")
    if ":" in spec:
        kind, _, arg = spec.partition(":")
        return kind.strip().lower(), arg.strip()
    return spec.strip().lower(), None



LOOPBACK_HOSTS = ("127.0.0.1", "localhost", "[::1]", "::1")


def allowed_origins(port):
    """Which pages may talk to this bridge.

    This used to answer every request with Access-Control-Allow-Origin: *, which
    is not a small thing on a machine at a staffed facility where somebody also
    reads the news. A wildcard means any page in that browser can read
    /export.csv, which is every stored frame including the position of a person
    in trouble, and can POST a forged acknowledgement to /send, telling a beacon
    help is coming when nobody has looked. That is D4 defeated by a browser tab.

    The console served from here is same origin. A console opened from disk
    sends the origin "null", which is allowed because that fallback is a
    documented way to run, and it is no weaker than the wildcard it replaces.
    """
    return {
        "http://127.0.0.1:%d" % port,
        "http://localhost:%d" % port,
        "http://[::1]:%d" % port,
        "null",
    }


def host_is_loopback(host_header):
    """Whether a Host header names this machine.

    A browser will happily resolve a hostname somebody else controls to
    127.0.0.1 and then treat this bridge as that origin, which is how a remote
    page reaches a service that only listens on loopback. The listening socket
    cannot see the difference. The Host header can.
    """
    if not host_header:
        return False
    host = host_header.strip()
    if host.startswith("["):  # bracketed IPv6, with or without a port
        host = host.split("]", 1)[0] + "]"
    elif ":" in host:
        host = host.rsplit(":", 1)[0]
    return host.lower() in LOOPBACK_HOSTS


def journal_line(frame, direction, when=None):
    """One line of the append-only journal.

    This is not the persistence decision. D6 is still open and this chooses no
    database. It is a record of what crossed the wire, written before anything
    is interpreted, so that a crash cannot destroy the evidence that a distress
    call arrived. That much is worth having whatever D6 lands on.
    """
    stamp = (when or datetime.now(timezone.utc)).isoformat(timespec="seconds")
    return json.dumps({"at": stamp, "dir": direction, "frame": frame}, sort_keys=True)


# ---------------------------------------------------------------------------
# Fan-out to connected consoles
# ---------------------------------------------------------------------------

class Hub:
    """Holds one queue per connected console. Live traffic only.

    Catching up a console that attaches late is the store's job, not this one.
    An in-memory ring buffer used to do it, and was removed when the store
    arrived: two mechanisms for the same thing is how one of them quietly stops
    being maintained.
    """

    def __init__(self):
        self._lock = threading.Lock()
        self._subscribers = set()

    def subscribe(self):
        q = queue.Queue(maxsize=200)
        with self._lock:
            self._subscribers.add(q)
        return q

    def unsubscribe(self, q):
        with self._lock:
            self._subscribers.discard(q)

    def broadcast(self, name, data):
        payload = sse_event(name, data)
        with self._lock:
            targets = list(self._subscribers)
        for q in targets:
            try:
                q.put_nowait(payload)
            except queue.Full:
                # A console that cannot keep up is a console that is wedged.
                # Dropping the oldest would silently lose a distress call, so
                # the connection is dropped instead and the console will show
                # the feed as lost, which is the truth.
                self.unsubscribe(q)

    @property
    def count(self):
        with self._lock:
            return len(self._subscribers)


# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

class Source:
    name = "unnamed"

    # Whether this source can honestly claim a gateway is alive. See NoSource.
    heartbeats = True

    def run(self, on_frame, stop):
        raise NotImplementedError

    def send(self, frame):
        """Returns (ok, detail). The base source cannot transmit."""
        return False, "this source cannot transmit, it is not attached to a radio"


class ReplaySource(Source):
    """Replays frames from a file. Exists because there is one board.

    With a single Heltec, the beacon and the base station cannot both be on the
    air, so this is how the whole console path gets exercised end to end.
    """

    name = "replay"

    def __init__(self, path, interval=2.0):
        self.path = path
        self.interval = interval
        self.sent = []

    def run(self, on_frame, stop):
        with open(self.path, "r", encoding="utf-8") as fh:
            lines = [ln.strip() for ln in fh]
        for line in lines:
            if stop.is_set():
                return
            if not line or line.startswith("#"):
                continue
            on_frame(line)
            stop.wait(self.interval)
        # Stay up after the file is exhausted so the heartbeat keeps running and
        # the console does not report a lost feed just because the script ended.
        while not stop.is_set():
            stop.wait(0.5)

    def send(self, frame):
        self.sent.append(frame)
        return True, "recorded, no radio attached"



class NoSource(Source):
    """Serves the console and the store with no radio at all.

    For reviewing stored incidents after the fact, or showing the console to
    someone. It deliberately sends no heartbeat: a heartbeat means a gateway is
    alive, and with no source there is no gateway. The console shows no live
    feed, which is the truth.
    """

    name = "none"
    heartbeats = False

    def run(self, on_frame, stop):
        while not stop.is_set():
            stop.wait(0.5)

    def send(self, frame):
        return False, "no radio attached, this bridge is serving the console only"


class StdinSource(Source):
    name = "stdin"

    def __init__(self):
        self.sent = []

    def run(self, on_frame, stop):
        for line in sys.stdin:
            if stop.is_set():
                return
            line = line.strip()
            if line:
                on_frame(line)

    def send(self, frame):
        self.sent.append(frame)
        print("[to mesh] %s" % frame, flush=True)
        return True, "printed, no radio attached"



def build_source(spec, serial_port=None, interval=2.0):
    kind, arg = parse_source_spec(spec)
    if kind == "replay":
        if not arg:
            raise ValueError("replay needs a file, for example replay:bridge/sample-feed.txt")
        return ReplaySource(arg, interval=interval)
    if kind == "none":
        return NoSource()

    # TrekSafe's own firmware. Imported here rather than at the top of the file
    # so that a bridge running a replay, or serving nothing but the stored
    # record, still starts on a machine where sim/ is missing or unreadable.
    if kind in ("node", "sim", "simulated", "gateway"):
        import node_source

        if kind == "node":
            return node_source.NodeOverUsbSource(port=serial_port or arg)
        if kind == "gateway":
            return node_source.GatewayOverUsbSource(port=serial_port or arg)
        # sim[:speed], where speed scales the clock so a demonstration does not
        # need thirty seconds of standing about to show a repeat.
        speed = 1.0
        if arg:
            try:
                speed = float(arg)
            except ValueError:
                raise ValueError("sim takes a speed multiplier, for example sim:10")
        return node_source.SimulatedMeshSource(speed=speed)
    if kind == "stdin":
        return StdinSource()

    # "meshtastic" and "meshtastic-tcp" used to be here. They went with D10,
    # along with the module they talked to, and are named in this message rather
    # than silently missing because anyone who has run this project before will
    # type them out of habit.
    if kind in ("meshtastic", "meshtastic-tcp", "tcp"):
        raise ValueError(
            "the %s source was removed with decision D10, when TrekSafe stopped "
            "being a Meshtastic module and got its own firmware. Use 'node' for a "
            "board running that firmware over USB, or 'sim' for no hardware at all. "
            "The old source is kept, unbuilt, in bridge/archive-meshtastic/." % kind)

    raise ValueError("unknown source %r. Expected one of:\n"
                     "  sim[:speed]        everything simulated, no hardware needed\n"
                     "  node[:port]        a board running the beacon firmware over USB\n"
                     "  gateway[:port]     a real gateway board with a radio\n"
                     "  replay:<file>      a scripted file of frames\n"
                     "  stdin              frames typed or piped in\n"
                     "  none               serve the console only, no feed" % spec)


# ---------------------------------------------------------------------------
# HTTP surface
# ---------------------------------------------------------------------------

class BridgeServer(ThreadingHTTPServer):
    """Quiet about a console that simply closed.

    A browser closing a tab resets the connection, and the default behaviour is
    to print a stack trace. This process's standard error is where a warden
    setting up a base station reads why it will not start, and burying that
    under tracebacks from ordinary tab closes is how a real message gets missed.
    Anything that is not a dropped connection still prints.
    """

    daemon_threads = True

    def handle_error(self, request, client_address):
        exc = sys.exception()
        if isinstance(exc, (BrokenPipeError, ConnectionResetError, ConnectionAbortedError)):
            return
        super().handle_error(request, client_address)


def make_handler(hub, state):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            pass  # the bridge prints what matters itself

        def _cors(self):
            # Named origins rather than a wildcard. See allowed_origins().
            origin = self.headers.get("Origin")
            if origin and origin in state["allowed_origins"]:
                self.send_header("Access-Control-Allow-Origin", origin)
                self.send_header("Vary", "Origin")

        def _origin_ok(self):
            """A request from a page this bridge does not serve is refused.

            A browser attaches Origin to every cross origin request and cannot be
            talked out of it, so this is what stops another tab driving the base
            station. A request with no Origin at all is a tool rather than a
            page, curl or the console's own fetch, and is allowed.
            """
            origin = self.headers.get("Origin")
            return origin is None or origin in state["allowed_origins"]

        def _guard(self):
            """Checks every request shares. Returns False once it has answered."""
            if not host_is_loopback(self.headers.get("Host")):
                self.send_error(403, "this bridge answers only to a loopback host name")
                return False
            if not self._origin_ok():
                self.send_error(403, "this bridge does not answer pages from another origin")
                return False
            return True

        def do_OPTIONS(self):
            if not self._guard():
                return
            self.send_response(204)
            self._cors()
            self.send_header("Access-Control-Allow-Headers", "Content-Type")
            self.send_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS")
            self.send_header("Content-Length", "0")
            self.end_headers()

        def do_GET(self):
            if not self._guard():
                return
            if self.path.startswith("/stream"):
                return self._stream()
            if self.path.startswith("/status"):
                return self._status()
            if self.path.startswith("/export.csv"):
                return self._export()
            return self._static()

        # Serving the console from here is not a convenience. A page opened from
        # file:// is a hostile environment for it: browsers differ on whether it
        # may load its own scripts, and OpenStreetMap refuses map tiles to a
        # request with no Referer, so the map is blank from disk whatever the
        # browser does. Served from the bridge it is one ordinary same origin
        # page, which also removes the cross origin question entirely.
        TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
                 ".css": "text/css; charset=utf-8", ".json": "application/json",
                 ".png": "image/png", ".svg": "image/svg+xml", ".ico": "image/x-icon"}

        def _static(self):
            root = state["console_dir"]
            if not root:
                return self.send_error(404, "this bridge is not serving the console")

            rel = self.path.split("?", 1)[0].split("#", 1)[0]
            rel = posixpath.normpath(rel).lstrip("/")
            if rel in ("", "."):
                rel = "index.html"

            target = os.path.normpath(os.path.join(root, rel))
            # Never serve outside the console directory, whatever the path says.
            # commonpath raises on inputs it cannot compare, and a path we
            # cannot reason about is one we refuse rather than serve.
            try:
                inside = os.path.commonpath(
                    [os.path.abspath(target), os.path.abspath(root)]) == os.path.abspath(root)
            except ValueError:
                inside = False
            if not inside:
                return self.send_error(403, "outside the console directory")
            if not os.path.isfile(target):
                return self.send_error(404, "no such file in the console directory")

            try:
                with open(target, "rb") as fh:
                    body = fh.read()
            except OSError as exc:
                return self.send_error(500, "could not read %s: %s" % (rel, exc))

            ctype = self.TYPES.get(os.path.splitext(target)[1].lower(), "application/octet-stream")
            self.send_response(200)
            self._cors()
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            # The operator must never be looking at a cached console after an
            # update has been installed.
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _export(self):
            """The whole stored record as CSV, for an incident review.

            Raw frames and operator decisions with their times, nothing
            interpreted. A review needs what arrived, not this system's reading
            of what arrived.
            """
            csv = state["export_csv"]()
            if csv is None:
                return self.send_error(404, "this bridge is running without a store")
            body = csv.encode("utf-8")
            self.send_response(200)
            self._cors()
            self.send_header("Content-Type", "text/csv; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Content-Disposition", 'attachment; filename="treksafe-record.csv"')
            self.end_headers()
            self.wfile.write(body)

        def _status(self):
            body = json.dumps({
                "source": state["source_name"],
                "consoles": hub.count,
                "frames_in": state["frames_in"],
                "frames_out": state["frames_out"],
                "started": state["started"],
                "stored": state["counts"](),
            }).encode()
            self.send_response(200)
            self._cors()
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _stream(self):
            # Attaching and reading the history must not interleave with a
            # frame arriving. Without this lock a frame that lands between the
            # two is both stored (so it is in the history) and broadcast (so it
            # is in the queue), and the console sees one distress call twice,
            # which inflates its repeat count.
            with state["attach_lock"]:
                q = hub.subscribe()
                history = state["history"]()
            self.send_response(200)
            self._cors()
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            print("[bridge] console connected, %d now attached" % hub.count, flush=True)
            try:
                # Tell a console the moment it attaches, so it does not sit
                # showing "no live feed" for a whole heartbeat interval.
                #
                # Only when there is actually a radio behind this. A heartbeat
                # means a gateway is alive, and greeting a console with one
                # while nothing is attached is the same lie as beating over a
                # dead radio, just at a more convincing moment.
                if state["heartbeats"] and state["source_connected"]():
                    self.wfile.write(sse_event("heartbeat", "hello").encode())
                    self.wfile.flush()

                # Then rebuild it. The console is the only thing that knows
                # what a frame means, so it is handed the raw history and works
                # its own incidents out again. Decision D6.
                if history:
                    print("[bridge] replaying %d stored records to a new console" % len(history), flush=True)
                for record in history:
                    self.wfile.write(sse_event("replay", json.dumps(record)).encode())
                self.wfile.write(sse_event("replay-done", str(len(history))).encode())
                self.wfile.flush()
                while True:
                    try:
                        payload = q.get(timeout=STREAM_PING_SECONDS)
                    except queue.Empty:
                        # A comment line. The console ignores it; the socket does
                        # not, so this is what notices a console that has gone.
                        #
                        # Without it this thread blocked on an empty queue until
                        # something was broadcast. With a source that sends no
                        # heartbeat, which is how the console is shown to
                        # somebody, nothing ever was: every reload left a thread
                        # parked forever and /status kept counting consoles that
                        # had closed hours ago.
                        payload = ": ping\n\n"
                    self.wfile.write(payload.encode())
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, ValueError, OSError):
                pass
            finally:
                hub.unsubscribe(q)
                print("[bridge] console disconnected, %d still attached" % hub.count, flush=True)

        def do_POST(self):
            if not self._guard():
                return
            if self.path.startswith("/action"):
                return self._action()
            if not self.path.startswith("/send"):
                return self.send_error(404, "try /send or /action")
            body = self._body()
            if body is None:
                return self._reply(400, {"ok": False, "detail": "unreadable request body"})
            frame = body.decode("utf-8", "replace").strip()

            if not frame:
                return self._reply(400, {"ok": False, "detail": "empty frame"})

            ok, detail = state["source"].send(frame)

            # Recorded after the radio took it, not before.
            #
            # An outbound frame in the store means a frame that went on the air,
            # and the console rebuilds an acknowledgement from exactly that. When
            # this recorded first, a send the radio refused was still stored as
            # sent, so after a restart the console showed an incident as
            # acknowledged that nobody had ever been told about, and it did not
            # alarm. A failed send is kept too, as an operator action, because it
            # is evidence and a review needs it, but it is not a frame and the
            # console must never rebuild an acknowledgement out of it.
            if ok:
                state["journal"](frame, "out")
                state["record_frame_out"](frame)
                state["frames_out"] += 1
                print("[bridge] to mesh: %s (%s)" % (frame, detail), flush=True)
            else:
                state["journal"](frame, "out-failed")
                state["record_action"]("send-failed", frame, detail)
                print("[bridge] COULD NOT SEND: %s (%s)" % (frame, detail), file=sys.stderr, flush=True)
            return self._reply(200 if ok else 503, {"ok": ok, "detail": detail})

        def _action(self):
            """An operator decision that leaves no trace on the radio.

            Acknowledgement needs nothing here, because it puts an ACK frame on
            the air and that frame is stored. Marking an unidentified incident
            as seen sends nothing, so without this a restart would alarm again
            for something a human already dealt with."""
            raw = self._body()
            if raw is None:
                return self._reply(400, {"ok": False, "detail": "unreadable request body"})
            try:
                body = json.loads(raw.decode("utf-8", "replace"))
                kind = str(body["kind"])
                subject = str(body["subject"])
            except (ValueError, KeyError, TypeError) as exc:
                return self._reply(400, {"ok": False, "detail": "unreadable action: %s" % exc})

            state["record_action"](kind, subject)
            print("[bridge] operator action: %s on %s" % (kind, subject), flush=True)
            return self._reply(200, {"ok": True, "detail": "recorded"})

        # No frame this contract defines is anywhere near this size. A bad or
        # hostile Content-Length would otherwise have the bridge try to allocate
        # whatever it was told to, and a base station that falls over is a base
        # station that is not listening for distress calls.
        MAX_BODY = 64 * 1024

        def _body(self):
            raw = self.headers.get("Content-Length")
            try:
                length = int(raw or 0)
            except (TypeError, ValueError):
                return None
            if length < 0 or length > self.MAX_BODY:
                return None
            try:
                return self.rfile.read(length)
            except OSError:
                return None

        def _reply(self, code, obj):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self._cors()
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    return Handler


# ---------------------------------------------------------------------------

def build_state(source, store=None, console_dir=None, journal=None, replay_hours=12,
                listen_port=DEFAULT_LISTEN):
    """Everything the request handler needs, in one place.

    main() and the tests both build it here on purpose. When this was a dict
    written out by hand in each, adding a key to one silently broke the other,
    and the tests went green against a shape production no longer used.
    """
    noop = lambda *a, **k: None  # noqa: E731

    def history():
        if not store:
            return []
        return store.history(now_ms() - int(replay_hours * 3600 * 1000))

    return {
        "source": source,
        "source_name": source.name,
        "frames_in": 0,
        "frames_out": 0,
        "started": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "journal": journal or noop,
        "history": history,
        "record_action": ((lambda kind, subject, detail=None: store.record_action(kind, subject, detail=detail))
                          if store else noop),
        "record_frame_out": (lambda f: store.record_frame(f, "out")) if store else noop,
        "counts": (lambda: store.counts()) if store else (lambda: None),
        "export_csv": (lambda: store.export_csv()) if store else (lambda: None),
        "console_dir": console_dir,
        "attach_lock": threading.Lock(),
        "heartbeats": getattr(source, "heartbeats", True),
        # A source that can lose its hardware and get it back reports that here.
        # One that cannot is always connected, which is what the default says.
        "source_connected": (lambda: getattr(source, "connected", True)),
        "allowed_origins": allowed_origins(listen_port),
    }


def fail(message):
    """A startup problem is printed as one plain sentence, never a traceback.
    The person reading it may be a warden setting up a base station, not the
    person who wrote this."""
    print("[bridge] CANNOT START: %s" % message, file=sys.stderr, flush=True)
    return 1


def main(argv=None):
    # Every default path is anchored to this repository, found from this file,
    # never to the directory the command happened to be typed in. Run from
    # anywhere. A cwd-relative default here once made the bridge crash when
    # started from the wrong directory, which reads as "the console did not
    # load" to the person at the screen.
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    ap = argparse.ArgumentParser(description="TrekSafe node to console bridge")
    ap.add_argument("--source", default="none",
                    help="replay:<file>, stdin, meshtastic, or none for console only")
    ap.add_argument("--serial-port", default=None,
                    help="serial device for the meshtastic source, autodetected if omitted")
    ap.add_argument("--listen", type=int, default=DEFAULT_LISTEN)
    ap.add_argument("--interval", type=float, default=2.0,
                    help="seconds between frames for the replay source")
    ap.add_argument("--console-dir", default=os.path.join(repo_root, "console"),
                    help="serve the console from here, so it runs as an ordinary web page")
    ap.add_argument("--no-console", action="store_true",
                    help="do not serve the console, only the feed endpoints")
    ap.add_argument("--store", default=os.path.join(repo_root, "bridge", "treksafe.db"),
                    help="SQLite file holding every frame and operator action, D6")
    ap.add_argument("--no-store", action="store_true",
                    help="run without durable storage, nothing survives a restart")
    ap.add_argument("--replay-hours", type=float, default=12,
                    help="how far back an attaching console is rebuilt from the store")
    ap.add_argument("--journal", default=None,
                    help="append every frame to this file before it is interpreted")
    ap.add_argument("--open", action="store_true",
                    help="open the console in the default browser once the bridge is up")
    args = ap.parse_args(argv)

    # A relative replay path is tried against the cwd first, then the repo, so
    # both `--source replay:bridge/sample-feed.txt` and an absolute path work
    # from any directory.
    kind, arg = parse_source_spec(args.source)
    if kind == "replay" and arg and not os.path.isabs(arg) and not os.path.isfile(arg):
        anchored = os.path.join(repo_root, arg)
        if os.path.isfile(anchored):
            args.source = "replay:" + anchored

    try:
        source = build_source(args.source, args.serial_port, args.interval)
    except ValueError as exc:
        return fail(str(exc))

    if source.name == "replay" and not os.path.isfile(source.path):
        return fail("replay file not found: %s" % source.path)

    console_dir = None if args.no_console else os.path.abspath(args.console_dir)
    if console_dir and not os.path.isfile(os.path.join(console_dir, "index.html")):
        return fail("no console at %s. Pass --console-dir with the right path, "
                    "or --no-console to serve only the feed." % console_dir)

    hub = Hub()
    stop = threading.Event()

    store = None
    if not args.no_store:
        try:
            os.makedirs(os.path.dirname(os.path.abspath(args.store)) or ".", exist_ok=True)
            store = Store(args.store)
        except (OSError, sqlite3.Error) as exc:
            return fail("could not open the store at %s (%s). Fix the path or "
                        "pass --no-store to run without one." % (args.store, exc))

    journal_lock = threading.Lock()

    def journal(frame, direction):
        if not args.journal:
            return
        try:
            with journal_lock, open(args.journal, "a", encoding="utf-8") as fh:
                fh.write(journal_line(frame, direction) + "\n")
        except OSError as exc:
            print("[bridge] journal write failed: %s" % exc, file=sys.stderr, flush=True)

    state = build_state(source, store=store, console_dir=console_dir,
                        journal=journal, replay_hours=args.replay_hours,
                        listen_port=args.listen)

    def on_frame(frame, link=None):
        state["frames_in"] += 1
        # Storing and broadcasting are one step, so a console attaching cannot
        # observe a frame in the store and in the live queue at the same time
        # and count it twice. See _stream.
        with state["attach_lock"]:
            # Stored before anything interprets it, so a crash cannot destroy
            # the evidence that a distress call arrived.
            if store:
                store.record_frame(frame, "in", link=link)
            journal(frame, "in")
            # Sent as JSON so link quality travels with the frame, the same
            # shape a replayed record has. The console reads both the same way.
            payload = dict(link or {})
            payload["frame"] = frame
            hub.broadcast("packet", json.dumps(payload))
        detail = ""
        if link:
            bits = []
            if "snr" in link:
                bits.append("snr %.1f" % link["snr"])
            if "hops" in link:
                bits.append("%d hop%s" % (link["hops"], "" if link["hops"] == 1 else "s"))
            if bits:
                detail = "  [" + ", ".join(bits) + "]"
        print("[bridge] from mesh: %s%s" % (frame, detail), flush=True)

    try:
        server = BridgeServer(("127.0.0.1", args.listen), make_handler(hub, state))
    except OSError as exc:
        if exc.errno == errno.EADDRINUSE:
            return fail("port %d is already in use, probably by another bridge. "
                        "Stop it, or pass --listen with a different port."
                        % args.listen)
        return fail("could not listen on port %d: %s" % (args.listen, exc))
    server.daemon_threads = True

    # If the source thread dies, the whole bridge dies, loudly. The alternative
    # is quietly worse than a crash: the heartbeat would keep telling every
    # console the gateway is healthy while nothing is reading the radio, which
    # is precisely the false confidence this product exists to prevent.
    source_died = threading.Event()

    def run_source():
        try:
            source.run(on_frame, stop)
            # A source that returns while the bridge is still meant to be
            # running has stopped reading, which is the same problem as a
            # crash: stdin hit end of input, or a driver gave up quietly.
            if not stop.is_set():
                print("[bridge] THE %s SOURCE STOPPED: it reached the end of its input"
                      % source.name, file=sys.stderr, flush=True)
                source_died.set()
        except Exception as exc:
            print("[bridge] THE %s SOURCE DIED: %s" % (source.name, exc),
                  file=sys.stderr, flush=True)
            source_died.set()

    threading.Thread(target=server.serve_forever, daemon=True).start()
    threading.Thread(target=run_source, daemon=True).start()

    # A heartbeat means a gateway is alive. A source that is not attached to a
    # radio must never send one, or the console would sit green over nothing.
    if state["heartbeats"]:
        def beat():
            while not stop.is_set():
                # Only while the source can actually hear the mesh.
                #
                # A source that has lost its board reconnects rather than dying,
                # which keeps a slept laptop or a nudged cable from taking the
                # whole base station down. The price of that leniency is that
                # something has to stop the heartbeat for the duration, or the
                # console would sit green over a gap where nothing was
                # listening. That is the precise false confidence D7 is about,
                # and it is worth more than the convenience of never checking.
                if state["source_connected"]():
                    hub.broadcast("heartbeat", str(int(time.time())))
                stop.wait(HEARTBEAT_SECONDS)

        threading.Thread(target=beat, daemon=True).start()
    else:
        print("[bridge] no radio attached, so no heartbeat: the console will "
              "show no live feed, which is true", flush=True)

    if store:
        print("[bridge] store %s, %s" % (args.store, store.counts()), flush=True)
    else:
        print("[bridge] running with no store, nothing survives a restart", flush=True)
    print("[bridge] listening on http://127.0.0.1:%d, source %s" % (args.listen, source.name), flush=True)

    url = "http://127.0.0.1:%d/" % args.listen
    if console_dir:
        print("[bridge] open the console at %s" % url, flush=True)
        print("[bridge] keep this window open. Closing it takes the console down with it.", flush=True)
        if args.open:
            webbrowser.open(url)
    else:
        print("[bridge] not serving a console, open console/index.html yourself", flush=True)

    try:
        while True:
            if source_died.is_set():
                stop.set()
                server.shutdown()
                print("[bridge] STOPPED: the %s source is gone, so this bridge shut "
                      "itself down rather than keep telling consoles the feed was "
                      "healthy. The consoles will show the feed as lost. Restart "
                      "the bridge." % source.name, file=sys.stderr, flush=True)
                return 1
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\n[bridge] stopping", flush=True)
        stop.set()
        server.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
