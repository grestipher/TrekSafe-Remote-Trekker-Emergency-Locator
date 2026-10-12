// Every tunable in the firmware, in one file.
//
// They are gathered here rather than scattered through the roles so that the
// question "what would I have to change to alter the behaviour of a deployed
// beacon" has one answer. Each one can be overridden at build time from
// platformio.ini with a -D flag, so a test build and a field build differ by
// their flags and not by their source.
//
// Anything below that is a judgement rather than a measurement says so. Most of
// them are judgements. None of them have been checked against a radio in a
// forest, and until they have, every number here is a proposal.

#pragma once

// ---------------------------------------------------------------------------
// Beacon
// ---------------------------------------------------------------------------

// How long the button must be held before a distress call goes out.
//
// A beacon that fires on a knock in a backpack sends wardens up a mountain for
// nothing. A beacon that needs a deliberate gesture is still usable by someone
// cold and frightened, as long as the gesture is one motion. Three seconds is
// the compromise, and it is a judgement.
#ifndef TREKSAFE_HOLD_MS
#define TREKSAFE_HOLD_MS 3000
#endif

// When an unacknowledged distress call is repeated, in milliseconds since the
// last one went out. The schedule widens and then holds at its last entry; it
// never stops on its own.
//
// PROPOSED, NOT RATIFIED. docs/payload-contract.md carries this as an open
// question. The reasoning is that a single distress call that does not reach a
// gateway is lost forever and the person has no way to know, so it repeats; and
// that the first minutes are when a repeat is most likely to find a gateway
// that a moving relay or a changed antenna angle has just brought into range,
// so the early intervals are short. The widening is the concession to airtime.
#ifndef TREKSAFE_SOS_REPEAT_MS
#define TREKSAFE_SOS_REPEAT_MS                                                                     \
    {                                                                                              \
        30000, 60000, 120000, 300000                                                               \
    }
#endif

// Once acknowledged, the beacon keeps the base station's picture of where the
// person is fresh, without raising anything. Ten minutes.
#ifndef TREKSAFE_CHECKIN_MS
#define TREKSAFE_CHECKIN_MS 600000
#endif

// Test affordance for a board with no button wired up, and for the simulator.
// When set, the beacon raises one distress call this many milliseconds after
// boot and then behaves exactly as it would after a real press.
//
// Zero, the default, means never. A beacon in the field fires when a person
// holds the button and at no other time, and a build with this set is not a
// beacon, which is why it announces itself in the log when it fires.
#ifndef TREKSAFE_TRIGGER_AFTER_MS
#define TREKSAFE_TRIGGER_AFTER_MS 0
#endif

// ---------------------------------------------------------------------------
// Repeater
// ---------------------------------------------------------------------------

// How often a fixed repeater reports its own health, so a dead one is known
// before it matters rather than during an incident. Fifteen minutes.
#ifndef TREKSAFE_TELEMETRY_MS
#define TREKSAFE_TELEMETRY_MS 900000
#endif

// How long a repeater waits before rebroadcasting something it has decided to
// relay.
//
// Not politeness. Every repeater that hears a frame decides to relay it at the
// same instant, so without a spread they all transmit together and collide, and
// the frame that reaches nobody is a distress call. The actual delay is a
// random value below this, drawn per frame. A hundred milliseconds is several
// times the airtime of one of these frames on a slow spreading factor.
#ifndef TREKSAFE_RELAY_JITTER_MS
#define TREKSAFE_RELAY_JITTER_MS 100
#endif

// ---------------------------------------------------------------------------
// Gateway
// ---------------------------------------------------------------------------

// How often the gateway tells the host it is alive, whether or not the mesh has
// said anything.
//
// This is what becomes the console's gateway heartbeat, and the distinction it
// carries is the point: a quiet mesh at three in the morning is normal, and a
// gateway that has stopped listening is not. Only a signal that is independent
// of traffic can tell those apart. Must stay comfortably under the console's
// 90 second warning threshold.
#ifndef TREKSAFE_GATEWAY_BEAT_MS
#define TREKSAFE_GATEWAY_BEAT_MS 15000
#endif
