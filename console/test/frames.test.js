/* Parser tests.
 *
 *   node --test console/test/
 *
 * This is the one piece of the console where a bug means a real distress call
 * is discarded, so it is tested directly rather than through the DOM.
 */
const test = require('node:test');
const assert = require('node:assert');
const F = require('../lib/frames.js');

function distress(line) {
  const c = F.classify(line);
  assert.equal(c.distress, true, `expected ${line} to be read as a distress call`);
  return F.hydrateDistress(c.fields);
}

function routine(line) {
  const c = F.classify(line);
  assert.equal(c.distress, false, `expected ${line} not to be read as a distress call`);
  return F.parseRoutine(c.fields);
}

test('a complete distress call', () => {
  const h = distress('SOS|1|A4F2|52.8734|-118.0814|12|1755000000|M|87');
  assert.equal(h.devid, 'A4F2');
  assert.equal(h.identified, true);
  assert.equal(h.noFix, false);
  assert.equal(h.lat, 52.8734);
  assert.equal(h.lon, -118.0814);
  assert.equal(h.acc, 12);
  assert.equal(h.cat, 'M');
  assert.equal(h.batt, 87);
  assert.equal(h.sentEpoch, 1755000000000);
  assert.deepEqual(h.degraded, []);
});

test('a valid check-in', () => {
  const r = routine('OK|1|A4F2|52.8734|-118.0814|12|1755000000');
  assert.equal(r.ok, true);
  assert.equal(r.msg.type, 'OK');
  assert.equal(r.msg.devid, 'A4F2');
  assert.equal(r.msg.noFix, false);
});

test('valid telemetry', () => {
  const r = routine('TEL|1|B117|64|4.0|-3.5');
  assert.equal(r.ok, true);
  assert.equal(r.msg.type, 'TEL');
  assert.equal(r.msg.batt, 64);
  assert.equal(r.msg.volts, 4.0);
  assert.equal(r.msg.snr, -3.5);
});

test('an unknown frame type is rejected with a reason', () => {
  const r = routine('XYZ|1|A4F2|1|2|3|4');
  assert.equal(r.ok, false);
  assert.match(r.reason, /unknown type "XYZ"/);
});

test('a check-in with a wrong schema version is rejected with a reason', () => {
  const r = routine('OK|2|A4F2|52.87|-118.08|12|1755000000');
  assert.equal(r.ok, false);
  assert.match(r.reason, /schema v2/);
});

test('a check-in with a malformed device id is rejected with a reason', () => {
  const r = routine('OK|1|ZZZZ|52.87|-118.08|12|1755000000');
  assert.equal(r.ok, false);
  assert.match(r.reason, /bad device id "ZZZZ"/);
});

test('a check-in with latitude out of range is rejected with a reason', () => {
  const r = routine('OK|1|A4F2|999|-118.08|12|1755000000');
  assert.equal(r.ok, false);
  assert.match(r.reason, /latitude 999 out of range/);
});

test('a check-in with longitude out of range is rejected with a reason', () => {
  const r = routine('OK|1|A4F2|52.87|999|12|1755000000');
  assert.equal(r.ok, false);
  assert.match(r.reason, /longitude 999 out of range/);
});

/* Everything below is the rule the product turns on: a distress frame is never
   discarded for a bad field. */

test('0|0 coordinates parse as a NO FIX distress call, not an error', () => {
  const h = distress('SOS|1|A4F2|0|0|12|1755000000|M|87');
  assert.equal(h.noFix, true);
  assert.equal(h.noFixReason, 'no fix reported by beacon');
  assert.equal(h.identified, true);
  assert.deepEqual(h.degraded, [], 'a beacon honestly reporting no lock is not a degraded frame');
});

test('empty coordinates parse as a NO FIX distress call', () => {
  const h = distress('SOS|1|A4F2|||12|1755000000|M|87');
  assert.equal(h.noFix, true);
  assert.equal(h.noFixReason, 'no fix reported by beacon');
  assert.deepEqual(h.degraded, []);
});

test('latitude out of range degrades to NO FIX and still raises the call', () => {
  const h = distress('SOS|1|A4F2|999|-118.0814|12|1755000000|M|87');
  assert.equal(h.noFix, true);
  assert.equal(h.noFixReason, 'coordinates out of range');
  assert.equal(h.identified, true, 'the rest of the frame is still readable');
  assert.equal(h.degraded.length, 1);
  assert.match(h.degraded[0], /out of range/);
});

test('longitude out of range degrades to NO FIX', () => {
  const h = distress('SOS|1|A4F2|52.8734|999|12|1755000000|M|87');
  assert.equal(h.noFix, true);
  assert.equal(h.noFixReason, 'coordinates out of range');
});

test('unreadable coordinates degrade to NO FIX', () => {
  const h = distress('SOS|1|A4F2|north|west|12|1755000000|M|87');
  assert.equal(h.noFix, true);
  assert.equal(h.noFixReason, 'coordinates unreadable');
  assert.match(h.degraded[0], /unreadable/);
});

test('a malformed device id still raises a call, marked unidentified', () => {
  const h = distress('SOS|1|ZZZZ|52.8734|-118.0814|12|1755000000|M|87');
  assert.equal(h.identified, false);
  assert.equal(h.devid, null);
  assert.equal(h.noFix, false, 'the position is still good and must be kept');
  assert.match(h.degraded[0], /device id "ZZZZ" unreadable/);
});

