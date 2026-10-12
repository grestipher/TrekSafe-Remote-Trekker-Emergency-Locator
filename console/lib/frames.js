/* TrekSafe payload contract v1, console side.
 *
 * Loaded as a classic script so index.html still opens from disk with no build
 * step and no internet, and required by node:test so the parser that decides
 * whether a distress call is heard is tested directly rather than by copy.
 *
 * Contract: docs/payload-contract.md, ratified August 2026.
 *
 *   SOS|1|<devid>|<lat>|<lon>|<acc_m>|<epoch>|<cat>|<batt>
 *   OK |1|<devid>|<lat>|<lon>|<acc_m>|<epoch>
 *   TEL|1|<devid>|<batt>|<volts>|<snr>
 *   ACK|1|<devid>|<incident_id>        base station to beacon
 */
(function (root, factory) {
  const api = factory();
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.TrekSafeFrames = api;
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  const SCHEMA = 1;
  const CATEGORY = { M: 'Medical', I: 'Injury', L: 'Lost', U: 'Unspecified' };

  /* Strict on purpose. parseFloat("52.8junk") returns 52.8, which would put a
     confident position on a warden's map from a field we could not actually
     read. A coordinate that is not entirely a number is a coordinate we do not
     have, and the console has a first class way to say that: NO FIX. The whole
     field must be numeric or the value is unknown. */
  const num = (v) => {
    const s = String(v == null ? '' : v).trim();
    if (s === '') return null;
    if (!/^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$/.test(s)) return null;
    const n = Number(s);
    return Number.isFinite(n) ? n : null;
  };

  /* Same reasoning for the schema version, and it keeps the console and the
     firmware agreeing: parseInt("1x") is 1, which would read a frame we cannot
     identify at version 1 field offsets. The firmware's atoiSafe already
     refuses that, and these two must not disagree about what version a frame
     claims to be. */
  const strictInt = (v) => {
    const s = String(v == null ? '' : v).trim();
    return /^[+-]?\d+$/.test(s) ? parseInt(s, 10) : null;
  };

  /* Stage one. Decides one thing only: does this frame claim to be a distress
     call? Nothing here can reject a frame that says SOS. Field validation
     happens in stage two, after the incident exists and the alarm has fired,
     because a distress call that fails a field check is still a distress call.

     Field 0 must read exactly SOS. Near misses are not treated as distress: a
     corrupted first byte cannot be told apart from unrelated mesh traffic, and
     false alarms have a cost of their own. */
  function classify(line) {
    const raw = (line || '').trim();
    const fields = raw.split('|').map((s) => s.trim());
    return { raw, fields, distress: (fields[0] || '').toUpperCase() === 'SOS' };
  }

  /* Stage two. Fills in whatever is readable and marks the rest unknown. This
     cannot fail and cannot discard an incident. Every substitution is recorded
     in degraded[] so it can be stated on screen and in the log: a value quietly
     replaced is a value silently dropped. */
  function hydrateDistress(fields) {
    const degraded = [];

    const ver = strictInt(fields[1]);
    if (ver !== SCHEMA) {
      /* The version sits early in the frame so an unknown schema can be spotted
         before anything after it is trusted. Read at v1 offsets, a later schema
         could produce a confidently wrong position, which is worse than none.
         The alarm still fires and the raw frame is kept for a human to read. */
      degraded.push(ver !== null
        ? `schema v${ver}, console speaks v${SCHEMA}, no field after the version was read`
        : 'schema version missing or unreadable, no field after it was read');
      return {
        devid: null, identified: false,
        lat: null, lon: null, noFix: true,
        noFixReason: 'schema not recognised, position not read',
        acc: null, sentEpoch: null, cat: 'U', batt: null, degraded
      };
    }

    const devidRaw = fields[2] || '';
    const devid = devidRaw.toUpperCase();
    const identified = /^[0-9A-F]{2,8}$/.test(devid);
    if (!identified) degraded.push(`device id "${devidRaw}" unreadable, incident marked unidentified`);

    /* 0|0 and empty coordinates are a beacon without a lock, which the contract
       calls a valid distress message. Unreadable and out of range coordinates
       are a fault, so they degrade to the same NO FIX state but are recorded as
       a fault rather than as a beacon reporting honestly. */
    const latRaw = fields[3] || '', lonRaw = fields[4] || '';
    const lat = num(latRaw), lon = num(lonRaw);
    let noFix = false, noFixReason = null;
    if (latRaw === '' && lonRaw === '') {
      noFix = true; noFixReason = 'no fix reported by beacon';
    } else if (lat === null || lon === null) {
      noFix = true; noFixReason = 'coordinates unreadable';
      degraded.push(`coordinates "${latRaw}", "${lonRaw}" unreadable, position recorded as NO FIX`);
    } else if (lat === 0 && lon === 0) {
      noFix = true; noFixReason = 'no fix reported by beacon';
    } else if (lat < -90 || lat > 90 || lon < -180 || lon > 180) {
      noFix = true; noFixReason = 'coordinates out of range';
      degraded.push(`coordinates ${latRaw}, ${lonRaw} out of range, position recorded as NO FIX`);
    }

    let cat = (fields[7] || 'U').toUpperCase();
    if (!CATEGORY[cat]) {
      degraded.push(`category "${fields[7]}" not recognised, recorded as unspecified`);
      cat = 'U';
    }

    return {
      devid: identified ? devid : null, identified,
      lat: noFix ? null : lat, lon: noFix ? null : lon, noFix, noFixReason,
      acc: num(fields[5]),
      sentEpoch: senderClock(fields[6]),
      cat, batt: num(fields[8]), degraded
    };
  }

  /* The contract says an epoch of 0 or empty means the sending device has no
     valid clock, which is the normal case for a beacon that has never had a
     lock. Returning null rather than a substituted local time is the whole
     point: a fabricated send time in an incident review is worse than an
     admitted gap. */
  function senderClock(field) {
    const epoch = strictInt(field);
    if (epoch === null || epoch <= 0) return null;
    /* Anything before 2001 is a device counting from its own boot, not a real
       clock. Treating it as a send time would put 1970 on an incident record. */
    if (epoch < 1000000000) return null;
    return epoch * 1000;
  }

  /* Non-distress frames keep the strict path. Neither a check-in nor telemetry
     raises an alarm, so rejecting one with a stated reason is still right. */
  function parseRoutine(fields) {
    const type = (fields[0] || '').toUpperCase();
    if (!['OK', 'TEL'].includes(type)) return { ok: false, reason: `unknown type "${fields[0]}"` };

    const ver = strictInt(fields[1]);
    if (ver === null) return { ok: false, reason: 'missing schema version' };
    if (ver !== SCHEMA) return { ok: false, reason: `schema v${ver}, console speaks v${SCHEMA}` };

    const devid = (fields[2] || '').toUpperCase();
    if (!/^[0-9A-F]{2,8}$/.test(devid)) return { ok: false, reason: `bad device id "${fields[2]}"` };

    if (type === 'TEL') {
      return { ok: true, msg: { type, devid, batt: num(fields[3]), volts: num(fields[4]), snr: num(fields[5]) } };
    }

    const lat = num(fields[3]), lon = num(fields[4]);
    const noFix = (lat === null || lon === null || (lat === 0 && lon === 0));
    if (!noFix) {
      if (lat < -90 || lat > 90) return { ok: false, reason: `latitude ${lat} out of range` };
      if (lon < -180 || lon > 180) return { ok: false, reason: `longitude ${lon} out of range` };
    }

    return { ok: true, msg: {
      type, devid, noFix,
      lat: noFix ? null : lat, lon: noFix ? null : lon,
      acc: num(fields[5]), sentEpoch: senderClock(fields[6]),
      batt: null, volts: null, snr: null
    } };
  }

  function buildAck(devid, incidentRef) {
    return `ACK|${SCHEMA}|${devid}|${incidentRef}`;
  }

  return { SCHEMA, CATEGORY, classify, hydrateDistress, parseRoutine, senderClock, buildAck };
});
