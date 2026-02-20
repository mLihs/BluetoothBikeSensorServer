/**
 * @file BBSAdvertisingIdentity.cpp
 * @brief Implementation of BluetoothBikeSensorServer Advertising Identity helpers
 */

#include "BBSAdvertisingIdentity.h"

#include "BBSIdentity.h"

#include <NimBLEAdvertising.h>
#include <NimBLEAdvertisementData.h>
#include <NimBLEUUID.h>
#include <string>

namespace bbs {
namespace adv_identity {

// Beacon Service UUID (128-bit UUID v4 for Homewind - randomly generated, frozen)
static const NimBLEUUID BEACON_SERVICE_UUID("8f3a7c92-4b2d-4a8e-9f1e-6d5c4b3a2e1f");

// Legacy ZWIFT_CTRL UUID (128-bit, optional)
static const NimBLEUUID ZWIFT_CTRL_LEGACY_UUID("FC820000-0000-0000-0000-000000000000");

// Standard service UUIDs (16-bit)
static const uint16_t HR_SERVICE_UUID_16 = 0x180D;
static const uint16_t CSC_SERVICE_UUID_16 = 0x1816;
static const uint16_t PWR_SERVICE_UUID_16 = 0x1818;
static const uint16_t ZWIFT_CTRL_SERVICE_UUID_16 = 0xFC82;

// Debouncing for advertising rebuilds (min 300ms between rebuilds)
static unsigned long lastAdvertisingRebuildMs = 0;
static const unsigned long ADVERTISING_REBUILD_DEBOUNCE_MS = 300;

void BuildBeaconServiceData(uint8_t out8[8], uint8_t sensorMask, uint8_t state,
                            uint8_t fwMajor, uint8_t fwMinor, uint16_t instanceId) {
  out8[0] = 0x01;
  out8[1] = 0x01;
  out8[2] = sensorMask;
  out8[3] = state;
  out8[4] = fwMajor;
  out8[5] = fwMinor;
  out8[6] = instanceId & 0xFF;
  out8[7] = (instanceId >> 8) & 0xFF;
}

void ConfigureAdvertising(NimBLEAdvertising* adv, uint8_t sensorMask, uint8_t state,
                          uint8_t fwMajor, uint8_t fwMinor,
                          bool advertiseZwiftLegacyUuidToo) {
  if (!adv) {
    return;
  }

  const String& deviceName = bbs::identity::GetBleDeviceNameLower();

  NimBLEAdvertisementData advData;
  advData.setName(deviceName.c_str());

  // Build and add Beacon Service Data (ALWAYS advertised)
  const uint16_t instanceId = bbs::identity::GetInstanceId();
  uint8_t beaconData[8];
  BuildBeaconServiceData(beaconData, sensorMask, state, fwMajor, fwMinor, instanceId);

  // Reuse buffer to reduce reallocations
  static std::string beaconBuffer;
  beaconBuffer.assign(reinterpret_cast<const char*>(beaconData), 8);
  advData.setServiceData(BEACON_SERVICE_UUID, beaconBuffer);

  // Add dynamic services based on sensorMask
  if (sensorMask & 0x01) advData.addServiceUUID(NimBLEUUID(HR_SERVICE_UUID_16));
  if (sensorMask & 0x02) advData.addServiceUUID(NimBLEUUID(CSC_SERVICE_UUID_16));
  if (sensorMask & 0x04) advData.addServiceUUID(NimBLEUUID(PWR_SERVICE_UUID_16));
  if (sensorMask & 0x08) {
    advData.addServiceUUID(NimBLEUUID(ZWIFT_CTRL_SERVICE_UUID_16));
    if (advertiseZwiftLegacyUuidToo) {
      advData.addServiceUUID(ZWIFT_CTRL_LEGACY_UUID);
    }
  }

  adv->setAdvertisementData(advData);

  // Clear scan response
  NimBLEAdvertisementData scanRespData;
  scanRespData.setName("");
  adv->setScanResponseData(scanRespData);
}

bool UpdateAdvertisingIfChanged(NimBLEAdvertising* adv, AdvSnapshot& snapshot,
                                uint8_t sensorMask, uint8_t state,
                                uint8_t fwMajor, uint8_t fwMinor,
                                bool advertiseZwiftLegacyUuidToo) {
  if (!adv) {
    return false;
  }

  AdvSnapshot newSnapshot;
  newSnapshot.sensorMask = sensorMask;
  newSnapshot.state = state;
  newSnapshot.fwMajor = fwMajor;
  newSnapshot.fwMinor = fwMinor;
  newSnapshot.instanceId = bbs::identity::GetInstanceId();
  newSnapshot.advertiseZwiftLegacyUuid = advertiseZwiftLegacyUuidToo;

  if (newSnapshot == snapshot) {
    return false;
  }

  const unsigned long now = millis();
  if (now - lastAdvertisingRebuildMs < ADVERTISING_REBUILD_DEBOUNCE_MS) {
    return false;
  }
  lastAdvertisingRebuildMs = now;

  const bool wasAdvertising = adv->isAdvertising();
  if (wasAdvertising) {
    adv->stop();
    yield();
  }

  ConfigureAdvertising(adv, sensorMask, state, fwMajor, fwMinor, advertiseZwiftLegacyUuidToo);
  snapshot = newSnapshot;

  if (wasAdvertising) {
    adv->start(0);
    yield();
  }

  return true;
}

}  // namespace adv_identity
}  // namespace bbs

