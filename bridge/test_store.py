"""Store tests. Decision D6. Standard library only."""

import os
import sqlite3
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from store import Store  # noqa: E402

SOS = "SOS|1|A4F2|52.8734|-118.0814|12|1755000000|M|87"
T0 = 1755000000000


class Recording(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.store = Store(os.path.join(self.dir.name, "t.db"))

    def tearDown(self):
        self.store.close()
        self.dir.cleanup()

    def test_a_frame_is_stored_with_its_direction(self):
        self.store.record_frame(SOS, "in", T0)
        rows = self.store.history(0)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["frame"], SOS)
        self.assertEqual(rows[0]["dir"], "in")
        self.assertEqual(rows[0]["at"], T0)

    def test_an_unknown_direction_is_refused(self):
        with self.assertRaises(ValueError):
            self.store.record_frame(SOS, "sideways", T0)

    def test_both_directions_are_counted_separately(self):
        self.store.record_frame(SOS, "in", T0)
        self.store.record_frame("ACK|1|A4F2|INC-001", "out", T0 + 1000)
        self.assertEqual(self.store.counts(), {"frames_in": 1, "frames_out": 1, "actions": 0})

    def test_an_operator_action_is_stored_against_the_frame_it_refers_to(self):
        self.store.record_action("seen", SOS, T0 + 500)
        rows = self.store.history(0)
        self.assertEqual(rows[0]["dir"], "action")
        self.assertEqual(rows[0]["kind"], "seen")
        self.assertEqual(rows[0]["frame"], SOS)

    def test_a_frame_carrying_odd_characters_round_trips(self):
        weird = "SOS|1|<img src=x>|52.8|-118.0|12|1755000000|M|87"
        self.store.record_frame(weird, "in", T0)
        self.assertEqual(self.store.history(0)[0]["frame"], weird)


