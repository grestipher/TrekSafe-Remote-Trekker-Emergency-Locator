const test = require('node:test');
const assert = require('node:assert');
const F = require('../lib/frames.js');
const { IncidentStore } = require('../lib/incidents.js');

const T0 = 1755000000000;

function feed(store, line, now, source) {
  const c = F.classify(line);
  return store.recordDistress(F.hydrateDistress(c.fields), {
    raw: c.raw, source: source || 'test', now: now
  });
}

const SOS = 'SOS|1|A4F2|52.8734|-118.0814|12|1755000000|M|87';
const SOS_NOFIX = 'SOS|1|A4F2|0|0|12|1755000000|M|87';
const SOS_UNIDENT = 'SOS|1|ZZZZ|52.8734|-118.0814|12|1755000000|M|87';

test('a distress frame creates an open incident', () => {
  const s = new IncidentStore();
  const { incident, isRepeat } = feed(s, SOS, T0);
  assert.equal(isRepeat, false);
  assert.equal(incident.ref, 'INC-001');
  assert.equal(incident.repeats, 0);
  assert.equal(s.openCount(), 1);
});

test('a repeat from the same beacon is the same incident, not a second one', () => {
  const s = new IncidentStore();
  feed(s, SOS, T0);
  const second = feed(s, SOS, T0 + 30000);
  assert.equal(second.isRepeat, true);
  assert.equal(s.incidents.length, 1, 'one person, one incident');
  assert.equal(second.incident.repeats, 1);
  assert.equal(second.incident.firstReceived, T0, 'the call started when it started');
  assert.equal(second.incident.lastReceived, T0 + 30000);
});

test('a call from the same beacon long afterwards is a new incident', () => {
  const s = new IncidentStore();
  feed(s, SOS, T0);
  const later = feed(s, SOS, T0 + 31 * 60 * 1000);
  assert.equal(later.isRepeat, false);
  assert.equal(s.incidents.length, 2);
});

test('two different beacons are two incidents', () => {
  const s = new IncidentStore();
  feed(s, SOS, T0);
  feed(s, 'SOS|1|B117|52.87|-118.08|12|1755000000|L|60', T0 + 1000);
  assert.equal(s.incidents.length, 2);
});

test('a repeat that finally carries a position updates the NO FIX incident', () => {
  const s = new IncidentStore();
  const first = feed(s, SOS_NOFIX, T0);
  assert.equal(first.incident.noFix, true);

  const second = feed(s, SOS, T0 + 30000);
  assert.equal(second.isRepeat, true);
  assert.equal(second.incident.noFix, false);
  assert.equal(second.incident.lat, 52.8734);
  assert.equal(second.incident.noFixReason, null);
});

test('a repeat with no position does not wipe a position already known', () => {
  const s = new IncidentStore();
  feed(s, SOS, T0);
  const second = feed(s, SOS_NOFIX, T0 + 30000);
  assert.equal(second.incident.noFix, false);
  assert.equal(second.incident.lat, 52.8734);
});

test('unidentified frames correlate on the raw frame, since there is no id', () => {
  const s = new IncidentStore();
  feed(s, SOS_UNIDENT, T0);
  const second = feed(s, SOS_UNIDENT, T0 + 30000);
  assert.equal(second.isRepeat, true);
  assert.equal(s.incidents.length, 1);
});

test('different unidentified frames stay separate incidents', () => {
  const s = new IncidentStore();
  feed(s, SOS_UNIDENT, T0);
  feed(s, 'SOS|1|YYYY|52.8734|-118.0814|12|1755000000|M|87', T0 + 1000);
  assert.equal(s.incidents.length, 2);
});

/* Acknowledgement is two phase. acknowledge() hands the frame over and nothing
   more; confirmAck() is what closes the incident, called only when something
   outside the console says the frame went out. Most tests below want the whole
   successful round trip, which is this. */
function ackConfirmed(s, ref, at) {
  const r = s.acknowledge(ref, at);
  if (r.ok) s.confirmAck(ref, at);
  return r;
}

