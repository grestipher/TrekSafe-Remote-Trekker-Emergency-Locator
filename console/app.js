/* Base station console, presentation layer.
 *
 * Everything that decides anything lives in lib/ and is tested by node:test.
 * This file draws it and wires the buttons. Keep logic out of it.
 *
 * Two rules shape the code here rather than the styling:
 *   Nothing that came off the air is ever assigned as HTML. Packet text goes in
 *   through textContent, always.
 *   The screen redraws when state changes, not on a timer. A timer redraw was
 *   swallowing acknowledgement clicks. See issue I5.
 */
(function () {
  'use strict';

  const F = window.TrekSafeFrames;
  const { IncidentStore } = window.TrekSafeIncidents;
  const { NodeHealth } = window.TrekSafeNodes;
  const { FeedWatchdog } = window.TrekSafeWatchdog;
  const { BeaconRegistry } = window.TrekSafeRegistry;
  const { CoverageLog } = window.TrekSafeCoverage;
  const { OverdueWatch } = window.TrekSafeOverdue;

  /* How often the console sounds again while anything is unacknowledged. The
     failure mode this defends against is a warden away from the desk, so once
     at arrival is not enough. Issue I4. */
  const ALARM_REPEAT_MS = 20000;

  /* How long a rebuild from the bridge may run before the console gives up on
     it and goes live anyway. Generous: twelve hours of frames is a lot of
     records. See the tick. */
  const REPLAY_TIMEOUT_MS = 30000;

  const state = {
    store: new IncidentStore(),
    health: new NodeHealth(),
    watchdog: new FeedWatchdog(),
    registry: new BeaconRegistry(window.TREKSAFE_REGISTRY || []),
    coverage: new CoverageLog(),
    overdue: new OverdueWatch(),
    overdueList: [],
    overdueSignature: null,
    badDatesAnnounced: false,
    selected: null,
    packets: 0,
    rejected: 0,
    lastPacket: null,
    replaying: false,
    replayStartedAt: null,
    /* Which stored records have already been rebuilt from, so a reconnection
       replays without counting everything twice.

       A map rather than a set, holding when each record was stored, so it can
       be pruned. The bridge only ever replays a fixed window, so a record older
       than that window will never be offered again and remembering it forever
       would be a slow leak on a console that runs all season. */
    replayedIds: new Map(),
    replayApplied: 0,
    nodeSignature: '',
    audio: { ctx: null, armed: false, failure: null, lastTone: 0 }
  };

  const $ = (id) => document.getElementById(id);
  const now = () => Date.now();

  /* ---------- small DOM helpers, used instead of innerHTML ---------- */

  function el(tag, cls, text) {
    const n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text != null) n.textContent = text;
    return n;
  }

  function clear(node) { while (node.firstChild) node.removeChild(node.firstChild); }

  function relative(ts) {
    const s = Math.floor((now() - ts) / 1000);
    if (s < 5) return 'now';
    if (s < 60) return s + 's ago';
    if (s < 3600) return Math.floor(s / 60) + 'm ago';
    return Math.floor(s / 3600) + 'h ago';
  }

  /* Elements carrying data-ts have their text refreshed by the clock tick, so
     relative times stay live without rebuilding the screen. */
  function agoSpan(cls, ts) {
    const n = el('span', cls, relative(ts));
    n.dataset.ts = String(ts);
    return n;
  }

  function clockTime(ts) { return new Date(ts).toLocaleTimeString(); }
  function fullTime(ts) { return new Date(ts).toLocaleString(); }

  /* ---------- event log ---------- */

  function log(message, kind, at) {
    const line = el('div', kind || '');
    // During a rebuild this is the time the frame was actually heard. A
    // distress call from three hours ago must not appear in the log stamped
    // with the moment the console happened to restart.
    line.appendChild(el('span', 't', clockTime(at || now())));
    line.appendChild(document.createTextNode(' '));
    // textContent, because this string routinely contains raw packet text.
    line.appendChild(document.createTextNode(message));
    const box = $('log');
    box.insertBefore(line, box.firstChild);
    while (box.children.length > 300) box.removeChild(box.lastChild);
  }

  /* ---------- audible alarm, issue I4 ---------- */

  function tone() {
    const ctx = state.audio.ctx;
    if (!ctx || !state.audio.armed) return;
    const t0 = ctx.currentTime;
    [0, 0.28, 0.56].forEach((off) => {
      const o = ctx.createOscillator(), g = ctx.createGain();
      o.type = 'square';
      o.frequency.value = 880;
      g.gain.setValueAtTime(0.0001, t0 + off);
      g.gain.exponentialRampToValueAtTime(0.18, t0 + off + 0.01);
      g.gain.exponentialRampToValueAtTime(0.0001, t0 + off + 0.20);
      o.connect(g); g.connect(ctx.destination);
      o.start(t0 + off); o.stop(t0 + off + 0.22);
    });
    state.audio.lastTone = now();
  }

  /* Arming is explicit and its result is shown, because the old behaviour was
     to fail silently: a browser blocks audio until a user gesture, so the first
     distress call of a shift arrived in silence and nobody knew. */
  function armAudio() {
    try {
      const Ctor = window.AudioContext || window.webkitAudioContext;
      if (!Ctor) throw new Error('this browser has no audio support');
      state.audio.ctx = state.audio.ctx || new Ctor();
      const done = () => {
        if (state.audio.ctx.state !== 'running') {
          state.audio.armed = false;
          state.audio.failure = 'the browser did not allow audio to start';
          log('Sound could not be armed: ' + state.audio.failure + '. The console is silent, watch the screen.', 'e');
        } else {
          state.audio.armed = true;
          state.audio.failure = null;
          tone();
          log('Sound armed. That tone is what an incident will sound like.', 'g');
        }
        render();
      };
      const p = state.audio.ctx.resume();
      if (p && typeof p.then === 'function') p.then(done, done); else done();
    } catch (e) {
      state.audio.armed = false;
      state.audio.failure = String((e && e.message) || e);
      log('Sound could not be armed: ' + state.audio.failure + '. The console is silent, watch the screen.', 'e');
      render();
    }
  }

  function disarmAudio() {
    state.audio.armed = false;
    state.audio.failure = null;
    log('Sound switched off by the operator. The console is silent.', 'e');
    render();
  }

  /* ---------- ingest ---------- */

  function ingest(line, source, at, link) {
    const when = at || now();
    state.packets++;
    /* Only ever forwards. A rebuild feeds historic records through here with
       the times they were actually heard, which is right for the incidents and
       wrong for this: without the guard, reconnecting to a bridge holding an
       older record would set "last packet" back hours while frames were still
       arriving, and that field is one of the few things telling an operator the
       feed is alive. */
    if (state.lastPacket == null || when > state.lastPacket) state.lastPacket = when;

    const c = F.classify(line);
    if (!c.raw) { state.rejected++; log('rejected (empty line)', 'e', when); render(); return; }

    if (c.distress) { raiseIncident(c, source, when, link); return; }

    const r = F.parseRoutine(c.fields);
    if (!r.ok) {
      state.rejected++;
      log('rejected (' + r.reason + '): ' + c.raw, 'e', when);
      render();
      return;
    }

    const m = r.msg;
    /* A check-in with a position and a signal reading is a measured point on
       the coverage map. Telemetry from a fixed repeater is not: its position is
       wherever it was installed, not where anyone walked. D7. */
    if (m.type === 'OK' && !m.noFix && !simulated(link)) {
      state.coverage.record({ lat: m.lat, lon: m.lon, at: when,
                              snr: link && link.snr, rssi: link && link.rssi,
                              hops: link && link.hops });
    }
    state.health.record(m.devid, {
      kind: m.type === 'TEL' ? 'fixed' : 'beacon',
      batt: m.batt, volts: m.volts, snr: m.snr, now: when
    });

    /* Routine traffic is not logged while rebuilding from stored history, or a
       restart would bury the distress calls under hours of telemetry. Every
       rejection and every distress call is still logged, so nothing that
       matters passes without a stated reason. */
    if (!state.replaying) {
      if (m.type === 'TEL') log('telemetry from repeater ' + m.devid + (m.batt != null ? ', battery ' + m.batt + '%' : ''), null, when);
      else log('check-in from ' + describeBeacon(m.devid), 'g', when);
    }
    render();
  }

  function raiseIncident(c, source, at, link) {
    const when = at || now();
    const h = F.hydrateDistress(c.fields);
    if (h.identified) state.health.record(h.devid, { batt: h.batt, now: when });

    if (!h.noFix && !simulated(link)) {
      state.coverage.record({ lat: h.lat, lon: h.lon, at: when,
                              snr: link && link.snr, rssi: link && link.rssi,
                              hops: link && link.hops });
    }

    const result = state.store.recordDistress(h, { raw: c.raw, source: source, now: when });
    const inc = result.incident;
    // How this one reached us, kept on the incident so the detail panel can
    // say whether it came in on a strong direct link or a marginal relayed one.
    if (link && (link.snr != null || link.hops != null)) {
      inc.link = { snr: link.snr, rssi: link.rssi, hops: link.hops,
                   simulated: simulated(link) };
    }

    /* A new call takes the screen, because the alarm must be unmissable. A
       repeat does not: the beacon repeats every thirty seconds, and a warden who
       has panned the map to work out an approach was having it dragged back
       under them every time. The marker still moves; the view and the selection
       stay where the operator put them. */
    const takeFocus = !result.isRepeat;
    if (takeFocus) state.selected = inc.ref;

    if (result.isRepeat) {
      log('DISTRESS ' + inc.ref + ' repeated by ' + describeBeacon(inc.devid) +
          ' (' + inc.repeats + ' repeat' + (inc.repeats === 1 ? '' : 's') + ')', 'e', when);
      if (inc.ackNotReaching) {
        log('  ' + inc.ref + ' is still calling after being acknowledged, the beacon has not heard us', 'e', when);
      }
    } else {
      log('DISTRESS ' + inc.ref + ' from ' + describeBeacon(inc.devid) + ' (' + F.CATEGORY[inc.cat] + ')', 'e', when);
      inc.degraded.forEach((note) => log('  ' + inc.ref + ' ' + note, 'e', when));
      if (!inc.identified) log('  ' + inc.ref + ' cannot be acknowledged, no device id to reply to', 'e', when);
    }

    /* While rebuilding, nothing is sounded and the map is left alone. The
       alarm is evaluated once at the end, on the state that actually remains
       open, so a restart does not replay hours of sirens. */
    if (!state.replaying) {
      tone();
      render();
      focusIncident(inc, takeFocus);
    }
  }

  /* Whether these readings came out of a simulator rather than off a radio.
   *
   * The source that runs a simulated mesh attaches this note to every frame,
   * and it decides two things. A simulated reading never enters the coverage
   * survey, because a survey built from invented numbers is precisely the false
   * confidence D7 exists to prevent and is worse than having no survey at all.
   * And it is said in words on the incident, so nothing a warden reads as a
   * measurement was made up by a program on this machine.
   *
   * The label has to survive every hop from the simulator to the screen. If it
   * ever stops arriving, invented signal readings start being presented as
   * measurements, and nothing on screen would look any different. */
  function simulated(link) {
    return !!(link && link.source_note === 'simulated');
  }

  function describeBeacon(devid) {
    if (!devid) return 'an unidentified beacon';
    const r = state.registry.lookup(devid);
    return r ? r.name + ' (' + devid + ')' : devid;
  }

  /* ---------- actions ---------- */

  /* The acknowledgement is not treated as sent until something outside this
     machine says it went out. Until then the incident stays open and the alarm
     stays up, because a beacon that has not heard us is a person who still
     believes nobody knows. D3. */
  function acknowledge(ref) {
    const r = state.store.acknowledge(ref, now());
    if (!r.ok) { log('not acknowledged: ' + r.reason, 'e'); render(); return; }

    if (!window.sendToMesh) {
      /* No bridge means no radio, so this cannot even be attempted. Said now
         rather than left to time out, and the incident stays open. */
      ackFailed(ref, 'there is no live feed, so it could not leave this machine');
      return;
    }

    log('sending -> ' + r.frame, null);
    render();
    window.sendToMesh(r.frame, {
      onSent: () => ackConfirmed(ref),
      onFailed: (reason) => ackFailed(ref, reason)
    });
  }

  function ackConfirmed(ref) {
    if (!state.store.confirmAck(ref, now()).ok) return;
    log(ref + ' acknowledged, the beacon has been told help is coming', 'g');
    render();
  }

  function ackFailed(ref, reason) {
    const r = state.store.failAck(ref, reason);
    if (!r.ok) return;
    log('ACKNOWLEDGEMENT NOT SENT for ' + ref + ': ' + reason +
        '. The beacon has not been told anything. This incident is still open.', 'e');
    render();
  }

  function markSeen(ref) {
    const inc = state.store.find(ref);
    const subject = inc ? inc.raw : null;
    const r = state.store.markSeen(ref, now());
    if (!r.ok) { log('not marked seen: ' + r.reason, 'e'); render(); return; }
    log(ref + ' marked seen by operator, no reply sent', 'g');
    /* Acknowledgement records itself, because it puts a frame on the air and
       the bridge stores that. Seeing an incident sends nothing, so it has to be
       recorded deliberately or a restart would alarm again for something a
       human already dealt with. Decision D6. */
    if (window.recordAction && subject) window.recordAction('seen', subject);
    else log('  not stored, so this will alarm again if the console restarts', 'e');
    render();
  }

  /* ---------- map ---------- */

  let map = null;
  const markers = new Map();

  function initMap() {
    try {
      map = L.map('map', { zoomControl: true, attributionControl: true }).setView([52.87, -118.08], 9);
      L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {
        maxZoom: 18, attribution: '&copy; OpenStreetMap contributors'
      }).addTo(map);
    } catch (e) {
      $('maphint').textContent = 'Map failed to load. Coordinates still appear in the incident detail.';
    }
  }

  /* moveView false draws the marker and leaves the map where the operator put
     it. Used for a repeat, which must update the position without stealing the
     view out from under someone reading it. */
  function focusIncident(inc, moveView) {
    const takeView = moveView !== false;
    if (takeView) $('m-sel').textContent = inc.ref + ' · ' + (inc.identified ? inc.devid : 'unidentified');
    if (!map || inc.noFix) return;
    if (markers.has(inc.ref)) map.removeLayer(markers.get(inc.ref));

    const mk = L.circleMarker([inc.lat, inc.lon], {
      radius: 9, color: '#e2483c', weight: 3, fillColor: '#e2483c', fillOpacity: 0.35
    }).addTo(map);

    // Popup content is built as a node so packet text is never parsed as HTML.
    const box = el('div');
    box.appendChild(el('strong', null, inc.identified ? describeBeacon(inc.devid) : 'Unidentified beacon'));
    box.appendChild(el('div', null, inc.ref + ' · ' + F.CATEGORY[inc.cat]));
    mk.bindPopup(box);

    markers.set(inc.ref, mk);
    if (!takeView) return;
    map.setView([inc.lat, inc.lon], 13);
    mk.openPopup();
  }

  /* ---------- render, called on state change only ---------- */

  function render() {
    const t = now();
    const open = state.store.openCount();
    const feedFailed = state.watchdog.isFailed(t);

    document.body.classList.toggle('alarm', open > 0);
    document.body.classList.toggle('feedfail', feedFailed);

    $('sysstate').textContent = open > 0 ? 'Alarm · ' + open + ' open' : 'Standby';
    $('s-open').textContent = String(open);
    $('s-pkts').textContent = state.rejected > 0
      ? state.packets + ' (' + state.rejected + ' rejected)'
      : String(state.packets);

    const cov = state.health.coverage(t);
    $('s-nodes').textContent = cov.known ? cov.up + ' / ' + cov.total : 'none known';

    $('s-last').textContent = state.lastPacket ? relative(state.lastPacket) : 'never';
    if (state.lastPacket) $('s-last').dataset.ts = String(state.lastPacket);

    renderFeedBanner(t);
    renderSound();
    renderQueue(t);
    renderNodes(t);
    renderDetail();
  }

  function renderFeedBanner(t) {
    const banner = $('feedbanner');
    banner.textContent = state.watchdog.describe(t);
    banner.hidden = state.watchdog.state(t) === 'ok';
    banner.classList.toggle('bad', state.watchdog.isFailed(t));
  }

  function renderSound() {
    const btn = $('sound');
    const label = $('s-sound');
    if (state.audio.armed) {
      btn.textContent = 'Switch sound off';
      label.textContent = 'Armed';
      label.classList.remove('bad');
    } else {
      btn.textContent = 'Arm sound';
      label.textContent = state.audio.failure ? 'BLOCKED' : 'NOT ARMED';
      label.classList.add('bad');
    }
  }

  function incidentLabel(i) {
    if (i.ackNotReaching) return 'Still calling, not heard us';
    if (i.seenNotReaching) return 'Still calling, nobody can reply';
    if (i.ackPending) return 'Sending acknowledgement';
    if (i.ackFailure) return 'Acknowledgement failed';
    if (i.ack) return 'Acknowledged';
    if (i.seen) return 'Seen, no reply possible';
    if (i.identified) return 'Awaiting acknowledgement';
    return 'Unidentified, cannot acknowledge';
  }

  function renderQueue(t) {
    const box = $('queue');
    clear(box);
    $('q-count').textContent = String(state.store.incidents.length);

    if (!state.store.incidents.length) {
      box.appendChild(el('div', 'empty',
        'No incidents. Press Distress in the simulator, or paste a packet, to see what one looks like.'));
      return;
    }

    for (const i of state.store.incidents) {
      const openish = state.store.isOpen(i);
      const row = el('div', 'inc ' + (openish ? 'open' : 'ackd') + (state.selected === i.ref ? ' sel' : ''));
      row.dataset.ref = i.ref;

      const r1 = el('div', 'r1');
      const reg = i.identified ? state.registry.lookup(i.devid) : null;
      r1.appendChild(el('span', 'id', reg ? reg.name : (i.identified ? i.devid : 'UNIDENTIFIED')));
      r1.appendChild(agoSpan('ago', i.lastReceived));
      row.appendChild(r1);

      const parts = [i.ref, F.CATEGORY[i.cat]];
      if (i.identified && reg) parts.push(i.devid);
      parts.push(i.noFix ? 'NO FIX' : i.lat.toFixed(4) + ', ' + i.lon.toFixed(4));
      if (i.repeats > 0) parts.push(i.repeats + ' repeat' + (i.repeats === 1 ? '' : 's'));
      row.appendChild(el('div', 'r2', parts.join(' · ')));

      row.appendChild(el('span', 'tag ' + tagClass(i), incidentLabel(i)));
      row.addEventListener('click', () => {
        state.selected = i.ref;
        render();
        focusIncident(i);
      });
      box.appendChild(row);
    }
  }

  function tagClass(i) {
    if (i.ackNotReaching || i.seenNotReaching) return 'open';
    if (i.ackPending) return 'sending';
    if (i.ack) return 'ackd';
    if (i.seen) return 'seen';
    return 'open';
  }

  function renderNodes(t) {
    const box = $('nodes');
    clear(box);
    const fixed = state.health.fixed();

    if (!fixed.length) {
      box.appendChild(el('div', 'empty',
        'No repeaters heard. Coverage is unknown, not confirmed.'));
      return;
    }

    for (const n of fixed.sort((a, b) => b.last - a.last)) {
      const status = state.health.status(n, t);
      const words = status === 'up' ? 'heard' : status === 'stale' ? 'overdue' : 'not heard';
      const row = el('div', 'node');
      row.appendChild(el('span', 'dot ' + status));
      row.appendChild(el('span', 'nm', n.devid));
      row.appendChild(el('span', 'mt', words));
      row.appendChild(agoSpan('mt', n.last));
      box.appendChild(row);
    }
  }

  function addRow(dl, label, value) {
    dl.appendChild(el('dt', null, label));
    dl.appendChild(el('dd', null, value));
  }

  function renderDetail() {
    const box = $('detail');
    clear(box);
    const i = state.store.find(state.selected);

    if (!i) {
      box.appendChild(el('div', 'empty', 'Select an incident to see its full record.'));
      return;
    }

    const dl = el('dl', 'kv');
    addRow(dl, 'Incident', i.ref);

    const reg = i.identified ? state.registry.lookup(i.devid) : null;
    if (!i.identified) {
      addRow(dl, 'Beacon', 'Unidentified');
    } else if (reg) {
      addRow(dl, 'Beacon', reg.name + ' · ' + i.devid);
      if (reg.party != null) addRow(dl, 'Party', String(reg.party));
      if (reg.dueBack) {
        /* Says PAST RETURN TIME rather than OVERDUE on purpose. This is the
           plain comparison with no grace period, so it goes true the minute a
           party is late, while the Overdue parties panel waits an hour before
           making a claim about anybody. Two different questions, and giving
           them the same word had the console appear to contradict itself. */
        const late = state.registry.isOverdue(i.devid, now());
        addRow(dl, 'Due back', new Date(reg.dueBack).toLocaleString() +
          (late ? ' · PAST RETURN TIME' : ''));
      }
      if (reg.notes) addRow(dl, 'Notes', reg.notes);
    } else {
      addRow(dl, 'Beacon', i.devid + ' · not in the registry');
    }

    addRow(dl, 'Category', F.CATEGORY[i.cat]);
    addRow(dl, 'Position', i.noFix
      ? 'NO FIX ACQUIRED' + (i.noFixReason ? ' · ' + i.noFixReason : '')
      : i.lat.toFixed(5) + ', ' + i.lon.toFixed(5));
    addRow(dl, 'Accuracy', i.acc != null ? i.acc + ' m' : 'unknown');

    // Issue I8: sent and received are different facts and only one of them is
    // ours. A beacon with no clock reports no send time, and that is stated.
    addRow(dl, 'Sent by beacon', i.sentEpoch != null ? fullTime(i.sentEpoch) : 'not reported, beacon clock not set');
    addRow(dl, 'First heard', fullTime(i.firstReceived));
    if (i.repeats > 0) {
      addRow(dl, 'Last heard', fullTime(i.lastReceived));
      addRow(dl, 'Repeats', String(i.repeats));
    }

    addRow(dl, 'Battery', i.batt != null ? i.batt + '%' : 'unknown');
    if (i.link) {
      const bits = [];
      if (i.link.snr != null) bits.push('signal ' + i.link.snr.toFixed(1) + ' dB');
      if (i.link.hops != null) {
        bits.push(i.link.hops === 0 ? 'heard directly'
                                    : 'relayed ' + i.link.hops + ' hop' + (i.link.hops === 1 ? '' : 's'));
      }
      // Said on the row itself, not only under Source. A warden reading a
      // signal strength is reading something they may act on, and a number that
      // came out of a simulator has to carry that with it wherever it appears.
      if (i.link.simulated) bits.push('SIMULATED, not measured');
      if (bits.length) addRow(dl, 'How it reached us', bits.join(', '));
    }
    addRow(dl, 'Source', i.source);
    addRow(dl, 'Raw', i.raw);
    if (i.degraded.length) addRow(dl, 'Incomplete', i.degraded.join('\n'));
    if (i.ack) addRow(dl, 'Acknowledged', clockTime(i.ackAt));
    if (i.seen) addRow(dl, 'Seen', clockTime(i.seenAt) + ', no reply sent');
    if (i.ackFailure) addRow(dl, 'Not sent', i.ackFailure);
    box.appendChild(dl);

    if (i.ackNotReaching) {
      box.appendChild(el('div', 'note bad',
        'This beacon is still calling for help after being acknowledged, so the reply has not reached it. ' +
        'Treat this incident as open and send the acknowledgement again.'));
    } else if (i.seenNotReaching) {
      box.appendChild(el('div', 'note bad',
        'This beacon is still calling for help. Nothing was ever sent back to it, because its device id ' +
        'could not be read, so the person has no way of knowing anybody heard them. Treat this incident as ' +
        'open. Reaching them needs something other than this console.'));
    } else if (i.ackPending) {
      box.appendChild(el('div', 'note',
        'The acknowledgement is on its way to the radio. This incident stays open until the transmission ' +
        'is confirmed, because until then the beacon has not been told anything.'));
    } else if (i.ackFailure) {
      box.appendChild(el('div', 'note bad',
        'The acknowledgement did not go out: ' + i.ackFailure + '. Nothing reached the beacon and the ' +
        'person still believes nobody has heard them. Send it again.'));
    } else if (!i.identified && !i.ack) {
      box.appendChild(el('div', 'note',
        'No acknowledgement possible. The device id in this frame could not be read, so there is no beacon ' +
        'to reply to. Mark the incident as seen once you have looked at it. That clears the alarm and tells ' +
        'nobody that help is coming.'));
    }

    const act = el('div', 'act');
    if (i.ackPending) {
      act.appendChild(disabled('Sending, not confirmed yet'));
    } else if (i.ackNotReaching || i.ackFailure) {
      act.appendChild(button('primary', 'Send acknowledgement again', () => acknowledge(i.ref)));
    } else if (i.ack) {
      act.appendChild(disabled('Acknowledgement sent'));
    } else if (i.identified) {
      act.appendChild(button('primary', 'Acknowledge and notify beacon', () => acknowledge(i.ref)));
    } else if (i.seenNotReaching) {
      act.appendChild(button('primary', 'Mark as seen again', () => markSeen(i.ref)));
    } else if (i.seen) {
      act.appendChild(disabled('Seen, no reply possible'));
    } else {
      act.appendChild(button('primary', 'Mark as seen', () => markSeen(i.ref)));
    }
    box.appendChild(act);
  }

  function button(cls, text, onClick) {
    const b = el('button', cls, text);
    b.addEventListener('click', onClick);
    return b;
  }

  function disabled(text) {
    const b = el('button', 'ghost', text);
    b.disabled = true;
    return b;
  }

  /* ---------- clock tick, touches only time text ---------- */

  function tick() {
    $('s-clock').textContent = clockTime(now());

    document.querySelectorAll('[data-ts]').forEach((n) => {
      n.textContent = relative(Number(n.dataset.ts));
    });

    // The watchdog changes state on its own as time passes, so its banner is
    // the one thing the tick has to re-evaluate.
    const t = now();
    renderFeedBanner(t);
    document.body.classList.toggle('feedfail', state.watchdog.isFailed(t));
    const cov = state.health.coverage(t);
    $('s-nodes').textContent = cov.known ? cov.up + ' / ' + cov.total : 'none known';

    /* A repeater goes stale because time passed, not because a packet arrived,
       and the mesh going quiet is exactly when one has died. Without this the
       panel kept reading "heard" beside a timestamp climbing past five minutes.
       Redrawn only when a status actually changes, so it is not rebuilt every
       second for nothing. */
    const signature = state.health.fixed()
      .map((n) => n.devid + ':' + state.health.status(n, t)).sort().join(',');
    if (signature !== state.nodeSignature) {
      state.nodeSignature = signature;
      renderNodes(t);
    }

    const survey = state.coverage.summary();
    $('cov-count').textContent = survey.points
      ? survey.points + ' point' + (survey.points === 1 ? '' : 's') + ' surveyed'
      : 'no survey data';

    checkOverdue(t);

    /* An acknowledgement nothing ever answered for. A wedged bridge leaves the
       frame in flight and the incident pending forever, which is an alarm that
       never resolves either way. Fail it and say so, so the operator can send
       it again. */
    state.store.expirePendingAcks(t).forEach((i) => {
      log('ACKNOWLEDGEMENT NOT SENT for ' + i.ref + ': ' + i.ackFailure +
          '. The beacon has not been told anything. This incident is still open.', 'e');
      render();
    });

    /* A rebuild that never finished would leave the console silent for the
       rest of the shift, because the tone and the map are suppressed while
       replaying. If the connection dies part way through the history, nothing
       would ever clear the flag. Time it out and carry on: an incomplete
       rebuild that alarms is far better than a complete one that cannot. */
    if (state.replaying && state.replayStartedAt &&
        t - state.replayStartedAt > REPLAY_TIMEOUT_MS) {
      log('The rebuild from the bridge did not finish. Showing what arrived, and ' +
          'the alarm is live again.', 'e');
      window.feedReplayDone(0);
      return;
    }

    if (state.store.openCount() > 0 && state.audio.armed &&
        t - state.audio.lastTone >= ALARM_REPEAT_MS) {
      tone();
    }
  }

  /* ---------- overdue parties ---------- */

  /* The only part of this console that raises something without anybody
     pressing a button. Kept visibly separate from the distress alarm: that
     alarm means a human pressed the button, and that meaning has to stay
     exact. */
  function checkOverdue(t) {
    const list = state.overdue.check(state.registry, state.health, t);
    state.overdue.clearAnnounced(list);

    const fresh = state.overdue.newlyOverdue(list);
    fresh.forEach((p) => log('OVERDUE: ' + state.overdue.describe(p, t), 'e'));

    /* A record with an unreadable return time is a party this watch cannot see
       at all. Said once, at startup, because it is a data problem an operator
       fixes rather than a situation that changes. */
    if (!state.badDatesAnnounced) {
      state.badDatesAnnounced = true;
      state.overdue.unreadable(state.registry).forEach((r) => {
        log('REGISTRY: return time "' + r.dueBack + '" for ' + r.name + ' (' + r.devid +
            ') cannot be read, so this party is not being watched for being overdue. ' +
            'Fix the record in registry-data.js.', 'e');
      });
    }

    const signature = list.map((p) => p.devid).join(',');
    if (signature !== state.overdueSignature || fresh.length) {
      state.overdueSignature = signature;
      state.overdueList = list;
      renderOverdue(t);
    }
  }

  function renderOverdue(t) {
    const box = $('overdue');
    const section = $('overdue-section');
    clear(box);
    section.hidden = state.overdueList.length === 0;
    $('overdue-count').textContent = String(state.overdueList.length);

    for (const p of state.overdueList) {
      const row = el('div', 'od');
      const r1 = el('div', 'r1');
      r1.appendChild(el('span', 'id', p.name));
      r1.appendChild(el('span', 'ago', p.party != null ? 'party of ' + p.party : ''));
      row.appendChild(r1);
      row.appendChild(el('div', 'r2', 'Due ' + new Date(p.dueBack).toLocaleString()));
      /* Through agoSpan, so the clock tick keeps it honest. This panel is
         rebuilt only when the set of overdue parties changes, so a plain string
         here froze at whatever it said when the party first appeared and sat
         there reading "last heard 4 minutes ago" for the rest of the shift. */
      const heard = el('div', 'r2');
      if (p.lastHeard == null) {
        heard.textContent = 'Never heard from';
      } else {
        heard.appendChild(document.createTextNode('Last heard '));
        heard.appendChild(agoSpan('', p.lastHeard));
      }
      row.appendChild(heard);
      if (p.notes) row.appendChild(el('div', 'r2', p.notes));
      box.appendChild(row);
    }
  }

  /* ---------- simulator ---------- */

  const SIM_CENTER = [52.87, -118.08];
  const SIM_IDS = ['A4F2', 'B117', 'C0DE'];
  // Repeater ids, and they have to be valid hex like any other device id. An
  // earlier draft used R001 here, which the parser rightly rejected, and the
  // repeater panel stayed empty with the reason sitting in the log.
  const SIM_REPEATERS = ['FA01', 'FA02', 'FA03'];

  function jitter() {
    return [SIM_CENTER[0] + (Math.random() - 0.5) * 0.35, SIM_CENTER[1] + (Math.random() - 0.5) * 0.55];
  }

  function pick(list) { return list[Math.floor(Math.random() * list.length)]; }

  function wire() {
    $('sim-sos').addEventListener('click', () => {
      const [la, lo] = jitter();
      const cat = pick(['M', 'I', 'L', 'U']);
      ingest('SOS|1|' + pick(SIM_IDS) + '|' + la.toFixed(4) + '|' + lo.toFixed(4) + '|' +
        (5 + Math.floor(Math.random() * 25)) + '|' + Math.floor(Date.now() / 1000) + '|' + cat + '|' +
        (40 + Math.floor(Math.random() * 60)), 'simulator');
    });

    $('sim-ok').addEventListener('click', () => {
      const [la, lo] = jitter();
      ingest('OK|1|' + pick(SIM_IDS) + '|' + la.toFixed(4) + '|' + lo.toFixed(4) + '|12|' +
        Math.floor(Date.now() / 1000), 'simulator');
    });

    $('sim-tel').addEventListener('click', () => {
      const id = pick(SIM_REPEATERS);
      ingest('TEL|1|' + id + '|' + (30 + Math.floor(Math.random() * 70)) + '|4.0|' +
        (Math.random() * 12 - 6).toFixed(1), 'simulator');
    });

    $('raw').addEventListener('keydown', (e) => {
      if (e.key === 'Enter' && e.target.value.trim()) {
        ingest(e.target.value, 'manual');
        e.target.value = '';
      }
    });

    $('sound').addEventListener('click', () => {
      if (state.audio.armed) disarmAudio(); else armAudio();
    });
  }

  /* ---------- hooks the bridge connects to ---------- */

  /* A frame arriving from the mesh.
   *
   * The source name follows the frame onto the incident and is shown in the
   * detail panel, so a frame that crossed a simulated mesh says so there rather
   * than being indistinguishable from one that came off a radio. */
  window.feedPacket = (line, link) =>
    ingest(line, simulated(link) ? 'simulated mesh' : 'mesh', null, link);

  // The gateway saying it is alive. Distinct from traffic on purpose: a quiet
  // mesh at 3am is normal, a dead gateway is not, and only this tells them
  // apart. Issue I3.
  window.feedHeartbeat = () => {
    const first = !state.watchdog.everConnected;
    state.watchdog.heartbeat(now());
    if (first) log('Live feed connected.', 'g');
    render();
  };

  /* The connection to the bridge broke.
   *
   * The watchdog is told, not just the log. Otherwise the banner keeps saying
   * the feed is healthy for the ninety seconds it takes a missing heartbeat to
   * register, while the acknowledgement path is already gone, and an operator
   * pressing Acknowledge is told there is no live feed by a console that says
   * there is one. */
  window.feedDisconnected = (reason) => {
    state.watchdog.disconnected();
    log('Live feed disconnected' + (reason ? ': ' + reason : '') + '.', 'e');
    render();
  };

  /* Rebuilding from what the bridge has stored. Decision D6.
   *
   * The bridge stores frames and operator actions and never incidents, so that
   * this console stays the only thing that reads a frame. One record at a time,
   * in the order it happened, put back through the same parser that handled it
   * live. */
  window.feedReplay = (record) => {
    if (!state.replaying) {
      state.replaying = true;
      state.replayStartedAt = now();
      state.replayApplied = 0;
    }

    /* A record this console has already rebuilt from is skipped.
     *
     * The bridge replays its whole window to every console that attaches, and a
     * console reattaches whenever the link blips. Without this, each replay ran
     * the same distress frames through the parser again: they correlated to the
     * incidents already on screen and were counted as repeats, so a flaky cable
     * read on the queue as a beacon calling over and over because nobody had
     * answered. Measured, not theorised: four reconnections turned one call into
     * three repeats.
     *
     * Identity comes from the store's own row id, so this is exact rather than
     * a guess at which frames look alike. A record with no id is applied, since
     * an older bridge that does not send one must still be able to rebuild a
     * console rather than silently rebuild nothing. */
    if (record && record.id) {
      if (state.replayedIds.has(record.id)) return;
      state.replayedIds.set(record.id, record.at || now());
    }
    state.replayApplied++;

    try {
      if (record.dir === 'in') {
        /* The stored record carries source_note, so a frame that crossed a
           simulated mesh is still known to have done so after a restart. It was
           not stored once, and the console surveyed replayed invented readings
           as measurements. See the column comment in bridge/store.py. */
        ingest(record.frame,
               simulated(record) ? 'simulated mesh, recovered' : 'mesh, recovered',
               record.at, record);
        return;
      }
      if (record.dir === 'out') {
        // The only thing the console sends is an acknowledgement.
        const f = F.classify(record.frame).fields;
        if ((f[0] || '').toUpperCase() === 'ACK') {
          state.store.applyHistoricalAck((f[2] || '').toUpperCase(), record.at);
        }
        return;
      }
      if (record.dir === 'action' && record.kind === 'seen') {
        state.store.applyHistoricalSeen(record.frame, record.at);
        return;
      }
      /* An acknowledgement the radio refused. The bridge stores it as an action
         rather than as an outbound frame, so nothing here rebuilds it into an
         acknowledgement that never happened, and the incident it belongs to
         stays open and alarms. Said out loud, because a warden coming back to a
         restarted console needs to know a reply was attempted and failed. */
      if (record.dir === 'action' && record.kind === 'send-failed') {
        log('an earlier acknowledgement never left this machine: ' + record.frame, 'e', record.at);
      }
    } catch (e) {
      // A single unreadable stored record must not stop the rest being rebuilt.
      log('could not replay a stored record: ' + ((e && e.message) || e), 'e');
    }
  };

  /* Forgets records too old for the bridge to ever replay again.
   *
   * The bridge replays a fixed window, twelve hours by default. Anything older
   * than that cannot come round a second time, so remembering it buys nothing
   * and costs memory on a console left running for a season. The margin is
   * generous because the cost of forgetting too early is one inflated repeat
   * count and the cost of keeping too long is a few bytes. */
  const REPLAY_MEMORY_MS = 48 * 60 * 60 * 1000;

  function forgetOldReplayedIds() {
    const cutoff = now() - REPLAY_MEMORY_MS;
    for (const [id, at] of state.replayedIds) {
      if (at < cutoff) state.replayedIds.delete(id);
    }
  }

  window.feedReplayDone = (count) => {
    if (!state.replaying && !Number(count)) return;
    state.replaying = false;
    state.replayStartedAt = null;

    // How much of this replay was actually new, rather than records this
    // console had already rebuilt from on an earlier connection.
    const applied = state.replayApplied;
    state.replayApplied = 0;
    forgetOldReplayedIds();
    const open = state.store.openCount();

    if (applied > 0) {
      log('Recovered ' + applied + ' stored record' + (applied === 1 ? '' : 's') +
          ' from the bridge: ' + state.store.incidents.length +
          ' incident' + (state.store.incidents.length === 1 ? '' : 's') +
          ', ' + open + ' still open.', open > 0 ? 'e' : 'g');
    }

    /* The alarm is judged once, on what actually remains open, rather than
       during the rebuild. An incident nobody replied to before the restart is
       still someone waiting, so it sounds now.
     *
     * Only when the rebuild actually told us something, though. A console that
     * reconnects has the same window replayed to it again, and every one of
     * those records is one it has already seen. Sounding the alarm and dragging
     * the map back on each of those turned a blinking link into a console that
     * beeped and jumped every few seconds while nothing had happened, which is
     * the fastest way to teach an operator to ignore the alarm. */
    if (open > 0 && applied > 0) {
      const newest = state.store.incidents.find((i) => state.store.isOpen(i));
      if (newest) { state.selected = newest.ref; }
      tone();
    }
    render();
    // The map moves only when the rebuild found something, for the same reason
    // the alarm does. A reconnection must not drag the view out from under an
    // operator who has panned it to plan an approach.
    if (applied > 0) {
      const sel = state.store.find(state.selected);
      if (sel) focusIncident(sel);
    }
  };

  // Exposed for tests and for the bridge to inspect. Not used by the UI.
  window.TrekSafeConsole = { state: state, ingest: ingest, render: render };

  initMap();
  wire();
  setInterval(tick, 1000);
  log('Console started.');
  render();
})();
