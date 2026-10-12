/* Node health.
 *
 * Fixed repeaters and visitor beacons are different things and only one of them
 * says anything about coverage. A beacon that was issued this morning and
 * switched off at the trailhead is not a gap in the network. Counting it as one
 * makes the coverage figure meaningless, which is the only thing that panel
 * exists to report. See issue I7 and decision D7.
 */
(function (root, factory) {
  const api = factory();
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.TrekSafeNodes = api;
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  const STALE_MS = 90 * 1000;
  const DOWN_MS = 300 * 1000;

  class NodeHealth {
    constructor(options) {
      const o = options || {};
      this.staleMs = o.staleMs || STALE_MS;
      this.downMs = o.downMs || DOWN_MS;
      // Enough to see a trend over days without growing without bound on a
      // station that runs all season.
      this.battSamples = o.battSamples || 200;
      this.nodes = new Map();
    }

    /* Telemetry is what a fixed repeater sends and a beacon does not, so it is
       what identifies infrastructure. Once a device has been seen as fixed it
       stays fixed, because a repeater that stops sending telemetry is a
       repeater in trouble, not a beacon. */
    record(devid, reading) {
      if (!devid) return null;
      const r = reading || {};
      const n = this.nodes.get(devid) || { devid, kind: 'beacon', firstSeen: r.now };
      n.last = r.now;
      if (r.kind === 'fixed') n.kind = 'fixed';
      if (r.volts != null) n.volts = r.volts;
      if (r.snr != null) n.snr = r.snr;

      if (r.batt != null) {
        n.batt = r.batt;
        /* A short battery history, kept so a repeater that is losing ground can
           be seen losing it. A solar node in November declines for weeks before
           it dies, and knowing that in advance is the difference between
           replacing a battery in daylight and discovering a hole in the network
           during an incident. */
        n.battHistory = n.battHistory || [];
        n.battHistory.push({ at: r.now, pct: r.batt });
        if (n.battHistory.length > this.battSamples) n.battHistory.shift();
      }

      this.nodes.set(devid, n);
      return n;
    }

    /* Which way a repeater's battery is going, and how fast.
     *
     * Deliberately refuses to answer without enough to answer with. A trend
     * drawn from two readings an hour apart would put a confident arrow on a
     * screen with nothing behind it, and a maintenance decision would get made
     * on it. */
    batteryTrend(node, options) {
      const o = options || {};
      const minSamples = o.minSamples || 3;
      const minSpanMs = o.minSpanMs || 60 * 60 * 1000;
      const h = (node && node.battHistory) || [];
      if (h.length < minSamples) return { known: false, reason: 'not enough readings yet' };

      const first = h[0], last = h[h.length - 1];
      const spanMs = last.at - first.at;
      if (spanMs < minSpanMs) return { known: false, reason: 'not enough time covered yet' };

      const deltaPct = last.pct - first.pct;
      const perDay = deltaPct / (spanMs / 86400000);

      let direction = 'steady';
      if (perDay <= -1) direction = 'falling';
      else if (perDay >= 1) direction = 'rising';

      const out = { known: true, direction, perDay, spanMs, from: first.pct, to: last.pct,
                    daysToEmpty: null };

      /* A run out date is only projected from at least a day of readings, and
         only while it is actually falling.
       *
       * Extrapolating a steady or charging node would invent a deadline. So
       * would extrapolating a few hours: a solar node drops steeply overnight
       * and climbs back at dawn, so an afternoon of decline scaled to a daily
       * rate produces a confident number that is simply the night ahead. A day
       * of readings covers at least one charge cycle. */
      const projectAfterMs = o.projectAfterMs || 24 * 60 * 60 * 1000;
      if (direction === 'falling' && perDay < 0 && spanMs >= projectAfterMs) {
        out.daysToEmpty = Math.max(0, last.pct / -perDay);
      }
      return out;
    }

    status(node, now) {
      const age = now - node.last;
      if (age > this.downMs) return 'down';
      if (age > this.staleMs) return 'stale';
      return 'up';
    }

    fixed() { return [...this.nodes.values()].filter((n) => n.kind === 'fixed'); }
    beacons() { return [...this.nodes.values()].filter((n) => n.kind === 'beacon'); }

    /* Coverage counts infrastructure only. Reported as an explicit "none known"
       rather than 0 of 0, which reads like everything is fine. */
    coverage(now) {
      const fixed = this.fixed();
      const up = fixed.filter((n) => this.status(n, now) === 'up').length;
      return { up, total: fixed.length, known: fixed.length > 0 };
    }
  }

  return { NodeHealth, STALE_MS, DOWN_MS };
});
