// Stub. See firmware/test/stubs/README.md.
#pragma once
#include "configuration.h"

namespace meshtastic {
class PowerStatus
{
  public:
    bool getHasBattery() const;
    int getBatteryChargePercent() const;
    int getBatteryVoltageMv() const;
};
}
extern meshtastic::PowerStatus *powerStatus;