test('acknowledging sends the contract frame and clears the alarm once confirmed', () => {
  const s = new IncidentStore();
  const { incident } = feed(s, SOS, T0);
  const r = ackConfirmed(s, incident.ref, T0 + 5000);
  assert.equal(r.ok, true);
  assert.equal(r.frame, 'ACK|1|A4F2|INC-001');
  assert.equal(s.openCount(), 0);
});

test('an acknowledgement nothing has confirmed does not clear the alarm', () => {
  const s = new IncidentStore();
  const { incident } = feed(s, SOS, T0);
  const r = s.acknowledge(incident.ref, T0 + 5000);
  assert.equal(r.ok, true);
  assert.equal(incident.ack, false, 'handing a frame over is not the beacon hearing it');
  assert.equal(incident.ackPending, true);
  assert.equal(s.openCount(), 1,
    'until something says the frame went out, nobody has told the beacon anything');
});

test('an acknowledgement that failed to send leaves the incident open and says why', () => {
  const s = new IncidentStore();
  const { incident } = feed(s, SOS, T0);
  s.acknowledge(incident.ref, T0 + 5000);
  const r = s.failAck(incident.ref, 'no radio attached');
  assert.equal(r.ok, true);
  assert.equal(incident.ack, false);
  assert.equal(incident.ackPending, false);
  assert.match(incident.ackFailure, /no radio attached/);
  assert.equal(s.openCount(), 1, 'a reply that never left must never read as a reply');
});

test('an acknowledgement that failed can be sent again', () => {
  const s = new IncidentStore();
  const { incident } = feed(s, SOS, T0);
  s.acknowledge(incident.ref, T0 + 5000);
  s.failAck(incident.ref, 'no radio attached');
  const retry = ackConfirmed(s, incident.ref, T0 + 9000);
  assert.equal(retry.ok, true);
  assert.equal(retry.frame, 'ACK|1|A4F2|INC-001');
  assert.equal(s.openCount(), 0);
});

test('an acknowledgement nothing ever answers for is timed out rather than left pending', () => {
  const s = new IncidentStore({ ackConfirmMs: 15000 });
  const { incident } = feed(s, SOS, T0);
  s.acknowledge(incident.ref, T0);

  assert.deepEqual(s.expirePendingAcks(T0 + 14000), [], 'not given up on too early');
  assert.equal(incident.ackPending, true);

  const expired = s.expirePendingAcks(T0 + 16000);
  assert.equal(expired.length, 1);
  assert.equal(incident.ackPending, false);
  assert.match(incident.ackFailure, /nothing confirmed/);
  assert.equal(s.openCount(), 1, 'a wedged bridge must not leave the alarm resolved either way');
});

test('a second acknowledgement is refused while the first is still in flight', () => {
  const s = new IncidentStore();
  const { incident } = feed(s, SOS, T0);
  s.acknowledge(incident.ref, T0 + 1000);
  const again = s.acknowledge(incident.ref, T0 + 1500);
  assert.equal(again.ok, false);
  assert.match(again.reason, /in flight/);
});

test('an incident with no device id cannot be acknowledged, and says why', () => {
  const s = new IncidentStore();
  const { incident } = feed(s, SOS_UNIDENT, T0);
  const r = s.acknowledge(incident.ref, T0 + 1000);
  assert.equal(r.ok, false);
  assert.match(r.reason, /no device id to reply to/);
  assert.equal(incident.ack, false);
  assert.equal(s.openCount(), 1, 'refusing to acknowledge must not quietly clear the alarm');
});

test('marking seen clears the alarm without sending anything', () => {
  const s = new IncidentStore();
  const { incident } = feed(s, SOS_UNIDENT, T0);
  const r = s.markSeen(incident.ref, T0 + 1000);
  assert.equal(r.ok, true);
  assert.equal(s.openCount(), 0);
  assert.equal(incident.ack, false, 'seen is not acknowledged and must never look like it');
});

test('an incident that can be acknowledged cannot be dismissed as merely seen', () => {
  const s = new IncidentStore();
  const { incident } = feed(s, SOS, T0);
  const r = s.markSeen(incident.ref, T0 + 1000);
  assert.equal(r.ok, false);
  assert.match(r.reason, /needs acknowledgement/);
  assert.equal(s.openCount(), 1);
});

