"""Bridge tests. Standard library only, no board and no Meshtastic install.

    python3 -m unittest discover -s bridge -p 'test_*.py'

Only the parts that can be wrong in a way that loses a frame are tested here.
The bridge holds no opinion about what a frame means, so there is nothing else
worth asserting about it.
"""

import contextlib
import json
import os
import sys
import tempfile
import threading
import time
import unittest
import urllib.request
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import treksafe_bridge as tb  # noqa: E402
from store import Store  # noqa: E402


class SseFormatting(unittest.TestCase):
    def test_a_plain_frame(self):
        self.assertEqual(
            tb.sse_event("packet", "SOS|1|A4F2|52.8|-118.0|12|1755000000|M|87"),
            "event: packet\ndata: SOS|1|A4F2|52.8|-118.0|12|1755000000|M|87\n\n")

    def test_a_frame_carrying_a_carriage_return_is_not_split_into_two(self):
        """Server-Sent Events end a line on a carriage return as well as a
        newline. Splitting on newlines alone left a lone carriage return to
        truncate the event at the console: a distress call arriving mangled,
        with nothing anywhere saying why."""
        out = tb.sse_event("packet", "SOS|1|A4F2\r|52.87|-118.08")
        for line in out.rstrip("\n").split("\n"):
            self.assertTrue(line.startswith("event: ") or line.startswith("data: "), line)
        self.assertNotIn("\r", out)

    def test_a_frame_carrying_a_newline_is_not_split_into_two(self):
        """A corrupted frame with a newline in it would otherwise end the event
        early and arrive at the console as two malformed distress calls."""
        out = tb.sse_event("packet", "SOS|1|A4F2\nEXTRA")
        self.assertEqual(out, "event: packet\ndata: SOS|1|A4F2\ndata: EXTRA\n\n")
        self.assertEqual(out.count("\n\n"), 1, "exactly one event boundary")

    def test_an_empty_frame_still_produces_a_well_formed_event(self):
        self.assertEqual(tb.sse_event("heartbeat", ""), "event: heartbeat\ndata: \n\n")


class SourceSpecs(unittest.TestCase):
    def test_replay_with_a_path(self):
        self.assertEqual(tb.parse_source_spec("replay:bridge/sample-feed.txt"),
                         ("replay", "bridge/sample-feed.txt"))

    def test_bare_kinds(self):
        self.assertEqual(tb.parse_source_spec("stdin"), ("stdin", None))
        self.assertEqual(tb.parse_source_spec("node"), ("node", None))
        self.assertEqual(tb.parse_source_spec("sim"), ("sim", None))

    def test_a_source_that_was_removed_says_what_replaced_it(self):
        """Anyone who has run this project before will type these out of habit,
        and "unknown source" would send them looking for a typo that is not
        there. See D10."""
        for gone in ("meshtastic", "meshtastic-tcp"):
            with self.assertRaises(ValueError) as ctx:
                tb.build_source(gone)
            message = str(ctx.exception)
            self.assertIn("D10", message)
            self.assertIn("node", message, "and what to use instead")

    def test_an_empty_spec_is_refused(self):
        with self.assertRaises(ValueError):
            tb.parse_source_spec("")

    def test_an_unknown_source_is_refused_by_name(self):
        with self.assertRaises(ValueError) as ctx:
            tb.build_source("carrierpigeon")
        self.assertIn("carrierpigeon", str(ctx.exception))

    def test_replay_without_a_file_is_refused(self):
        with self.assertRaises(ValueError):
            tb.build_source("replay")

    def test_the_none_source_serves_the_console_without_a_radio(self):
        src = tb.build_source("none")
        self.assertEqual(src.name, "none")
        ok, detail = src.send("ACK|1|A4F2|INC-001")
        self.assertFalse(ok)
        self.assertIn("no radio", detail)

    def test_a_source_with_no_radio_never_claims_a_gateway_is_alive(self):
        """A heartbeat means a gateway is alive. Sending one with nothing
        attached is the exact false confidence D7 forbids."""
        self.assertFalse(tb.build_source("none").heartbeats)
        self.assertTrue(tb.build_source("stdin").heartbeats)
        self.assertTrue(tb.build_source("sim").heartbeats)

    def test_a_source_with_no_radio_refuses_to_send_and_says_why(self):
        ok, detail = tb.Source().send("ACK|1|A4F2|INC-001")
        self.assertFalse(ok)
        self.assertIn("cannot transmit", detail)





