#pragma once

#include <NimBLEDevice.h>
#include <string>

#include "../BluetoothBikeSensorServer.h"

struct BLEAdvertisingManagerContext {
  NimBLEAdvertising** advertising;
  std::string* advertisingName;
  uint8_t* enabledSensorTypes;
};

void BLEAdvertisingManagerSetContext(const BLEAdvertisingManagerContext& context);

void BLEAdvertisingManagerSetupInitialAdvertising(const NimBLEUUID& hrServiceUuid,
                                                  const NimBLEUUID& cscServiceUuid,
                                                  const NimBLEUUID& powerServiceUuid);

void BLEAdvertisingManagerUpdateServices(bool restart,
                                         const NimBLEUUID& hrServiceUuid,
                                         const NimBLEUUID& cscServiceUuid,
                                         const NimBLEUUID& powerServiceUuid);

void BLEAdvertisingManagerUpdatePayload(bool restart,
                                        const NimBLEUUID& hrServiceUuid,
                                        const NimBLEUUID& cscServiceUuid,
                                        const NimBLEUUID& powerServiceUuid);

void BLEAdvertisingManagerRefreshName(bool restart,
                                       const NimBLEUUID& hrServiceUuid,
                                       const NimBLEUUID& cscServiceUuid,
                                       const NimBLEUUID& powerServiceUuid);
