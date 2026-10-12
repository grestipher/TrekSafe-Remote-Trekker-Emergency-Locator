// Stub. See firmware/test/stubs/README.md.
#pragma once
#include "configuration.h"

typedef uint32_t NodeNum;
#define NODENUM_BROADCAST 0xFFFFFFFFu

enum meshtastic_PortNum { meshtastic_PortNum_TEXT_MESSAGE_APP = 1 };

enum meshtastic_MeshPacket_Priority {
    meshtastic_MeshPacket_Priority_DEFAULT = 0,
    meshtastic_MeshPacket_Priority_HIGH = 70,
};

enum { meshtastic_MeshPacket_decoded_tag = 1 };

struct meshtastic_Data {
    meshtastic_PortNum portnum;
    struct { size_t size; uint8_t bytes[256]; } payload;
};

struct meshtastic_MeshPacket {
    NodeNum to;
    uint8_t channel;
    bool want_ack;
    int which_payload_variant;
    meshtastic_MeshPacket_Priority priority;
    meshtastic_Data decoded;
};

enum ProcessMessage { CONTINUE, STOP };
