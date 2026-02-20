/**
 * @file BBSIdentity.cpp
 * @brief Implementation of BluetoothBikeSensorServer Identity (core)
 */

#include "BBSIdentity.h"

#include <Arduino.h>
#include <string.h>

namespace bbs {
namespace identity {

// Cached values to avoid repeated MAC reads / string rebuilds
static uint32_t cachedId32 = 0;
static bool instanceIdValid = false;  // separate flag to handle 0x0000 case
static String cachedDeviceName;
static String deviceNamePrefix = "homewind";  // Default prefix

void SetDeviceNamePrefix(const char* prefix) {
  if (!prefix || strlen(prefix) == 0) {
    deviceNamePrefix = "homewind";
  } else {
    // Sanitize prefix: hostname-safe [a-z0-9-] only
    deviceNamePrefix = String(prefix);
    deviceNamePrefix.toLowerCase();

    for (size_t i = 0; i < deviceNamePrefix.length(); i++) {
      char c = deviceNamePrefix[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
        deviceNamePrefix[i] = '-';
      }
    }

    // Remove duplicate '--'
    int pos = 0;
    while ((pos = deviceNamePrefix.indexOf("--")) >= 0) {
      deviceNamePrefix.remove(pos, 1);
    }

    // Trim leading/trailing '-'
    while (deviceNamePrefix.length() > 0 && deviceNamePrefix[0] == '-') {
      deviceNamePrefix.remove(0, 1);
    }
    while (deviceNamePrefix.length() > 0 &&
           deviceNamePrefix[deviceNamePrefix.length() - 1] == '-') {
      deviceNamePrefix.remove(deviceNamePrefix.length() - 1, 1);
    }

    if (deviceNamePrefix.length() == 0) {
      deviceNamePrefix = "homewind";
    }
  }

  // Clear cached name so it will be regenerated with new prefix
  cachedDeviceName = "";
}

uint16_t GetInstanceId() {
  if (instanceIdValid) {
    return static_cast<uint16_t>(cachedId32 & 0xFFFFu);
  }

  // Lower 32 bits of eFuse MAC (WiFi-style suffix)
  const uint64_t efuseMac = ESP.getEfuseMac();
  cachedId32 = static_cast<uint32_t>(efuseMac & 0xFFFFFFFFu);
  instanceIdValid = true;
  return static_cast<uint16_t>(cachedId32 & 0xFFFFu);
}

const String& GetBleDeviceNameLower() {
  // Reserve capacity once to minimize reallocations
  static bool reserved = false;
  if (!reserved) {
    cachedDeviceName.reserve(32);  // "homewind-da4eb580" ~20 chars
    reserved = true;
  }

  (void)GetInstanceId();
  const unsigned long id32 = static_cast<unsigned long>(cachedId32);
  char expectedName[48];
  snprintf(expectedName, sizeof(expectedName), "%s-%08lx", deviceNamePrefix.c_str(), id32);

  // Only reassign if changed
  if (cachedDeviceName.length() == 0 || cachedDeviceName != expectedName) {
    cachedDeviceName = String(expectedName);
  }

  return cachedDeviceName;
}

const String& GetMdnsHostnameLower() {
  return GetBleDeviceNameLower();
}

}  // namespace identity
}  // namespace bbs

