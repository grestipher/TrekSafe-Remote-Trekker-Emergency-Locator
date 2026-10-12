"""Durable store for the bridge. Decision D6.

Holds two things: every frame that crossed the wire, in both directions, and
every operator action that is not derivable from a frame.

It deliberately does not hold incidents. The bridge has no opinion about what a
frame means, and giving it one would mean a second parser alongside the
console's. Two parsers drifting apart is how the same distress call ends up read
two ways by two halves of one system. The console replays what is stored here
and rebuilds its own state, so there is exactly one interpreter.

SQLite because it is in the Python standard library, is a single file that can be
copied to back it up, and is crash safe under write-ahead logging. A base station
is one machine at a staffed facility with no guaranteed internet, which is the
case SQLite is best at and a database server is worst at.
"""

import sqlite3
import threading
import time
from datetime import datetime, timezone


def csv_field(value):
    """A frame is pipe delimited, but it came off the air and could contain
    anything. Quoted so a corrupted one cannot shift every later column."""
    text = str(value)
    if any(c in text for c in ',"\n\r'):
        return '"' + text.replace('"', '""') + '"'
    return text

SCHEMA = """
CREATE TABLE IF NOT EXISTS frames (
    id     INTEGER PRIMARY KEY AUTOINCREMENT,
    at     INTEGER NOT NULL,          -- unix milliseconds
    dir    TEXT    NOT NULL,          -- 'in' from the mesh, 'out' to the mesh
    frame  TEXT    NOT NULL,
    -- Link quality, read from the packet layer, which only this process sees.
    -- Null for anything the radio did not report, and for everything sent. The
    -- coverage survey D7 promises is built from these. See docs/decisions.md.
    snr    REAL,
    rssi   INTEGER,
    hops   INTEGER,
    -- Where the readings above came from, when it is not a radio. 'simulated'
    -- for anything produced by the simulated mesh in sim/.
    --
    -- Stored because the label has to survive a restart. It did not, once: the
    -- console excluded simulated readings from the coverage survey while they
    -- were live, then replayed them out of here with nothing saying what they
    -- were and surveyed them as measurements. A coverage map built from invented
    -- numbers is the exact false confidence D7 exists to prevent, and nothing on
    -- screen looked any different.
    source_note TEXT
);
CREATE INDEX IF NOT EXISTS frames_at ON frames(at);

CREATE TABLE IF NOT EXISTS actions (
    id     INTEGER PRIMARY KEY AUTOINCREMENT,
    at     INTEGER NOT NULL,
    kind   TEXT    NOT NULL,          -- 'seen'
    subject TEXT   NOT NULL,          -- the raw frame the action refers to
    detail TEXT
);
CREATE INDEX IF NOT EXISTS actions_at ON actions(at);
"""


def now_ms():
    return int(time.time() * 1000)


