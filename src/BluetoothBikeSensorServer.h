#pragma once

/* Example - Multi-Sensor Usage:
#include <BluetoothBikeSensorServer.h>

void onHeartRateChanged(uint16_t hr) {
  Serial.print("Heart rate updated: ");
  Serial.println(hr);
}

void onSpeedCadenceChanged(uint16_t speed, uint16_t cadence) {
  Serial.print("Speed/Cadence: ");
  Serial.print(speed);
  Serial.print(" m/s, ");
  Serial.print(cadence);
  Serial.println(" rpm");
}

void onPowerChanged(int16_t power) {
  Serial.print("Power: ");
  Serial.print(power);
  Serial.println(" W");
}

void onHRConnected() {
  Serial.println("Heart rate sensor connected!");
}

void onHRDisconnected() {
  Serial.println("Heart rate sensor disconnected!");
}

void onCSCConnected() {
  Serial.println("Speed/Cadence sensor connected!");
}

void onCSCDisconnected() {
  Serial.println("Speed/Cadence sensor disconnected!");
}

void onPowerConnected() {
  Serial.println("Power meter connected!");
}

void onPowerDisconnected() {
  Serial.println("Power meter disconnected!");
}

void onCentralConnected(uint8_t count) {
  Serial.print("Central connected. Count: ");
  Serial.println(count);
}

void onCentralDisconnected(uint8_t count) {
  Serial.print("Central disconnected. Remaining: ");
  Serial.println(count);
}

void setup() {
  Serial.begin(115200);
  // Device name is now MAC-based: "{prefix}-xxxxxxxx" (default prefix "homewind")
  // Optional: Change prefix with BikeSensorServerSetDeviceNamePrefix("yourprefix")
  BluetoothBikeSensorServerBegin();  // Uses default "homewind" prefix
  
  // Set data callbacks
  BluetoothBikeSensorServerSetHeartRateCallback(onHeartRateChanged);
  BluetoothBikeSensorServerSetSpeedCadenceCallback(onSpeedCadenceChanged);
  BluetoothBikeSensorServerSetPowerCallback(onPowerChanged);
  
  // Set per-sensor-type connect/disconnect callbacks
  BikeSensorServerSetSensorConn(SENSOR_TYPE_HEART_RATE, onHRConnected);
  BikeSensorServerSetSensorDisc(SENSOR_TYPE_HEART_RATE, onHRDisconnected);
  BikeSensorServerSetSensorConn(SENSOR_TYPE_SPEED_CADENCE, onCSCConnected);
  BikeSensorServerSetSensorDisc(SENSOR_TYPE_SPEED_CADENCE, onCSCDisconnected);
  BikeSensorServerSetSensorConn(SENSOR_TYPE_POWER_METER, onPowerConnected);
  BikeSensorServerSetSensorDisc(SENSOR_TYPE_POWER_METER, onPowerDisconnected);
  
  // Set central device callbacks
  BikeSensorServerSetClientConn(onCentralConnected);
  BikeSensorServerSetClientDisc(onCentralDisconnected);
  
  // Add sensors by device name (NimBLE-based name-only connection)
  BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, "TICKR FIT");
  BikeSensorServerAddSensor(SENSOR_TYPE_SPEED_CADENCE, "Ridesense");
  BikeSensorServerAddSensor(SENSOR_TYPE_POWER_METER, "Power Meter");
}

void loop() {
  BluetoothBikeSensorServerLoop();

  // Check and display sensor data
  if (BluetoothBikeSensorServerHasHeartRate()) {
    Serial.print("HR: ");
    Serial.println(BluetoothBikeSensorServerGetLastHeartRate());
  }
  
  if (BluetoothBikeSensorServerHasSpeedCadence()) {
    uint16_t speed, cadence;
    if (BluetoothBikeSensorServerGetLastSpeedCadence(&speed, &cadence)) {
      Serial.print("Speed: ");
      Serial.print(speed / 100.0 * 3.6); // Convert to km/h
      Serial.print(" km/h, Cadence: ");
      Serial.print(cadence);
      Serial.println(" rpm");
    }
  }
  
  if (BluetoothBikeSensorServerHasPower()) {
    Serial.print("Power: ");
    Serial.print(BluetoothBikeSensorServerGetLastPower());
    Serial.println(" W");
  }
  
  // Check connection status for each sensor type
  if (!BikeSensorServerIsSensorConnected(SENSOR_TYPE_HEART_RATE)) {
    Serial.println("Waiting for heart rate sensor...");
  }
  if (!BikeSensorServerIsSensorConnected(SENSOR_TYPE_SPEED_CADENCE)) {
    Serial.println("Waiting for speed/cadence sensor...");
  }
  if (!BikeSensorServerIsSensorConnected(SENSOR_TYPE_POWER_METER)) {
    Serial.println("Waiting for power meter...");
  }
  
  delay(1000);
}

// Discovery example:
void onDiscoveryComplete(uint8_t count) {
  Serial.printf("\n=== Discovery complete: %d sensors found ===\n", count);
  
  // List all found sensors
  Serial.println("All sensors:");
  Serial.println(BikeSensorServerListSensors(LIST_FORMAT_FULL));
  
  // List only heart rate sensors
  Serial.println("\nHeart rate sensors:");
  Serial.println(BikeSensorServerListSensorsByType(SENSOR_TYPE_HEART_RATE, LIST_FORMAT_FULL));
  
  // Access individual sensors
  for (uint8_t i = 0; i < BikeSensorServerGetSensorCount(); i++) {
    SensorType type;
    char name[64];
    uint8_t mac[6];
    char macStr[18];
    if (BikeSensorServerGetSensor(i, &type, name, sizeof(name), mac)) {
      snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      Serial.printf("Sensor %d: Type=%d, Name=%s, MAC=%s\n", i, type, name, macStr);
    }
  }
  
  Serial.println("\n=== Starting new discovery scan in 2 seconds ===\n");
  delay(2000);
  
  // Start a new discovery scan (previous results are automatically deleted)
  BikeSensorServerStartDiscovery(SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_POWER_METER);
}

void setupDiscovery() {
  Serial.begin(115200);
  // Device name is now MAC-based: "{prefix}-xxxxxxxx" (default prefix "homewind")
  BluetoothBikeSensorServerBegin();  // Uses default "homewind" prefix
  
  // Set discovery callback
  BikeSensorServerSetDiscoveryCompleteCallback(onDiscoveryComplete);
  
  // Set discovery duration (optional, default is 20 seconds)
  BikeSensorServerSetDiscoveryDuration(20000);
  
  // Set max sensors (optional, default is 12)
  BikeSensorServerSetMaxSensors(12);
  
  // Start first discovery for heart rate and power meter sensors
  BikeSensorServerStartDiscovery(SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_POWER_METER);
  // Discovery runs for 20 seconds, then callback is called after any
  // in-progress sensor processing finishes. The callback then starts a new scan.
}

void loopDiscovery() {
  BluetoothBikeSensorServerLoop();
  
  // Discovery is handled automatically in the loop
  // Callback will be called when discovery completes, which starts a new scan
}
*/

