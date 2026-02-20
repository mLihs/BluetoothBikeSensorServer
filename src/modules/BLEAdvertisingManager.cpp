#include "BLEAdvertisingManager.h"

#include "../BLETelemetry.h"

namespace {
BLEAdvertisingManagerContext g_context = {};

NimBLEAdvertising* getAdvertising() {
  return g_context.advertising ? *g_context.advertising : nullptr;
}

std::string* getAdvertisingName() {
  return g_context.advertisingName;
}

uint8_t getEnabledSensorTypes() {
  return g_context.enabledSensorTypes ? *g_context.enabledSensorTypes : 0;
}
}  // namespace

void BLEAdvertisingManagerSetContext(const BLEAdvertisingManagerContext& context) {
  g_context = context;
}

void BLEAdvertisingManagerSetupInitialAdvertising(const NimBLEUUID& hrServiceUuid,
                                                  const NimBLEUUID& cscServiceUuid,
                                                  const NimBLEUUID& powerServiceUuid) {
  std::string* advertisingName = getAdvertisingName();
  if (!advertisingName) {
    return;
  }

  // Setup NimBLE Advertising (standard API)
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  if (g_context.advertising) {
    *g_context.advertising = advertising;
  }

  // Stop advertising if it's already running (to clear any previous data)
  if (advertising->isAdvertising()) {
    advertising->stop();
    yield();  // Non-blocking: allow other tasks to run
  }

  // Build fresh advertising data with only enabled services
  // Using NimBLEAdvertisementData ensures we don't accumulate old service UUIDs
  NimBLEAdvertisementData advData;
  advData.setName(advertisingName->c_str());

  uint8_t enabledSensorTypes = getEnabledSensorTypes();
  // Only add service UUIDs for enabled sensor types
  if (enabledSensorTypes & SENSOR_TYPE_HEART_RATE) {
    advData.addServiceUUID(hrServiceUuid);
    BLE_LIB_LOGI_F("[SERVER] Adding HR service to advertising");
  }
  if (enabledSensorTypes & SENSOR_TYPE_SPEED_CADENCE) {
    advData.addServiceUUID(cscServiceUuid);
    BLE_LIB_LOGI_F("[SERVER] Adding CSC service to advertising");
  }
  if (enabledSensorTypes & SENSOR_TYPE_POWER_METER) {
    advData.addServiceUUID(powerServiceUuid);
    BLE_LIB_LOGI_F("[SERVER] Adding Power service to advertising");
  }

  // Set the advertising data (this replaces any previous data)
  advertising->setAdvertisementData(advData);

  // Also set scan response data to empty (to ensure no old data persists)
  NimBLEAdvertisementData scanRespData;
  scanRespData.setName(""); // Clear scan response
  advertising->setScanResponseData(scanRespData);

  // Small delay to ensure data is set before starting
  delay(50);

  advertising->start(0); // 0 = advertise forever
  BLE_LIB_LOGI_F("[SERVER] Advertising started - verify with BLE scanner that only HR service is advertised");
}

// Update advertising services based on enabled sensor types
// Note: NimBLE accumulates service UUIDs, so we need to rebuild the payload from scratch
void BLEAdvertisingManagerUpdateServices(bool restart,
                                         const NimBLEUUID& hrServiceUuid,
                                         const NimBLEUUID& cscServiceUuid,
                                         const NimBLEUUID& powerServiceUuid) {
  NimBLEAdvertising* advertising = getAdvertising();
  std::string* advertisingName = getAdvertisingName();
  if (!advertising || !advertisingName) {
    return;
  }

  // Stop advertising if it's running (we need to rebuild the payload)
  bool wasAdvertising = advertising->isAdvertising();
  if (wasAdvertising) {
    advertising->stop();
    BLETelemetry::recordAdvertisingStop();
    delay(100); // Small delay to ensure stop completes
  }

  // Get a fresh advertising object to avoid accumulated service UUIDs
  // NimBLE's getAdvertising() returns a singleton, but we can rebuild the payload
  advertising = NimBLEDevice::getAdvertising();
  if (g_context.advertising) {
    *g_context.advertising = advertising;
  }

  // Build fresh advertising data with only enabled services
  NimBLEAdvertisementData advData;
  advData.setName(advertisingName->c_str());

  uint8_t enabledSensorTypes = getEnabledSensorTypes();
  // Only add service UUIDs for enabled sensor types
  if (enabledSensorTypes & SENSOR_TYPE_HEART_RATE) {
    advData.addServiceUUID(hrServiceUuid);
  }
  if (enabledSensorTypes & SENSOR_TYPE_SPEED_CADENCE) {
    advData.addServiceUUID(cscServiceUuid);
  }
  if (enabledSensorTypes & SENSOR_TYPE_POWER_METER) {
    advData.addServiceUUID(powerServiceUuid);
  }

  // Set the fresh advertising data
  advertising->setAdvertisementData(advData);

  // Telemetry: Record advertising refresh
  BLETelemetry::recordAdvertisingRefresh();

  if (restart && wasAdvertising) {
    advertising->start(0); // 0 = advertise forever
    BLETelemetry::recordAdvertisingStart();
    BLE_LIB_LOGHOT_F("[ADV] Updated advertising services based on enabled sensor types");
  }
}

void BLEAdvertisingManagerUpdatePayload(bool restart,
                                        const NimBLEUUID& hrServiceUuid,
                                        const NimBLEUUID& cscServiceUuid,
                                        const NimBLEUUID& powerServiceUuid) {
  NimBLEAdvertising* advertising = getAdvertising();
  std::string* advertisingName = getAdvertisingName();
  if (!advertising || !advertisingName) {
    return;
  }

  // Update advertising data with new name using standard NimBLEAdvertising API
  advertising->setName(advertisingName->c_str());

  // Update services based on enabled sensor types
  BLEAdvertisingManagerUpdateServices(restart, hrServiceUuid, cscServiceUuid, powerServiceUuid);
}

void BLEAdvertisingManagerRefreshName(bool restart,
                                       const NimBLEUUID& hrServiceUuid,
                                       const NimBLEUUID& cscServiceUuid,
                                       const NimBLEUUID& powerServiceUuid) {
  std::string* advertisingName = getAdvertisingName();
  if (!advertisingName) {
    return;
  }

  // Always use MAC-based identity ({prefix}-xxxxxxxx format)
  // v2.2.4.7: GetBleDeviceNameLower() returns const String& - safe to use .c_str() directly
  const String& deviceNameRef = bbs::identity::GetBleDeviceNameLower();
  const char* desiredCStr = deviceNameRef.c_str();

  // Note: We no longer append sensor name or suffix - MAC-based identity is stable
  // If sensor name needs to be shown, it should be in service data or manufacturer data

  // v2.2.4.7: Direktvergleich ohne temporäre std::string Kopie (eliminiert Heap-Alloc)
  bool changed = (*advertisingName != desiredCStr);
  if (changed) {
    *advertisingName = desiredCStr;  // Nur bei Änderung zuweisen
    BLE_LIB_LOGHOT_F("Advertising name set to: %s", advertisingName->c_str());
    BLEAdvertisingManagerUpdatePayload(restart, hrServiceUuid, cscServiceUuid, powerServiceUuid);
  }
}
