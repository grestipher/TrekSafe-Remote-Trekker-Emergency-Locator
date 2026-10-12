// Stub. Upstream exposes the global as a pointer, which is why the module
// writes service->sendToMesh rather than service.sendToMesh.
#pragma once
#include "mesh-types.h"

enum RxSource { RX_SRC_LOCAL, RX_SRC_RADIO };

class MeshService
{
  public:
    void sendToMesh(meshtastic_MeshPacket *p, RxSource src = RX_SRC_LOCAL, bool ccToPhone = false);
};
extern MeshService *service;