#include <Arduino.h>
#include <NimBLEDevice.h>

// Feature flags (compile-time configuration)
#include "config/BBSConfig.h"

#ifndef BBS_CONNECT_INTERVAL_MS
  #define BBS_CONNECT_INTERVAL_MS 30000
#endif

// Include BBS Identity modules (internal use)
// Note: HomewindBleIdentity.h is kept for external compatibility
#include "modules/BBSIdentity.h"
#include "modules/BBSAdvertisingIdentity.h"

// =========================
// Minimal compile-time logging (Serial) — Variant A: GEN / HOT / NET
// =========================
// Enable via build flags or uncomment:
//   #define BLE_LIB_DEBUG_GEN
//   #define BLE_LIB_DEBUG_HOT
//   #define BLE_LIB_DEBUG_NET

// =========================
// Telemetry System (Phase 0) - Performance Monitoring
// =========================
// Telemetry is DISABLED by default for minimal overhead.
//
// To ENABLE telemetry, call in your setup():
//   BLETelemetry::setEnabled(true);
//
// When enabled, collects metrics for:
// - Loop execution times (min/avg/max, slow loop counts)
// - Heap memory usage and fragmentation over time
// - BLE operations (connections, disconnections, scans, advertising)
//
// Telemetry output is printed every 30 seconds when enabled.
// See examples/TelemetryExample for usage.

// Still supported: build flags (-DCPULOAD_DEBUG_HOT, etc.).


// This file is auto-included by the library (see ESP32CPULoad.h).
// Put your compile-time flags here so they apply to the library's
// .cpp as well (Arduino compiles libraries separately).

// General lifecycle/errors
//#define BLE_LIB_DEBUG_GEN

// High-frequency (prints once per measurement window, default ~1 Hz)
//#define BLE_LIB_DEBUG_HOT

// Reserved for future
// #define BLE_LIB_DEBUG_NET




#if defined(__has_include)
  #if __has_include("DebugConfig.h")
    #include "DebugConfig.h"
  #elif __has_include(<DebugConfig.h>)
    #include <DebugConfig.h>
  #endif
#endif



