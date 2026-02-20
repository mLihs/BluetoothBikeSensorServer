/**
 * @file BBSAdvertisingIdentity.h
 * @brief BluetoothBikeSensorServer Advertising Identity helpers
 *
 * Builds Homewind-style "beacon" service-data payload and configures advertising
 * based on a sensorMask/state/fw version snapshot.
 *
 * This module depends on NimBLE types and is separated from core identity logic.
 */
#pragma once

#include <Arduino.h>
#include <stdint.h>

// Forward declaration
class NimBLEAdvertising;

namespace bbs {
namespace adv_identity {

/**
 * @brief Structure to hold advertising snapshot for change detection
 */
struct AdvSnapshot {
  uint8_t sensorMask;
  uint8_t state;
  uint8_t fwMajor;
  uint8_t fwMinor;
  uint16_t instanceId;
  bool advertiseZwiftLegacyUuid;

  AdvSnapshot()
      : sensorMask(0),
        state(0),
        fwMajor(0),
        fwMinor(0),
        instanceId(0),
        advertiseZwiftLegacyUuid(false) {}

  bool operator==(const AdvSnapshot& other) const {
    return sensorMask == other.sensorMask && state == other.state &&
           fwMajor == other.fwMajor && fwMinor == other.fwMinor &&
           instanceId == other.instanceId &&
           advertiseZwiftLegacyUuid == other.advertiseZwiftLegacyUuid;
  }

  bool operator!=(const AdvSnapshot& other) const { return !(*this == other); }
};

/**
 * @brief Build the 8-byte Beacon Service Data payload
 *
 * Format:
 * [0] = 0x01 (protocol version)
 * [1] = 0x01 (device type: SensorHub)
 * [2] = sensorMask (HR=0x01, CSC=0x02, PWR=0x04, ZWIFT_CTRL=0x08)
 * [3] = state
 * [4] = fwMajor
 * [5] = fwMinor
 * [6] = instanceId LSB
 * [7] = instanceId MSB
 */
void BuildBeaconServiceData(uint8_t out8[8], uint8_t sensorMask, uint8_t state,
                            uint8_t fwMajor, uint8_t fwMinor, uint16_t instanceId);

/**
 * @brief Configure BLE advertising with identity.
 *
 * Does NOT start advertising. Caller controls start/stop.
 */
void ConfigureAdvertising(NimBLEAdvertising* adv, uint8_t sensorMask, uint8_t state,
                          uint8_t fwMajor, uint8_t fwMinor,
                          bool advertiseZwiftLegacyUuidToo);

/**
 * @brief Update advertising only if configuration changed (snapshot + debounce)
 *
 * @return true if advertising was rebuilt
 */
bool UpdateAdvertisingIfChanged(NimBLEAdvertising* adv, AdvSnapshot& snapshot,
                                uint8_t sensorMask, uint8_t state,
                                uint8_t fwMajor, uint8_t fwMinor,
                                bool advertiseZwiftLegacyUuidToo);

}  // namespace adv_identity
}  // namespace bbs

