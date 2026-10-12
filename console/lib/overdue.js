/* Overdue parties.
 *
 * Every other part of this system waits for someone to press a button. This is
 * the part that does not. A person who is unconscious, whose beacon is at the
 * bottom of a creek, or who never got as far as reaching for it, presses
 * nothing, and the only signal left is that they said they would be back by
 * four and it is now nine.
 *
 * That signal is already on this machine: the trailhead desk records a return
 * time when it issues the beacon, and the console knows when each beacon was
 * last heard. Nothing new goes on the air for this.
 *
 * It is deliberately not a distress alarm. A distress alarm means a human
 * pressed the button, and that meaning must stay exact. Overdue is its own
 * state, shown its own way, and a warden decides what it is worth.
 */
(function (root, factory) {
  const api = factory();
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.TrekSafeOverdue = api;
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  /* How late a party has to be before the console says anything. Parties run
     late constantly: a slow descent, a long lunch, a misjudged pace. Flagging
     at the minute they are due would train an operator to ignore this panel,
     which is the only way it can actually fail. */
  const GRACE_MS = 60 * 60 * 1000;

  class OverdueWatch {
    constructor(options) {
      const o = options || {};
      this.graceMs = o.graceMs != null ? o.graceMs : GRACE_MS;
      this.announced = new Set();
    }

    /* Records whose return time cannot be read at all.
     *
     * A typo in this field is not a harmless data problem. The party it belongs
     * to silently drops out of the only check in this system that runs without
     * anybody pressing a button, and nobody finds out until the day it matters.
     * Reported separately from the overdue list, because an unreadable date is
     * not the same claim as a late party, and the operator has to fix a record
     * rather than decide about a person. */
    unreadable(registry) {
      const out = [];
      for (const record of registry.byId.values()) {
        if (!record.dueBack) continue;
        if (Number.isFinite(Date.parse(record.dueBack))) continue;
        out.push({
          devid: record.devid,
          name: record.name || record.devid,
          dueBack: String(record.dueBack)
        });
      }
      return out;
    }

    /* Everything currently overdue, worst first. Pure: pass in the registry and
       the node health, get back a list. */
    check(registry, health, now) {
      const out = [];
      for (const record of registry.byId.values()) {
        if (!record.dueBack) continue;
        const due = Date.parse(record.dueBack);
        // An unreadable date is reported by unreadable(), not silently skipped.
        if (!Number.isFinite(due)) continue;

        const overdueByMs = now - (due + this.graceMs);
        if (overdueByMs <= 0) continue;

        const node = health && health.nodes ? health.nodes.get(record.devid) : null;
        out.push({
          devid: record.devid,
          name: record.name || record.devid,
          party: record.party != null ? record.party : null,
          notes: record.notes || null,
          dueBack: due,
          /* How late they are against the time they gave, not against the
             grace period, because that is the number a warden is deciding on. */
          lateByMs: now - due,
          lastHeard: node ? node.last : null
        });
      }
      out.sort((a, b) => b.lateByMs - a.lateByMs);
      return out;
    }

    /* The ones not yet announced, so the event log records each party once
       rather than every second. */
    newlyOverdue(list) {
      const fresh = list.filter((p) => !this.announced.has(p.devid));
      fresh.forEach((p) => this.announced.add(p.devid));
      return fresh;
    }

    /* A party that came back, or whose record was corrected, can be announced
       again if it goes overdue later. */
    clearAnnounced(list) {
      const current = new Set(list.map((p) => p.devid));
      for (const devid of [...this.announced]) {
        if (!current.has(devid)) this.announced.delete(devid);
      }
    }

    /* Plain English, for a warden. Says what is known and what is not, because
       "not heard from at all" and "checked in an hour ago" are very different
       situations and the difference decides what happens next. */
    describe(party, now) {
      const late = hours(party.lateByMs);
      if (party.lastHeard == null) {
        return party.name + ' is ' + late + ' past the return time and has not been heard from at all.';
      }
      return party.name + ' is ' + late + ' past the return time. Last heard ' +
             hours(now - party.lastHeard) + ' ago.';
    }
  }

  function hours(ms) {
    const m = Math.floor(ms / 60000);
    if (m < 60) return m + (m === 1 ? ' minute' : ' minutes');
    const h = Math.floor(m / 60);
    const rem = m % 60;
    if (h < 24) return h + (h === 1 ? ' hour' : ' hours') + (rem ? ' ' + rem + ' min' : '');
    const d = Math.floor(h / 24);
    return d + (d === 1 ? ' day' : ' days');
  }

  return { OverdueWatch, GRACE_MS };
});