#if BBS_ENABLE_VERBOSE_LOG && defined(BLE_LIB_DEBUG_GEN)
  #define BLE_LIB_LOGI_F(...) do { Serial.printf("[I] " __VA_ARGS__); Serial.println(); } while(0)
  #define BLE_LIB_LOGW_F(...) do { Serial.printf("[W] " __VA_ARGS__); Serial.println(); } while(0)
  #define BLE_LIB_LOGE_F(...) do { Serial.printf("[E] " __VA_ARGS__); Serial.println(); } while(0)
#else
  #define BLE_LIB_LOGI_F(...) do {} while(0)
  #define BLE_LIB_LOGW_F(...) do {} while(0)
  #define BLE_LIB_LOGE_F(...) do {} while(0)
#endif

#if BBS_ENABLE_VERBOSE_LOG && defined(BLE_LIB_DEBUG_HOT)
  #define BLE_LIB_LOGHOT_F(...) do { Serial.printf("[H] " __VA_ARGS__); Serial.println(); } while(0)
#else
  #define BLE_LIB_LOGHOT_F(...) do {} while(0)
#endif

#if BBS_ENABLE_VERBOSE_LOG && defined(BLE_LIB_DEBUG_NET)
  #define BLE_LIB_LOGNET_F(...) do { Serial.printf("[N] " __VA_ARGS__); Serial.println(); } while(0)
#else
  #define BLE_LIB_LOGNET_F(...) do {} while(0)
#endif

// Sensor Type definitions for discovery
enum SensorType {
  SENSOR_TYPE_HEART_RATE = 0x01,
  SENSOR_TYPE_SPEED_CADENCE = 0x02,
  SENSOR_TYPE_POWER_METER = 0x04
};

// List format options
enum SensorListFormat {
  LIST_FORMAT_NAME,
  LIST_FORMAT_MAC,
  LIST_FORMAT_FULL
};

// Connection mode (v2.2.5: can be set in Begin())
enum BBSConnectionMode {
  // Scan-based: Find sensors by name, requires BikeSensorServerAddSensor()
  BBS_MODE_SCAN = 0,
  
  // MAC-based Sleep-Scan: Periodic short scans to find MAC and connect
  // Requires BikeSensorServerAddSensorByMac()
  BBS_MODE_MAC = 1,
  
  // Legacy aliases (for backward compatibility)
  BBS_CONN_MODE_CONTINUOUS_SCAN = BBS_MODE_SCAN,
  BBS_CONN_MODE_PERIODIC_MAC = BBS_MODE_MAC
};

// BLE Address Type for BikeSensorServerAddSensorByMac()
// Use the value shown in Discovery output: 0 = Public, 1 = Random
// Example: Discovery shows "[HR] TICKR (f0:13:c3:ee:56:97) 0" -> use 0
//          Discovery shows "[CSC] Ridesense (fc:a6:1f:27:45:16) 1" -> use 1

/**
 * @brief Initialize the BluetoothBikeSensorServer library
 * 
 * The device name parameter is now used as a prefix for the MAC-based identity.
 * The actual BLE device name will be "{prefix}-xxxxxxxx" where xxxxxxxx is derived from eFuse MAC (lower 32-bit).
 * Default prefix is "homewind" if not provided.
 * 
 * @param deviceNamePrefix Optional device name prefix (will be converted to lowercase).
 *                        If NULL or empty, defaults to "homewind".
 *                        Example: "MyDevice" -> "mydevice-a1b2" (where a1b2 is from MAC)
 * @param enabledSensorTypes Bitmask of sensor types to enable (default: all)
 * @param connectionMode Connection mode (v2.2.5):
 *                       - BBS_MODE_SCAN (default): Scan for sensors by name, use BikeSensorServerAddSensor()
 *                       - BBS_MODE_MAC: MAC Sleep-Scan mode, use BikeSensorServerAddSensorByMac()
 * @param txPower BLE transmit power level (default: ESP_PWR_LVL_P9, rarely needs changing)
 * 
 * @example Scan mode (find sensors by name):
 * @code
 * BluetoothBikeSensorServerBegin("mydevice", SENSOR_TYPE_HEART_RATE);
 * BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, "HR Sensor Name");
 * @endcode
 * 
 * @example MAC mode (direct connection):
 * @code
 * BluetoothBikeSensorServerBegin("mydevice", SENSOR_TYPE_HEART_RATE, BBS_MODE_MAC);
 * BikeSensorServerAddSensorByMac(SENSOR_TYPE_HEART_RATE, "aa:bb:cc:dd:ee:ff");
 * @endcode
 */