class Journal(unittest.TestCase):
    def test_a_line_records_direction_and_frame(self):
        when = datetime(2026, 8, 22, 1, 2, 3, tzinfo=timezone.utc)
        parsed = json.loads(tb.journal_line("SOS|1|A4F2", "in", when))
        self.assertEqual(parsed["frame"], "SOS|1|A4F2")
        self.assertEqual(parsed["dir"], "in")
        self.assertEqual(parsed["at"], "2026-08-22T01:02:03+00:00")

    def test_a_frame_with_a_newline_stays_on_one_journal_line(self):
        line = tb.journal_line("SOS|1|A4F2\nEXTRA", "in")
        self.assertNotIn("\n", line)
        self.assertEqual(json.loads(line)["frame"], "SOS|1|A4F2\nEXTRA")


class HubFanOut(unittest.TestCase):
    def test_every_console_gets_every_frame(self):
        hub = tb.Hub()
        a, b = hub.subscribe(), hub.subscribe()
        hub.broadcast("packet", "SOS|1|A4F2")
        self.assertIn("SOS|1|A4F2", a.get_nowait())
        self.assertIn("SOS|1|A4F2", b.get_nowait())
        self.assertEqual(hub.count, 2)

    def test_a_console_that_cannot_keep_up_is_dropped_rather_than_fed_gaps(self):
        hub = tb.Hub()
        q = hub.subscribe()
        for _ in range(q.maxsize + 5):
            hub.broadcast("packet", "SOS|1|A4F2")
        self.assertEqual(hub.count, 0,
                         "a wedged console is disconnected so it shows the feed as lost")

    def test_unsubscribing_stops_delivery(self):
        hub = tb.Hub()
        q = hub.subscribe()
        hub.unsubscribe(q)
        hub.broadcast("packet", "SOS|1|A4F2")
        self.assertEqual(hub.count, 0)
        self.assertTrue(q.empty())


class StartupChecks(unittest.TestCase):
    """main() refuses bad setups with a plain sentence and exit code 1, before
    any thread starts. These run main() directly; every case returns."""

    def _run(self, argv):
        import contextlib, io
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            code = tb.main(argv)
        return code, err.getvalue()

    def test_an_unknown_source_fails_with_the_name_in_the_message(self):
        code, err = self._run(["--source", "pigeon", "--no-store"])
        self.assertEqual(code, 1)
        self.assertIn("pigeon", err)

    def test_a_missing_replay_file_fails_before_anything_starts(self):
        code, err = self._run(["--source", "replay:no-such-feed.txt", "--no-store"])
        self.assertEqual(code, 1)
        self.assertIn("replay file not found", err)

    def test_a_missing_console_directory_fails_with_the_path_named(self):
        code, err = self._run(["--source", "stdin", "--no-store",
                               "--console-dir", "/no/such/console"])
        self.assertEqual(code, 1)
        self.assertIn("/no/such/console", err)
        self.assertIn("--no-console", err, "the message must offer the way out")

    def test_a_busy_port_fails_with_advice_rather_than_a_traceback(self):
        import socket
        blocker = socket.socket()
        blocker.bind(("127.0.0.1", 0))
        port = blocker.getsockname()[1]
        blocker.listen(1)
        feed = tempfile.NamedTemporaryFile(suffix=".txt", delete=False)
        feed.close()
        try:
            code, err = self._run(["--source", "replay:" + feed.name, "--no-store",
                                   "--listen", str(port)])
            self.assertEqual(code, 1)
            self.assertIn("already in use", err)
            self.assertIn("--listen", err)
        finally:
            blocker.close()
            os.unlink(feed.name)

    def test_defaults_hold_from_any_working_directory(self):
        """The regression that shipped: cwd-relative defaults crashed the
        bridge or 404ed the console when it was started from anywhere but the
        repository root. Run from a temp directory, the startup checks for the
        console and the store must both pass, proven by failing later, on a
        deliberately busy port."""
        import socket
        blocker = socket.socket()
        blocker.bind(("127.0.0.1", 0))
        port = blocker.getsockname()[1]
        blocker.listen(1)
        before = os.getcwd()
        tmp = tempfile.mkdtemp()
        try:
            os.chdir(tmp)
            code, err = self._run(["--source", "replay:bridge/sample-feed.txt",
                                   "--listen", str(port)])
            self.assertEqual(code, 1)
            self.assertIn("already in use", err,
                          "expected only the port to fail: console, store and "
                          "replay path must all resolve against the repo, not the cwd")
        finally:
            os.chdir(before)
            blocker.close()
            os.rmdir(tmp)