class Replay(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.store = Store(os.path.join(self.dir.name, "t.db"))

    def tearDown(self):
        self.store.close()
        self.dir.cleanup()

    def test_history_is_oldest_first_so_the_console_rebuilds_in_order(self):
        self.store.record_frame("TEL|1|FA01|64|4.0|-3.5", "in", T0)
        self.store.record_frame(SOS, "in", T0 + 1000)
        self.store.record_frame("ACK|1|A4F2|INC-001", "out", T0 + 2000)
        ats = [r["at"] for r in self.store.history(0)]
        self.assertEqual(ats, sorted(ats))
        self.assertEqual(self.store.history(0)[0]["frame"], "TEL|1|FA01|64|4.0|-3.5")

    def test_an_action_lands_after_the_frame_it_refers_to(self):
        """Applied the other way round, the console would try to mark an
        incident seen before that incident exists."""
        self.store.record_frame(SOS, "in", T0)
        self.store.record_action("seen", SOS, T0 + 1)
        rows = self.store.history(0)
        self.assertEqual([r["dir"] for r in rows], ["in", "action"])

    def test_records_older_than_the_window_are_not_replayed(self):
        self.store.record_frame(SOS, "in", T0)
        self.store.record_frame(SOS, "in", T0 + 60000)
        self.assertEqual(len(self.store.history(T0 + 30000)), 1)

    def test_replay_is_capped_so_a_long_running_station_cannot_flood_a_console(self):
        for i in range(50):
            self.store.record_frame(SOS, "in", T0 + i)
        self.assertEqual(len(self.store.history(0, limit=10)), 10)

    def test_the_cap_drops_the_oldest_frames_and_keeps_the_newest(self):
        """The newest frames are the ones still open. Losing those to keep an
        hour of old telemetry would be exactly backwards."""
        for i in range(50):
            self.store.record_frame("SOS|1|A4F2|%d" % i, "in", T0 + i)
        kept = self.store.history(0, limit=5)
        self.assertEqual([r["frame"] for r in kept],
                         ["SOS|1|A4F2|%d" % i for i in range(45, 50)])

    def test_an_operator_action_is_never_crowded_out_by_frames(self):
        """An action dropped to make room for telemetry would have the console
        alarm again for an incident a human already dealt with."""
        self.store.record_action("seen", SOS, T0)
        for i in range(50):
            self.store.record_frame("TEL|1|FA01|%d" % i, "in", T0 + 1 + i)
        kept = self.store.history(0, limit=5)
        kinds = [r for r in kept if r["dir"] == "action"]
        self.assertEqual(len(kinds), 1, "the operator's decision survived the cap")
        self.assertEqual(kinds[0]["kind"], "seen")


class LinkQualityStorage(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.store = Store(os.path.join(self.dir.name, "t.db"))

    def tearDown(self):
        self.store.close()
        self.dir.cleanup()

    def test_link_quality_is_stored_and_replayed(self):
        self.store.record_frame(SOS, "in", T0, link={"snr": 6.5, "rssi": -19, "hops": 2})
        r = self.store.history(0)[0]
        self.assertEqual((r["snr"], r["rssi"], r["hops"]), (6.5, -19, 2))

    def test_a_frame_with_no_reading_carries_no_fields_at_all(self):
        """The console must be able to tell "no signal reading" from "a reading
        of zero"."""
        self.store.record_frame(SOS, "in", T0)
        r = self.store.history(0)[0]
        for field in ("snr", "rssi", "hops"):
            self.assertNotIn(field, r)

    def test_zero_hops_is_carried_because_heard_directly_is_a_real_answer(self):
        self.store.record_frame(SOS, "in", T0, link={"hops": 0})
        self.assertEqual(self.store.history(0)[0]["hops"], 0)

    def test_the_export_quotes_a_frame_that_could_shift_the_columns(self):
        self.store.record_frame('SOS|1|A4F2|a,b|"q"', "in", T0)
        line = [l for l in self.store.export_csv().splitlines() if "A4F2" in l][0]
        self.assertIn('"SOS|1|A4F2|a,b|""q"""', line)

    def test_the_export_includes_operator_decisions_in_time_order(self):
        self.store.record_frame(SOS, "in", T0)
        self.store.record_action("seen", SOS, T0 + 1000)
        rows = self.store.export_csv().splitlines()
        self.assertIn("operator:seen", rows[2])


class OlderDatabase(unittest.TestCase):
    def test_a_database_from_before_link_quality_is_upgraded_not_wiped(self):
        """A station running all season must not have to be wiped to take an
        update, and its stored incidents are evidence."""
        d = tempfile.TemporaryDirectory()
        path = os.path.join(d.name, "old.db")

        old = sqlite3.connect(path)
        old.executescript("""
            CREATE TABLE frames (id INTEGER PRIMARY KEY AUTOINCREMENT,
                at INTEGER NOT NULL, dir TEXT NOT NULL, frame TEXT NOT NULL);
            CREATE TABLE actions (id INTEGER PRIMARY KEY AUTOINCREMENT,
                at INTEGER NOT NULL, kind TEXT NOT NULL, subject TEXT NOT NULL, detail TEXT);
        """)
        old.execute("INSERT INTO frames (at, dir, frame) VALUES (?,?,?)", (T0, "in", SOS))
        old.commit(); old.close()

        store = Store(path)
        rows = store.history(0)
        self.assertEqual(len(rows), 1, "the old record survived the upgrade")
        self.assertEqual(rows[0]["frame"], SOS)
        store.record_frame(SOS, "in", T0 + 1, link={"snr": 3.0})
        self.assertEqual(store.history(0)[1]["snr"], 3.0)
        store.close()
        d.cleanup()


class SimulatedReadingsStayLabelled(unittest.TestCase):
    """A signal reading that was invented must still be known to be invented
    after a restart.

    This is here because it was once not true. The console excluded simulated
    readings from the coverage survey while they were live, but the label was
    not stored, so a restart replayed them with nothing saying what they were
    and they went into the survey as measurements. A coverage map built from
    invented numbers is the exact false confidence D7 exists to prevent, and
    nothing on screen looked any different.
    """

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.store = Store(os.path.join(self.dir.name, "t.db"))

    def tearDown(self):
        self.store.close()
        self.dir.cleanup()

    def test_the_label_comes_back_out_of_the_store(self):
        self.store.record_frame(SOS, "in", T0,
                                link={"snr": -2.5, "rssi": -104, "hops": 2,
                                      "source_note": "simulated"})
        row = self.store.history(0)[0]
        self.assertEqual(row["source_note"], "simulated")
        self.assertEqual(row["snr"], -2.5, "the readings come back too")

    def test_a_reading_off_a_real_radio_carries_no_label(self):
        """Absent rather than 'real', so the only thing that can be claimed is
        the thing that was actually recorded."""
        self.store.record_frame(SOS, "in", T0, link={"snr": 6.25, "hops": 1})
        self.assertNotIn("source_note", self.store.history(0)[0])

    def test_the_exported_record_says_so_too(self):
        """An incident review has to be able to tell which readings were real."""
        self.store.record_frame(SOS, "in", T0, link={"snr": -2.5, "source_note": "simulated"})
        csv = self.store.export_csv()
        self.assertIn("source_note", csv.splitlines()[0])
        self.assertIn("simulated", csv)

    def test_an_older_database_gains_the_column_rather_than_being_wiped(self):
        d = tempfile.TemporaryDirectory()
        path = os.path.join(d.name, "old.db")
        old = sqlite3.connect(path)
        old.executescript("""
            CREATE TABLE frames (id INTEGER PRIMARY KEY AUTOINCREMENT,
                at INTEGER NOT NULL, dir TEXT NOT NULL, frame TEXT NOT NULL,
                snr REAL, rssi INTEGER, hops INTEGER);
            CREATE TABLE actions (id INTEGER PRIMARY KEY AUTOINCREMENT,
                at INTEGER NOT NULL, kind TEXT NOT NULL, subject TEXT NOT NULL, detail TEXT);
        """)
        old.execute("INSERT INTO frames (at, dir, frame, snr) VALUES (?,?,?,?)",
                    (T0, "in", SOS, 4.0))
        old.commit()
        old.close()

        store = Store(path)
        self.assertEqual(store.history(0)[0]["snr"], 4.0, "the old record survived")
        store.record_frame(SOS, "in", T0 + 1, link={"source_note": "simulated"})
        self.assertEqual(store.history(0)[1]["source_note"], "simulated")
        store.close()
        d.cleanup()


class SurvivesRestart(unittest.TestCase):
    def test_records_are_still_there_after_the_process_that_wrote_them_is_gone(self):
        d = tempfile.TemporaryDirectory()
        path = os.path.join(d.name, "t.db")

        first = Store(path)
        first.record_frame(SOS, "in", T0)
        first.record_action("seen", SOS, T0 + 1)
        first.close()          # stands in for the bridge being killed

        second = Store(path)
        rows = second.history(0)
        self.assertEqual(len(rows), 2, "a restarted bridge must still know what happened")
        self.assertEqual(rows[0]["frame"], SOS)
        self.assertEqual(second.counts()["frames_in"], 1)
        second.close()
        d.cleanup()

    def test_the_database_is_left_readable_when_a_write_is_never_committed(self):
        """Write-ahead logging is what keeps a kill -9 from leaving a truncated
        file. Simulated here by opening a second connection mid-transaction."""
        d = tempfile.TemporaryDirectory()
        path = os.path.join(d.name, "t.db")
        store = Store(path)
        store.record_frame(SOS, "in", T0)

        raw = sqlite3.connect(path)
        raw.execute("BEGIN")
        raw.execute("INSERT INTO frames (at, dir, frame) VALUES (?,?,?)", (T0 + 1, "in", "LOST"))
        raw.close()            # rolled back, never committed

        rows = store.history(0)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["frame"], SOS)
        store.close()
        d.cleanup()


class ConcurrentWriters(unittest.TestCase):
    def test_frames_arriving_while_a_console_reads_are_not_lost(self):
        d = tempfile.TemporaryDirectory()
        store = Store(os.path.join(d.name, "t.db"))

        def writer(n):
            for i in range(20):
                store.record_frame("%s|%d" % (SOS, n * 100 + i), "in", T0 + i)

        threads = [threading.Thread(target=writer, args=(n,)) for n in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        self.assertEqual(store.counts()["frames_in"], 80)
        store.close()
        d.cleanup()


if __name__ == "__main__":
    unittest.main()


class RecordsAreIdentifiable(unittest.TestCase):
    """Every stored record comes back with a stable id.

    This is what lets a console recognise a record it has already rebuilt from,
    and it matters far more than it sounds. The bridge replays its whole window
    to every console that attaches, and a console reattaches whenever the link
    blips. Without an id the console ran the same distress frames through its
    parser again on every reconnection, correlated them to the incidents already
    on screen, and counted them as repeats. A flaky cable read on the queue as a
    beacon calling over and over because nobody had answered.
    """

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.store = Store(os.path.join(self.dir.name, "t.db"))

    def tearDown(self):
        self.store.close()
        self.dir.cleanup()

    def test_every_record_has_one(self):
        self.store.record_frame(SOS, "in", T0)
        self.store.record_frame("ACK|1|A4F2|INC-001", "out", T0 + 1)
        self.store.record_action("seen", "SOS|1|ZZZZ|0|0|||U|", T0 + 2)
        for record in self.store.history(0):
            self.assertIn("id", record, record)
            self.assertTrue(record["id"])

    def test_ids_are_unique_across_frames_and_actions(self):
        """The two tables have separate id sequences, so an unprefixed id would
        have a frame and an action collide on small numbers."""
        self.store.record_frame(SOS, "in", T0)
        self.store.record_action("seen", SOS, T0 + 1)
        ids = [r["id"] for r in self.store.history(0)]
        self.assertEqual(len(ids), len(set(ids)), ids)

    def test_an_id_is_the_same_every_time_it_is_replayed(self):
        """A console reconnecting has to see the same identity it saw before, or
        deduplicating on it achieves nothing."""
        self.store.record_frame(SOS, "in", T0)
        first = [r["id"] for r in self.store.history(0)]
        second = [r["id"] for r in self.store.history(0)]
        self.assertEqual(first, second)

    def test_two_identical_frames_at_the_same_instant_are_still_distinct(self):
        """A beacon repeating fast, or a frame heard twice, must not collapse
        into one record: the whole point of storing raw frames is that the
        console decides what they mean."""
        self.store.record_frame(SOS, "in", T0)
        self.store.record_frame(SOS, "in", T0)
        ids = [r["id"] for r in self.store.history(0)]
        self.assertEqual(len(ids), 2)
        self.assertNotEqual(ids[0], ids[1])
