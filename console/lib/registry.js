/* Beacon registry. Issue I9.
 *
 * A warden seeing A4F2 cannot act on it. The registration process that issues
 * the beacon at the trailhead already holds a name and a return date, so the
 * console looks the device id up locally and shows something a person can act
 * on.
 *
 * The mesh never carries any of this. Nothing in this file is ever put into a
 * frame, and the ACK frame carries the device id only. That is the privacy
 * position: identity lives at the base station, the radio carries a number.
 *
 * Where the record itself lives is decision D6 and is not settled. Today the
 * console reads console/registry-data.js, which an operator edits by hand.
 */
(function (root, factory) {
  const api = factory();
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.TrekSafeRegistry = api;
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  class BeaconRegistry {
    constructor(records) {
      this.byId = new Map();
      this.load(records || []);
    }

    load(records) {
      this.byId.clear();
      for (const r of records) {
        if (!r || !r.devid) continue;
        const devid = String(r.devid).toUpperCase();
        this.byId.set(devid, Object.assign({}, r, { devid }));
      }
      return this.byId.size;
    }

    /* Returns null for an unknown beacon, which is a normal state rather than
       an error: a beacon can be issued before the record reaches this machine,
       and a distress call from an unregistered beacon is still a distress
       call. */
    lookup(devid) {
      if (!devid) return null;
      return this.byId.get(String(devid).toUpperCase()) || null;
    }

    /* A party that is already overdue when the call comes in changes how a
       warden reads it. Returns null when there is no return date to compare. */
    isOverdue(devid, now) {
      const r = this.lookup(devid);
      if (!r || !r.dueBack) return null;
      const due = Date.parse(r.dueBack);
      if (!Number.isFinite(due)) return null;
      return now > due;
    }

    get size() { return this.byId.size; }
  }

  return { BeaconRegistry };
});