class EndToEndOverHttp(unittest.TestCase):
    """Starts the real server on a loopback port and drives it over HTTP."""

    PORT = 8743

    @classmethod
    def setUpClass(cls):
        cls.source = tb.ReplaySource(os.devnull)
        cls.hub = tb.Hub()
        cls.dir = tempfile.TemporaryDirectory()
        cls.store = Store(os.path.join(cls.dir.name, "t.db"))
        # Built by the same function main() uses, so the tests cannot go green
        # against a shape production no longer has.
        cls.state = tb.build_state(cls.source, store=cls.store,
                                   console_dir=os.path.abspath("console"),
                                   listen_port=cls.PORT)
        cls.server = tb.BridgeServer(("127.0.0.1", cls.PORT),
                                     tb.make_handler(cls.hub, cls.state))
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.store.close()
        cls.dir.cleanup()

    def url(self, path):
        return "http://127.0.0.1:%d%s" % (self.PORT, path)

    def test_status_reports_what_the_bridge_is_attached_to(self):
        with urllib.request.urlopen(self.url("/status"), timeout=5) as r:
            body = json.loads(r.read())
        self.assertEqual(body["source"], "replay")
        self.assertIn("frames_in", body)

    def test_an_acknowledgement_reaches_the_source(self):
        req = urllib.request.Request(self.url("/send"), data=b"ACK|1|A4F2|INC-001",
                                     headers={"Content-Type": "text/plain"}, method="POST")
        with urllib.request.urlopen(req, timeout=5) as r:
            body = json.loads(r.read())
        self.assertTrue(body["ok"])
        self.assertIn("ACK|1|A4F2|INC-001", self.source.sent)

    def test_an_empty_acknowledgement_is_refused(self):
        req = urllib.request.Request(self.url("/send"), data=b"   ",
                                     headers={"Content-Type": "text/plain"}, method="POST")
        try:
            urllib.request.urlopen(req, timeout=5)
            self.fail("expected a refusal")
        except urllib.error.HTTPError as e:
            self.assertEqual(e.code, 400)

    def test_an_acknowledgement_is_stored_so_it_survives_a_restart(self):
        req = urllib.request.Request(self.url("/send"), data=b"ACK|1|C0DE|INC-009",
                                     headers={"Content-Type": "text/plain"}, method="POST")
        urllib.request.urlopen(req, timeout=5).read()
        stored = [r["frame"] for r in self.store.history(0) if r["dir"] == "out"]
        self.assertIn("ACK|1|C0DE|INC-009", stored)

    def test_an_operator_action_is_recorded(self):
        """Marking an unidentified incident as seen sends no frame, so without
        this the console would alarm again for it after a restart."""
        body = json.dumps({"kind": "seen", "subject": "SOS|1|ZZZZ|0|0|||U|"}).encode()
        req = urllib.request.Request(self.url("/action"), data=body,
                                     headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req, timeout=5) as r:
            self.assertTrue(json.loads(r.read())["ok"])
        kinds = [r.get("kind") for r in self.store.history(0) if r["dir"] == "action"]
        self.assertIn("seen", kinds)

    def test_an_unreadable_action_is_refused_rather_than_stored(self):
        req = urllib.request.Request(self.url("/action"), data=b"not json",
                                     headers={"Content-Type": "application/json"}, method="POST")
        try:
            urllib.request.urlopen(req, timeout=5)
            self.fail("expected a refusal")
        except urllib.error.HTTPError as e:
            self.assertEqual(e.code, 400)

    def test_the_console_is_told_a_specific_origin_and_never_a_wildcard(self):
        """A wildcard let any page in the operator's browser read /export.csv,
        which is every stored frame including the position of a person in
        trouble."""
        req = urllib.request.Request(
            self.url("/status"), headers={"Origin": "http://127.0.0.1:%d" % self.PORT})
        with urllib.request.urlopen(req, timeout=5) as r:
            self.assertEqual(r.headers.get("Access-Control-Allow-Origin"),
                             "http://127.0.0.1:%d" % self.PORT)

    def test_a_page_from_another_origin_cannot_read_the_incident_record(self):
        req = urllib.request.Request(self.url("/export.csv"),
                                     headers={"Origin": "https://evil.example"})
        try:
            urllib.request.urlopen(req, timeout=5)
            self.fail("expected a refusal")
        except urllib.error.HTTPError as e:
            self.assertEqual(e.code, 403)

    def test_a_page_from_another_origin_cannot_forge_an_acknowledgement(self):
        """Telling a beacon help is coming when nobody looked is exactly what D4
        forbids, and text/plain is a CORS simple request, so without this check
        any tab could do it with no preflight to stop it."""
        before = list(self.source.sent)
        req = urllib.request.Request(self.url("/send"), data=b"ACK|1|A4F2|INC-666",
                                     headers={"Content-Type": "text/plain",
                                              "Origin": "https://evil.example"},
                                     method="POST")
        try:
            urllib.request.urlopen(req, timeout=5)
            self.fail("expected a refusal")
        except urllib.error.HTTPError as e:
            self.assertEqual(e.code, 403)
        self.assertEqual(self.source.sent, before, "nothing must have reached the radio")

    def test_a_request_naming_someone_elses_host_is_refused(self):
        """A name somebody else controls can be pointed at 127.0.0.1, which is
        how a remote page reaches a service bound to loopback. The socket cannot
        tell; the Host header can."""
        req = urllib.request.Request(self.url("/status"), headers={"Host": "attacker.example"})
        try:
            urllib.request.urlopen(req, timeout=5)
            self.fail("expected a refusal")
        except urllib.error.HTTPError as e:
            self.assertEqual(e.code, 403)



    def test_an_attaching_console_is_replayed_the_stored_history(self):
        self.store.record_frame("SOS|1|B0B0|52.8|-118.0|12|1755000000|L|70", "in")
        seen = []
        done = threading.Event()

        def reader():
            with urllib.request.urlopen(self.url("/stream"), timeout=10) as r:
                for raw in r:
                    seen.append(raw.decode())
                    if "replay-done" in seen[-1]:
                        done.set()
                        return

        threading.Thread(target=reader, daemon=True).start()
        self.assertTrue(done.wait(10), "the console was never told the replay finished")
        joined = "".join(seen)
        self.assertIn("event: replay", joined)
        self.assertIn("SOS|1|B0B0", joined)
        self.assertLess(joined.index("event: heartbeat"), joined.index("event: replay"),
                        "a console learns the feed is alive before it is rebuilt")

    def test_the_bridge_serves_the_console_itself(self):
        """A page opened from disk is a hostile environment for this console:
        browsers differ on whether it may load its own scripts, and OSM refuses
        map tiles to a request with no Referer. Served from here it is one
        ordinary same origin page."""
        with urllib.request.urlopen(self.url("/"), timeout=5) as r:
            body = r.read().decode()
            self.assertEqual(r.headers["Content-Type"], "text/html; charset=utf-8")
        self.assertIn("Base Station Console", body)
        self.assertIn('src="lib/frames.js"', body)

    def test_the_console_scripts_are_served_with_a_usable_content_type(self):
        with urllib.request.urlopen(self.url("/lib/frames.js"), timeout=5) as r:
            self.assertEqual(r.headers["Content-Type"], "text/javascript; charset=utf-8")
            self.assertIn("hydrateDistress", r.read().decode())

    def test_the_console_is_never_served_from_cache(self):
        """An operator must not be looking at a stale console after an update."""
        with urllib.request.urlopen(self.url("/"), timeout=5) as r:
            self.assertEqual(r.headers["Cache-Control"], "no-store")

    def test_nothing_outside_the_console_directory_is_served(self):
        for attempt in ("/../CLAUDE.md", "/..%2fCLAUDE.md", "/lib/../../bridge/store.py",
                        "/....//CLAUDE.md"):
            try:
                with urllib.request.urlopen(self.url(attempt), timeout=5) as r:
                    served = r.read().decode("utf-8", "replace")
                self.assertNotIn("TrekSafe base station", served, "escaped the console directory")
                self.assertNotIn("import sqlite3", served, "escaped the console directory")
            except urllib.error.HTTPError as e:
                self.assertIn(e.code, (403, 404))

    def test_an_absurd_content_length_is_refused_rather_than_allocated(self):
        """A base station that falls over is a base station that is not
        listening for distress calls."""
        req = urllib.request.Request(self.url("/send"), data=b"ACK|1|A4F2|INC-001",
                                     headers={"Content-Type": "text/plain"}, method="POST")
        req.add_header("Content-Length", str(500 * 1024 * 1024))
        try:
            urllib.request.urlopen(req, timeout=5)
            self.fail("expected a refusal")
        except urllib.error.HTTPError as e:
            self.assertEqual(e.code, 400)
        except (urllib.error.URLError, OSError):
            pass  # the client gave up first, which is also a refusal

    def test_the_stored_record_exports_as_csv_for_a_review(self):
        self.store.record_frame("SOS|1|A4F2|52.8|-118.0|12|1755000000|M|87", "in",
                                link={"snr": 6.5, "hops": 1})
        with urllib.request.urlopen(self.url("/export.csv"), timeout=5) as r:
            body = r.read().decode()
            self.assertIn("text/csv", r.headers["Content-Type"])
            self.assertIn("attachment", r.headers["Content-Disposition"])
        self.assertIn("at_utc,at_ms,direction,snr,rssi,hops,source_note,frame", body)
        self.assertIn("SOS|1|A4F2", body)
        self.assertIn("6.5", body)

    def test_a_missing_console_file_is_a_plain_404(self):
        try:
            urllib.request.urlopen(self.url("/nope.js"), timeout=5)
            self.fail("expected a 404")
        except urllib.error.HTTPError as e:
            self.assertEqual(e.code, 404)

    def test_a_bridge_with_no_radio_never_greets_a_console_with_a_heartbeat(self):
        """A heartbeat means a gateway is alive. Greeting a console with one
        while nothing is attached is the same lie as beating over a dead radio,
        just at a more convincing moment. Verified against a second server
        whose source is 'none'."""
        from http.server import ThreadingHTTPServer
        state = tb.build_state(tb.build_source("none"), store=self.store,
                               console_dir=os.path.abspath("console"))
        self.assertFalse(state["heartbeats"])
        hub = tb.Hub()
        srv = ThreadingHTTPServer(("127.0.0.1", 0), tb.make_handler(hub, state))
        srv.daemon_threads = True
        port = srv.server_address[1]
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        try:
            seen = []
            with urllib.request.urlopen("http://127.0.0.1:%d/stream" % port, timeout=10) as r:
                for raw in r:
                    seen.append(raw.decode())
                    if "replay-done" in seen[-1]:
                        break
            joined = "".join(seen)
            self.assertIn("event: replay-done", joined, "history is still replayed")
            self.assertNotIn("event: heartbeat", joined,
                             "no radio, so the console must show no live feed")
        finally:
            srv.shutdown(); srv.server_close()

    def test_a_console_receives_a_frame_over_the_stream(self):
        received = []
        done = threading.Event()

        def reader():
            with urllib.request.urlopen(self.url("/stream"), timeout=10) as r:
                for raw in r:
                    line = raw.decode()
                    received.append(line)
                    if "SOS|1|A4F2|52.8|-118.0|12|1755000000|M|87" in line:
                        done.set()
                        return

        t = threading.Thread(target=reader, daemon=True)
        t.start()

        for _ in range(50):
            if self.hub.count:
                break
            threading.Event().wait(0.1)

        self.hub.broadcast("packet", "SOS|1|A4F2|52.8|-118.0|12|1755000000|M|87")
        self.assertTrue(done.wait(10), "the frame never arrived at the console")
        self.assertTrue(any("event: heartbeat" in ln for ln in received),
                        "a console must be told the feed is alive as soon as it attaches")


