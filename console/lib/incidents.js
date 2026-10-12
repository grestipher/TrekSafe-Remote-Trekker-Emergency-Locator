/* Incident store. Holds what the console knows about distress calls and the
 * two ways an incident can leave the alarm state.
 *
 * Kept away from the DOM so it can be tested directly. Nothing here touches a
 * browser API.
 */
(function (root, factory) {
  const api = factory(typeof require === 'function' ? require('./frames.js') : root.TrekSafeFrames);
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.TrekSafeIncidents = api;
})(typeof self !== 'undefined' ? self : this, function (Frames) {
  'use strict';

  /* A beacon that repeats an unacknowledged call is one person, not many. Two
     frames from the same device inside this window are the same incident.
     Longer than the firmware's slowest repeat interval, with room to spare for
     a frame that took a while to find a gateway. */
  const REPEAT_WINDOW_MS = 30 * 60 * 1000;

  /* How long after an acknowledgement a repeat is still explained by a frame
     that was already in the air. Past this, a beacon that is still calling has
     not heard the acknowledgement, and the person still believes nobody knows.
     That re-opens the incident, because it is exactly the state D3 exists to
     prevent.

     The same window applies to an incident a human only marked as seen. Nothing
     was sent there either, so a beacon still calling after it is a person who
     still believes nobody knows, which is the same state for the same reason. */
  const ACK_GRACE_MS = 120 * 1000;

  /* How long an acknowledgement may sit unconfirmed before the console stops
     believing it went out. The bridge answers a send in milliseconds; a reply
     that has not arrived in this long means the bridge is wedged or the machine
     lost it, and either way nobody told the beacon anything. */
  const ACK_CONFIRM_MS = 15 * 1000;

  class IncidentStore {
    constructor(options) {
      const o = options || {};
      this.repeatWindowMs = o.repeatWindowMs || REPEAT_WINDOW_MS;
      this.ackGraceMs = o.ackGraceMs || ACK_GRACE_MS;
      this.ackConfirmMs = o.ackConfirmMs || ACK_CONFIRM_MS;
      this.incidents = []; // newest first
      this.seq = 0;
    }

    find(ref) { return this.incidents.find((i) => i.ref === ref) || null; }

    /* An incident leaves the alarm when a human has either acknowledged it or,
       where there is nobody to acknowledge to, confirmed they have seen it. An
       incident whose beacon kept calling afterwards counts as open again. */
    openCount() { return this.incidents.filter((i) => this.isOpen(i)).length; }

    /* An acknowledgement the bridge has not confirmed yet does not close
       anything. Until something outside this machine says the frame went out,
       the honest state is that nobody has told the beacon, and the alarm is how
       the console says so. Clearing on the click alone was the old behaviour and
       it turned a failed send into a green screen. */
    isOpen(i) {
      if (i.ackNotReaching || i.seenNotReaching) return true;
      if (i.ackPending) return true;
      return !i.ack && !i.seen;
    }

    /* Finds the incident a repeated frame belongs to. Identified beacons match
       on device id. An unidentified frame has no id to match on, so it matches
       an identical raw frame, which is what a repeating beacon with a broken id
       field actually produces. */
    correlate(hydrated, raw, now) {
      const windowStart = now - this.repeatWindowMs;
      return this.incidents.find((i) => {
        if (i.lastReceived < windowStart) return false;
        if (hydrated.identified) return i.identified && i.devid === hydrated.devid;
        return !i.identified && i.raw === raw;
      }) || null;
    }

    /* Records a distress frame. Returns the incident and whether it was a
       repeat of one already on screen. */
    recordDistress(hydrated, context) {
      const now = context.now;
      const existing = this.correlate(hydrated, context.raw, now);

      if (existing) {
        existing.repeats++;
        existing.lastReceived = now;
        existing.raw = context.raw;
        existing.degraded = hydrated.degraded;
        existing.cat = hydrated.cat;
        existing.batt = hydrated.batt;
        existing.acc = hydrated.acc;
        existing.sentEpoch = hydrated.sentEpoch;

        /* A repeat that finally carries a position is the whole reason NO FIX
           is allowed to alarm without one. */
        if (!hydrated.noFix) {
          existing.lat = hydrated.lat;
          existing.lon = hydrated.lon;
          existing.noFix = false;
          existing.noFixReason = null;
        }

        if (existing.ack && now - existing.ackAt > this.ackGraceMs) existing.ackNotReaching = true;
        /* Same reasoning for an incident a human only looked at. Nothing was
           sent, so a beacon still calling this long afterwards has not been told
           anything by anybody, and that is the state D3 exists to prevent. */
        if (existing.seen && now - existing.seenAt > this.ackGraceMs) existing.seenNotReaching = true;

        return { incident: existing, isRepeat: true };
      }

      const incident = {
        ref: 'INC-' + String(++this.seq).padStart(3, '0'),
        devid: hydrated.devid,
        identified: hydrated.identified,
        lat: hydrated.lat, lon: hydrated.lon,
        noFix: hydrated.noFix, noFixReason: hydrated.noFixReason,
        acc: hydrated.acc,
        sentEpoch: hydrated.sentEpoch,
        cat: hydrated.cat,
        batt: hydrated.batt,
        degraded: hydrated.degraded,
        source: context.source,
        raw: context.raw,
        firstReceived: now,
        lastReceived: now,
        repeats: 0,
        ack: false, ackAt: null,
        seen: false, seenAt: null,
        ackNotReaching: false,
        seenNotReaching: false,
        /* An acknowledgement in flight: the frame has been handed to the bridge
           and nothing has confirmed it left. See acknowledge(). */
        ackPending: false, ackPendingAt: null, ackFailure: null
      };
      this.incidents.unshift(incident);
      return { incident, isRepeat: false };
    }

    /* The return path this whole design rests on. Returns the frame to put on
       the mesh, or a reason it cannot be sent.
     *
     * Two phase, deliberately. This marks the acknowledgement as in flight and
     * nothing more. The incident stays open, the alarm stays up, and only
     * confirmAck() closes it, called when whatever carries the frame says it
     * actually went out.
     *
     * The single phase version closed the incident on the click. A bridge with
     * no radio attached answers such a send with a refusal, and the console had
     * already gone green and quiet: it reported an acknowledgement that never
     * left the machine, which is the exact state D3 exists to prevent, dressed
     * up as the thing that prevents it. */
    acknowledge(ref, now) {
      const inc = this.find(ref);
      if (!inc) return { ok: false, reason: `no incident ${ref}` };
      if (inc.ackPending) return { ok: false, reason: `${ref} already has an acknowledgement in flight` };
      if (inc.ack && !inc.ackNotReaching) return { ok: false, reason: `${ref} is already acknowledged` };
      if (!inc.identified) {
        /* No readable device id means no address to reply to. The button that
           calls this is not rendered for such an incident; this guard is here
           so no other path can send a frame into the dark. */
        return { ok: false, reason: `${ref} has no device id to reply to` };
      }

      inc.ackPending = true;
      inc.ackPendingAt = now;
      inc.ackFailure = null;
      return { ok: true, frame: Frames.buildAck(inc.devid, inc.ref) };
    }

    /* Something outside this machine confirmed the frame went out. Only now is
       the person considered told, and only now does the alarm clear. */
    confirmAck(ref, now) {
      const inc = this.find(ref);
      if (!inc || !inc.ackPending) return { ok: false, reason: `no acknowledgement in flight for ${ref}` };
      inc.ackPending = false;
      inc.ackPendingAt = null;
      inc.ack = true;
      inc.ackAt = now;
      inc.ackNotReaching = false;
      inc.seenNotReaching = false;
      return { ok: true };
    }

    /* It did not go out. The incident returns to exactly where it was, which is
       open, and carries the reason so the console can say it in words rather
       than leaving a warden to notice a button changed back. */
    failAck(ref, reason) {
      const inc = this.find(ref);
      if (!inc || !inc.ackPending) return { ok: false, reason: `no acknowledgement in flight for ${ref}` };
      inc.ackPending = false;
      inc.ackPendingAt = null;
      inc.ackFailure = reason || 'the acknowledgement did not go out';
      return { ok: true, incident: inc };
    }

    /* An acknowledgement nothing ever answered for. A wedged bridge or a lost
       reply leaves the frame in flight forever, and an incident that sits
       pending forever is an alarm that never resolves either way. Time it out
       and say so: an operator who is told the reply failed can send it again,
       and one who is told nothing cannot. */
    expirePendingAcks(now) {
      const stale = this.incidents.filter(
        (i) => i.ackPending && now - i.ackPendingAt >= this.ackConfirmMs);
      stale.forEach((i) => this.failAck(i.ref, 'nothing confirmed the acknowledgement was sent'));
      return stale;
    }

    /* Rebuilding from stored history. Decision D6.
     *
     * The bridge stores frames and operator actions, never incidents, so that
     * the console stays the only thing that interprets a frame. Replaying that
     * history through recordDistress rebuilds the incident list, and these two
     * put back the parts a frame alone does not carry: that a human replied,
     * and that a human looked.
     *
     * Neither sends anything. They record what already happened.
     *
     * The bridge stores an outbound frame only once the radio has taken it, so
     * an ACK in the history is one that actually went out. A send that failed
     * is stored as an operator action instead and never reaches here, which is
     * what keeps a failed acknowledgement from coming back as a real one. */
    applyHistoricalAck(devid, at) {
      if (!devid) return null;
      const inc = this.incidents.find((i) => i.identified && i.devid === devid && !i.ack);
      if (!inc) return null;
      inc.ackPending = false;
      inc.ackPendingAt = null;
      inc.ack = true;
      inc.ackAt = at;
      inc.ackNotReaching = false;
      inc.seenNotReaching = false;
      return inc;
    }

    applyHistoricalSeen(raw, at) {
      const inc = this.incidents.find((i) => !i.identified && i.raw === raw && !i.seen);
      if (!inc) return null;
      inc.seen = true;
      inc.seenAt = at;
      inc.seenNotReaching = false;
      return inc;
    }

    /* For an incident that cannot be acknowledged. A human still confirms they
       have looked at it, which is what clears the alarm, and without this an
       unidentifiable frame would hold the console red for the rest of the
       shift. No frame is sent. Nothing tells the beacon help is coming. */
    markSeen(ref, now) {
      const inc = this.find(ref);
      if (!inc) return { ok: false, reason: `no incident ${ref}` };
      /* A beacon still calling long after a human looked has re-opened the
         incident, and looking again is the only action there is for it. Refusing
         would leave the operator with a red console and no button, which is how
         an alarm becomes furniture. */
      if (inc.seen && !inc.seenNotReaching) return { ok: false, reason: `${ref} is already marked seen` };
      if (inc.identified) {
        return { ok: false, reason: `${ref} has a device id and needs acknowledgement` };
      }
      inc.seen = true;
      inc.seenAt = now;
      inc.seenNotReaching = false;
      return { ok: true };
    }
  }

  return { IncidentStore, REPEAT_WINDOW_MS, ACK_GRACE_MS, ACK_CONFIRM_MS };
});
