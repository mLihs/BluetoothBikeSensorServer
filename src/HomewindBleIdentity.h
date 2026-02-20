/**
 * @file HomewindBleIdentity.h
 * @brief Compatibility wrapper for legacy Homewind identity API
 *
 * The library has been modularized into:
 * - `modules/BBSIdentity.*` (core identity: prefix / instanceId / deviceName)
 * - `modules/BBSAdvertisingIdentity.*` (optional NimBLE advertising identity helpers)
 *
 * This header keeps the historical `homewind::` namespace and function names
 * to avoid breaking external code that might include `HomewindBleIdentity.h`.
 */

#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "modules/BBSIdentity.h"
#include "modules/BBSAdvertisingIdentity.h"

// Forward declaration
class NimBLEAdvertising;

namespace homewind {

// Type aliases
using AdvSnapshot = bbs::adv_identity::AdvSnapshot;

// Function aliases (keeps legacy names / namespace)
inline void SetDeviceNamePrefix(const char* prefix) { bbs::identity::SetDeviceNamePrefix(prefix); }
inline uint16_t GetInstanceId() { return bbs::identity::GetInstanceId(); }
inline const String& GetBleDeviceNameLower() { return bbs::identity::GetBleDeviceNameLower(); }
inline const String& GetMdnsHostnameLower() { return bbs::identity::GetMdnsHostnameLower(); }

inline void BuildBeaconServiceData(uint8_t out8[8], uint8_t sensorMask, uint8_t state,
                                   uint8_t fwMajor, uint8_t fwMinor, uint16_t instanceId) {
  bbs::adv_identity::BuildBeaconServiceData(out8, sensorMask, state, fwMajor, fwMinor, instanceId);
}

inline void ConfigureAdvertising(NimBLEAdvertising* adv, uint8_t sensorMask, uint8_t state,
                                 uint8_t fwMajor, uint8_t fwMinor, bool advertiseZwiftLegacyUuidToo) {
  bbs::adv_identity::ConfigureAdvertising(adv, sensorMask, state, fwMajor, fwMinor, advertiseZwiftLegacyUuidToo);
}

inline bool UpdateAdvertisingIfChanged(NimBLEAdvertising* adv, AdvSnapshot& snapshot,
                                       uint8_t sensorMask, uint8_t state,
                                       uint8_t fwMajor, uint8_t fwMinor,
                                       bool advertiseZwiftLegacyUuidToo) {
  return bbs::adv_identity::UpdateAdvertisingIfChanged(adv, snapshot, sensorMask, state, fwMajor, fwMinor,
                                                       advertiseZwiftLegacyUuidToo);
}

}  // namespace homewind

