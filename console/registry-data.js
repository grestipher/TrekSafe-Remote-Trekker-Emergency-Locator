/* Beacon registration records, issue I9.
 *
 * This is what the trailhead desk fills in when it hands a beacon to a visitor.
 * It never goes on the air. The mesh carries the device id and nothing else,
 * and the acknowledgement frame carries the device id only. Identity stays on
 * this machine.
 *
 * The records below are sample data so the console can be demonstrated. Replace
 * them. Where this file eventually lives, and whether it stays a file at all,
 * is decision D6.
 *
 * devid     four uppercase hex characters, printed on the beacon
 * name      whatever the desk records, initials are enough
 * party     how many people are with that beacon
 * dueBack   ISO 8601, when they said they would be back
 * notes     anything a warden should know before responding
 *
 * A real record is written out in full, like this:
 *
 *   { devid: 'A4F2', name: 'S. Chen', party: 2,
 *     dueBack: '2026-08-31T18:00:00Z', notes: 'Skyline trail, overnight' }
 *
 * Always write the offset, the trailing Z or a +HH:MM. A return time with no
 * offset is read as this machine's local time, and a bare date with no time at
 * all is read as midnight UTC, so the same text means two different instants
 * depending on how much of it was typed. The console says so in the event log
 * when it cannot read one of these at all, but it cannot warn about a date it
 * can read and you did not mean.
 *
 * The sample dates below are worked out from the clock rather than written down,
 * which is a thing only sample data should do. It keeps the demonstration
 * showing what it is meant to show on whatever day it is run: two parties still
 * out within their time, and one who said they would be back this morning and
 * was not. Replace all of this with real records and real dates.
 */
(function () {
  const hoursFromNow = (h) => new Date(Date.now() + h * 3600 * 1000).toISOString();

  window.TREKSAFE_REGISTRY = [
    { devid: 'A4F2', name: 'S. Chen', party: 2, dueBack: hoursFromNow(9),
      notes: 'Skyline trail, overnight' },
    { devid: 'B117', name: 'J. Okafor', party: 1, dueBack: hoursFromNow(-5),
      notes: 'Solo, carries satellite phone' },
    { devid: 'C0DE', name: 'M. Tremblay', party: 4, dueBack: hoursFromNow(28),
      notes: 'Guided group, two under 12' }
  ];
})();