class SendOutcomeIsWhatGetsStored(unittest.TestCase):
    """A frame stored as outbound means a frame that went on the air.

    The console rebuilds an acknowledgement from exactly that, so a send the
    radio refused must never be stored as one. It used to be: the store was
    written before the send was attempted, so after a restart the console showed
    an incident as acknowledged that nobody had ever been told about, and did not
    alarm for it.
    """

    PORT = 8744

    @classmethod
    def setUpClass(cls):
        cls.source = tb.NoSource()  # refuses every send, like a bridge with no radio
        cls.hub = tb.Hub()
        cls.dir = tempfile.TemporaryDirectory()
        cls.store = Store(os.path.join(cls.dir.name, "t.db"))
        cls.state = tb.build_state(cls.source, store=cls.store, listen_port=cls.PORT)
        cls.server = tb.BridgeServer(("127.0.0.1", cls.PORT),
                                     tb.make_handler(cls.hub, cls.state))
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.store.close()
        cls.dir.cleanup()

    def send(self, frame):
        req = urllib.request.Request("http://127.0.0.1:%d/send" % self.PORT, data=frame,
                                     headers={"Content-Type": "text/plain"}, method="POST")
        with contextlib.suppress(urllib.error.HTTPError):
            urllib.request.urlopen(req, timeout=5)
            self.fail("this source refuses every send, so a success is the bug")

    def test_a_refused_send_is_not_stored_as_a_frame_that_went_out(self):
        self.send(b"ACK|1|A4F2|INC-001")
        out = [r["frame"] for r in self.store.history(0) if r["dir"] == "out"]
        self.assertEqual(out, [], "a refused acknowledgement is not an acknowledgement")

    def test_a_refused_send_is_still_kept_as_evidence(self):
        self.send(b"ACK|1|C0DE|INC-002")
        failed = [r for r in self.store.history(0)
                  if r["dir"] == "action" and r.get("kind") == "send-failed"]
        self.assertTrue(failed, "a review needs to see that a reply was attempted and failed")
        self.assertIn("ACK|1|C0DE|INC-002", [r["frame"] for r in failed])