void BluetoothBikeSensorServerBegin(
  const char *deviceNamePrefix = nullptr, 
  uint8_t enabledSensorTypes = SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_SPEED_CADENCE | SENSOR_TYPE_POWER_METER, 
  BBSConnectionMode connectionMode = BBS_MODE_SCAN,
  esp_power_level_t txPower = ESP_PWR_LVL_P9
);

// ============================================================================
// Two-phase initialization (advanced)
// ============================================================================
// Motivation: Allow initializing only the NimBLE stack early (minimal heap impact),
// and creating the server/services/advertising later (after other subsystems).
//
// Phase 1: BluetoothBikeSensorServerInitStack(...)
//   - Calls NimBLEDevice::init() and configures internal module contexts.
//   - Does NOT create the relay server/services/advertising.
//   - Does NOT set isInitialised=true (BluetoothBikeSensorServerLoop will be a no-op).
//
// Phase 2: BluetoothBikeSensorServerStartServer()
//   - Creates the relay server/services/advertising and starts scan manager.
//   - Sets isInitialised=true and starts BLETelemetry (if enabled).
//
// Notes:
// - Calling BluetoothBikeSensorServerBegin() remains the default "one-shot" init.
// - If the stack was already initialized via InitStack(), Begin() will reuse it and only start the server.
void BluetoothBikeSensorServerInitStack(
  const char *deviceNamePrefix = nullptr,
  uint8_t enabledSensorTypes = SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_SPEED_CADENCE | SENSOR_TYPE_POWER_METER,
  BBSConnectionMode connectionMode = BBS_MODE_SCAN,
  esp_power_level_t txPower = ESP_PWR_LVL_P9
);
bool BluetoothBikeSensorServerStartServer();
bool BluetoothBikeSensorServerIsStackInitialized();
void BluetoothBikeSensorServerLoop();
void BluetoothBikeSensorServerCheckCentralConnections();

#if BBS_ENABLE_GETTERS
bool BluetoothBikeSensorServerHasCentral();
uint8_t BluetoothBikeSensorServerGetCentralCount();

uint16_t BluetoothBikeSensorServerGetLastHeartRate();
bool BluetoothBikeSensorServerHasHeartRate();
uint32_t BluetoothBikeSensorServerGetLastHeartRateTimestamp();

// Speed/Cadence data access
bool BluetoothBikeSensorServerGetLastSpeedCadence(uint16_t* speed, uint16_t* cadence);
bool BluetoothBikeSensorServerHasSpeedCadence();
uint32_t BluetoothBikeSensorServerGetLastSpeedCadenceTimestamp();

// Power Meter data access
int16_t BluetoothBikeSensorServerGetLastPower();
bool BluetoothBikeSensorServerHasPower();
uint32_t BluetoothBikeSensorServerGetLastPowerTimestamp();

const char *BluetoothBikeSensorServerGetServerName();
#else
static inline bool BluetoothBikeSensorServerHasCentral() { return false; }
static inline uint8_t BluetoothBikeSensorServerGetCentralCount() { return 0; }
static inline uint16_t BluetoothBikeSensorServerGetLastHeartRate() { return 0; }
static inline bool BluetoothBikeSensorServerHasHeartRate() { return false; }
static inline uint32_t BluetoothBikeSensorServerGetLastHeartRateTimestamp() { return 0; }
static inline bool BluetoothBikeSensorServerGetLastSpeedCadence(uint16_t* speed, uint16_t* cadence) {
  if (speed) *speed = 0;
  if (cadence) *cadence = 0;
  return false;
}
static inline bool BluetoothBikeSensorServerHasSpeedCadence() { return false; }
static inline uint32_t BluetoothBikeSensorServerGetLastSpeedCadenceTimestamp() { return 0; }
static inline int16_t BluetoothBikeSensorServerGetLastPower() { return 0; }
static inline bool BluetoothBikeSensorServerHasPower() { return false; }
static inline uint32_t BluetoothBikeSensorServerGetLastPowerTimestamp() { return 0; }
static inline const char *BluetoothBikeSensorServerGetServerName() { return ""; }
#endif

/**
 * @brief Set the device name prefix for MAC-based identity
 * 
 * Sets the prefix used for BLE device name and mDNS hostname.
 * Format will be: "{prefix}-xxxxxxxx" where xxxxxxxx is the WiFi-style suffix from eFuse MAC (lower 32-bit).
 * Default is "homewind" if not set.
 * 
 * @param prefix Device name prefix (will be converted to lowercase)
 * 
 * @note This affects both BLE device name and mDNS hostname.
 *       Changes take effect on next advertising update or restart.
 */
void BikeSensorServerSetDeviceNamePrefix(const char* prefix);