class Store:
    """Append only in practice. Nothing here updates or deletes a record."""

    def __init__(self, path):
        self.path = path
        # The bridge writes from its source thread and reads from request
        # threads, so the connection is shared under a lock rather than opened
        # per thread. Writes are small and rare enough that this never contends.
        self._lock = threading.Lock()
        self._db = sqlite3.connect(path, check_same_thread=False)
        self._db.row_factory = sqlite3.Row
        with self._lock:
            # Write-ahead logging is what makes a kill -9 or a power cut leave a
            # readable database rather than a truncated one.
            self._db.execute("PRAGMA journal_mode=WAL")
            self._db.execute("PRAGMA synchronous=FULL")
            self._db.executescript(SCHEMA)
            self._migrate()
            self._db.commit()

    def _migrate(self):
        """Adds columns a database written by an older build does not have.

        A station that has been running all season must not have to be wiped to
        take an update, and its stored incidents are evidence.
        """
        have = {row["name"] for row in self._db.execute("PRAGMA table_info(frames)")}
        for column, decl in (("snr", "REAL"), ("rssi", "INTEGER"), ("hops", "INTEGER"),
                             ("source_note", "TEXT")):
            if column not in have:
                self._db.execute("ALTER TABLE frames ADD COLUMN %s %s" % (column, decl))

    def record_frame(self, frame, direction, at=None, link=None):
        """link carries what the radio reported about the packet: snr, rssi and
        hops. Absent for anything typed, replayed or sent."""
        if direction not in ("in", "out"):
            raise ValueError("direction must be 'in' or 'out', got %r" % (direction,))
        at = now_ms() if at is None else int(at)
        m = link or {}
        with self._lock:
            cur = self._db.execute(
                "INSERT INTO frames (at, dir, frame, snr, rssi, hops, source_note) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)",
                (at, direction, frame, m.get("snr"), m.get("rssi"), m.get("hops"),
                 m.get("source_note")))
            self._db.commit()
            return cur.lastrowid

    def record_action(self, kind, subject, at=None, detail=None):
        """An operator decision that leaves no trace on the radio.

        Acknowledgement does not need this: it puts an ACK frame on the air and
        that frame is stored. Marking an unidentified incident as seen sends
        nothing, so without this it would be forgotten on restart and the
        console would alarm again for something a human already dealt with.
        """
        at = now_ms() if at is None else int(at)
        with self._lock:
            cur = self._db.execute(
                "INSERT INTO actions (at, kind, subject, detail) VALUES (?, ?, ?, ?)",
                (at, kind, subject, detail))
            self._db.commit()
            return cur.lastrowid

    def history(self, since_ms, limit=5000):
        """Everything since a point in time, oldest first, frames and actions
        interleaved in the order they happened.

        The order matters. An action has to be applied after the frame it refers
        to, or the console would mark an incident seen before it exists.
        """
        with self._lock:
            # The cap applies to frames only, and takes the newest. Actions are
            # a handful of operator decisions; dropping one to make room for
            # another telemetry frame would have the console alarm again for an
            # incident a human already dealt with. They are never why a replay
            # is large.
            frames = self._db.execute(
                "SELECT id, at, dir, frame, snr, rssi, hops, source_note FROM frames "
                "WHERE at >= ? ORDER BY at DESC, id DESC LIMIT ?",
                (int(since_ms), limit)).fetchall()
            actions = self._db.execute(
                "SELECT id, at, kind, subject FROM actions WHERE at >= ? ORDER BY at, id",
                (int(since_ms),)).fetchall()

        # Back to oldest first. A truncated replay loses the oldest history
        # rather than the newest, because the newest is what is still open.
        records = []
        for r in reversed(frames):
            # The row id travels with the record so a console can tell one
            # record from another exactly.
            #
            # It is what makes replaying idempotent, and that matters more than
            # it sounds. A console reconnects whenever the link blips, and the
            # bridge replays the whole window every time it does. Without a way
            # to recognise a record it has already applied, the console counted
            # each replayed distress frame as the beacon calling again, so a
            # flaky cable read on screen as a person still shouting for help.
            #
            # Prefixed by kind because frames and actions have separate id
            # sequences and would otherwise collide on small numbers.
            rec = {"id": "f%d" % r["id"], "at": r["at"], "dir": r["dir"], "frame": r["frame"]}
            # Only carried when the radio actually reported it, so the console
            # can tell "no signal reading" from "a reading of zero".
            for field in ("snr", "rssi", "hops", "source_note"):
                if r[field] is not None:
                    rec[field] = r[field]
            records.append(rec)
        records += [{"id": "a%d" % r["id"], "at": r["at"], "dir": "action",
                     "kind": r["kind"], "frame": r["subject"]}
                    for r in actions]
        # Stable, so an action still lands after a frame recorded in the same
        # millisecond and the console cannot mark an incident seen before it
        # exists.
        records.sort(key=lambda r: r["at"])
        return records

    def counts(self):
        with self._lock:
            f_in = self._db.execute("SELECT COUNT(*) FROM frames WHERE dir='in'").fetchone()[0]
            f_out = self._db.execute("SELECT COUNT(*) FROM frames WHERE dir='out'").fetchone()[0]
            acts = self._db.execute("SELECT COUNT(*) FROM actions").fetchone()[0]
        return {"frames_in": f_in, "frames_out": f_out, "actions": acts}

    def export_csv(self):
        """The whole record, as evidence, for an incident review.

        Raw frames and operator actions with their times, and nothing
        interpreted. What a review needs is what arrived, not this system's
        reading of what arrived.
        """
        rows = ["at_utc,at_ms,direction,snr,rssi,hops,source_note,frame"]
        with self._lock:
            frames = self._db.execute(
                "SELECT at, dir, frame, snr, rssi, hops, source_note "
                "FROM frames ORDER BY at, id").fetchall()
            actions = self._db.execute(
                "SELECT at, kind, subject FROM actions ORDER BY at, id").fetchall()

        merged = [(r["at"], r["dir"], r["frame"], r["snr"], r["rssi"], r["hops"],
                   r["source_note"]) for r in frames]
        merged += [(r["at"], "operator:" + r["kind"], r["subject"], None, None, None, None)
                   for r in actions]
        merged.sort(key=lambda t: t[0])

        for at, direction, frame, snr, rssi, hops, note in merged:
            stamp = datetime.fromtimestamp(at / 1000, timezone.utc).isoformat(timespec="seconds")
            rows.append(",".join([
                stamp, str(at), direction,
                "" if snr is None else str(snr),
                "" if rssi is None else str(rssi),
                "" if hops is None else str(hops),
                "" if note is None else csv_field(note),
                csv_field(frame),
            ]))
        return "\n".join(rows) + "\n"

    def close(self):
        with self._lock:
            self._db.close()
