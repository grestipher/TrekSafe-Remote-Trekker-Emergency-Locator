// TrekSafe payload contract v1, frame construction and acknowledgement parsing.
//
// This is the payload: what a distress call says. It knows nothing about how
// that payload reaches anyone, which is the link layer's job in link.h and the
// transport's in transport.h. The split matters, because the payload survived
// the radio stack underneath it being replaced entirely, and these frames did
// not change by a single byte when it was. See firmware/archive-meshtastic.
//
// This file deliberately includes nothing from Arduino and nothing from any
// radio library. It is plain C++ so it can be compiled and tested on a
// development machine, which matters because this is the one piece of firmware
// where a bug means a real distress call is malformed or an acknowledgement is
// missed. See firmware/test/test_contract.cpp, which runs without a board.
//
// Contract: docs/payload-contract.md, version 1, ratified August 2026.
//
//   SOS|1|<devid>|<lat>|<lon>|<acc_m>|<epoch>|<cat>|<batt>
//   OK |1|<devid>|<lat>|<lon>|<acc_m>|<epoch>
//   TEL|1|<devid>|<batt>|<volts>|<snr>
//   ACK|1|<devid>|<incident_id>        base station to beacon

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace treksafe {

// Schema version. Field 2 of every frame. See the contract on why it sits
// this early: an unknown version can be spotted before anything after it is
// trusted.
static const int kSchema = 1;

// Longest frame this contract can produce, plus room for a terminator. An SOS
// is roughly 45 bytes, which is what keeps airtime low on Long Fast.
static const size_t kFrameMax = 96;

// A device id is 2 to 8 uppercase hex characters. Four is what the beacon
// uses, taken from the low half of the board's hardware serial number. See
// deviceIdFromSerial.
static const size_t kDevIdMax = 9;

// Sentinels for values the beacon does not have. Each one is emitted as an
// empty field, which the console already reads as unknown.
static const int kUnknownInt = -1;
static const uint32_t kClockNotSet = 0;

enum Category {
    CAT_MEDICAL = 'M',
    CAT_INJURY = 'I',
    CAT_LOST = 'L',
    CAT_UNSPECIFIED = 'U',
};

// One distress reading. Nothing here is required to be present. A beacon that
// has no GPS lock, no clock and no battery reading still sends a valid frame,
// because a distress call with nothing but an identifier is still a distress
// call.
struct Distress {
    const char *devid;   // null or empty means the beacon could not identify itself
    bool hasFix;         // false emits 0|0, which the contract defines as no fix yet
    double lat;
    double lon;
    int accM;            // kUnknownInt emits an empty field
    uint32_t epoch;      // kClockNotSet emits an empty field
    char cat;            // anything unrecognised is emitted as U
    int battPct;         // kUnknownInt emits an empty field
};

// A parsed acknowledgement from the base station.
struct Ack {
    char devid[kDevIdMax];
    char incident[24];
    bool hasIncident;    // an acknowledgement without an incident id is still valid
};

// Derives the four character device id printed on a beacon from the board's
// hardware serial number, which on an ESP32 is the low half of the factory
// programmed MAC address. out must hold at least kDevIdMax bytes.
//
// Derived rather than provisioned on purpose: it is stable for the life of the
// board, survives a reflash, and needs no separate step at the trailhead where
// somebody would eventually flash two beacons with the same id. The cost is
// that two boards in a fleet could collide on the low sixteen bits, which is
// roughly one chance in 65536 per pair. The registry at the base station is
// where that is caught, because it is the only place that knows the fleet.
void deviceIdFromSerial(uint32_t serial, char *out);

// True if s is 2 to 8 uppercase hex characters, which is what the contract
// requires of a device id.
bool isValidDevId(const char *s);

// Frame builders. Each writes a null terminated frame into out and returns its
// length, or 0 if out is too small. None of them can fail on bad input: a
// value that cannot be represented is emitted as an empty field rather than
// stopping the frame from being sent.
size_t buildSos(char *out, size_t cap, const Distress &d);
size_t buildOk(char *out, size_t cap, const Distress &d);
size_t buildTel(char *out, size_t cap, const char *devid, int battPct, double volts, double snr);

// Parses an acknowledgement addressed to ourDevId. Returns false if the line
// is not an acknowledgement for us.
//
// This parser is deliberately more forgiving than the console's. A malformed
// acknowledgement that is thrown away leaves a person believing nobody heard
// them, so anything that plainly says ACK, speaks our schema and names our
// device is accepted, including one with the incident id missing.
bool parseAck(const char *line, const char *ourDevId, Ack &out);


} // namespace treksafe