test('acknowledging twice is refused rather than sending a second frame', () => {
  const s = new IncidentStore();
  const { incident } = feed(s, SOS, T0);
  ackConfirmed(s, incident.ref, T0 + 1000);
  const again = s.acknowledge(incident.ref, T0 + 2000);
  assert.equal(again.ok, false);
  assert.match(again.reason, /already acknowledged/);
});

/* The case that matters most: the beacon never heard us. */

test('a repeat just after acknowledgement is a frame that was already in the air', () => {
  const s = new IncidentStore();
  const { incident } = feed(s, SOS, T0);
  ackConfirmed(s, incident.ref, T0 + 5000);
  const repeat = feed(s, SOS, T0 + 5000 + 30 * 1000);
  assert.equal(repeat.incident.ackNotReaching, false);
  assert.equal(s.openCount(), 0, 'no false alarm for a frame that crossed with the reply');
});

test('a beacon still calling well after acknowledgement re-opens the incident', () => {
  const s = new IncidentStore();
  const { incident } = feed(s, SOS, T0);
  ackConfirmed(s, incident.ref, T0 + 5000);
  const repeat = feed(s, SOS, T0 + 5000 + 3 * 60 * 1000);
  assert.equal(repeat.incident.ackNotReaching, true);
  assert.equal(s.openCount(), 1, 'the person still believes nobody has heard them');
});

test('an incident whose beacon is still calling can be acknowledged again', () => {
  const s = new IncidentStore();
  const { incident } = feed(s, SOS, T0);
  ackConfirmed(s, incident.ref, T0 + 5000);
  feed(s, SOS, T0 + 5000 + 3 * 60 * 1000);
  const retry = ackConfirmed(s, incident.ref, T0 + 400000);
  assert.equal(retry.ok, true);
  assert.equal(retry.frame, 'ACK|1|A4F2|INC-001');
  assert.equal(s.openCount(), 0);
});

/* The same case for an incident nobody could reply to. Nothing was ever sent
   there, so a beacon still calling is a person who has been told nothing at
   all by anybody. */

test('a repeat just after marking seen is a frame that was already in the air', () => {
  const s = new IncidentStore();
  const { incident } = feed(s, SOS_UNIDENT, T0);
  s.markSeen(incident.ref, T0 + 5000);
  const repeat = feed(s, SOS_UNIDENT, T0 + 5000 + 30 * 1000);
  assert.equal(repeat.incident.seenNotReaching, false);
  assert.equal(s.openCount(), 0);
});

test('a beacon still calling well after being marked seen re-opens the incident', () => {
  const s = new IncidentStore();
  const { incident } = feed(s, SOS_UNIDENT, T0);
  s.markSeen(incident.ref, T0 + 5000);
  const repeat = feed(s, SOS_UNIDENT, T0 + 5000 + 3 * 60 * 1000);
  assert.equal(repeat.incident.seenNotReaching, true);
  assert.equal(s.openCount(), 1,
    'nothing was ever sent, so the person still believes nobody has heard them');
});

test('an incident re-opened by a beacon that kept calling can be marked seen again', () => {
  const s = new IncidentStore();
  const { incident } = feed(s, SOS_UNIDENT, T0);
  s.markSeen(incident.ref, T0 + 5000);
  feed(s, SOS_UNIDENT, T0 + 5000 + 3 * 60 * 1000);
  const again = s.markSeen(incident.ref, T0 + 400000);
  assert.equal(again.ok, true, 'a red console with no button on it is one nobody looks at');
  assert.equal(s.openCount(), 0);
});

/* Rebuilding from stored history. Decision D6. The bridge stores frames and
   operator actions, never incidents, and the console works them out again. */

test('replaying stored frames rebuilds the same incidents', () => {
  const live = new IncidentStore();
  feed(live, SOS, T0);
  feed(live, SOS, T0 + 30000);

  const rebuilt = new IncidentStore();
  feed(rebuilt, SOS, T0);
  feed(rebuilt, SOS, T0 + 30000);

  assert.equal(rebuilt.incidents.length, live.incidents.length);
  assert.equal(rebuilt.incidents[0].repeats, live.incidents[0].repeats);
  assert.equal(rebuilt.incidents[0].ref, live.incidents[0].ref);
  assert.equal(rebuilt.incidents[0].firstReceived, T0, 'the call still started when it started');
});

