// Stub mirroring the upstream class the module derives from.
#pragma once
#include "mesh-types.h"

class SinglePortModule
{
  public:
    SinglePortModule(const char *name, meshtastic_PortNum port) : name(name), ourPortNum(port) {}
    virtual ~SinglePortModule() {}

  protected:
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) { (void)mp; return ProcessMessage::CONTINUE; }
    meshtastic_MeshPacket *allocDataPacket();

    const char *name;
    meshtastic_PortNum ourPortNum;
};
