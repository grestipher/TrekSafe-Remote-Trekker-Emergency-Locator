// Stub. Upstream returns 0 when no valid clock has been set, which is exactly
// the case the payload contract calls "clock not set".
#pragma once
#include "configuration.h"

enum RTCQuality { RTCQualityNone = 0, RTCQualityDevice = 1, RTCQualityGPS = 4 };
uint32_t getValidTime(RTCQuality minQuality, bool local = false);
