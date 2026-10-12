/* Coverage survey. Decision D7.
 *
 * D7 says coverage is documented honestly, including gaps, and that the map is
 * published to visitors rather than held internally. That is a promise the
 * product could not keep until now, because nothing was recording where the
 * mesh actually worked.
 *
 * Every frame that arrives carrying both a position and link quality is one
 * measured point: at this spot, a packet got through, at this signal, over this
 * many hops. Enough of them is the coverage map. The gaps in it are the part
 * that matters, and the part a visitor is owed before they rely on this.
 *
 * The metadata comes from the packet layer, which only the gateway and the
 * bridge see. It never touched the payload contract: the radio reports signal
 * and noise for every packet it receives, and the link envelope carries the hop
 * count. See docs/payload-contract.md.
 *
 * Only measured readings reach here. Anything a simulator invented is marked as
 * such all the way from the source to this file and is excluded by the caller,
 * because a coverage map built from invented numbers is the exact false
 * confidence D7 exists to prevent.
 *
 * This lives in the console because reading a frame is the console's job, and
 * a second parser in the bridge is how the two halves start disagreeing.
 */
(function (root, factory) {
  const api = factory();
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.TrekSafeCoverage = api;
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  /* Points closer together than this are the same place for survey purposes.
     Roughly 100 metres at these latitudes. Without it a stationary beacon
     checking in all afternoon would stack hundreds of points on one spot and
     make the map look like coverage it has not actually surveyed. */
  const GRID_DEGREES = 0.001;

  class CoverageLog {
    constructor(options) {
      const o = options || {};
      this.gridDegrees = o.gridDegrees || GRID_DEGREES;
      this.points = new Map(); // grid key -> measurement
    }

    key(lat, lon) {
      const g = this.gridDegrees;
      return Math.round(lat / g) + ',' + Math.round(lon / g);
    }

    /* Records one measured point. Ignores anything without both a position and
       a signal reading: a frame with no link quality says nothing about
       coverage, and a frame with no position says nothing about where. */
    record(measurement) {
      const m = measurement || {};
      if (m.lat == null || m.lon == null) return null;
      if (m.snr == null && m.rssi == null) return null;

      const k = this.key(m.lat, m.lon);
      const existing = this.points.get(k);

      if (!existing) {
        const point = {
          lat: m.lat, lon: m.lon, key: k,
          bestSnr: m.snr != null ? m.snr : null,
          worstSnr: m.snr != null ? m.snr : null,
          rssi: m.rssi != null ? m.rssi : null,
          maxHops: m.hops != null ? m.hops : null,
          heard: 1,
          firstAt: m.at, lastAt: m.at
        };
        this.points.set(k, point);
        return point;
      }

      existing.heard++;
      existing.lastAt = m.at;
      if (m.snr != null) {
        /* Both ends are kept. The best is what the link can do; the worst is
           what it did when conditions were against it, and a survey that only
           records good days is the false confidence D7 is about. */
        if (existing.bestSnr == null || m.snr > existing.bestSnr) existing.bestSnr = m.snr;
        if (existing.worstSnr == null || m.snr < existing.worstSnr) existing.worstSnr = m.snr;
      }
      /* Kept the same way as SNR, worst wins. Overwriting with the latest
         reading meant a spot surveyed twenty times reported whichever packet
         happened to arrive last, so a marginal location could read comfortable
         because one packet got lucky. A survey that records good days is the
         false confidence D7 exists to stop. */
      if (m.rssi != null && (existing.rssi == null || m.rssi < existing.rssi)) existing.rssi = m.rssi;
      if (m.hops != null && (existing.maxHops == null || m.hops > existing.maxHops)) {
        existing.maxHops = m.hops;
      }
      return existing;
    }

    all() { return [...this.points.values()]; }
    get size() { return this.points.size; }

    /* Coarse bands, for drawing and for talking to a non radio person. The
       thresholds are the usual LoRa rules of thumb: above zero is comfortable,
       down to about minus ten still demodulates, below that is the edge. */
    static band(snr) {
      if (snr == null) return 'unknown';
      if (snr >= 0) return 'good';
      if (snr >= -10) return 'workable';
      return 'marginal';
    }

    summary() {
      const bands = { good: 0, workable: 0, marginal: 0, unknown: 0 };
      let maxHops = 0;
      for (const p of this.points.values()) {
        bands[CoverageLog.band(p.worstSnr)]++;
        if (p.maxHops != null && p.maxHops > maxHops) maxHops = p.maxHops;
      }
      return { points: this.points.size, bands, maxHops };
    }

    /* The survey as something that can leave this machine. Deliberately the
       measurements and nothing else: no device ids, no names. A coverage map
       published to visitors must not be a record of who was where. */
    toCsv() {
      const rows = ['lat,lon,worst_snr,best_snr,worst_rssi,max_hops,times_heard'];
      for (const p of this.all()) {
        rows.push([
          p.lat.toFixed(5), p.lon.toFixed(5),
          p.worstSnr != null ? p.worstSnr : '',
          p.bestSnr != null ? p.bestSnr : '',
          p.rssi != null ? p.rssi : '',
          p.maxHops != null ? p.maxHops : '',
          p.heard
        ].join(','));
      }
      return rows.join('\n');
    }
  }

  return { CoverageLog, GRID_DEGREES };
});