test('a stored acknowledgement is put back, so a restart does not alarm again', () => {
  const s = new IncidentStore();
  feed(s, SOS, T0);
  assert.equal(s.openCount(), 1);

  const inc = s.applyHistoricalAck('A4F2', T0 + 5000);
  assert.ok(inc, 'the acknowledgement found its incident');
  assert.equal(inc.ack, true);
  assert.equal(inc.ackAt, T0 + 5000);
  assert.equal(s.openCount(), 0);
});

test('a stored acknowledgement for a beacon with no incident is ignored quietly', () => {
  const s = new IncidentStore();
  feed(s, SOS, T0);
  assert.equal(s.applyHistoricalAck('FFFF', T0 + 5000), null);
  assert.equal(s.openCount(), 1, 'the real incident is untouched');
});

test('a stored seen action is put back for an unidentified incident', () => {
  const s = new IncidentStore();
  const { incident } = feed(s, SOS_UNIDENT, T0);
  const found = s.applyHistoricalSeen(incident.raw, T0 + 1000);
  assert.ok(found);
  assert.equal(found.seen, true);
  assert.equal(s.openCount(), 0);
});

test('rebuilding a beacon that kept calling after the acknowledgement still re-opens it', () => {
  const s = new IncidentStore();
  feed(s, SOS, T0);
  s.applyHistoricalAck('A4F2', T0 + 5000);
  feed(s, SOS, T0 + 5000 + 3 * 60 * 1000);
  assert.equal(s.find('INC-001').ackNotReaching, true);
  assert.equal(s.openCount(), 1, 'the person was still waiting when the console died');
});

test('an incident nobody replied to before the restart is still open after it', () => {
  const s = new IncidentStore();
  feed(s, SOS, T0);
  feed(s, 'SOS|1|B117|52.87|-118.08|12|1755000000|L|60', T0 + 1000);
  s.applyHistoricalAck('B117', T0 + 2000);
  assert.equal(s.openCount(), 1);
  assert.equal(s.incidents.find((i) => s.isOpen(i)).devid, 'A4F2');
});

test('acknowledging an incident that does not exist is refused with a reason', () => {
  const s = new IncidentStore();
  const r = s.acknowledge('INC-999', T0);
  assert.equal(r.ok, false);
  assert.match(r.reason, /no incident INC-999/);
});

/* Replaying stored history, and doing it more than once.
 *
 * The bridge replays its whole window to every console that attaches, and a
 * console reattaches whenever the link blips. These pin down that rebuilding
 * twice from the same records must not change what is on screen. */

test('rebuilding from the same record twice does not invent a repeat', () => {
  /* Measured before it was fixed: four reconnections turned one distress call
     into three repeats, and on a warden's screen a repeat means the person is
     still calling because nobody has answered. A flaky cable read as somebody
     shouting for help. */
  const s = new IncidentStore();
  const seen = new Set();

  // What the console does with each replayed record: skip anything already
  // rebuilt from, identified by the store's own row id.
  const replay = (id, at) => {
    if (seen.has(id)) return;
    seen.add(id);
    feed(s, SOS, at);
  };

  replay('f1', T0);
  assert.equal(s.incidents.length, 1);
  assert.equal(s.incidents[0].repeats, 0);

  // The same window, replayed again on reconnection, and again.
  replay('f1', T0);
  replay('f1', T0);
  replay('f1', T0);
  assert.equal(s.incidents.length, 1, 'still one incident');
  assert.equal(s.incidents[0].repeats, 0,
    'and still no repeats, because nothing new actually arrived');
});

test('a genuine repeat is still a repeat', () => {
  /* The other half. Deduplicating on the record id must not swallow the
     beacon actually calling again, which is a different stored record. */
  const s = new IncidentStore();
  const seen = new Set();
  const replay = (id, at) => {
    if (seen.has(id)) return;
    seen.add(id);
    feed(s, SOS, at);
  };

  replay('f1', T0);
  replay('f2', T0 + 30000);
  assert.equal(s.incidents.length, 1, 'one person, not two');
  assert.equal(s.incidents[0].repeats, 1, 'and the beacon did call again');
});
