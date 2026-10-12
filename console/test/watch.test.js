/* Overdue parties, coverage survey, and repeater battery trend. */
const test = require('node:test');
const assert = require('node:assert');
const { OverdueWatch } = require('../lib/overdue.js');
const { CoverageLog } = require('../lib/coverage.js');
const { NodeHealth } = require('../lib/nodes.js');
const { BeaconRegistry } = require('../lib/registry.js');

const HOUR = 3600000;
const DAY = 86400000;
const DUE = Date.parse('2026-08-22T18:00:00Z');

function registry(records) { return new BeaconRegistry(records); }

/* ---------- overdue ---------- */

const PARTY = [{ devid: 'A4F2', name: 'S. Chen', party: 2, dueBack: '2026-08-22T18:00:00Z' }];

test('a party still within its return time is not overdue', () => {
  const w = new OverdueWatch();
  assert.deepEqual(w.check(registry(PARTY), null, DUE - HOUR), []);
});

test('a party a little late is not flagged, because parties run late constantly', () => {
  const w = new OverdueWatch();
  assert.deepEqual(w.check(registry(PARTY), null, DUE + 30 * 60000), [],
    'flagging at the minute they are due trains an operator to ignore the panel');
});

test('a party past the grace period is overdue', () => {
  const w = new OverdueWatch();
  const list = w.check(registry(PARTY), null, DUE + 2 * HOUR);
  assert.equal(list.length, 1);
  assert.equal(list[0].name, 'S. Chen');
  assert.equal(list[0].party, 2);
  assert.equal(list[0].lateByMs, 2 * HOUR, 'measured against the time they gave, not the grace');
});

test('a beacon with no return date is never overdue, rather than assumed', () => {
  const w = new OverdueWatch();
  assert.deepEqual(w.check(registry([{ devid: 'B117', name: 'No date' }]), null, DUE + DAY), []);
});

test('an unparseable return date is ignored rather than guessed at', () => {
  const w = new OverdueWatch();
  assert.deepEqual(w.check(registry([{ devid: 'B117', name: 'X', dueBack: 'sometime' }]), null, DUE + DAY), []);
});

test('the worst overdue party is listed first', () => {
  const w = new OverdueWatch();
  const list = w.check(registry([
    { devid: 'A4F2', name: 'Late', dueBack: '2026-08-22T18:00:00Z' },
    { devid: 'B117', name: 'Later', dueBack: '2026-08-21T18:00:00Z' }
  ]), null, DUE + 3 * HOUR);
  assert.deepEqual(list.map((p) => p.name), ['Later', 'Late']);
});

test('whether the party has been heard from is carried, because it decides what happens next', () => {
  const w = new OverdueWatch();
  const health = new NodeHealth();
  health.record('A4F2', { now: DUE + HOUR });
  const list = w.check(registry(PARTY), health, DUE + 2 * HOUR);
  assert.equal(list[0].lastHeard, DUE + HOUR);
  assert.match(w.describe(list[0], DUE + 2 * HOUR), /Last heard/);
});

test('a party never heard from at all says exactly that', () => {
  const w = new OverdueWatch();
  const list = w.check(registry(PARTY), new NodeHealth(), DUE + 2 * HOUR);
  assert.equal(list[0].lastHeard, null);
  assert.match(w.describe(list[0], DUE + 2 * HOUR), /has not been heard from at all/);
});

test('each party is announced once, not once a second', () => {
  const w = new OverdueWatch();
  const list = w.check(registry(PARTY), null, DUE + 2 * HOUR);
  assert.equal(w.newlyOverdue(list).length, 1);
  assert.equal(w.newlyOverdue(list).length, 0, 'the event log must not fill with one party');
});

test('a party that comes back can be announced again if it goes overdue later', () => {
  const w = new OverdueWatch();
  const list = w.check(registry(PARTY), null, DUE + 2 * HOUR);
  w.newlyOverdue(list);
  w.clearAnnounced([]);
  assert.equal(w.newlyOverdue(list).length, 1);
});

/* ---------- coverage survey, D7 ---------- */

test('a frame with a position and a signal reading is a surveyed point', () => {
  const c = new CoverageLog();
  const p = c.record({ lat: 52.8734, lon: -118.0814, snr: 6, hops: 1, at: 1 });
  assert.ok(p);
  assert.equal(c.size, 1);
  assert.equal(p.bestSnr, 6);
  assert.equal(p.maxHops, 1);
});

test('a frame with no signal reading says nothing about coverage', () => {
  const c = new CoverageLog();
  assert.equal(c.record({ lat: 52.87, lon: -118.08, at: 1 }), null);
  assert.equal(c.size, 0);
});

test('a frame with no position says nothing about where', () => {
  const c = new CoverageLog();
  assert.equal(c.record({ snr: 6, at: 1 }), null);
  assert.equal(c.size, 0);
});

test('a stationary beacon does not stack hundreds of points on one spot', () => {
  const c = new CoverageLog();
  for (let i = 0; i < 50; i++) c.record({ lat: 52.87341, lon: -118.08142, snr: 5, at: i });
  assert.equal(c.size, 1);
  assert.equal(c.all()[0].heard, 50);
});

test('the worst reading at a place is kept, not only the best', () => {
  const c = new CoverageLog();
  c.record({ lat: 52.87, lon: -118.08, snr: 8, at: 1 });
  c.record({ lat: 52.87, lon: -118.08, snr: -9, at: 2 });
  const p = c.all()[0];
  assert.equal(p.bestSnr, 8);
  assert.equal(p.worstSnr, -9, 'a survey that only records good days is false confidence');
});

