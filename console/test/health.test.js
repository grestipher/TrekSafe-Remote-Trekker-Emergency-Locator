const test = require('node:test');
const assert = require('node:assert');
const { NodeHealth } = require('../lib/nodes.js');
const { FeedWatchdog } = require('../lib/watchdog.js');
const { BeaconRegistry } = require('../lib/registry.js');

const T0 = 1755000000000;

/* Node health, issue I7. */

test('telemetry marks a device as fixed infrastructure', () => {
  const h = new NodeHealth();
  h.record('B117', { kind: 'fixed', batt: 64, now: T0 });
  assert.equal(h.fixed().length, 1);
  assert.equal(h.beacons().length, 0);
});

test('a distress call does not make a visitor beacon into a repeater', () => {
  const h = new NodeHealth();
  h.record('A4F2', { batt: 87, now: T0 });
  assert.equal(h.fixed().length, 0);
  assert.equal(h.beacons().length, 1);
});

test('coverage counts repeaters only, so a switched off beacon is not a gap', () => {
  const h = new NodeHealth();
  h.record('B117', { kind: 'fixed', now: T0 });
  h.record('A4F2', { now: T0 });
  const c = h.coverage(T0 + 1000);
  assert.deepEqual(c, { up: 1, total: 1, known: true });
});

test('coverage says plainly when no repeater has ever been heard', () => {
  const h = new NodeHealth();
  h.record('A4F2', { now: T0 });
  const c = h.coverage(T0);
  assert.equal(c.known, false, '0 of 0 must not be able to read as everything is fine');
});

test('a repeater that goes quiet stays a repeater and shows as down', () => {
  const h = new NodeHealth();
  h.record('B117', { kind: 'fixed', now: T0 });
  h.record('B117', { now: T0 + 1000 });
  const n = h.nodes.get('B117');
  assert.equal(n.kind, 'fixed');
  assert.equal(h.status(n, T0 + 400000), 'down');
  assert.equal(h.coverage(T0 + 400000).up, 0);
});

test('node status thresholds', () => {
  const h = new NodeHealth();
  const n = h.record('B117', { kind: 'fixed', now: T0 });
  assert.equal(h.status(n, T0 + 1000), 'up');
  assert.equal(h.status(n, T0 + 120000), 'stale');
  assert.equal(h.status(n, T0 + 400000), 'down');
});

/* Feed watchdog, issue I3 and decision D7. */

test('a console that has never had a live feed says so', () => {
  const w = new FeedWatchdog();
  assert.equal(w.state(T0), 'never');
  assert.equal(w.isFailed(T0), true, 'no feed is a failure state, not a neutral one');
  assert.match(w.describe(T0), /No live feed connected/);
});

test('a heartbeat makes the feed healthy', () => {
  const w = new FeedWatchdog();
  w.heartbeat(T0);
  assert.equal(w.state(T0 + 1000), 'ok');
  assert.equal(w.isFailed(T0 + 1000), false);
});

test('an overdue gateway warns before it is called lost', () => {
  const w = new FeedWatchdog();
  w.heartbeat(T0);
  assert.equal(w.state(T0 + 89000), 'ok');
  assert.equal(w.state(T0 + 90000), 'warning');
  assert.equal(w.state(T0 + 299000), 'warning');
  assert.equal(w.state(T0 + 300000), 'lost');
});

test('a lost feed is a failure state and says how long it has been gone', () => {
  const w = new FeedWatchdog();
  w.heartbeat(T0);
  assert.equal(w.isFailed(T0 + 300000), true);
  assert.match(w.describe(T0 + 360000), /Live feed lost/);
  assert.match(w.describe(T0 + 360000), /6 minutes/);
  assert.match(w.describe(T0 + 360000), /not reaching this console/);
});

test('a recovered gateway clears the failure', () => {
  const w = new FeedWatchdog();
  w.heartbeat(T0);
  assert.equal(w.state(T0 + 400000), 'lost');
  w.heartbeat(T0 + 400000);
  assert.equal(w.state(T0 + 401000), 'ok');
});

/* Beacon registry, issue I9. */

const RECORDS = [
  { devid: 'A4F2', name: 'S. Chen', party: 2, dueBack: '2026-08-22T18:00:00Z' },
  { devid: 'b117', name: 'J. Okafor', party: 1, dueBack: '2026-08-25T12:00:00Z' }
];

test('a device id looks up a registration regardless of case', () => {
  const r = new BeaconRegistry(RECORDS);
  assert.equal(r.lookup('A4F2').name, 'S. Chen');
  assert.equal(r.lookup('a4f2').name, 'S. Chen');
  assert.equal(r.lookup('B117').name, 'J. Okafor');
  assert.equal(r.size, 2);
});

test('an unregistered beacon is a normal state, not an error', () => {
  const r = new BeaconRegistry(RECORDS);
  assert.equal(r.lookup('FFFF'), null);
  assert.equal(r.lookup(null), null);
});

test('a party past its return date is flagged', () => {
  const r = new BeaconRegistry(RECORDS);
  const due = Date.parse('2026-08-22T18:00:00Z');
  assert.equal(r.isOverdue('A4F2', due - 3600000), false);
  assert.equal(r.isOverdue('A4F2', due + 3600000), true);
});

test('no return date means no overdue judgement rather than a guess', () => {
  const r = new BeaconRegistry([{ devid: 'C001', name: 'No date' }]);
  assert.equal(r.isOverdue('C001', T0), null);
  assert.equal(r.isOverdue('FFFF', T0), null);
});
