// Stub. See firmware/test/stubs/README.md.
#pragma once
#include "configuration.h"

namespace meshtastic {
class GPSStatus
{
  public:
    bool getHasLock() const;
    int32_t getLatitude() const;   // 1e-7 degrees, as upstream
    int32_t getLongitude() const;
};
}
extern meshtastic::GPSStatus *gpsStatus;
