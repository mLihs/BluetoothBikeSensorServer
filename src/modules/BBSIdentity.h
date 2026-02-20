/**
 * @file BBSIdentity.h
 * @brief BluetoothBikeSensorServer Identity (core)
 *
 * Core identity utilities for the library:
 * - Stable instance ID derived from eFuse MAC (WiFi-style suffix)
 * - Stable lowercase device name "{prefix}-xxxxxxxx"
 * - Prefix sanitization (hostname-safe)
 *
 * This module is intentionally independent of NimBLE types so it can be reused
 * across server, advertising, and optional integrations (e.g., mDNS).
 */
#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace bbs {
namespace identity {

/**
 * @brief Set the device name prefix (sanitized, lowercase)
 *
 * If prefix is null/empty or sanitizes to empty, defaults to "homewind".
 *
 * @param prefix Device name prefix (will be converted to lowercase)
 */
void SetDeviceNamePrefix(const char* prefix);

/**
 * @brief Get stable 16-bit instance ID derived from eFuse MAC lower 32-bit.
 */
uint16_t GetInstanceId();

/**
 * @brief Get cached lowercase BLE device name "{prefix}-xxxxxxxx".
 *
 * @return const String& reference to cached device name
 */
const String& GetBleDeviceNameLower();

/**
 * @brief Get cached lowercase hostname (same as BLE device name).
 *
 * @return const String& reference to cached hostname
 */
const String& GetMdnsHostnameLower();

}  // namespace identity
}  // namespace bbs