test('an unknown category degrades to unspecified', () => {
  const h = distress('SOS|1|A4F2|52.8734|-118.0814|12|1755000000|Q|87');
  assert.equal(h.cat, 'U');
  assert.match(h.degraded[0], /category "Q" not recognised/);
});

test('a missing trailing field leaves the value unknown, not the frame rejected', () => {
  const h = distress('SOS|1|A4F2|52.8734|-118.0814|12|1755000000|M');
  assert.equal(h.batt, null);
  assert.equal(h.cat, 'M');
  assert.equal(h.identified, true);
});

test('a bare SOS with nothing else still raises a call', () => {
  const h = distress('SOS');
  assert.equal(h.identified, false);
  assert.equal(h.noFix, true);
  assert.equal(h.cat, 'U');
});

test('an unknown schema on a distress frame alarms but trusts no field after it', () => {
  const h = distress('SOS|2|A4F2|52.8734|-118.0814|12|1755000000|M|87');
  assert.equal(h.identified, false, 'field positions cannot be trusted under an unknown schema');
  assert.equal(h.noFix, true);
  assert.equal(h.lat, null);
  assert.equal(h.batt, null);
  assert.match(h.degraded[0], /schema v2/);
});

test('a missing schema on a distress frame alarms the same way', () => {
  const h = distress('SOS||A4F2|52.8734|-118.0814|12|1755000000|M|87');
  assert.equal(h.identified, false);
  assert.match(h.degraded[0], /schema version missing/);
});

test('lowercase and padded frames are read', () => {
  const h = distress('  sos|1|a4f2|52.8734|-118.0814|12|1755000000|m|87  ');
  assert.equal(h.devid, 'A4F2');
  assert.equal(h.cat, 'M');
});

test('a near miss on the frame type is not treated as distress', () => {
  assert.equal(F.classify('SOZ|1|A4F2').distress, false);
  assert.equal(F.classify('SOS!|1|A4F2').distress, false);
  assert.equal(F.classify('').distress, false);
});

/* The sender clock. Issue I8: a fabricated send time on an incident record is
   worse than an admitted gap. */

test('an absent sender clock is null, never a substituted local time', () => {
  assert.equal(F.senderClock(''), null);
  assert.equal(F.senderClock(undefined), null);
  assert.equal(F.senderClock('0'), null, 'the contract says 0 means the clock is not set');
  assert.equal(F.senderClock('notatime'), null);
});

test('an implausible sender clock is null rather than 1970', () => {
  assert.equal(F.senderClock('12345'), null);
});

test('a plausible sender clock is kept in milliseconds', () => {
  assert.equal(F.senderClock('1755000000'), 1755000000000);
});

test('a distress frame with no clock reports no send time', () => {
  const h = distress('SOS|1|A4F2|52.8734|-118.0814|12||M|87');
  assert.equal(h.sentEpoch, null);
});

/* Strict field parsing. A field that is only partly a number is a field we
   could not read, and guessing at it puts a confident wrong position on a
   warden's map. */

test('a half numeric coordinate is NO FIX, not a confident wrong position', () => {
  const h = distress('SOS|1|A4F2|52.8734junk|-118.0814|12|1755000000|M|87');
  assert.equal(h.noFix, true);
  assert.equal(h.noFixReason, 'coordinates unreadable');
  assert.equal(h.lat, null, 'parseFloat would have returned 52.8734 here');
});

test('a half numeric battery is unknown rather than a guess', () => {
  const h = distress('SOS|1|A4F2|52.8734|-118.0814|12|1755000000|M|87%');
  assert.equal(h.batt, null);
});

test('a half numeric accuracy is unknown', () => {
  const h = distress('SOS|1|A4F2|52.8734|-118.0814|12m|1755000000|M|87');
  assert.equal(h.acc, null);
});

test('ordinary numeric forms still read', () => {
  const h = distress('SOS|1|A4F2|-52.5|+118.25|0|1755000000|M|100');
  assert.equal(h.lat, -52.5);
  assert.equal(h.lon, 118.25);
  assert.equal(h.acc, 0);
  assert.equal(h.batt, 100);
});

test('a garbled schema version is not read as version 1', () => {
  const h = distress('SOS|1x|A4F2|52.8734|-118.0814|12|1755000000|M|87');
  assert.equal(h.identified, false, 'parseInt("1x") is 1, which would trust v1 offsets');
  assert.match(h.degraded[0], /schema version missing or unreadable/);
});

test('the console and the firmware agree about a garbled schema version', () => {
  // firmware/src/TrekSafeFrames.cpp atoiSafe refuses "1x" too. These two must
  // not disagree about what version a frame claims to be.
  const r = routine('OK|1x|A4F2|52.87|-118.08|12|1755000000');
  assert.equal(r.ok, false);
  assert.match(r.reason, /missing schema version/);
});

test('a half numeric sender clock is treated as no clock', () => {
  assert.equal(F.senderClock('1755000000x'), null);
});

test('the acknowledgement frame matches the contract', () => {
  assert.equal(F.buildAck('A4F2', 'INC-001'), 'ACK|1|A4F2|INC-001');
});
