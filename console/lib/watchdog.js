/* Feed watchdog. Issue I3, decision D7.
 *
 * An operator who believes they are covered when they are not is the worst
 * state this console can be in, and it is the state it sits in by default: a
 * quiet screen looks identical whether the mesh is calm or the gateway died an
 * hour ago.
 *
 * Driven by a gateway heartbeat rather than by mesh traffic. A quiet mesh at
 * 3am is normal. A gateway that has stopped talking is not, and only the
 * heartbeat can tell those apart.
 */
(function (root, factory) {
  const api = factory();
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.TrekSafeWatchdog = api;
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  const WARN_MS = 90 * 1000;
  const LOST_MS = 300 * 1000;

  /* States, worst last:
       never    no gateway has ever reported in, so there is no live feed at all
       ok       heard from recently
       warning  overdue, may be a slow link
       lost     gone long enough that it must be treated as no coverage
  */
  class FeedWatchdog {
    constructor(options) {
      const o = options || {};
      this.warnMs = o.warnMs || WARN_MS;
      this.lostMs = o.lostMs || LOST_MS;
      this.lastBeat = null;
      this.everConnected = false;
      this.knownDown = false;
    }

    heartbeat(now) {
      this.lastBeat = now;
      this.everConnected = true;
      this.knownDown = false;
    }

    /* The connection to the bridge broke, and we know it now rather than
       inferring it later from silence.
     *
     * Without this the console kept saying the feed was healthy for up to the
     * full ninety seconds it takes a missing heartbeat to show, while the
     * acknowledgement path was already gone. An operator pressing Acknowledge
     * in that window was told there was no live feed by a console whose banner
     * said there was one. Two parts of the same screen contradicting each other
     * is worse than either message alone, because it makes the operator stop
     * believing the screen.
     *
     * Silence and a known break are different facts and this is the second one.
     * A heartbeat clears it. */
    disconnected() {
      this.knownDown = true;
    }

    state(now) {
      if (!this.everConnected) return 'never';
      if (this.knownDown) return 'lost';
      const age = now - this.lastBeat;
      if (age >= this.lostMs) return 'lost';
      if (age >= this.warnMs) return 'warning';
      return 'ok';
    }

    /* Plain English for a warden, not a status code. No jargon, and the text
       carries the meaning on its own so nothing depends on colour. */
    describe(now) {
      switch (this.state(now)) {
        case 'never':
          return 'No live feed connected. This console is showing simulated and pasted packets only.';
        case 'lost':
          if (this.knownDown) {
            return 'Live feed lost. The connection to the gateway broke. ' +
                   'Distress calls are not reaching this console and an acknowledgement ' +
                   'cannot be sent.';
          }
          return 'Live feed lost. Nothing from the gateway for ' + minutes(now - this.lastBeat) +
                 '. Distress calls are not reaching this console.';
        case 'warning':
          return 'Live feed overdue. Nothing from the gateway for ' + minutes(now - this.lastBeat) + '.';
        default:
          return 'Live feed healthy.';
      }
    }

    /* Loss of feed has to be as visible as an alarm, so the caller needs to
       know when to change the whole screen rather than a label. */
    isFailed(now) {
      const s = this.state(now);
      return s === 'lost' || s === 'never';
    }
  }

  function minutes(ms) {
    const m = Math.floor(ms / 60000);
    if (m < 1) return Math.floor(ms / 1000) + ' seconds';
    if (m === 1) return '1 minute';
    return m + ' minutes';
  }

  return { FeedWatchdog, WARN_MS, LOST_MS };
});
