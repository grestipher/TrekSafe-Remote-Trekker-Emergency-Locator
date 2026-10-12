/* Console side of the bridge. Decision D5.
 *
 * The bridge is a separate process. This connects to it over a local HTTP
 * connection: Server-Sent Events for frames coming off the mesh, and a POST for
 * acknowledgements going back out.
 *
 * window.sendToMesh is defined only while the bridge is actually reachable. If
 * it is not defined, the console says out loud that an acknowledgement did not
 * leave the machine, which is the truth and is worth more than a button that
 * appears to work.
 */
(function () {
  'use strict';

  /* When the bridge is serving this page, talk to it on the same origin. That
     is the normal way to run: no cross origin question, and OpenStreetMap will
     serve map tiles, which it refuses to a page opened from disk because such a
     request carries no Referer.

     Opened from disk the console still works, and reaches for the bridge on the
     loopback address. */
  const servedByBridge = location.protocol === 'http:' || location.protocol === 'https:';
  const BRIDGE = window.TREKSAFE_BRIDGE_URL || (servedByBridge ? '' : 'http://127.0.0.1:8730');
  const BRIDGE_LABEL = BRIDGE || location.origin;

  if (typeof window.EventSource !== 'function') {
    window.feedDisconnected('this browser cannot open a live feed');
    return;
  }

  let announcedDown = false;

  function connect() {
    const es = new EventSource(BRIDGE + '/stream');

    /* A packet arrives as JSON carrying the frame and whatever the radio
       reported about how it got here. Older payloads were the bare frame, so a
       string that is not JSON is still read as one rather than dropped: losing
       a distress call to a format change would be the worst possible trade. */
    es.addEventListener('packet', (e) => {
      let frame = e.data, link = null;
      try {
        const parsed = JSON.parse(e.data);
        if (parsed && typeof parsed.frame === 'string') {
          frame = parsed.frame;
          link = parsed;
        }
      } catch (err) { /* a bare frame, which is still a frame */ }
      window.feedPacket(frame, link);
    });

    es.addEventListener('heartbeat', () => {
      announcedDown = false;
      /* Outbound only works while the feed is up. Defining these here, and
         removing them on failure, is what keeps the console honest about
         whether an acknowledgement actually went anywhere. */
      window.sendToMesh = sendToMesh;
      window.recordAction = recordAction;
      window.feedHeartbeat();
    });

    /* Stored history, replayed when this console attaches. Decision D6. One
       record per event so a single unreadable one cannot take the rest with it. */
    es.addEventListener('replay', (e) => {
      try {
        window.feedReplay(JSON.parse(e.data));
      } catch (err) {
        window.feedDisconnected('a stored record could not be read: ' + err.message);
      }
    });

    es.addEventListener('replay-done', (e) => window.feedReplayDone(Number(e.data) || 0));

    es.onerror = function () {
      delete window.sendToMesh;
      delete window.recordAction;
      /* If the stream died part way through the history, the console is still
         in rebuild mode, which suppresses the alarm tone. Close it out here so
         a distress call arriving after a reconnect is audible. */
      if (window.feedReplayDone) window.feedReplayDone(0);
      /* EventSource retries on its own. Only the first failure is announced,
         otherwise a bridge that is simply not running would fill the event log
         and bury a real distress call. The watchdog is what escalates this into
         a visible loss of feed. */
      if (!announcedDown) {
        announcedDown = true;
        window.feedDisconnected('cannot reach the bridge at ' + BRIDGE_LABEL);
      }
    };
  }

  /* Reports back which way the send went, rather than logging a failure into
     the event log and leaving the console showing the incident as handled. The
     console keeps the incident open until onSent fires, so this callback is the
     only thing that closes an alarm. */
  function sendToMesh(frame, outcome) {
    const o = outcome || {};
    const sent = o.onSent || function () {};
    const failed = o.onFailed || function () {};

    fetch(BRIDGE + '/send', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: frame
    }).then(function (r) {
      return r.json().catch(function () { return { ok: r.ok, detail: 'no detail given' }; });
    }).then(function (result) {
      if (result.ok) { sent(); return; }
      failed('the bridge could not transmit it: ' + result.detail);
    }).catch(function (err) {
      failed('it did not reach the bridge: ' + err.message);
    });
  }

  /* An operator decision that leaves no trace on the radio, so it has to be
     recorded deliberately or it is forgotten on restart. */
  function recordAction(kind, subject) {
    fetch(BRIDGE + '/action', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ kind: kind, subject: subject })
    }).then(function (r) {
      if (!r.ok) window.feedDisconnected('the bridge did not store that action, it will alarm again on restart');
    }).catch(function (err) {
      window.feedDisconnected('that action was not stored: ' + err.message);
    });
  }

  connect();
})();