void BluetoothBikeSensorServerSetHeartRateCallback(void (*callback)(uint16_t heartRate));
void BluetoothBikeSensorServerSetSpeedCadenceCallback(void (*callback)(uint16_t speed, uint16_t cadence));
void BluetoothBikeSensorServerSetPowerCallback(void (*callback)(int16_t power));

void BikeSensorServerSetConnectionMode(BBSConnectionMode mode);

/**
 * Get the current connection mode.
 * @return Current connection mode (BBS_MODE_SCAN or BBS_MODE_MAC)
 */
BBSConnectionMode BikeSensorServerGetConnectionMode();

void BikeSensorServerSetConnectIntervalMs(uint32_t intervalMs);

/**
 * Trigger immediate reconnection attempt for disconnected sensors.
 * 
 * In MAC Sleep-Scan mode: Resets the scan timer and starts an immediate scan.
 * In Scan mode: Restarts scanning if not already running.
 * 
 * Useful for implementing a "Reconnect" button in UI.
 * Only affects disconnected sensors - connected sensors remain unaffected.
 * 
 * @note This is a non-blocking call. The actual connection will happen
 *       asynchronously in the next Loop() iterations.
 */
void BikeSensorServerTriggerReconnect();

// Per-sensor-type callbacks
void BikeSensorServerSetSensorConn(SensorType type, void (*callback)());
void BikeSensorServerSetSensorDisc(SensorType type, void (*callback)());

#if BBS_ENABLE_CENTRAL_CB
void BikeSensorServerSetClientConn(void (*callback)(uint8_t centralCount));
void BikeSensorServerSetClientDisc(void (*callback)(uint8_t centralCount));
void BikeSensorServerSetCCCDEnabled(void (*callback)());
void BikeSensorServerSetCCCDDisabled(void (*callback)());
#else
static inline void BikeSensorServerSetClientConn(void (*)(uint8_t)) {}
static inline void BikeSensorServerSetClientDisc(void (*)(uint8_t)) {}
static inline void BikeSensorServerSetCCCDEnabled(void (*)()) {}
static inline void BikeSensorServerSetCCCDDisabled(void (*)()) {}
#endif

void BluetoothBikeSensorServerResetServer();
void BluetoothBikeSensorServerFullRestart();

// Runtime sensor management API
/**
 * Add a sensor to connect to by device name and type.
 * The library will scan for the device matching the name and connect automatically when found.
 * This is a name-only API - MAC addresses are resolved internally via scanning.
 * 
 * IMPORTANT: Requires NimBLE-Arduino 2.x. Uses name-based connection workflow.
 * 
 * @param type Sensor type (SENSOR_TYPE_HEART_RATE, SENSOR_TYPE_SPEED_CADENCE, or SENSOR_TYPE_POWER_METER)
 * @param sensorName Device name to search for (e.g., "Ridesense", "TICKR FIT")
 * @return true if sensor was added successfully, false otherwise
 */
bool BikeSensorServerAddSensor(SensorType type, const char* sensorName);

/**
 * Add a sensor to connect to by MAC address and type.
 * The library will connect using periodic Sleep-Scan cycles to detect the MAC
 * and only connect after the target MAC was seen.
 * 
 * @param type Sensor type (SENSOR_TYPE_HEART_RATE, SENSOR_TYPE_SPEED_CADENCE, or SENSOR_TYPE_POWER_METER)
 * @param sensorName Optional device name (can be NULL or empty, used for logging only)
 * @param macAddress MAC address in format "XX:XX:XX:XX:XX:XX" (e.g., "00:1A:2B:3C:4D:5E")
 * @param addrType BLE address type: 0 = Public, 1 = Random, -1 = Auto-detect (not recommended)
 *                 NOTE: Run Discovery to find the correct type! Discovery shows the value at the end:
 *                 "[HR] TICKR (f0:13:c3:ee:56:97) 0" -> use 0
 *                 "[CSC] Ridesense (fc:a6:1f:27:45:16) 1" -> use 1
 * @return true if sensor was added successfully, false otherwise
 * 
 * @example Using address type from Discovery output:
 * @code
 * // Discovery shows: [HR] TICKR FIT (f0:13:c3:ee:56:97) 0
 * BikeSensorServerAddSensorByMac(SENSOR_TYPE_HEART_RATE, nullptr, "f0:13:c3:ee:56:97", 0);
 * 
 * // Discovery shows: [CSC] Ridesense (fc:a6:1f:27:45:16) 1
 * BikeSensorServerAddSensorByMac(SENSOR_TYPE_SPEED_CADENCE, nullptr, "fc:a6:1f:27:45:16", 1);
 * @endcode
 */