class StreamNoticesAConsoleThatWentAway(unittest.TestCase):
    """A console that closed must stop being counted as attached.

    The stream thread used to block on an empty queue with no timeout, so it
    only noticed a dead socket when something was broadcast. With a source that
    sends no heartbeat, which is how the console is shown to somebody, nothing
    ever was: every reload parked a thread forever and /status kept counting
    consoles that had closed hours ago.
    """

    PORT = 8745

    @classmethod
    def setUpClass(cls):
        cls.source = tb.NoSource()  # sends no heartbeat, so nothing else wakes the stream
        cls.hub = tb.Hub()
        cls.state = tb.build_state(cls.source, listen_port=cls.PORT)
        cls.server = tb.BridgeServer(("127.0.0.1", cls.PORT),
                                     tb.make_handler(cls.hub, cls.state))
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def test_a_closed_console_is_dropped_without_any_traffic_to_notice_it_with(self):
        original = tb.STREAM_PING_SECONDS
        tb.STREAM_PING_SECONDS = 0.2
        try:
            conn = urllib.request.urlopen("http://127.0.0.1:%d/stream" % self.PORT, timeout=5)
            conn.read(1)  # attached and streaming
            self.assertEqual(self.hub.count, 1)
            conn.close()

            deadline = time.time() + 10
            while self.hub.count and time.time() < deadline:
                time.sleep(0.1)
            self.assertEqual(self.hub.count, 0, "a console that closed is not a console")
        finally:
            tb.STREAM_PING_SECONDS = original


if __name__ == "__main__":
    unittest.main()