test('signal bands read in plain terms', () => {
  assert.equal(CoverageLog.band(3), 'good');
  assert.equal(CoverageLog.band(-5), 'workable');
  assert.equal(CoverageLog.band(-15), 'marginal');
  assert.equal(CoverageLog.band(null), 'unknown');
});

test('the summary bands a place by its worst reading', () => {
  const c = new CoverageLog();
  c.record({ lat: 52.87, lon: -118.08, snr: 9, at: 1 });
  c.record({ lat: 52.87, lon: -118.08, snr: -14, at: 2 });
  c.record({ lat: 53.10, lon: -118.40, snr: 4, at: 3 });
  const s = c.summary();
  assert.equal(s.points, 2);
  assert.equal(s.bands.marginal, 1);
  assert.equal(s.bands.good, 1);
});

test('the exported survey carries measurements and no identities', () => {
  const c = new CoverageLog();
  c.record({ lat: 52.8734, lon: -118.0814, snr: 6, rssi: -95, hops: 2, at: 1, devid: 'A4F2' });
  const csv = c.toCsv();
  assert.match(csv, /^lat,lon,worst_snr,best_snr,worst_rssi,max_hops,times_heard/);
  assert.match(csv, /52\.87340,-118\.08140,6,6,-95,2,1/);
  assert.doesNotMatch(csv, /A4F2/, 'a map published to visitors is not a record of who was where');
});

test('a surveyed place keeps its worst signal strength, not its most recent', () => {
  const c = new CoverageLog();
  c.record({ lat: 52.87, lon: -118.08, snr: 2, rssi: -118, at: 1 });
  c.record({ lat: 52.87, lon: -118.08, snr: 2, rssi: -80, at: 2 });
  assert.equal(c.all()[0].rssi, -118,
    'one lucky packet arriving last must not make a marginal spot read comfortable');
});

test('a party whose return time cannot be read is reported, not silently skipped', () => {
  const w = new OverdueWatch();
  const bad = w.unreadable(registry([
    { devid: 'A4F2', name: 'Fine', dueBack: '2026-08-22T18:00:00Z' },
    { devid: 'B117', name: 'Typo', dueBack: '22/08/2026 6pm' },
    { devid: 'C0DE', name: 'No date given' }
  ]));
  assert.equal(bad.length, 1, 'a record with no return time at all is not an error');
  assert.equal(bad[0].devid, 'B117');
  assert.match(bad[0].dueBack, /22\/08\/2026/,
    'the operator has to see the text they typed to be able to fix it');
});

/* ---------- repeater battery trend ---------- */

function trendNode(readings) {
  const h = new NodeHealth();
  readings.forEach((r) => h.record('FA01', { kind: 'fixed', batt: r.pct, now: r.at }));
  return { health: h, node: h.nodes.get('FA01') };
}

test('a repeater losing charge over days is reported as falling, with a run out date', () => {
  const { health, node } = trendNode(
    Array.from({ length: 10 }, (_, i) => ({ pct: 95 - i * 2, at: i * DAY / 2 })));
  const t = health.batteryTrend(node);
  assert.equal(t.known, true);
  assert.equal(t.direction, 'falling');
  assert.ok(t.daysToEmpty > 0);
});

test('a few hours of decline is not extrapolated to a run out date', () => {
  /* A solar node drops steeply overnight and climbs back at dawn. Scaling an
     afternoon of decline to a daily rate produces a confident number that is
     really just the night ahead. */
  const { health, node } = trendNode(
    Array.from({ length: 6 }, (_, i) => ({ pct: 91 - i * 5, at: i * HOUR })));
  const t = health.batteryTrend(node);
  assert.equal(t.known, true);
  assert.equal(t.direction, 'falling');
  assert.equal(t.daysToEmpty, null, 'less than a charge cycle of readings');
});

test('a charging repeater is not given a run out date', () => {
  const { health, node } = trendNode(
    Array.from({ length: 6 }, (_, i) => ({ pct: 40 + i * 5, at: i * DAY })));
  const t = health.batteryTrend(node);
  assert.equal(t.direction, 'rising');
  assert.equal(t.daysToEmpty, null, 'extrapolating a charging node would invent a deadline');
});

test('a steady repeater is steady', () => {
  const { health, node } = trendNode(
    Array.from({ length: 6 }, (_, i) => ({ pct: 88, at: i * DAY })));
  assert.equal(health.batteryTrend(node).direction, 'steady');
});

test('too few readings gives no trend rather than a confident line through two points', () => {
  const { health, node } = trendNode([{ pct: 90, at: 0 }, { pct: 40, at: DAY }]);
  const t = health.batteryTrend(node);
  assert.equal(t.known, false);
  assert.match(t.reason, /not enough readings/);
});

test('readings crammed into a few minutes give no trend', () => {
  const { health, node } = trendNode(
    Array.from({ length: 8 }, (_, i) => ({ pct: 90 - i, at: i * 60000 })));
  const t = health.batteryTrend(node);
  assert.equal(t.known, false);
  assert.match(t.reason, /not enough time/);
});

test('battery history does not grow without bound on a station running all season', () => {
  const h = new NodeHealth({ battSamples: 10 });
  for (let i = 0; i < 500; i++) h.record('FA01', { kind: 'fixed', batt: 80, now: i * 60000 });
  assert.equal(h.nodes.get('FA01').battHistory.length, 10);
});