bool BikeSensorServerAddSensorByMac(SensorType type, const char* sensorName, const char* macAddress, int8_t addrType);
bool BikeSensorServerAddSensorByMac(SensorType type, const char* sensorName, const char* macAddress);
bool BikeSensorServerAddSensorByMac(SensorType type, const char* macAddress);

/**
 * Remove a sensor and disconnect.
 * @param type Sensor type to remove
 * @return true if sensor was removed, false if type invalid
 */
bool BikeSensorServerRemoveSensor(SensorType type);

/**
 * Check if a specific sensor type is currently connected.
 * @param type Sensor type to check
 * @return true if connected, false otherwise
 */
bool BikeSensorServerIsSensorConnected(SensorType type);

#if BBS_ENABLE_GETTERS
/**
 * Get the MAC address of the active sensor for a given type.
 * @param type Sensor type
 * @param mac Output buffer for MAC address (must be at least 18 bytes)
 * @param len Size of output buffer
 * @return true if sensor is active and MAC was copied, false otherwise
 */
bool BikeSensorServerGetSensorMac(SensorType type, char* mac, size_t len);

/**
 * Get the name of the active sensor for a given type.
 * @param type Sensor type
 * @param name Output buffer for sensor name
 * @param len Size of output buffer
 * @return true if sensor is active and name was copied, false otherwise
 */
bool BikeSensorServerGetSensorName(SensorType type, char* name, size_t len);

/**
 * Get the battery level of the active sensor for a given type.
 * @param type Sensor type
 * @param batteryLevel Output parameter for battery level (0-100, 255 = unknown)
 * @return true if sensor is active and battery level is available, false otherwise
 */
bool BikeSensorServerGetBatteryLevel(SensorType type, uint8_t* batteryLevel);

/**
 * Check if the active sensor for a given type has battery level information.
 * @param type Sensor type
 * @return true if sensor is active and has battery service, false otherwise
 */
bool BikeSensorServerHasBatteryLevel(SensorType type);
#else
static inline bool BikeSensorServerGetSensorMac(SensorType, char* mac, size_t len) {
  if (mac && len > 0) mac[0] = '\0';
  return false;
}
static inline bool BikeSensorServerGetSensorName(SensorType, char* name, size_t len) {
  if (name && len > 0) name[0] = '\0';
  return false;
}
static inline bool BikeSensorServerGetBatteryLevel(SensorType, uint8_t* batteryLevel) {
  if (batteryLevel) *batteryLevel = 255;
  return false;
}
static inline bool BikeSensorServerHasBatteryLevel(SensorType) { return false; }
#endif

// Discovery functions
// ============================================================================
// Scan Management Functions (Internal/Advanced)
// ============================================================================

/**
 * Start server mode scanning (continuous scan for sensor connection).
 * This is called automatically by BluetoothBikeSensorServerBegin().
 * Only use this if you need to manually restart scanning.
 * 
 * Note: Consider using BLEScanManager::startServerModeScan() instead (Phase 1 module)
 */
void startScan();

/**
 * Start discovery scan with specified parameters.
 * Internal function - prefer using BikeSensorServerStartDiscovery() instead.
 * 
 * @param sensorTypeMask Bitmask of sensor types to discover
 */
void startDiscoveryScan(uint8_t sensorTypeMask);

/**
 * Stop any active discovery scan.
 * Internal function - prefer using BikeSensorServerStopDiscovery() instead.
 */
void stopDiscoveryScan();

// ============================================================================
// Discovery Mode Functions (Public API)
// ============================================================================

/**
 * Start discovery mode to search for Bluetooth sensors.
 * Automatically deletes all previously found sensors before starting.
 * Pauses normal mode operations during discovery.
 * @param sensorTypeMask Bitmask of sensor types to search for:
 *   SENSOR_TYPE_HEART_RATE (0x01)
 *   SENSOR_TYPE_SPEED_CADENCE (0x02)
 *   SENSOR_TYPE_POWER_METER (0x04)
 *   Can combine: SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_POWER_METER
 */
void BikeSensorServerStartDiscovery(uint8_t sensorTypeMask);

/**
 * Manually stop discovery mode before timeout.
 * Discovery will stop and callback will be called after processing finishes.
 */
void BikeSensorServerStopDiscovery();

/**
 * Delete all discovered sensors from the list.
 */
void BikeSensorServerDeleteAllSensors();

#if BBS_ENABLE_GETTERS
/**
 * Get total count of discovered sensors.
 * @return Number of discovered sensors
 */
uint8_t BikeSensorServerGetSensorCount();

/**
 * Get count of discovered sensors of a specific type.
 * @param type Sensor type to count
 * @return Number of sensors of the specified type
 */
uint8_t BikeSensorServerGetSensorCountByType(SensorType type);

/**
 * Get sensor information by index.
 * @param index Sensor index (0 to count-1)
 * @param type Output parameter for sensor type
 * @param name Output buffer for sensor name
 * @param nameLen Size of name buffer
 * @param mac Output buffer for MAC address (6 bytes)
 * @return true if sensor found, false if index invalid
 */
bool BikeSensorServerGetSensor(uint8_t index, SensorType* type, char* name, size_t nameLen, uint8_t mac[6]);

/**
 * Get sensor information by MAC address.
 * @param mac MAC address to search for
 * @param type Output parameter for sensor type
 * @param name Output buffer for sensor name
 * @param nameLen Size of name buffer
 * @return true if sensor found, false if not found
 */
bool BikeSensorServerGetSensorByMac(const char* mac, SensorType* type, char* name, size_t nameLen);

/**
 * Get formatted list of all discovered sensors.
 * @param format Format type: LIST_FORMAT_NAME, LIST_FORMAT_MAC, or LIST_FORMAT_FULL
 * @return Formatted string (static buffer, overwritten on next call)
 */
const char* BikeSensorServerListSensors(SensorListFormat format);

/**
 * Get formatted list of sensors of a specific type.
 * @param type Sensor type to filter
 * @param format Format type: LIST_FORMAT_NAME, LIST_FORMAT_MAC, or LIST_FORMAT_FULL
 * @return Formatted string (static buffer, overwritten on next call)
 */
const char* BikeSensorServerListSensorsByType(SensorType type, SensorListFormat format);
#else
static inline uint8_t BikeSensorServerGetSensorCount() { return 0; }
static inline uint8_t BikeSensorServerGetSensorCountByType(SensorType) { return 0; }
static inline bool BikeSensorServerGetSensor(uint8_t, SensorType* type, char* name, size_t nameLen, uint8_t mac[6]) {
  if (type) *type = static_cast<SensorType>(0);
  if (name && nameLen > 0) name[0] = '\0';
  if (mac) memset(mac, 0, 6);
  return false;
}
static inline bool BikeSensorServerGetSensorByMac(const char*, SensorType* type, char* name, size_t nameLen) {
  if (type) *type = static_cast<SensorType>(0);
  if (name && nameLen > 0) name[0] = '\0';
  return false;
}
static inline const char* BikeSensorServerListSensors(SensorListFormat) { return ""; }
static inline const char* BikeSensorServerListSensorsByType(SensorType, SensorListFormat) { return ""; }
#endif

/**
 * Set discovery duration in milliseconds.
 * @param durationMs Discovery duration (default: 20000 = 20 seconds)
 */
void BikeSensorServerSetDiscoveryDuration(uint32_t durationMs);

#if BBS_ENABLE_GETTERS
/**
 * Get current discovery duration.
 * @return Discovery duration in milliseconds
 */
uint32_t BikeSensorServerGetDiscoveryDuration();

/**
 * Set maximum number of sensors to discover.
 * @param maxSensors Maximum sensors (default: 12)
 */
void BikeSensorServerSetMaxSensors(uint8_t maxSensors);

/**
 * Get maximum number of sensors.
 * @return Maximum sensors limit
 */
uint8_t BikeSensorServerGetMaxSensors();
#else
static inline uint32_t BikeSensorServerGetDiscoveryDuration() { return 0; }
static inline uint8_t BikeSensorServerGetMaxSensors() { return 0; }
#endif

/**
 * Pause the BLE Relay Server (Virtual Sensor Server).
 * Stops advertising if running. State is saved and can be restored with BikeSensorServerResume().
 * Useful when opening sensor configuration UI to prevent interference.
 */
void BikeSensorServerPause();

/**
 * Resume the BLE Relay Server (Virtual Sensor Server).
 * Restarts advertising if it was running before pause.
 * Should be called after closing sensor configuration UI.
 */
void BikeSensorServerResume();

#if BBS_ENABLE_GETTERS
/**
 * Check if the BLE Relay Server is currently advertising.
 * @return true if advertising is active, false otherwise
 */
bool BikeSensorServerIsAdvertising();
#else
static inline bool BikeSensorServerIsAdvertising() { return false; }
#endif

/**
 * Force restart advertising for the BLE Relay Server.
 * Use this if advertising has stopped unexpectedly and needs to be restarted.
 * This will restart advertising regardless of pause state.
 * @return true if advertising was restarted successfully, false otherwise
 */
bool BikeSensorServerRestartAdvertising();

/**
 * Refresh BLE advertising with new identity (manufacturer data and service UUID).
 * This is used for Zwift cache bypass - changes advertising identity without MAC rotation.
 * Stops advertising, clears payload, rebuilds with new manufacturer data and service UUID, then restarts.
 * 
 * @param deviceName Device name to advertise
 * @param manufacturerData Pointer to manufacturer data array (typically 8 bytes)
 * @param manufacturerDataLength Length of manufacturer data array
 * @param primaryServiceUuid Primary service UUID to advertise (16-bit, e.g., 0x180D for HR, 0x1818 for Power)
 * @return true if advertising was refreshed successfully, false otherwise
 */
bool BikeSensorServerRefreshAdvertisingWithIdentity(const char* deviceName, 
                                                     const uint8_t* manufacturerData, 
                                                     size_t manufacturerDataLength,
                                                     uint16_t primaryServiceUuid);

/**
 * Set callback function called when discovery completes.
 * Called after timeout or manual stop, and after any in-progress sensor processing finishes.
 * @param callback Function pointer: void callback(uint8_t count)
 */
void BikeSensorServerSetDiscoveryCompleteCallback(void (*callback)(uint8_t count));

#if BBS_ENABLE_GETTERS
/**
 * Check if discovery mode is currently active.
 * @return true if discovery is running, false otherwise
 */
bool BikeSensorServerIsDiscoveryActive();

/**
 * Check if server-mode scanning is currently active.
 * @return true if scan is running, false otherwise
 */
bool BikeSensorServerIsScanning();
#else
static inline bool BikeSensorServerIsDiscoveryActive() { return false; }
static inline bool BikeSensorServerIsScanning() { return false; }
#endif

/**
 * Dynamically change the enabled sensor types at runtime.
 * This function will:
 * - Update the enabled sensor types
 * - Create/remove services as needed
 * - Update advertising to reflect new services
 * - Disconnect all connected central devices (forces them to rediscover)
 * 
 * @param newEnabledSensorTypes Bitmask of sensor types to enable:
 *   SENSOR_TYPE_HEART_RATE (0x01)
 *   SENSOR_TYPE_SPEED_CADENCE (0x02)
 *   SENSOR_TYPE_POWER_METER (0x04)
 *   Can combine: SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_SPEED_CADENCE
 * 
 * @note Central devices (like Zwift) will need to reconnect to see the updated services.
 *       The function automatically disconnects all centrals to force rediscovery.
 */
void BikeSensorServerSetEnabledSensorTypes(uint8_t newEnabledSensorTypes);

/**
 * Enable/disable preserving sensor MAC address on disconnect for faster reconnection.
 * When enabled, the library will keep the sensor's MAC address and attempt direct reconnection
 * without scanning. When disabled (default), sensor MAC is cleared and rescan is performed.
 * 
 * @param preserve true to preserve sensor MAC on disconnect (faster reconnection),
 *                 false to clear sensor MAC and rescan (default, more conservative)
 * 
 * @note Sensor MAC Preservation is faster but less conservative. Use if:
 *       - You trust your sensors have stable MAC addresses
 *       - You want faster reconnection times
 *       - You're using static/public MAC addresses (not privacy/random)
 */
void BikeSensorServerSetPreserveMacOnDisconnect(bool preserve);

#if BBS_ENABLE_GETTERS
/**
 * Get current Sensor MAC Preservation setting.
 * @return true if sensor MAC is preserved on disconnect, false otherwise
 */
bool BikeSensorServerGetPreserveMacOnDisconnect();

/**
 * Get the last disconnect reason code for a sensor type.
 * Useful for debugging connection issues.
 * 
 * @param type Sensor type to query
 * @return BLE HCI disconnect reason code (0 = never disconnected or unknown)
 * 
 * Common reason codes:
 *   0x13 (19) = Remote User Terminated (peer closed connection normally)
 *   0x16 (22) = Local Host Terminated (we closed the connection)
 *   0x08 (8)  = Connection Timeout
 *   0x3E (62) = Connection Failed To Establish
 */
int BikeSensorServerGetLastDisconnectReason(SensorType type);

/**
 * Get a human-readable string for a BLE disconnect reason code.
 * 
 * @param reason BLE HCI disconnect reason code
 * @return Pointer to static string describing the reason
 */
const char* BikeSensorServerGetDisconnectReasonString(int reason);
#else
static inline bool BikeSensorServerGetPreserveMacOnDisconnect() { return false; }
static inline int BikeSensorServerGetLastDisconnectReason(SensorType) { return 0; }
static inline const char* BikeSensorServerGetDisconnectReasonString(int) { return ""; }
#endif

