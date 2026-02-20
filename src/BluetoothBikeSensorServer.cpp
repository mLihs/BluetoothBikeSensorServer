#include "BluetoothBikeSensorServer.h"
#include "BLETelemetry.h"
#include "config/SensorRegistry.h"
#include "modules/BLEScanManager.h"
#include "modules/BLEConnectionManager.h"
#include "modules/SensorDataParser.h"
#include "modules/BLEAdvertisingManager.h"

/*
 * arduino-esp32 3.3.7 BT memory fix:
 * In 3.3.7 the core releases BT controller memory (~36KB) unless btInUse() returns true.
 * NimBLE-Arduino does not include esp32-hal-bt-mem.h, so _btLibraryInUse stays false
 * and BT memory gets released before NimBLEDevice::init() → TLSF assert crash at boot.
 *
 * Fix: Include esp32-hal-bt-mem.h (constructor sets _btLibraryInUse = true).
 *
 * Alternative (if include fails): uncomment and remove the include above:
 * extern "C" bool btInUse(void) {
 *   return true;
 * }
 */
#if defined(ARDUINO_ARCH_ESP32)
#include "esp32-hal-bt-mem.h"
#endif

#include <NimBLEDevice.h>
// NimBLEDevice.h includes all necessary NimBLE classes (NimBLEServer, NimBLEClient, etc.)

#if BBS_ENABLE_INIT_HEAP_LOG
  #if defined(ARDUINO_ARCH_ESP32)
    #include <esp_heap_caps.h>
  #endif
#endif

#include <stdio.h>
#include <string>
#include <string.h>

// Forward declarations for functions in global scope (defined later in file)
void printVirtualDeviceState();

// Anonymous namespace for internal constants and helpers
// NOTE: Namespace intentionally left open - closing it causes heap regression!
// Functions declared in header (startScan, etc.) are defined outside this namespace
namespace {
// Service & Characteristic UUIDs (always defined - used by advertising manager)
// Actual service creation is guarded by BBS_COMPILE_* flags below
const NimBLEUUID HR_SERVICE_UUID((uint16_t)0x180D);
const NimBLEUUID HR_MEAS_CHAR_UUID((uint16_t)0x2A37);
const NimBLEUUID SPEED_CADENCE_SERVICE_UUID((uint16_t)0x1816);
const NimBLEUUID SPEED_CADENCE_CHAR_UUID((uint16_t)0x2A5B);
const NimBLEUUID POWER_SERVICE_UUID((uint16_t)0x1818);
const NimBLEUUID POWER_CHAR_UUID((uint16_t)0x2A63);

// Helper function to get sensor index from type
int getSensorIndex(SensorType type) {
  return BLEConnectionManagerGetSensorIndex(type);
}

// Helper function to get sensor type from index
SensorType getSensorTypeFromIndex(int index) {
  return BLEConnectionManagerGetSensorTypeFromIndex(index);
}

void resetSensorMac(ActiveSensor& sensor) {
  sensor.hasMac = false;
  memset(sensor.mac, 0, sizeof(sensor.mac));
  sensor.address = NimBLEAddress((uint8_t*)"\0\0\0\0\0\0", 6);
  sensor.addrType = 0;
}

bool parseMacString(const char* macStr, uint8_t out[6]) {
  if (!macStr || !out) {
    return false;
  }
  unsigned int bytes[6];
  if (sscanf(macStr, "%2x:%2x:%2x:%2x:%2x:%2x",
             &bytes[0], &bytes[1], &bytes[2],
             &bytes[3], &bytes[4], &bytes[5]) != 6) {
    return false;
  }
  for (int i = 0; i < 6; ++i) {
    out[i] = static_cast<uint8_t>(bytes[i]);
  }
  return true;
}

void updateSensorMacFromAddress(ActiveSensor& sensor, const NimBLEAddress& addr) {
  const uint8_t* mac = addr.getVal();
  // NimBLEAddress::getVal() returns bytes in reverse order (Little-Endian BLE format)
  // We need to reverse them to store in Big-Endian format (standard MAC format)
  for (int i = 0; i < 6; i++) {
    sensor.mac[i] = mac[5 - i];
  }
  sensor.hasMac = true;
  sensor.address = addr;
  sensor.addrType = addr.getType();
}

void formatSensorMac(const ActiveSensor& sensor, char out[18]) {
  if (!sensor.hasMac) {
    out[0] = '\0';
    return;
  }
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           sensor.mac[0], sensor.mac[1], sensor.mac[2],
           sensor.mac[3], sensor.mac[4], sensor.mac[5]);
}
// NOTE: Anonymous namespace NOT closed here - continues to end of file

// Discovered sensor structure (matching async example pattern)
struct DiscoveredSensor {
  char name[32];    // Fixed buffer - max BLE device name length (no heap alloc)
  char mac[18];     // "XX:XX:XX:XX:XX:XX\0" (no heap alloc)
  bool hasHR;
  bool hasCSC;
  bool hasPWR;
  uint8_t addrType;  // BLE address type: 0=Public, 1=Random
  
  // Safe setters
  void setName(const char* n) {
    strncpy(name, n ? n : "", sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
  }
  void setMac(const char* m) {
    strncpy(mac, m ? m : "", sizeof(mac) - 1);
    mac[sizeof(mac) - 1] = '\0';
  }
  void clear() {
    name[0] = '\0';
    mac[0] = '\0';
    hasHR = false;
    hasCSC = false;
    hasPWR = false;
    addrType = 0;
  }
  
  // Helper to get SensorType (for backward compatibility)
  SensorType getType() const {
    if (hasHR) return SENSOR_TYPE_HEART_RATE;
    if (hasCSC) return SENSOR_TYPE_SPEED_CADENCE;
    if (hasPWR) return SENSOR_TYPE_POWER_METER;
    return (SensorType)0;
  }
};


// Discovery state (matching async example pattern)
const int MAX_DISCOVERED_SENSORS = 50;
DiscoveredSensor g_discoveredSensors[MAX_DISCOVERED_SENSORS];
int g_discoveredCount = 0;
bool g_discoveryRunning = false;
bool g_discoveryDone = false;

// Active sensors - one per type (max 3: HR, CSC, Power)
ActiveSensor* activeSensors = BLEConnectionManagerGetSensors();  // Index 0=HR, 1=CSC, 2=Power
static BBSConnectionMode g_connectionMode = BBS_MODE_SCAN;
static uint32_t g_connectIntervalMs = BBS_CONNECT_INTERVAL_MS;

// Sleep Scan Mode: Scan periodisch statt kontinuierlich (konfigurierbar in BBSConfig.h)
static unsigned long g_lastSleepScanMs = 0;
static const uint32_t SLEEP_SCAN_INTERVAL_MS = BBS_SLEEP_SCAN_INTERVAL_MS;
static const uint32_t SLEEP_SCAN_DURATION_MS = BBS_SLEEP_SCAN_DURATION_MS;

// Global initialization flag (declared early so callbacks can access it)
bool isInitialised = false;
// Two-phase init flag: NimBLE stack initialized, but server/services not yet started
static bool s_stackInitialised = false;

#if BBS_ENABLE_INIT_HEAP_LOG
static float bbsCalcFrag(uint32_t largest, uint32_t freeHeap) {
  if (freeHeap == 0) return 0.0f;
  return 100.0f - (100.0f * (float)largest / (float)freeHeap);
}

static void bbsInitHeapLog(const char* phase) {
  if (!phase) phase = "?";
  const uint32_t free8 = ESP.getFreeHeap();
  const uint32_t lrg8 = ESP.getMaxAllocHeap();

  // Keep output stable (no dynamic allocation)
#if defined(ARDUINO_ARCH_ESP32)
  const uint32_t freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const uint32_t lrgInt = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  Serial.printf("[BBS][Heap] %s: free=%u largest=%u frag=%.1f%% | freeInt=%u lrgInt=%u fragInt=%.1f%%\n",
                phase, free8, lrg8, bbsCalcFrag(lrg8, free8), freeInt, lrgInt, bbsCalcFrag(lrgInt, freeInt));
#else
  Serial.printf("[BBS][Heap] %s: free=%u largest=%u frag=%.1f%%\n",
                phase, free8, lrg8, bbsCalcFrag(lrg8, free8));
#endif
}
#endif

// Initialize active sensors array
void initializeActiveSensors() {
  BLEConnectionManagerInitActiveSensors();
}

NimBLEServer *server = nullptr;

#if BBS_COMPILE_HEART_RATE
NimBLEService *hrServiceLocal = nullptr;
NimBLECharacteristic *hrCharLocal = nullptr;
#endif

#if BBS_COMPILE_CSC
NimBLEService *cscServiceLocal = nullptr;
NimBLECharacteristic *cscCharLocal = nullptr;
#endif

#if BBS_COMPILE_POWER
NimBLEService *powerServiceLocal = nullptr;
NimBLECharacteristic *powerCharLocal = nullptr;
#endif

std::string advertisingName = "";  // Will be set to MAC-based name in Begin()
esp_power_level_t initialTxPower = ESP_PWR_LVL_P9;
uint8_t enabledSensorTypes = SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_SPEED_CADENCE | SENSOR_TYPE_POWER_METER; // Default: all sensors enabled
bool configCaptured = false;
bool preserveMacOnDisconnect = true;  // Default: preserve sensor MAC for fast reconnection

// Standard advertising pointer - will be set via NimBLEDevice::getAdvertising()
// Note: Using standard NimBLEAdvertising API (not extended advertising)
NimBLEAdvertising* pAdvertising = nullptr;
uint8_t lastCentralCount = 0;

// Heart Rate data
#if BBS_COMPILE_HEART_RATE
uint16_t lastHeartRate = 0;
bool heartRateValid = false;
uint32_t lastHeartRateTimestamp = 0;
#endif

// Speed/Cadence data
#if BBS_COMPILE_CSC
SpeedCadenceData speedCadenceData = {0, 0, false, 0, 0, 0, 0, 0};
#endif

// Power Meter data
#if BBS_COMPILE_POWER
PowerData powerData = {0, false, 0};
#endif

// Callbacks
#if BBS_COMPILE_HEART_RATE
void (*heartRateCallback)(uint16_t) = nullptr;
#endif
#if BBS_COMPILE_CSC
void (*speedCadenceCallback)(uint16_t speed, uint16_t cadence) = nullptr;
#endif
#if BBS_COMPILE_POWER
void (*powerCallback)(int16_t power) = nullptr;
#endif
void (*centralConnectedCallback)(uint8_t) = nullptr;
void (*centralDisconnectedCallback)(uint8_t) = nullptr;
void (*sensorConnectedCallbacks[3])() = {nullptr, nullptr, nullptr};  // Per-type callbacks
void (*sensorDisconnectedCallbacks[3])() = {nullptr, nullptr, nullptr};
void (*cccdEnabledCallback)() = nullptr;
void (*cccdDisabledCallback)() = nullptr;
void (*g_discoveryCompleteCallback)(uint8_t) = nullptr;

// Server mode state for saving/restoring (discovery mode)
struct ServerModeState {
  bool wasScanning;
  bool wasAdvertising;  // Virtual Sensor Server (Relay Server) state
};

// Discovery mode variables (matching async example pattern)
ServerModeState savedState;
uint32_t discoveryDuration = 20000; // default 20 seconds
uint8_t maxSensors = 12; // default max sensors
uint8_t g_discoveryTypeMask = 0;  // Sensor type mask for current discovery

// Global state to track if server was paused manually (not during discovery)
static bool g_serverManuallyPaused = false;
static bool g_serverWasAdvertisingBeforePause = false;

// Forward declarations - must be before scan callbacks
// Note: startScan(), startDiscoveryScan(), stopDiscoveryScan() are now declared in .h
// Note: printVirtualDeviceState() is now in global scope (no forward declaration needed)

// Server callbacks - optional in NimBLE 2.x, can be set via server->setCallbacks() if needed
// For now, we'll handle advertising restart manually in CheckCentralConnections

// Characteristic callbacks - not needed for basic operation in NimBLE 2.x

// Track CCCD state
bool lastCCCDState = false;
uint32_t lastCCCDCheck = 0;
// Track notification success/failure for debugging
#if BBS_COMPILE_HEART_RATE
uint32_t hrNotifySuccessCount = 0;
uint32_t hrNotifyFailureCount = 0;
#endif
#if BBS_COMPILE_CSC
uint32_t cscNotifySuccessCount = 0;
uint32_t cscNotifyFailureCount = 0;
#endif
#if BBS_COMPILE_POWER
uint32_t powerNotifySuccessCount = 0;
uint32_t powerNotifyFailureCount = 0;
#endif
uint32_t lastNotifyStatsPrint = 0;

void clearSensorState() {
#if BBS_COMPILE_HEART_RATE
  // Clear heart rate data
  heartRateValid = false;
  lastHeartRate = 0;
  lastHeartRateTimestamp = 0;
#endif
}

// Mirror function generator macro - eliminates 3x code duplication.
// Each mirror function: sets characteristic value, notifies connected centrals,
// tracks success/failure counts, and logs failures periodically.
#define DEFINE_MIRROR_FUNC(FUNC_NAME, CHAR_PTR, SUCCESS_CNT, FAIL_CNT, TAG) \
void FUNC_NAME(const uint8_t *data, size_t len) {                          \
  if (!(CHAR_PTR)) return;                                                  \
  (CHAR_PTR)->setValue(data, len);                                          \
  if (server && server->getConnectedCount() > 0) {                         \
    if ((CHAR_PTR)->notify()) {                                             \
      (SUCCESS_CNT)++;                                                      \
    } else {                                                                \
      (FAIL_CNT)++;                                                         \
      if ((FAIL_CNT) % 10 == 1) {                                          \
        BLE_LIB_LOGHOT_F("[MIRROR] " TAG " notify failed (count: %lu)"     \
                         " - CCCD may not be enabled", (FAIL_CNT));         \
      }                                                                     \
    }                                                                       \
  }                                                                         \
}

#if BBS_COMPILE_HEART_RATE
DEFINE_MIRROR_FUNC(mirrorHeartRatePacket, hrCharLocal, hrNotifySuccessCount, hrNotifyFailureCount, "HR")
#endif

#if BBS_COMPILE_CSC
DEFINE_MIRROR_FUNC(mirrorCSCPacket, cscCharLocal, cscNotifySuccessCount, cscNotifyFailureCount, "CSC")
#endif

#if BBS_COMPILE_POWER
DEFINE_MIRROR_FUNC(mirrorPowerPacket, powerCharLocal, powerNotifySuccessCount, powerNotifyFailureCount, "Power")
#endif

#undef DEFINE_MIRROR_FUNC

// ============================================================================
// SENSOR REGISTRY - one line per sensor type (inside anonymous namespace)
// Must be after mirror functions (same namespace) and uses parse functions
// (external linkage from SensorDataParser.cpp).
// ============================================================================
static const SensorTypeConfig SENSOR_REGISTRY[] = {
#if BBS_COMPILE_HEART_RATE
  { SENSOR_TYPE_HEART_RATE,    SENSOR_TYPE_HEART_RATE,    "HR",    "0x180D (HR)",    0x180D, 0x2A37, parseAndLogHR,    mirrorHeartRatePacket },
#endif
#if BBS_COMPILE_CSC
  { SENSOR_TYPE_SPEED_CADENCE, SENSOR_TYPE_SPEED_CADENCE, "CSC",   "0x1816 (CSC)",   0x1816, 0x2A5B, parseAndLogCSC,   mirrorCSCPacket },
#endif
#if BBS_COMPILE_POWER
  { SENSOR_TYPE_POWER_METER,   SENSOR_TYPE_POWER_METER,   "Power", "0x1818 (Power)", 0x1818, 0x2A63, parseAndLogPower, mirrorPowerPacket },
#endif
};
static constexpr int SENSOR_REGISTRY_COUNT = sizeof(SENSOR_REGISTRY) / sizeof(SENSOR_REGISTRY[0]);

/** Find config by SensorType. Returns nullptr if type is not compiled in. */
const SensorTypeConfig* getSensorConfig(SensorType type) {
  for (int i = 0; i < SENSOR_REGISTRY_COUNT; i++) {
    if (SENSOR_REGISTRY[i].type == type) return &SENSOR_REGISTRY[i];
  }
  return nullptr;
}

/** Get short name ("HR", "CSC", "Power", or "?"). */
const char* getSensorTypeName(SensorType type) {
  const SensorTypeConfig* cfg = getSensorConfig(type);
  return cfg ? cfg->shortName : "?";
}

/** Get descriptive service name for logging ("0x180D (HR)", etc.). */
const char* getSensorServiceName(SensorType type) {
  const SensorTypeConfig* cfg = getSensorConfig(type);
  return cfg ? cfg->serviceName : "?";
}

class MyAdvertisedDeviceCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    BLETelemetry::recordScanResult();
    // Check for compiled sensor types
    // Check haveServiceUUID first (like the working script does)
#if BBS_COMPILE_HEART_RATE
    bool hasHR = advertisedDevice->haveServiceUUID() && advertisedDevice->isAdvertisingService(HR_SERVICE_UUID);
#else
    bool hasHR = false;
#endif
#if BBS_COMPILE_CSC
    bool hasCSC = advertisedDevice->haveServiceUUID() && advertisedDevice->isAdvertisingService(SPEED_CADENCE_SERVICE_UUID);
#else
    bool hasCSC = false;
#endif
#if BBS_COMPILE_POWER
    bool hasPower = advertisedDevice->haveServiceUUID() && advertisedDevice->isAdvertisingService(POWER_SERVICE_UUID);
#else
    bool hasPower = false;
#endif
    
#if defined(BLE_LIB_DEBUG_HOT)
    // v2.2.4.7 FIX: toString() nur im Debug-Modus (nicht in Production)
    // This is acceptable because BLE_LIB_DEBUG_HOT is disabled in production builds
    const char* name = advertisedDevice->haveName() ? advertisedDevice->getName().c_str() : "(no name)";
    std::string addrStr = advertisedDevice->getAddress().toString();
    int rssi = advertisedDevice->getRSSI();
    BLE_LIB_LOGHOT_F("[SCAN] Found: %s | %s | RSSI: %d | Services: HR=%s, CSC=%s, Power=%s",
                  name, addrStr.c_str(), rssi,
                  hasHR ? "yes" : "no", hasCSC ? "yes" : "no", hasPower ? "yes" : "no");
#endif

    // PROPER FIX v2.2.4: Hybrid Name/MAC matching with LOCK and RATE-LIMIT
    // Strategy (GUARANTEED "only once"):
    //   1. If hasResolvedAddress=true → MAC ONLY (ZERO alloc, works even with RPA!)
    //   2. If hasResolvedAddress=false → Name with STRICT rate-limit (2s cooldown)
    //   3. When match found → LOCK IT IN with hasResolvedAddress=true
    // This GUARANTEES zero heap allocations in normal operation (after first discovery)
    
    NimBLEAddress addr = advertisedDevice->getAddress();  // NimBLEAddress object for actual use
    uint8_t addrType = advertisedDevice->getAddress().getType();
    uint32_t now = millis();
    
    // Check each sensor type - hybrid Name/MAC matching with LOCK
    for (int i = 0; i < 3; i++) {
      ActiveSensor &sensor = activeSensors[i];
      if (!sensor.hasTarget) {
        // No sensor configured for this slot
        continue;
      }
      if (sensor.isConnected) {
        // Already connected
        continue;
      }
      
      // Debug: Log which sensor we're looking for (first time only)
      static bool sensorSearchLogged[3] = {false, false, false};
      if (!sensorSearchLogged[i]) {
        BLE_LIB_LOGI_F("[SCAN] Looking for sensor %d: name='%s', hasResolvedAddress=%d", 
               i, sensor.name, sensor.hasResolvedAddress);
        sensorSearchLogged[i] = true;
      }
      
      bool matches = false;
      
      // Strategy 1: RESOLVED → MAC ONLY (guaranteed zero alloc!)
      if (sensor.hasResolvedAddress) {
        // MAC matching ONLY - no name checks, no getName() calls!
        matches = (sensor.address == addr);
        
#if defined(BLE_LIB_DEBUG_HOT)
        // v2.2.4.7 FIX: Stack-Buffer statt toString() (eliminiert Heap-Alloc im Debug-Modus)
        // Note: toString() returns std::string, but we copy immediately to stack buffers
        std::string addrStr = addr.toString();
        std::string sensorStr = sensor.address.toString();
        char addrBuf[18], sensorBuf[18];
        strncpy(addrBuf, addrStr.c_str(), sizeof(addrBuf) - 1);
        addrBuf[sizeof(addrBuf) - 1] = '\0';
        strncpy(sensorBuf, sensorStr.c_str(), sizeof(sensorBuf) - 1);
        sensorBuf[sizeof(sensorBuf) - 1] = '\0';
        BLE_LIB_LOGHOT_F("[SCAN] MAC Match (resolved): %s == %s -> %s", 
               addrBuf, sensorBuf, matches ? "MATCH" : "NO MATCH");
#endif
        
      // Strategy 2: NOT RESOLVED → Name matching (like v2.2.3, but with lock after)
      // v2.2.4.3 FIX: Removed strict rate-limiting and RSSI filter that blocked discovery!
      // The ONLY requirement is haveName()=true - everything else was too restrictive.
      } else {
        bool hasName = advertisedDevice->haveName();
        
        if (hasName) {
          // v2.2.4.3: Simple name matching (like v2.2.3)
          // Rate-limiting and RSSI filter REMOVED - they were blocking valid sensors!
          sensor.nameCheckCount++; // Telemetry
          
          const char* deviceName = advertisedDevice->getName().c_str();
          if (deviceName[0] != '\0' && sensor.name[0] != '\0') {
            matches = (strcmp(deviceName, sensor.name) == 0);
            BLE_LIB_LOGI_F("[SCAN] Name check #%d: '%s' == '%s' -> %s",
                   sensor.nameCheckCount, deviceName, sensor.name,
                   matches ? "MATCH" : "NO MATCH");
          }
        }
        // Note: If haveName()=false, we simply skip - BLE spec says name is optional in advertisement.
        // The device will be found on the next advertisement that includes the name.
      }
      
      if (!matches) continue; // No match, try next sensor
      
      // Match found! Now check if device has the required service
      bool hasService = false;
      
      // MAC-Mode (hasResolvedAddress=true): Skip service check!
      // We already know the sensor type from BikeSensorServerAddSensorByMac()
      // Not all sensors broadcast their service UUID in advertisements
      if (sensor.hasResolvedAddress) {
        hasService = true;  // Trust the MAC match, skip service UUID check
      } else {
        // Name-based mode: Check service UUID in advertisement
        switch(sensor.type) {
          case SENSOR_TYPE_HEART_RATE:
            hasService = hasHR;
            break;
          case SENSOR_TYPE_SPEED_CADENCE:
            hasService = hasCSC;
            break;
          case SENSOR_TYPE_POWER_METER:
            hasService = hasPower;
            break;
          default:
            continue;
        }
      }
      
      if (hasService) {
        // v2.2.4: Store address and LOCK IT IN permanently!
        updateSensorMacFromAddress(sensor, addr);
        
        if (!sensor.hasResolvedAddress) {
          // First discovery - LOCK the address!
          sensor.hasResolvedAddress = true; // ← CRITICAL: Lock it in!
          
          // Update name to actual device name
          const char* deviceName = advertisedDevice->haveName() ? advertisedDevice->getName().c_str() : sensor.name;
          sensor.setName(deviceName);
        
          char macBuf[18];
          formatSensorMac(sensor, macBuf);
          BLE_LIB_LOGI_F("[SCAN] ✓ Address RESOLVED and LOCKED: %s → %s (type=%d, checks=%d)",
                 sensor.name, macBuf, sensor.type, sensor.nameCheckCount);
          
          // TODO: Optional - persist to NVS for reboot-survival
          // saveResolvedAddress(sensor.type, addr);
        } else {
          // Reconnection via MAC - already resolved
          char macBuf[18];
          formatSensorMac(sensor, macBuf);
          BLE_LIB_LOGI_F("[SCAN] ✓ Sensor found by MAC (already resolved): %s (type=%d)",
                 macBuf, sensor.type);
        }
        
BLE_LIB_LOGHOT_F("[SCAN] Will connect in loop...");
        
        // Set connection flag
        sensor.doConnect = true;
        
        // Don't stop scan - continue scanning for other sensors
        // Only stop if ALL sensors with targets are actually CONNECTED (not just found)
        // This ensures sensors are independent - one can connect while scan continues for others
        bool allSensorsConnected = true;
        for (int j = 0; j < 3; j++) {
          if (activeSensors[j].hasTarget && !activeSensors[j].isConnected) {
            allSensorsConnected = false;
          break;
        }
      }
        
        if (allSensorsConnected) {
BLE_LIB_LOGI_F("[SCAN] All sensors connected - stopping scan.");
          NimBLEDevice::getScan()->stop();
        } else {
BLE_LIB_LOGHOT_F("[SCAN] Sensor found, but continuing scan for other sensors...");
        }
        
        break; // Found match, exit loop
      }
    }
  }
};

MyAdvertisedDeviceCallbacks advertisedDeviceCallbacks;
NimBLEScan *pBLEScan = nullptr;

// Discovery scan callbacks
// Discovery scan callbacks - async pattern using onScanEnd (matching BLE_BikeSensorScan_Async.ino)
class DiscoveryScanCallbacks : public NimBLEScanCallbacks {
  void onScanEnd(const NimBLEScanResults &results, int reason) override {
    if (!g_discoveryRunning) return;
    
    BLE_LIB_LOGI_F(">>> onScanEnd, reason=%d", reason);
    
    // Clear previous results (char[] - no heap involved, just zero out)
    for (int i = 0; i < g_discoveredCount; i++) {
      g_discoveredSensors[i].clear();
    }
    g_discoveredCount = 0;
    
    int foundCount = results.getCount();
    BLE_LIB_LOGI_F("Scan fertig, Rohanzahl: %d", foundCount);
    
    // Process all results (matching async example lines 57-80)
    for (int i = 0; i < foundCount && g_discoveredCount < maxSensors && g_discoveredCount < MAX_DISCOVERED_SENSORS; ++i) {
      const NimBLEAdvertisedDevice* advPtr = results.getDevice(i);
      if (!advPtr) continue;
      const NimBLEAdvertisedDevice& adv = *advPtr;
      
      // Check for compiled services
#if BBS_COMPILE_HEART_RATE
      bool hasHR = adv.isAdvertisingService(HR_SERVICE_UUID);
#else
      bool hasHR = false;
#endif
#if BBS_COMPILE_CSC
      bool hasCSC = adv.isAdvertisingService(SPEED_CADENCE_SERVICE_UUID);
#else
      bool hasCSC = false;
#endif
#if BBS_COMPILE_POWER
      bool hasPWR = adv.isAdvertisingService(POWER_SERVICE_UUID);
#else
      bool hasPWR = false;
#endif
      
      // Only include if it matches the discovery type mask
      bool matchesMask = false;
      if ((g_discoveryTypeMask & SENSOR_TYPE_HEART_RATE) && hasHR) matchesMask = true;
      if ((g_discoveryTypeMask & SENSOR_TYPE_SPEED_CADENCE) && hasCSC) matchesMask = true;
      if ((g_discoveryTypeMask & SENSOR_TYPE_POWER_METER) && hasPWR) matchesMask = true;
      
      if (!matchesMask) continue;
      
      DiscoveredSensor d;
      d.clear();
      if (adv.haveName())
        d.setName(adv.getName().c_str());
      else
        d.setName("(kein Name)");
      
      // v2.2.4.7 FIX: toString() wird nur bei Discovery aufgerufen (User-Aktion, nicht Hot-Path)
      // This is acceptable because discovery is a rare user action, not continuous scanning
      d.setMac(adv.getAddress().toString().c_str());
      d.addrType = adv.getAddress().getType();  // Store BLE address type (0=Public, 1=Random)
      d.hasHR = hasHR;
      d.hasCSC = hasCSC;
      d.hasPWR = hasPWR;
      
      g_discoveredSensors[g_discoveredCount++] = d;
    }
    
    // Sort discovered sensors by name (in-place selection sort).
    // With char[] members (Phase 5), struct swap is a plain memcpy (~54 bytes)
    // with zero heap involvement - no need for the old index-array approach
    // that required a full sorted[MAX_DISCOVERED_SENSORS] temp array on the stack.
    for (int i = 0; i < g_discoveredCount - 1; ++i) {
      int minIdx = i;
      for (int j = i + 1; j < g_discoveredCount; ++j) {
        if (strcmp(g_discoveredSensors[j].name, g_discoveredSensors[minIdx].name) < 0) {
          minIdx = j;
        }
      }
      if (minIdx != i) {
        DiscoveredSensor tmp = g_discoveredSensors[i];
        g_discoveredSensors[i] = g_discoveredSensors[minIdx];
        g_discoveredSensors[minIdx] = tmp;
      }
    }
    
    g_discoveryRunning = false;
    g_discoveryDone = true;
    
    // CRITICAL FIX: Restore server-mode scan state after discovery ends
    if (pBLEScan) {
      // Restore callbacks (without this, AddSensor/sleep-scan won't find devices)
      pBLEScan->setScanCallbacks(&advertisedDeviceCallbacks);
      // Reset maxResults to 0 (= callbacks only, no result storage).
      // Discovery sets maxResults>0 to store results for onScanEnd().
      // Server/sleep-scan uses onResult() per device and must NOT store results,
      // otherwise the sleep-scan auto-stops after maxResults devices are found.
      pBLEScan->setMaxResults(0);
      BLE_LIB_LOGI_F("[DISCOVERY] Callbacks + maxResults(0) restored to server mode");
    }
  }
};

DiscoveryScanCallbacks discoveryScanCallbacks;

// State management functions
void saveServerModeState() {
  savedState.wasScanning = (pBLEScan != nullptr && pBLEScan->isScanning());
  savedState.wasAdvertising = (pAdvertising != nullptr && pAdvertising->isAdvertising());  // Save Virtual Server state
}

void restoreServerModeState() {
  // Note: scanning and advertising will be restored by resumeServerMode()
  // Sensor connections are managed by activeSensors array, not legacy variables
}

void pauseServerMode() {
  saveServerModeState();
  
  // Stop Virtual Sensor Server (Relay Server / pAdvertising) if running (per UML flowchart)
  if (pAdvertising) {
    pAdvertising->stop();
    delay(200); // Delay to ensure advertising stops
BLE_LIB_LOGI_F("[DISCOVERY] Stopped advertising (Virtual Sensor Server)");
  }
  
  // Stop current scan if active
  if (pBLEScan && pBLEScan->isScanning()) {
    pBLEScan->stop();
    delay(100); // Small delay to ensure scan stops
  }
  
  // Note: Sensor connections are managed by activeSensors array
  // They will continue to work during discovery mode
}

void resumeServerMode() {
  restoreServerModeState();
  
  // Restart Virtual Sensor Server (Relay Server / pAdvertising) if it was running before (per UML flowchart)
  if (savedState.wasAdvertising && pAdvertising) {
    pAdvertising->start(0); // 0 = advertise forever
    delay(200); // Delay to ensure advertising starts
BLE_LIB_LOGI_F("[DISCOVERY] Restarted advertising (Virtual Sensor Server)");
  }
  
  // Restore server mode scan callbacks
  if (pBLEScan) {
    pBLEScan->setScanCallbacks(&advertisedDeviceCallbacks);
  }
  
  // Check if any sensors need scanning (not just restore previous state)
  // This ensures sensors added during discovery will connect after discovery ends
  bool anySensorNeedsScan = false;
  for (int i = 0; i < 3; i++) {
    if (activeSensors[i].hasTarget && !activeSensors[i].isConnected) {
      // Sensor needs scan if: not connected AND (not found yet OR connection failed)
      // Phase 1: Check if sensor needs scan (no valid address stored)
      if (!activeSensors[i].doConnect ||
          (activeSensors[i].doConnect && !activeSensors[i].isConnected &&
           !activeSensors[i].hasMac)) {
        anySensorNeedsScan = true;
        break;
      }
    }
  }
  
  // Start scan if sensors need it OR if it was running before
  // Use startScan() for continuous scan (not 5-second scan)
  if (anySensorNeedsScan || savedState.wasScanning) {
    if (pBLEScan) {
      startScan();  // Use continuous scan instead of pBLEScan->start(5, false)
    } else {
      startScan();
    }
  }
}


void setupLocalBLEServer() {
  BLE_LIB_LOGI_F("[SERVER] setupLocalBLEServer called - enabledSensorTypes: 0x%02X (HR=%s, CSC=%s, Power=%s)", 
         enabledSensorTypes,
         (enabledSensorTypes & SENSOR_TYPE_HEART_RATE) ? "yes" : "no",
         (enabledSensorTypes & SENSOR_TYPE_SPEED_CADENCE) ? "yes" : "no",
         (enabledSensorTypes & SENSOR_TYPE_POWER_METER) ? "yes" : "no");
  
  server = NimBLEDevice::createServer();
  // Server callbacks are optional - we handle advertising restart manually

  // Create services only for compiled & enabled sensor types
#if BBS_COMPILE_HEART_RATE
  if (enabledSensorTypes & SENSOR_TYPE_HEART_RATE) {
    // Heart Rate Service
    hrServiceLocal = server->createService(HR_SERVICE_UUID);
    hrCharLocal = hrServiceLocal->createCharacteristic(HR_MEAS_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);
    // CCCD handled automatically in NimBLE
    hrServiceLocal->start();
BLE_LIB_LOGI_F("[SERVER] Heart Rate service created");
  }
#endif

#if BBS_COMPILE_CSC
  if (enabledSensorTypes & SENSOR_TYPE_SPEED_CADENCE) {
    // Speed/Cadence Service
    cscServiceLocal = server->createService(SPEED_CADENCE_SERVICE_UUID);
    cscCharLocal = cscServiceLocal->createCharacteristic(SPEED_CADENCE_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);
    // CCCD handled automatically in NimBLE
    cscServiceLocal->start();
BLE_LIB_LOGI_F("[SERVER] Speed/Cadence service created");
  }
#endif

#if BBS_COMPILE_POWER
  if (enabledSensorTypes & SENSOR_TYPE_POWER_METER) {
    // Power Meter Service
    powerServiceLocal = server->createService(POWER_SERVICE_UUID);
    powerCharLocal = powerServiceLocal->createCharacteristic(POWER_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);
    // CCCD handled automatically in NimBLE
    powerServiceLocal->start();
BLE_LIB_LOGI_F("[SERVER] Power Meter service created");
  }
#endif

  BLEAdvertisingManagerSetupInitialAdvertising(HR_SERVICE_UUID, SPEED_CADENCE_SERVICE_UUID, POWER_SERVICE_UUID);
  
  // Build service list string for logging (registry-based)
  char serviceList[64] = "";
  int len = 0;
  for (int i = 0; i < SENSOR_REGISTRY_COUNT; i++) {
    if (enabledSensorTypes & SENSOR_REGISTRY[i].typeBit) {
      int written = snprintf(serviceList + len, sizeof(serviceList) - len,
                             len > 0 ? ", %s" : "%s", SENSOR_REGISTRY[i].shortName);
      if (written > 0) len += written;
      if (len >= static_cast<int>(sizeof(serviceList))) { len = sizeof(serviceList) - 1; break; }
    }
  }
  
  BLE_LIB_LOGI_F("[SERVER] Local BLE server advertising (%s services).", serviceList[0] ? serviceList : "none");
  BLE_LIB_LOGI_F("[SERVER] Enabled sensor types: 0x%02X", enabledSensorTypes);
  ::printVirtualDeviceState();  // Call function in global scope
}

// Legacy function name for backward compatibility
void setupLocalHRServer() {
  setupLocalBLEServer();
}

// Connect and subscribe to a specific sensor type (NimBLE 2.x name-based workflow)
bool connectAndSubscribeSensor(SensorType type) {
  int index = getSensorIndex(type);
  if (index < 0) return false;
  
  ActiveSensor &sensor = activeSensors[index];
  if (!sensor.hasTarget) return false;
  
  // Check if connection was triggered by scan
  if (!sensor.doConnect && !sensor.isConnected) {
    return false; // Wait for scan to find device
  }
  
  // Clear doConnect flag
  sensor.doConnect = false;
  
  // Radio stabilization delay (500ms after scan stop)
  // Sleep Scan Mode: kürzeres Delay da Scan bereits durch NimBLE gestoppt wurde
  if (g_connectionMode == BBS_MODE_MAC) {
    delay(100);  // Kurzes Delay für Sleep Scan
  } else {
    delay(500);  // Volles Delay für kontinuierlichen Scan
  }
  
  // Create client if needed (each sensor gets its own client)
  // v2.2.5: Client is created ONCE and reused - never deleted during normal operation
  if (!sensor.client) {
    sensor.client = NimBLEDevice::createClient();
    // Set client callbacks (NimBLE 2.x)
    // v2.2.5: deleteCallbacks=false because we use static callbacks (allocated once, never freed)
    // This eliminates heap allocations from repeated callback creation
    sensor.client->setClientCallbacks(BLEConnectionManagerCreateClientCallbacks(type), false);
    // Set connection parameters BEFORE connect (NimBLE 2.x requirement)
    sensor.client->setConnectionParams(12, 24, 0, 200);
    
    // Sleep Scan Mode: Kurzer Connect-Timeout (5s statt 30s)
    // MAC wurde gerade im Scan gefunden, also sollte Connect schnell gehen
    if (g_connectionMode == BBS_MODE_MAC) {
      sensor.client->setConnectTimeout(5000);  // 5 Sekunden Timeout
      BLE_LIB_LOGI_F("[CLIENT] Created client with 5s connect timeout (Sleep Scan mode)");
    }
    
    BLE_LIB_LOGI_F("[CLIENT] Created client for %s sensor (reusable, static callbacks).", getSensorTypeName(type));
  }
  
  // Disconnect first if already connected to a different device
  if (sensor.client->isConnected()) {
    NimBLEAddress currentAddr = sensor.client->getPeerAddress();
    if (currentAddr != sensor.address) {
BLE_LIB_LOGI_F("[CLIENT] Disconnecting from previous device...");
      sensor.client->disconnect();
      delay(500); // Give it time to disconnect
    } else {
      // Already connected to correct device
      sensor.isConnected = true;
      return true;
    }
  }
  
  // Connect if not already connected
  if (!sensor.client->isConnected()) {
    const char* sensorTypeStr = getSensorTypeName(type);
    char macBuf[18];
    formatSensorMac(sensor, macBuf);
    BLE_LIB_LOGI_F("[MAIN] Connecting to %s (%s, type=%d) - %s sensor...",
           sensor.name, macBuf, sensor.addrType, sensorTypeStr);
    
    uint32_t connectStart = millis();
    bool connected = false;
    
    // Connect using MAC address directly if useMacDirect is true, otherwise use advertised device
    if (sensor.useMacDirect && sensor.hasMac) {
      // Direct MAC address connection
      BLE_LIB_LOGI_F("[CLIENT] Connecting directly by MAC address: %s", macBuf);
      connected = sensor.client->connect(sensor.address);
    } else if (sensor.hasMac) {
      // Phase 1 Fix: Connect using stored address directly (no advertisedDevice pointer needed)
      BLE_LIB_LOGI_F("[CLIENT] Connecting using stored address: %s (type: %d)",
                     macBuf, sensor.addrType);
      connected = sensor.client->connect(sensor.address);
    } else {
      BLE_LIB_LOGE_F("[CLIENT] Error: No MAC address stored!");
      sensor.doConnect = false;
      if (!sensor.useMacDirect) {
        startScan();
      }
      return false;
    }
    
    uint32_t connectTime = millis() - connectStart;
    
    if (!connected) {
      BLE_LIB_LOGE_F("[CLIENT] Connection failed for %s sensor after %lu ms.", sensorTypeStr, connectTime);
      BLE_LIB_LOGI_F("[CLIENT] Will retry...");
      BLE_LIB_LOGHOT_F("Connection failed.");
      sensor.isConnected = false;
      
      // Telemetry: Record connection failure
      BLETelemetry::recordConnectionResult(false);
      
      // v2.2.5 FIX: DO NOT delete client on connection failure!
      // Keep the client for reuse to prevent heap fragmentation.
      // The client will be reused on next connection attempt.
      // Clear only transient state, not the client itself.
      sensor.characteristic = nullptr;
      
      // Restart scan only if not using direct MAC connection
      if (!sensor.useMacDirect) {
        startScan();
      } else if (g_connectionMode != BBS_MODE_MAC) {
        // MAC-based sensor in Scan mode: retry connection after interval
        sensor.doConnect = true;
      }
      // Sleep Scan mode: doConnect bleibt false - nächster Sleep Scan wird in Loop gestartet
      return false;
    }
    
    // Telemetry: Record connection success
    BLETelemetry::recordConnectionResult(true);
    
    BLE_LIB_LOGI_F("[CLIENT] Connection successful to %s sensor (took %lu ms).", sensorTypeStr, connectTime);
  }
  
  sensor.isConnected = sensor.client->isConnected();
  BLE_LIB_LOGHOT_F("Connected to sensor: %s", sensor.name);
  
  // Get the appropriate service config from registry
  const SensorTypeConfig* cfg = getSensorConfig(type);
  if (!cfg) return false;

  NimBLEUUID serviceUUID((uint16_t)cfg->serviceUUID16);
  NimBLEUUID charUUID((uint16_t)cfg->charUUID16);
  auto parseFunc = cfg->parseFunc;
  auto mirrorFunc = cfg->mirrorFunc;
  
  // Small delay after connection to ensure service discovery can complete
  delay(100);
  
  // Get service - add delay and retry like working script
  const char* serviceName = cfg->serviceName;
  const char* sensorTypeStr = cfg->shortName;
  BLE_LIB_LOGI_F("[CLIENT] Searching for service %s...", serviceName);
  
  NimBLERemoteService *svc = sensor.client->getService(serviceUUID);
  if (!svc) {
    BLE_LIB_LOGE_F("[ERROR] %s Service not found, disconnecting.", sensorTypeStr);
BLE_LIB_LOGHOT_F("Service not found, disconnecting.");
    sensor.client->disconnect();
    sensor.isConnected = false;
    return false;
  }
  
  BLE_LIB_LOGI_F("[CLIENT] %s Service found.", sensorTypeStr);
  
  // Get characteristic
  sensor.characteristic = svc->getCharacteristic(charUUID);
  if (!sensor.characteristic) {
    BLE_LIB_LOGE_F("[ERROR] %s Characteristic not found, disconnecting.", sensorTypeStr);
    sensor.client->disconnect();
    sensor.isConnected = false;
    return false;
  }
  
  if (!sensor.characteristic->canNotify()) {
    BLE_LIB_LOGE_F("[ERROR] %s Characteristic cannot notify, disconnecting.", sensorTypeStr);
    sensor.client->disconnect();
    sensor.isConnected = false;
    return false;
  }
  
  BLE_LIB_LOGI_F("[CLIENT] %s Characteristic found and can notify.", sensorTypeStr);
  
  // Subscribe to notifications (NimBLE 2.x - subscribe() handles CCCD automatically)
  BLE_LIB_LOGI_F("[CLIENT] Subscribing to notifications for %s sensor...", sensorTypeStr);
  
  auto notifyCallback = [parseFunc, mirrorFunc](NimBLERemoteCharacteristic* c, uint8_t* data, size_t len, bool isNotify) {
    (void)c;
    (void)isNotify;
    if (parseFunc) parseFunc(data, len);
    if (mirrorFunc) mirrorFunc(data, len);
  };
  
  if (!sensor.characteristic->subscribe(true, notifyCallback, true)) {
    BLE_LIB_LOGE_F("[ERROR] Failed to subscribe to notifications for %s sensor.", sensorTypeStr);
    sensor.client->disconnect();
    sensor.isConnected = false;
    return false;
  }
  
  BLE_LIB_LOGI_F("[CLIENT] Notifications subscribed for %s sensor.", sensorTypeStr);
  
  BLE_LIB_LOGHOT_F("Notifications subscribed for sensor type: %s", sensorTypeStr);
  
  // Check for Battery Service (0x180F) and read Battery Level (0x2A19)
  BLE_LIB_LOGI_F("[CLIENT] Checking for Battery Service on %s sensor...", sensorTypeStr);
  
  NimBLEUUID batteryServiceUUID((uint16_t)0x180F);
  NimBLEUUID batteryLevelCharUUID((uint16_t)0x2A19);
  
  NimBLERemoteService *batterySvc = sensor.client->getService(batteryServiceUUID);
  if (batterySvc) {
    sensor.batteryCharacteristic = batterySvc->getCharacteristic(batteryLevelCharUUID);
    if (sensor.batteryCharacteristic) {
      if (sensor.batteryCharacteristic->canRead()) {
        // v2.2.16: Stack-buffer statt std::string (eliminiert Heap-Allokation)
        // Battery Level ist immer genau 1 Byte (0-100%)
        NimBLEAttValue val = sensor.batteryCharacteristic->readValue();
        if (val.length() >= 1) {
          sensor.batteryLevel = val[0];
          sensor.hasBatteryService = true;
          BLE_LIB_LOGI_F("[CLIENT] Battery level: %u%%", sensor.batteryLevel);
        } else {
BLE_LIB_LOGW_F("[CLIENT] Battery level read returned empty value");
          sensor.hasBatteryService = false;
          sensor.batteryLevel = 255;
        }
      } else {
BLE_LIB_LOGW_F("[CLIENT] Battery Level characteristic cannot be read");
        sensor.hasBatteryService = false;
        sensor.batteryLevel = 255;
      }
    } else {
BLE_LIB_LOGW_F("[CLIENT] Battery Level characteristic not found");
      sensor.hasBatteryService = false;
      sensor.batteryLevel = 255;
    }
  } else {
    BLE_LIB_LOGHOT_F("[CLIENT] Battery Service not available");
    sensor.hasBatteryService = false;
    sensor.batteryLevel = 255;
  }
  
  // Reset reconnection failure count on successful connection
  sensor.reconnectFailureCount = 0;
  
  // Call per-type callback
  if (sensorConnectedCallbacks[index]) {
    sensorConnectedCallbacks[index]();
  }
  
  return true;
}

} // namespace - close before scan functions (they need external linkage for BLEScanManager)

// Scan functions in global scope (called from BLEScanManager.cpp via extern)
void startScan() {
  // Safety check: Don't start scan if BLE is not initialized (e.g., during restart)
  if (!isInitialised) {
BLE_LIB_LOGW_F("[SCAN] Cannot start scan - BLE not initialized");
    return;
  }
  
  // Don't start scan if discovery is running
  if (g_discoveryRunning) {
BLE_LIB_LOGW_F("[SCAN] Cannot start scan - discovery is running");
    return;
  }
  
  if (!pBLEScan) {
    pBLEScan = NimBLEDevice::getScan();
    BLEScanManager::configureScanParameters(pBLEScan, BLEScanManager::SCAN_MODE_SERVER);
    pBLEScan->setMaxResults(0);  // Callbacks only, no result storage (prevents auto-stop)
    BLETelemetry::setScanDuplicateFilter(true);
  }
  
  // CRITICAL FIX: ALWAYS ensure callbacks are set to server mode (not discovery mode)
  // This must happen BEFORE the isScanning() check to fix race condition after discovery
  // Without this, callbacks may remain on discoveryScanCallbacks after discovery ends
  pBLEScan->setScanCallbacks(&advertisedDeviceCallbacks);
  
  // Check if scan is already running - don't restart if it is
  if (pBLEScan->isScanning()) {
    BLE_LIB_LOGHOT_F("[SCAN] Scan already running, callbacks restored");
    return;
  }
  
  pBLEScan->start(0, false);  // 0 = scan forever (like working example)
BLE_LIB_LOGI_F("[SCAN] Started scanning for sensors (HR, CSC, Power)...");
  
  // Telemetry: Record scan start
  BLETelemetry::recordScanStart();
}

// Sleep Scan: Scan alle 30 Sekunden für 2 Sekunden
// Nutzt EXAKT die gleiche Scan-Logik wie startScan() - nur mit Timer-basiertem Start/Stop
void startSleepScan() {
  if (!isInitialised) return;
  if (g_discoveryRunning) return;
  
  unsigned long now = millis();
  
  // Prüfe ob Scan bereits läuft
  if (pBLEScan && pBLEScan->isScanning()) {
    // Prüfe ob Scan-Dauer abgelaufen ist
    if (now - g_lastSleepScanMs >= SLEEP_SCAN_DURATION_MS) {
      pBLEScan->stop();
      pBLEScan->clearResults();  // v2.2.12: Prevent scan result accumulation
      BLE_LIB_LOGI_F("[SLEEP-SCAN] Scan stopped after %lu ms (results cleared)", SLEEP_SCAN_DURATION_MS);
      BLETelemetry::recordScanStop();
      return;
    }
    return;  // Scan läuft noch
  }
  
  // Prüfe ob Intervall abgelaufen ist (oder erster Start mit g_lastSleepScanMs == 0)
  if (g_lastSleepScanMs != 0 && (now - g_lastSleepScanMs < SLEEP_SCAN_INTERVAL_MS)) {
    return;  // Noch nicht Zeit für nächsten Scan
  }
  
  // Starte Scan - EXAKT wie startScan() (der funktionierende normale Modus)
  if (!pBLEScan) {
    pBLEScan = NimBLEDevice::getScan();
    BLEScanManager::configureScanParameters(pBLEScan, BLEScanManager::SCAN_MODE_SERVER);
    pBLEScan->setMaxResults(0);  // Callbacks only, no result storage (prevents auto-stop)
    BLETelemetry::setScanDuplicateFilter(true);
  }
  
  pBLEScan->setScanCallbacks(&advertisedDeviceCallbacks);
  
  // start(0, false) = scan forever, wir stoppen manuell nach SLEEP_SCAN_DURATION_MS
  bool scanStarted = pBLEScan->start(0, false);
  if (scanStarted) {
    g_lastSleepScanMs = now;
    BLE_LIB_LOGI_F("[SLEEP-SCAN] Started %lu ms scan", SLEEP_SCAN_DURATION_MS);
    BLETelemetry::recordScanStart();
  } else {
    BLE_LIB_LOGE_F("[SLEEP-SCAN] Failed to start scan");
  }
}

// Discovery asynchron starten (matching BLE_BikeSensorScan_Async.ino pattern)
void startDiscoveryScan(uint8_t sensorTypeMask) {
  if (g_discoveryRunning) return; // Already scanning
  
  // Pause server mode (stops Virtual Server if running)
  pauseServerMode();
  
  // Get scan instance
  if (!pBLEScan) {
    pBLEScan = NimBLEDevice::getScan();
  }
  
  // Stop any existing scan and clear results (async example pattern - line 135-136)
  pBLEScan->stop();
  delay(100); // Small delay to ensure stop completes
  pBLEScan->clearResults();
  
  // Set discovery callbacks
  pBLEScan->setScanCallbacks(&discoveryScanCallbacks);
  
  // Configure discovery scan parameters (faster interval/window)
  BLEScanManager::configureScanParameters(pBLEScan, BLEScanManager::SCAN_MODE_DISCOVERY);
  BLETelemetry::setScanDuplicateFilter(true);
  // Store unlimited results (0xFF) so the scan runs for the full duration.
  // Using maxSensors here would auto-stop the scan after that many BLE devices
  // are found, potentially missing the target sensor in crowded environments.
  // The actual sensor-type filtering happens in onScanEnd().
  pBLEScan->setMaxResults(0xFF);
  
  // Set discovery type mask
  g_discoveryTypeMask = sensorTypeMask;
  g_discoveryDone = false;
  g_discoveryRunning = true;
  
  BLE_LIB_LOGI_F("Starte Discovery für %lu Sekunden (async)...", discoveryDuration / 1000);
  
  // Start async scan (matching async example line 147: duration_ms, isContinue=false, restart=true)
  pBLEScan->start(discoveryDuration, false, true);
  
  // Telemetry: Record discovery start
  BLETelemetry::recordDiscoveryStart();
}

// Manual stop function for discovery (if needed)
void stopDiscoveryScan() {
  if (!g_discoveryRunning) {
    return;
  }
  
  // Stop the scan
  if (pBLEScan && pBLEScan->isScanning()) {
    pBLEScan->stop();
  }
  
  // The onScanEnd callback will handle cleanup
}

// Debug function to print virtual device and server state
void printVirtualDeviceState() {
#if defined(BLE_LIB_DEBUG_HOT)
  BLE_LIB_LOGHOT_F("=== Virtual Device Server State ===");
  
  // Server state
  if (server) {
    BLE_LIB_LOGHOT_F("Server: EXISTS | Connected centrals: %u", server->getConnectedCount());
  } else {
    BLE_LIB_LOGHOT_F("Server: NULL");
  }
  
#if BBS_COMPILE_HEART_RATE
  // HR Service state
  if (hrServiceLocal) {
    BLE_LIB_LOGHOT_F("HR Service: EXISTS | Characteristic: %s", hrCharLocal ? "EXISTS" : "NULL");
  } else {
    BLE_LIB_LOGHOT_F("HR Service: NULL");
  }
#endif
  
#if BBS_COMPILE_CSC
  // CSC Service state
  if (cscServiceLocal) {
    BLE_LIB_LOGHOT_F("CSC Service: EXISTS | Characteristic: %s", cscCharLocal ? "EXISTS" : "NULL");
  } else {
    BLE_LIB_LOGHOT_F("CSC Service: NULL");
  }
#endif
  
#if BBS_COMPILE_POWER
  // Power Service state
  if (powerServiceLocal) {
    BLE_LIB_LOGHOT_F("Power Service: EXISTS | Characteristic: %s", powerCharLocal ? "EXISTS" : "NULL");
  } else {
    BLE_LIB_LOGHOT_F("Power Service: NULL");
  }
#endif
  
  // Advertising state
  if (pAdvertising) {
    BLE_LIB_LOGHOT_F("Advertising: ACTIVE | Name: '%s'", advertisingName.c_str());
  } else {
    BLE_LIB_LOGHOT_F("Advertising: NULL");
  }
  
  BLE_LIB_LOGHOT_F("===================================");
#endif
}

// Note: isInitialised is declared earlier so callbacks can access it
// Note: Anonymous namespace closed before startScan() at line ~1180

bool BluetoothBikeSensorServerIsStackInitialized() {
  return s_stackInitialised;
}

void BluetoothBikeSensorServerInitStack(const char *deviceName,
                                        uint8_t enabledSensorTypesParam,
                                        BBSConnectionMode connectionMode,
                                        esp_power_level_t txPower) {
  if (s_stackInitialised) {
    return;
  }

#if BBS_ENABLE_INIT_HEAP_LOG
  bbsInitHeapLog("BEGIN:STACK:ENTER");
#endif

  // v2.2.5: Set connection mode FIRST (before any scan logic)
  g_connectionMode = connectionMode;
  BLE_LIB_LOGI_F("[INIT] Connection mode: %s",
                 connectionMode == BBS_MODE_MAC ? "SLEEP_SCAN (MAC-based)" : "CONTINUOUS_SCAN (name-based)");

  // Store enabled sensor types FIRST, before any other operations
  enabledSensorTypes = enabledSensorTypesParam;
  BLE_LIB_LOGI_F("[INIT] Enabled sensor types parameter: 0x%02X", enabledSensorTypesParam);
  BLE_LIB_LOGI_F("[INIT] Enabled sensor types stored: 0x%02X (HR=%s, CSC=%s, Power=%s)",
                 enabledSensorTypes,
                 (enabledSensorTypes & SENSOR_TYPE_HEART_RATE) ? "yes" : "no",
                 (enabledSensorTypes & SENSOR_TYPE_SPEED_CADENCE) ? "yes" : "no",
                 (enabledSensorTypes & SENSOR_TYPE_POWER_METER) ? "yes" : "no");

  if (!configCaptured) {
    initialTxPower = txPower;
    configCaptured = true;
  }

  // Use MAC-based identity as default (bbs::identity::GetBleDeviceNameLower())
  // Device name parameter is now used only for setting the prefix if provided
  if (deviceName && strlen(deviceName) > 0) {
    // If device name provided, use it as prefix (will be converted to lowercase)
    bbs::identity::SetDeviceNamePrefix(deviceName);
  }
  // Get MAC-based device name
  // v2.2.4.7: GetBleDeviceNameLower() returns const String& - use reference to avoid copy
  const String& macBasedName = bbs::identity::GetBleDeviceNameLower();
  advertisingName = macBasedName.c_str();

#if BBS_ENABLE_INIT_HEAP_LOG
  bbsInitHeapLog("BEGIN:STACK:BEFORE_NIMBLE_INIT");
#endif

  NimBLEDevice::init(advertisingName.c_str());
  NimBLEDevice::setPower(txPower);

#if BBS_ENABLE_INIT_HEAP_LOG
  bbsInitHeapLog("BEGIN:STACK:AFTER_NIMBLE_INIT");
#endif

  // Initialize connection manager context and active sensors array
  BLEConnectionManagerContext connectionContext = {
    &isInitialised,
    &preserveMacOnDisconnect,
    startScan,
    sensorDisconnectedCallbacks
  };
  BLEConnectionManagerSetContext(connectionContext);
  initializeActiveSensors();

  SensorDataParserContext parserContext = {
#if BBS_COMPILE_HEART_RATE
    &lastHeartRate,
    &heartRateValid,
    &lastHeartRateTimestamp,
    &heartRateCallback,
#endif
#if BBS_COMPILE_CSC
    &speedCadenceData,
    &speedCadenceCallback,
#endif
#if BBS_COMPILE_POWER
    &powerData,
    &powerCallback
#endif
  };
  SensorDataParserSetContext(parserContext);

  BLEAdvertisingManagerContext advertisingContext = {
    &pAdvertising,
    &advertisingName,
    &enabledSensorTypes
  };
  BLEAdvertisingManagerSetContext(advertisingContext);

#if BBS_ENABLE_INIT_HEAP_LOG
  bbsInitHeapLog("BEGIN:STACK:EXIT");
#endif

  s_stackInitialised = true;
}

bool BluetoothBikeSensorServerStartServer() {
  if (isInitialised) {
    return true;
  }
  if (!s_stackInitialised) {
    BLE_LIB_LOGE_F("[INIT] Cannot start server: NimBLE stack not initialized. Call BluetoothBikeSensorServerInitStack() first.");
    return false;
  }

#if BBS_ENABLE_INIT_HEAP_LOG
  bbsInitHeapLog("BEGIN:SERVER:ENTER");
#endif

  // Now setup server - this will use the enabledSensorTypes we already set
  setupLocalHRServer();

#if BBS_ENABLE_INIT_HEAP_LOG
  bbsInitHeapLog("BEGIN:SERVER:AFTER_SETUP_SERVER");
#endif

  // Initialize scan manager (Phase 1 - Task 2.1)
  BLEScanManager::begin();

#if BBS_ENABLE_INIT_HEAP_LOG
  bbsInitHeapLog("BEGIN:SERVER:AFTER_SCANMANAGER");
#endif

  isInitialised = true;

  // Initialize telemetry system (Phase 0)
  BLETelemetry::begin();

#if BBS_ENABLE_INIT_HEAP_LOG
  bbsInitHeapLog("BEGIN:SERVER:EXIT");
#endif

  return true;
}

void BluetoothBikeSensorServerBegin(const char *deviceName,
                                   uint8_t enabledSensorTypesParam,
                                   BBSConnectionMode connectionMode,
                                   esp_power_level_t txPower) {
  if (isInitialised) {
    return;
  }

  // If stack isn't initialized yet, do Phase 1 now.
  if (!s_stackInitialised) {
    BluetoothBikeSensorServerInitStack(deviceName, enabledSensorTypesParam, connectionMode, txPower);
  } else {
    // Stack already initialized: still update enabled sensor types and connection mode to match intent
    // (This only affects later server start; runtime changes should use BikeSensorServerSetEnabledSensorTypes.)
    g_connectionMode = connectionMode;
    enabledSensorTypes = enabledSensorTypesParam;
  }

  // Phase 2: start the server/services/advertising
  (void)BluetoothBikeSensorServerStartServer();
}

void BikeSensorServerSetEnabledSensorTypes(uint8_t newEnabledSensorTypes) {
  if (!isInitialised || !server) {
BLE_LIB_LOGE_F("[DYNAMIC] Cannot change sensor types - server not initialized");
    return;
  }

  // Check if anything actually changed
  if (enabledSensorTypes == newEnabledSensorTypes) {
BLE_LIB_LOGI_F("[DYNAMIC] Sensor types unchanged, no action needed");
    return;
  }

  BLE_LIB_LOGI_F("[DYNAMIC] Changing enabled sensor types from 0x%02X to 0x%02X", 
         enabledSensorTypes, newEnabledSensorTypes);

  // Disconnect all connected centrals first (forces them to rediscover)
  uint8_t connectedCount = server->getConnectedCount();
  if (connectedCount > 0) {
    BLE_LIB_LOGI_F("[DYNAMIC] Disconnecting %u central device(s) to force rediscovery", connectedCount);
    // Get all connected clients and disconnect them
    // Note: NimBLE doesn't provide direct access to all clients, but we can stop advertising
    // and the centrals will eventually disconnect. However, we can also just update and they'll
    // need to reconnect anyway.
    
    // Stop advertising temporarily
    if (pAdvertising && pAdvertising->isAdvertising()) {
      pAdvertising->stop();
      delay(100);
    }
  }

  // Update enabled sensor types
  uint8_t oldEnabledSensorTypes = enabledSensorTypes;
  enabledSensorTypes = newEnabledSensorTypes;

  // Create services that are newly enabled (if they don't exist)
#if BBS_COMPILE_HEART_RATE
  if ((enabledSensorTypes & SENSOR_TYPE_HEART_RATE) && !hrServiceLocal) {
    hrServiceLocal = server->createService(HR_SERVICE_UUID);
    hrCharLocal = hrServiceLocal->createCharacteristic(HR_MEAS_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);
    hrServiceLocal->start();
BLE_LIB_LOGI_F("[DYNAMIC] Heart Rate service created");
  }
#endif

#if BBS_COMPILE_CSC
  if ((enabledSensorTypes & SENSOR_TYPE_SPEED_CADENCE) && !cscServiceLocal) {
    cscServiceLocal = server->createService(SPEED_CADENCE_SERVICE_UUID);
    cscCharLocal = cscServiceLocal->createCharacteristic(SPEED_CADENCE_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);
    cscServiceLocal->start();
BLE_LIB_LOGI_F("[DYNAMIC] Speed/Cadence service created");
  }
#endif

#if BBS_COMPILE_POWER
  if ((enabledSensorTypes & SENSOR_TYPE_POWER_METER) && !powerServiceLocal) {
    powerServiceLocal = server->createService(POWER_SERVICE_UUID);
    powerCharLocal = powerServiceLocal->createCharacteristic(POWER_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);
    powerServiceLocal->start();
BLE_LIB_LOGI_F("[DYNAMIC] Power Meter service created");
  }
#endif

  // Note: Services that are no longer enabled will remain on the server but won't be advertised.
  // This is acceptable because:
  // - New connections won't see them in advertising
  // - Existing connections are being disconnected
  // - The services won't receive data if no sensor is connected

  // Update advertising to reflect new enabled services
  BLEAdvertisingManagerUpdateServices(true, HR_SERVICE_UUID, SPEED_CADENCE_SERVICE_UUID, POWER_SERVICE_UUID);

  BLE_LIB_LOGI_F("[DYNAMIC] Sensor types updated successfully. New types: 0x%02X (HR=%s, CSC=%s, Power=%s)", 
         enabledSensorTypes,
         (enabledSensorTypes & SENSOR_TYPE_HEART_RATE) ? "yes" : "no",
         (enabledSensorTypes & SENSOR_TYPE_SPEED_CADENCE) ? "yes" : "no",
         (enabledSensorTypes & SENSOR_TYPE_POWER_METER) ? "yes" : "no");
BLE_LIB_LOGI_F("[DYNAMIC] Central devices will need to reconnect to see updated services");
}

void BluetoothBikeSensorServerLoop() {
  if (!isInitialised) {
    return;
  }
  
  // Start loop timing measurement (Phase 0 Telemetry)
  uint32_t loopStartTime = BLETelemetry::startLoopMeasurement();

  unsigned long now = millis();

  // Heartbeat - print "." every 1000ms (matching async example lines 154-159)
  static unsigned long lastBeat = 0;
  if (now - lastBeat >= 1000) {
    BLE_LIB_LOGHOT_F(".");
    lastBeat = now;
  }

  // Handle discovery mode - async pattern (matching async example lines 162-185)
  if (g_discoveryDone) {
    g_discoveryDone = false;

#if defined(BLE_LIB_DEBUG_GEN)
    BLE_LIB_LOGI_F("");
    BLE_LIB_LOGI_F("");
    BLE_LIB_LOGI_F("=== Gefundene Sensoren (NAME | ART | MAC) ===");

    if (g_discoveredCount == 0) {
      BLE_LIB_LOGI_F("(keine HR/CSC/Power Sensoren gefunden)");
    } else {
      BLE_LIB_LOGI_F("-----------------------------------------------");
      for (int i = 0; i < g_discoveredCount; ++i) {
        // v2.2.4.7: Build ART string using stack buffer (eliminates String allocations)
        char artBuf[16] = "";
        size_t pos = 0;
        if (g_discoveredSensors[i].hasHR) {
          if (pos > 0) { pos += snprintf(artBuf + pos, sizeof(artBuf) - pos, "+"); }
          pos += snprintf(artBuf + pos, sizeof(artBuf) - pos, "HR");
        }
        if (g_discoveredSensors[i].hasCSC) {
          if (pos > 0) { pos += snprintf(artBuf + pos, sizeof(artBuf) - pos, "+"); }
          pos += snprintf(artBuf + pos, sizeof(artBuf) - pos, "CSC");
        }
        if (g_discoveredSensors[i].hasPWR) {
          if (pos > 0) { pos += snprintf(artBuf + pos, sizeof(artBuf) - pos, "+"); }
          pos += snprintf(artBuf + pos, sizeof(artBuf) - pos, "PWR");
        }
        if (pos == 0) {
          strcpy(artBuf, "-");
        }
        
        BLE_LIB_LOGI_F("%s | %s | %s", g_discoveredSensors[i].name, artBuf, g_discoveredSensors[i].mac);
      }
      BLE_LIB_LOGI_F("-----------------------------------------------");
    }
    BLE_LIB_LOGI_F("Discovery beendet.");
#endif

    // Telemetry: Record discovery complete
    BLETelemetry::recordDiscoveryComplete(g_discoveredCount);

    // Call completion callback
    if (g_discoveryCompleteCallback) {
      g_discoveryCompleteCallback(g_discoveredCount);
    }

    // Restore server mode (resumes Virtual Server if it was running)
    resumeServerMode();
  }

  // Skip server mode operations during discovery
  if (g_discoveryRunning) {
    return;
  }

  if (g_connectionMode == BBS_MODE_MAC) {
    // Sleep Scan Mode: Scan alle 30 Sekunden für 2 Sekunden
    // Nutzt bestehende Scan-Infrastruktur und MAC-Matching-Logik
    
    // Prüfe ob alle Sensoren connected sind
    bool allSensorsConnected = true;
    bool anySensorNeedsScan = false;
    for (int i = 0; i < 3; i++) {
      if (activeSensors[i].hasTarget) {
        if (!activeSensors[i].isConnected) {
          allSensorsConnected = false;
          anySensorNeedsScan = true;
        }
      }
    }
    
    // Wenn alle connected → Scan stoppen und Timer zurücksetzen
    if (allSensorsConnected) {
      if (pBLEScan && pBLEScan->isScanning()) {
        pBLEScan->stop();
        pBLEScan->clearResults();  // v2.2.12: Prevent scan result accumulation
        BLE_LIB_LOGI_F("[SLEEP-SCAN] All sensors connected - stopping scan (results cleared).");
        BLETelemetry::recordScanStop();
      }
      g_lastSleepScanMs = 0;  // Reset für schnellen Reconnect bei Disconnect
    } else if (anySensorNeedsScan) {
      // Sleep Scan starten (prüft selbst ob Zeit gekommen ist)
      startSleepScan();
    }
  } else {
    // Check if scan should be running - ensure it runs as long as ANY sensor needs it
    // Sensors are independent - scan continues even if one is already connected
    bool anySensorNeedsScan = false;
    bool allSensorsConnected = true;
    for (int i = 0; i < 3; i++) {
      if (activeSensors[i].hasTarget) {
        if (!activeSensors[i].isConnected) {
          allSensorsConnected = false;
          // Sensor needs scan if: not connected AND (not found yet OR connection failed)
          // Phase 1: Check if sensor needs scan
          if (!activeSensors[i].doConnect ||
              (activeSensors[i].doConnect && !activeSensors[i].isConnected &&
               !activeSensors[i].hasMac)) {
            anySensorNeedsScan = true;
          }
        }
      }
    }
    
    // Keep scan running if any sensor needs it (independent operation)
    if (anySensorNeedsScan) {
      if (!pBLEScan || !pBLEScan->isScanning()) {
BLE_LIB_LOGI_F("[LOOP] Sensor(s) need scanning - starting/restarting scan...");
        startScan();
      }
    } else if (allSensorsConnected && pBLEScan && pBLEScan->isScanning()) {
      // All sensors connected - can stop scan to save power
      pBLEScan->stop();
      pBLEScan->clearResults();  // v2.2.12: Prevent scan result accumulation
      BLE_LIB_LOGI_F("[LOOP] All sensors connected - stopping scan (results cleared).");
      BLETelemetry::recordScanStop();
    }
  }
  
  // Note: Relay server should remain advertising even when all sensors are connected
  // so that central devices (bike computers) can connect to receive sensor data.
  // Auto-stop functionality removed as it breaks the relay functionality.

  // Handle connections for all sensor types
  for (int i = 0; i < 3; i++) {
    ActiveSensor &sensor = activeSensors[i];
    
    // Check if scan found device and triggered connection
    if (sensor.hasTarget && sensor.doConnect && !sensor.isConnected) {
      BLE_LIB_LOGI_F("[LOOP] Attempting connection for sensor type %d (name: '%s')...", i, sensor.name);
      // Try to connect
      if (!connectAndSubscribeSensor(getSensorTypeFromIndex(i))) {
        // Connection failed
        sensor.reconnectFailureCount++;
        BLE_LIB_LOGW_F("[LOOP] Connection failed for sensor type: %d (failure count: %u)", i, sensor.reconnectFailureCount);
        
        // Telemetry: Record reconnect attempt
        BLETelemetry::recordReconnectAttempt();
        
        // If Sensor MAC Preservation is enabled, only fall back to scan after multiple failures
        // This allows for transient connection issues without losing the sensor MAC
        const uint8_t maxFastReconnectAttempts = preserveMacOnDisconnect ? 3 : 1;
        
        // Sleep Scan Mode: doConnect bleibt false, nächster Sleep Scan wird automatisch gestartet
        if (g_connectionMode == BBS_MODE_MAC && sensor.useMacDirect) {
          char macBuf[18];
          formatSensorMac(sensor, macBuf);
          BLE_LIB_LOGW_F("[SLEEP-SCAN] Connect failed for sensor %d (%s) - will retry in next scan cycle", i, macBuf);
          sensor.doConnect = false;  // Nächster Sleep Scan setzt doConnect wieder
          sensor.reconnectFailureCount = 0;
        } else if (sensor.useMacDirect) {
          // MAC-based sensor in Scan mode: keep MAC and retry with interval
          sensor.doConnect = true;
          sensor.reconnectFailureCount = 0;
          BLE_LIB_LOGHOT_F("[LOOP] MAC-based sensor: Will retry reconnection after interval (MAC preserved)");
        } else if (sensor.reconnectFailureCount >= maxFastReconnectAttempts || !preserveMacOnDisconnect) {
          // Scan-based mode: Fall back to scanning - clear sensor MAC and advertised device
          BLE_LIB_LOGI_F("[LOOP] Falling back to scan-based reconnection");
          sensor.doConnect = false;
          sensor.reconnectFailureCount = 0;
          resetSensorMac(sensor);
          // Restart scan to find this sensor again
          if (!pBLEScan || !pBLEScan->isScanning()) {
            BLE_LIB_LOGI_F("[LOOP] Restarting scan after connection failure...");
            startScan();
          }
        } else {
          // Retry fast reconnection - keep MAC and try again after delay
          sensor.doConnect = true;
          BLE_LIB_LOGHOT_F("[LOOP] Will retry fast reconnection (attempt %u/%u)", 
                 sensor.reconnectFailureCount, maxFastReconnectAttempts);
        }
      } else {
        // Connection successful - reset failure count
        sensor.reconnectFailureCount = 0;
      }
    }
    
    // Check connection state
    bool wasConnected = sensor.isConnected;
    bool isNowConnected = sensor.client && sensor.client->isConnected();
    sensor.isConnected = isNowConnected;
    
    if (!wasConnected && isNowConnected) {
      // Sensor just connected - notifications are already subscribed via connectAndSubscribeSensor
      // No need to manually write CCCD in NimBLE 2.x
      
      BLE_LIB_LOGI_F("[LOOP] Sensor type %d just connected!", i);
      
      // Reset reconnection failure count on successful connection
      sensor.reconnectFailureCount = 0;
      
      // Clear doConnect flag since we're now connected
      sensor.doConnect = false;
      
      // Check if all sensors are now connected - stop scan if so
      bool allNowConnected = true;
      for (int j = 0; j < 3; j++) {
        if (activeSensors[j].hasTarget && !activeSensors[j].isConnected) {
          allNowConnected = false;
          break;
        }
      }
      if (allNowConnected && pBLEScan && pBLEScan->isScanning()) {
        pBLEScan->stop();
        pBLEScan->clearResults();  // v2.2.12: Prevent scan result accumulation
        BLE_LIB_LOGI_F("[LOOP] All sensors now connected - stopping scan (results cleared).");
        BLETelemetry::recordScanStop();
      }
      
      // Note: Relay server should remain advertising even when all sensors are connected
      // so that central devices (bike computers) can connect to receive sensor data.
    } else if (wasConnected && !isNowConnected) {
      // Sensor disconnected - try to reconnect
      BLE_LIB_LOGHOT_F("Sensor disconnected, type: %d", i);
      sensor.isConnected = false;
      
      // Reset battery level info on disconnect
      sensor.batteryLevel = 255;
      sensor.hasBatteryService = false;
      sensor.batteryCharacteristic = nullptr;
      
      // Handle fast reconnection if Sensor MAC Preservation is enabled
      if (preserveMacOnDisconnect && sensor.hasTarget && sensor.hasMac) {
        // Phase 1: Fast reconnection path using stored address (no advertisedDevice needed)
          sensor.doConnect = true;  // Trigger reconnection
BLE_LIB_LOGHOT_F("[LOOP] Attempting fast reconnection with preserved sensor MAC...");
        } else {
          // Sensor MAC preserved but advertisedDevice lost - fall back to scan
BLE_LIB_LOGW_F("[LOOP] Sensor MAC preserved but advertisedDevice missing - rescanning");
          resetSensorMac(sensor);
          if (!pBLEScan || !pBLEScan->isScanning()) {
            startScan();
        }
      }
      
      // Will retry connection on next loop iteration
      if (sensorDisconnectedCallbacks[i]) {
        sensorDisconnectedCallbacks[i]();
      }
      
      // Restart relay server advertising if it was stopped (e.g., manually paused)
      // This allows centrals to reconnect when sensors are reconnecting
      if (!g_serverManuallyPaused && pAdvertising && !pAdvertising->isAdvertising()) {
        pAdvertising->start(0); // 0 = advertise forever
BLE_LIB_LOGI_F("[LOOP] Sensor disconnected - restarted relay server advertising.");
        BLETelemetry::recordAdvertisingStart();
      }
    }
  }
  
  BluetoothBikeSensorServerCheckCentralConnections();
  
  // End loop timing measurement and record heap (Phase 0 Telemetry)
  BLETelemetry::endLoopMeasurement(loopStartTime);
  
  // Record heap periodically (every ~10 seconds via heartbeat)
  static unsigned long lastHeapRecord = 0;
  if (now - lastHeapRecord >= 10000) {
    BLETelemetry::recordHeap();
    lastHeapRecord = now;
  }
  
  // Print telemetry summary periodically
  BLETelemetry::printIfDue(BLE_TELEMETRY_PRINT_INTERVAL_MS);
}

void BluetoothBikeSensorServerCheckCentralConnections() {
  if (!server) {
    return;
  }
  uint8_t connectedCount = server->getConnectedCount();
  if (connectedCount < lastCentralCount) {
    BLE_LIB_LOGI_F("[CENTRAL] Central disconnected from BLE server '%s'. Remaining centrals: %u", 
           advertisingName.c_str(), connectedCount);
    printVirtualDeviceState();
    
    // Telemetry: Record central disconnection
    BLETelemetry::recordCentralDisconnect();
    
    if (centralDisconnectedCallback) {
      centralDisconnectedCallback(connectedCount);
    }
    
    // CRITICAL FIX: Restart advertising when all centrals disconnect
    // NimBLE may stop advertising when a connection is established, and doesn't
    // automatically restart it when all connections close. This ensures the
    // relay server remains discoverable by sensor apps.
    if (connectedCount == 0 && !g_serverManuallyPaused && pAdvertising && !pAdvertising->isAdvertising()) {
      pAdvertising->start(0); // 0 = advertise forever
BLE_LIB_LOGI_F("[CENTRAL] All centrals disconnected - restarted advertising to remain discoverable");
      BLETelemetry::recordAdvertisingStart();
    }
  } else if (connectedCount > lastCentralCount) {
    BLE_LIB_LOGI_F("[CENTRAL] Central connected to BLE server '%s'. Total centrals: %u", 
           advertisingName.c_str(), connectedCount);
    printVirtualDeviceState();
    
    // Telemetry: Record central connection
    BLETelemetry::recordCentralConnect();
    
    if (centralConnectedCallback) {
      centralConnectedCallback(connectedCount);
    }
  }
  
  // Print state periodically (every 10 seconds) for debugging
  static uint32_t lastStatePrint = 0;
  if (millis() - lastStatePrint > 10000) {
    lastStatePrint = millis();
    printVirtualDeviceState();
  }
  if (connectedCount != lastCentralCount) {
    lastCentralCount = connectedCount;
  }
  
  // Check CCCD state periodically (every 500ms)
  // Use first available characteristic to detect central connection state
  NimBLECharacteristic* _cccdCheckChar = nullptr;
#if BBS_COMPILE_HEART_RATE
  if (!_cccdCheckChar) _cccdCheckChar = hrCharLocal;
#endif
#if BBS_COMPILE_CSC
  if (!_cccdCheckChar) _cccdCheckChar = cscCharLocal;
#endif
#if BBS_COMPILE_POWER
  if (!_cccdCheckChar) _cccdCheckChar = powerCharLocal;
#endif
  if (_cccdCheckChar && connectedCount > 0 && (millis() - lastCCCDCheck > 500)) {
    lastCCCDCheck = millis();
    // Check if notifications are enabled by reading the descriptor value
    // Note: BLE2902 manages this internally, we check via getNotificationsEnabled if available
    // For ESP32 BLE Arduino, we can check if the characteristic can notify
    // This is a simplified check - in practice, CCCD state is managed by the stack
    bool currentCCCDState = (connectedCount > 0); // Simplified: if connected, assume enabled
    if (currentCCCDState != lastCCCDState) {
      if (currentCCCDState) {
BLE_LIB_LOGHOT_F("CCCD: Notifications enabled by central.");
        if (cccdEnabledCallback) {
          cccdEnabledCallback();
        }
      } else {
BLE_LIB_LOGHOT_F("CCCD: Notifications disabled by central.");
        if (cccdDisabledCallback) {
          cccdDisabledCallback();
        }
      }
      lastCCCDState = currentCCCDState;
    }
  }
  
  // Log notification statistics periodically (every 30 seconds) for debugging
  if (connectedCount > 0 && (millis() - lastNotifyStatsPrint > 30000)) {
    lastNotifyStatsPrint = millis();
#if BBS_COMPILE_HEART_RATE
    uint32_t hrTotal = hrNotifySuccessCount + hrNotifyFailureCount;
#else
    uint32_t hrTotal = 0;
#endif
#if BBS_COMPILE_CSC
    uint32_t cscTotal = cscNotifySuccessCount + cscNotifyFailureCount;
#else
    uint32_t cscTotal = 0;
#endif
#if BBS_COMPILE_POWER
    uint32_t powerTotal = powerNotifySuccessCount + powerNotifyFailureCount;
#else
    uint32_t powerTotal = 0;
#endif
    
    if (hrTotal > 0 || cscTotal > 0 || powerTotal > 0) {
      BLE_LIB_LOGHOT_F("[MIRROR STATS] HR: %lu/%lu | CSC: %lu/%lu | Power: %lu/%lu", 
#if BBS_COMPILE_HEART_RATE
             hrNotifySuccessCount, hrTotal,
#else
             (uint32_t)0, (uint32_t)0,
#endif
#if BBS_COMPILE_CSC
             cscNotifySuccessCount, cscTotal,
#else
             (uint32_t)0, (uint32_t)0,
#endif
#if BBS_COMPILE_POWER
             powerNotifySuccessCount, powerTotal);
#else
             (uint32_t)0, (uint32_t)0);
#endif
      
      // Warn if there are failures
      bool hasFailures = false;
#if BBS_COMPILE_HEART_RATE
      if (hrNotifyFailureCount > 0) hasFailures = true;
#endif
#if BBS_COMPILE_CSC
      if (cscNotifyFailureCount > 0) hasFailures = true;
#endif
#if BBS_COMPILE_POWER
      if (powerNotifyFailureCount > 0) hasFailures = true;
#endif
      if (hasFailures) {
BLE_LIB_LOGW_F("[MIRROR] Some notifications are failing - ensure centrals have enabled notifications (CCCD)");
      }
    }
  }
}

#if BBS_ENABLE_GETTERS
bool BluetoothBikeSensorServerHasCentral() {
  return server && server->getConnectedCount() > 0;
}

uint8_t BluetoothBikeSensorServerGetCentralCount() {
  return server ? server->getConnectedCount() : 0;
}

#if BBS_COMPILE_HEART_RATE
uint16_t BluetoothBikeSensorServerGetLastHeartRate() {
  return lastHeartRate;
}

bool BluetoothBikeSensorServerHasHeartRate() {
  return heartRateValid;
}

uint32_t BluetoothBikeSensorServerGetLastHeartRateTimestamp() {
  return lastHeartRateTimestamp;
}
#else
uint16_t BluetoothBikeSensorServerGetLastHeartRate() { return 0; }
bool BluetoothBikeSensorServerHasHeartRate() { return false; }
uint32_t BluetoothBikeSensorServerGetLastHeartRateTimestamp() { return 0; }
#endif

#if BBS_COMPILE_CSC
// Speed/Cadence data access functions
// Note: Returns speed in 0.01 m/s units and cadence in rpm
// For speed in km/h: speed_kmh = speed / 100.0 * 3.6
// For speed in mph: speed_mph = speed / 100.0 * 2.237
bool BluetoothBikeSensorServerGetLastSpeedCadence(uint16_t* speed, uint16_t* cadence) {
  if (!speedCadenceData.valid || !speed || !cadence) {
    return false;
  }
  *speed = speedCadenceData.speed;
  *cadence = speedCadenceData.cadence;
  return true;
}

bool BluetoothBikeSensorServerHasSpeedCadence() {
  return speedCadenceData.valid;
}

uint32_t BluetoothBikeSensorServerGetLastSpeedCadenceTimestamp() {
  return speedCadenceData.timestamp;
}
#else
bool BluetoothBikeSensorServerGetLastSpeedCadence(uint16_t* speed, uint16_t* cadence) {
  if (speed) *speed = 0;
  if (cadence) *cadence = 0;
  return false;
}
bool BluetoothBikeSensorServerHasSpeedCadence() { return false; }
uint32_t BluetoothBikeSensorServerGetLastSpeedCadenceTimestamp() { return 0; }
#endif

#if BBS_COMPILE_POWER
// Power Meter data access functions
int16_t BluetoothBikeSensorServerGetLastPower() {
  return powerData.power;
}

bool BluetoothBikeSensorServerHasPower() {
  return powerData.valid;
}

uint32_t BluetoothBikeSensorServerGetLastPowerTimestamp() {
  return powerData.timestamp;
}
#else
int16_t BluetoothBikeSensorServerGetLastPower() { return 0; }
bool BluetoothBikeSensorServerHasPower() { return false; }
uint32_t BluetoothBikeSensorServerGetLastPowerTimestamp() { return 0; }
#endif

const char *BluetoothBikeSensorServerGetServerName() {
  // Return MAC-based device name (always use current advertising name)
  return advertisingName.c_str();
}
#endif

void BluetoothBikeSensorServerSetHeartRateCallback(void (*callback)(uint16_t heartRate)) {
#if BBS_COMPILE_HEART_RATE
  heartRateCallback = callback;
#else
  (void)callback;
#endif
}

void BluetoothBikeSensorServerSetSpeedCadenceCallback(void (*callback)(uint16_t speed, uint16_t cadence)) {
#if BBS_COMPILE_CSC
  speedCadenceCallback = callback;
#else
  (void)callback;
#endif
}

void BluetoothBikeSensorServerSetPowerCallback(void (*callback)(int16_t power)) {
#if BBS_COMPILE_POWER
  powerCallback = callback;
#else
  (void)callback;
#endif
}

void BikeSensorServerSetConnectionMode(BBSConnectionMode mode) {
  g_connectionMode = mode;
  if (g_connectionMode == BBS_MODE_MAC) {
    // Sleep Scan Mode: Stoppe laufenden Scan und reset Timer
    if (pBLEScan && pBLEScan->isScanning()) {
      pBLEScan->stop();
      pBLEScan->clearResults();  // v2.2.12: Prevent scan result accumulation
      BLETelemetry::recordScanStop();
    }
    g_lastSleepScanMs = 0;  // Erlaube sofortigen ersten Scan
  }
}

BBSConnectionMode BikeSensorServerGetConnectionMode() {
  return g_connectionMode;
}

void BikeSensorServerSetConnectIntervalMs(uint32_t intervalMs) {
  g_connectIntervalMs = intervalMs;
}

void BikeSensorServerTriggerReconnect() {
  if (!isInitialised) return;
  
  // Check if any sensor needs reconnection
  bool anySensorDisconnected = false;
  for (int i = 0; i < 3; i++) {
    if (activeSensors[i].hasTarget && !activeSensors[i].isConnected) {
      anySensorDisconnected = true;
      // In Scan mode: Set doConnect to trigger connection attempt
      if (g_connectionMode == BBS_MODE_SCAN) {
        activeSensors[i].doConnect = true;
      }
    }
  }
  
  if (!anySensorDisconnected) {
    BLE_LIB_LOGI_F("[RECONNECT] All sensors already connected - nothing to do");
    return;
  }
  
  if (g_connectionMode == BBS_MODE_MAC) {
    // MAC Sleep-Scan mode: Reset timer to allow immediate scan
    g_lastSleepScanMs = 0;
    BLE_LIB_LOGI_F("[RECONNECT] Triggered immediate Sleep-Scan for disconnected sensors");
    // Start scan immediately (will be picked up in next Loop iteration)
    startSleepScan();
  } else {
    // Scan mode: Start/restart scan
    if (!pBLEScan || !pBLEScan->isScanning()) {
      BLE_LIB_LOGI_F("[RECONNECT] Starting scan for disconnected sensors");
      startScan();
    } else {
      BLE_LIB_LOGI_F("[RECONNECT] Scan already running - will connect when sensors found");
    }
  }
}

// New per-type callback functions
void BikeSensorServerSetSensorConn(SensorType type, void (*callback)()) {
  int index = getSensorIndex(type);
  if (index >= 0 && index < 3) {
    sensorConnectedCallbacks[index] = callback;
  }
}

void BikeSensorServerSetSensorDisc(SensorType type, void (*callback)()) {
  int index = getSensorIndex(type);
  if (index >= 0 && index < 3) {
    sensorDisconnectedCallbacks[index] = callback;
  }
}

#if BBS_ENABLE_CENTRAL_CB
void BikeSensorServerSetClientConn(void (*callback)(uint8_t centralCount)) {
  centralConnectedCallback = callback;
}

void BikeSensorServerSetClientDisc(void (*callback)(uint8_t centralCount)) {
  centralDisconnectedCallback = callback;
}

void BikeSensorServerSetCCCDEnabled(void (*callback)()) {
  cccdEnabledCallback = callback;
}

void BikeSensorServerSetCCCDDisabled(void (*callback)()) {
  cccdDisabledCallback = callback;
}
#endif

void BluetoothBikeSensorServerResetServer() {
BLE_LIB_LOGHOT_F("BLE heart server: resetting advertising.");
  BLEAdvertisingManagerRefreshName(true, HR_SERVICE_UUID, SPEED_CADENCE_SERVICE_UUID, POWER_SERVICE_UUID);
}

void BluetoothBikeSensorServerFullRestart() {
  if (!isInitialised) {
    return;
  }
BLE_LIB_LOGHOT_F("BLE heart server: full restart requested.");
  
  // Preserve settings across restart
  uint8_t savedEnabledSensorTypes = enabledSensorTypes;
  BBSConnectionMode savedConnectionMode = g_connectionMode;
  
  // Clear sensor state
  clearSensorState();
  
  // Disconnect all sensors
  for (int i = 0; i < 3; i++) {
    if (activeSensors[i].client && activeSensors[i].client->isConnected()) {
      activeSensors[i].client->disconnect();
    }
    activeSensors[i].hasTarget = false;
    activeSensors[i].isConnected = false;
  }
  
  if (pAdvertising) {
    pAdvertising->stop();
  }
  NimBLEDevice::deinit(true);
  delay(500);

  server = nullptr;
#if BBS_COMPILE_HEART_RATE
  hrServiceLocal = nullptr;
  hrCharLocal = nullptr;
#endif
#if BBS_COMPILE_CSC
  cscServiceLocal = nullptr;
  cscCharLocal = nullptr;
#endif
#if BBS_COMPILE_POWER
  powerServiceLocal = nullptr;
  powerCharLocal = nullptr;
#endif
  pAdvertising = nullptr;
  pBLEScan = nullptr;
  isInitialised = false;

  // Restart with preserved settings
  // Use MAC-based name for restart (device name prefix was already set during initial Begin)
  BluetoothBikeSensorServerBegin(nullptr, savedEnabledSensorTypes, savedConnectionMode, initialTxPower);
}

// Runtime sensor management API functions
// Name-only API - MAC addresses are resolved internally via scanning
bool BikeSensorServerAddSensor(SensorType type, const char* sensorName) {
  if (!isInitialised || !sensorName) {
    return false;
  }
  
  int index = getSensorIndex(type);
  if (index < 0) {
    return false;
  }
  
  ActiveSensor &sensor = activeSensors[index];
  
  // Clear existing target
  if (sensor.hasTarget) {
    BikeSensorServerRemoveSensor(type);
  }
  
  // Set target by name (MAC will be set when device is found in scan)
  sensor.setName(sensorName);
  sensor.hasTarget = true;
  sensor.isConnected = false;
  sensor.doConnect = false;
  resetSensorMac(sensor); // Invalid address until found
  
  // v2.2.4: Initialize resolution tracking
  sensor.hasResolvedAddress = false;
  sensor.nameCheckCount = 0;
  
  BLE_LIB_LOGI_F("[ADD] Added sensor: type=%d, name='%s'", type, sensorName);
  
  if (g_connectionMode == BBS_MODE_SCAN) {
    // CRITICAL FIX: Ensure scan is in server mode and restart it
    // After discovery, the scan may have already "seen" the device during discovery
    // and won't report it again until the scan is restarted (NimBLE behavior)
    if (pBLEScan) {
      // Always restore server-mode callbacks (may have been set to discovery callbacks)
      pBLEScan->setScanCallbacks(&advertisedDeviceCallbacks);
      
      // CRITICAL: Always restart scan to clear the "already seen" device list
      // NimBLE only reports each device once per scan session
      if (pBLEScan->isScanning()) {
        pBLEScan->stop();
        // Note: No delay needed - NimBLE handles stop/start transition internally
      }
      
      pBLEScan->start(0, false);  // 0 = scan forever
      BLE_LIB_LOGI_F("[ADD] Scan restarted for new sensor");
    } else {
      // pBLEScan not initialized - use startScan() to initialize and start
      startScan();
    }
  }
  
  BLE_LIB_LOGI_F("[ADD] Sensor added - will search by name: %s", sensorName);
  
  return true;
}

// Main implementation with explicit address type parameter
// addrType: 0=Public, 1=Random, -1=Auto-detect (based on MAC bits)
bool BikeSensorServerAddSensorByMac(SensorType type, const char* sensorName, const char* macAddress, int8_t addrTypeParam) {
  if (!isInitialised || !macAddress) {
    return false;
  }
  
  int index = getSensorIndex(type);
  if (index < 0) {
    return false;
  }
  
  ActiveSensor &sensor = activeSensors[index];
  
  if (sensor.hasTarget) {
    BikeSensorServerRemoveSensor(type);
  }
  
  if (sensorName && sensorName[0] != '\0') {
    sensor.setName(sensorName);
  } else {
    sensor.clearName();
  }
  
  sensor.hasTarget = true;
  sensor.isConnected = false;
  sensor.useMacDirect = true;
  
  uint8_t macBytes[6];
  if (!parseMacString(macAddress, macBytes)) {
    BLE_LIB_LOGE_F("[ADD] Invalid MAC format: %s", macAddress);
    return false;
  }

  
  uint8_t addrType;
  if (addrTypeParam >= 0) {
    // User explicitly specified address type
    addrType = (uint8_t)addrTypeParam;
    BLE_LIB_LOGI_F("[ADD] MAC %s using explicit address type=%d (%s)", 
                   macAddress, addrType, addrType ? "random" : "public");
  } else {
    // Auto-detect address type from MAC:
    // Random Static Address: two most significant bits of first byte are '11'
    // Public Address: otherwise
    // NOTE: This heuristic may not work for all devices!
    addrType = ((macBytes[0] & 0xC0) == 0xC0) ? 1 : 0;
    BLE_LIB_LOGI_F("[ADD] MAC %s auto-detected as %s address (type=%d) - use explicit type if wrong!", 
                   macAddress, addrType ? "random" : "public", addrType);
  }
  
  NimBLEAddress addr(macBytes, addrType);
  
  // Sleep Scan Mode: MAC ist bekannt, doConnect wird durch Scan-Callback gesetzt
  if (g_connectionMode == BBS_MODE_MAC) {
    sensor.doConnect = false;  // Wird durch Scan-Callback gesetzt wenn MAC gefunden
    sensor.hasResolvedAddress = true;  // MAC-Matching aktivieren
    g_lastSleepScanMs = 0;  // Erlaube sofortigen ersten Scan
  } else {
    // Normaler Scan-Mode: direct connect
    sensor.doConnect = true;
  }
  updateSensorMacFromAddress(sensor, addr);
  
  sensor.hasResolvedAddress = true;
  sensor.nameCheckCount = 0;
  
  // Debug: Verify MAC was set correctly
  char macBuf[18];
  formatSensorMac(sensor, macBuf);
  BLE_LIB_LOGI_F("[ADD] Added sensor by MAC: type=%d, name='%s', mac=%s, addrType=%d, mode=%s", type,
                 sensorName ? sensorName : "", macAddress, addrType,
                 (g_connectionMode == BBS_MODE_MAC) ? "SLEEP_SCAN" : "CONTINUOUS_SCAN");
  
  return true;
}

// Overload without explicit address type (uses auto-detection)
bool BikeSensorServerAddSensorByMac(SensorType type, const char* sensorName, const char* macAddress) {
  return BikeSensorServerAddSensorByMac(type, sensorName, macAddress, -1);  // -1 = auto-detect
}

// Overload with just MAC address (uses auto-detection)
bool BikeSensorServerAddSensorByMac(SensorType type, const char* macAddress) {
  return BikeSensorServerAddSensorByMac(type, nullptr, macAddress, -1);  // -1 = auto-detect
}

bool BikeSensorServerRemoveSensor(SensorType type) {
  if (!isInitialised) {
    return false;
  }
  
  int index = getSensorIndex(type);
  if (index < 0) {
    return false;
  }
  
  ActiveSensor &sensor = activeSensors[index];
  
  // Disconnect if connected
  if (sensor.client && sensor.client->isConnected()) {
    sensor.client->disconnect();
  }
  
  // v2.2.5: Delete client ONLY when explicitly removing a sensor
  // This is the only place where client deletion is allowed (besides BLE restart)
  if (sensor.client) {
    NimBLEDevice::deleteClient(sensor.client);
    sensor.client = nullptr;
  }
  
  // Clear state
  sensor.hasTarget = false;
  sensor.isConnected = false;
  resetSensorMac(sensor);
  sensor.clearName();
  // Reset battery level info
  sensor.batteryLevel = 255;
  sensor.hasBatteryService = false;
  sensor.batteryCharacteristic = nullptr;
  sensor.characteristic = nullptr;
  // v2.2.4: Reset resolution tracking
  sensor.hasResolvedAddress = false;
  sensor.nameCheckCount = 0;
  
  // Call disconnect callback
  if (sensorDisconnectedCallbacks[index]) {
    sensorDisconnectedCallbacks[index]();
  }
  
  BLE_LIB_LOGHOT_F("Removed sensor: Type=%d", (int)type);
  
  return true;
}

bool BikeSensorServerIsSensorConnected(SensorType type) {
  int index = getSensorIndex(type);
  if (index < 0) {
    return false;
  }
  
  ActiveSensor &sensor = activeSensors[index];
  return sensor.isConnected && sensor.client && sensor.client->isConnected();
}

#if BBS_ENABLE_GETTERS
bool BikeSensorServerGetSensorMac(SensorType type, char* mac, size_t len) {
  int index = getSensorIndex(type);
  if (index < 0 || !mac || len < 18) {
    return false;
  }
  
  ActiveSensor &sensor = activeSensors[index];
  if (!sensor.hasTarget || !sensor.hasMac) {
    return false;
  }
  
  if (len < 18) {
    return false;
  }
  formatSensorMac(sensor, mac);
  return true;
}

bool BikeSensorServerGetSensorName(SensorType type, char* name, size_t len) {
  int index = getSensorIndex(type);
  if (index < 0 || !name || len == 0) {
    return false;
  }
  
  ActiveSensor &sensor = activeSensors[index];
  if (!sensor.hasTarget || sensor.name[0] == '\0') {
    return false;
  }
  
  size_t nameLen = strlen(sensor.name);
  if (nameLen >= len) {
    return false;
  }
  
  strncpy(name, sensor.name, len - 1);
  name[len - 1] = '\0';
  return true;
}

bool BikeSensorServerGetBatteryLevel(SensorType type, uint8_t* batteryLevel) {
  if (!isInitialised || !batteryLevel) {
    return false;
  }
  
  int index = getSensorIndex(type);
  if (index < 0) {
    return false;
  }
  
  ActiveSensor &sensor = activeSensors[index];
  if (!sensor.isConnected || !sensor.hasBatteryService) {
    return false;
  }
  
  *batteryLevel = sensor.batteryLevel;
  return true;
}

bool BikeSensorServerHasBatteryLevel(SensorType type) {
  if (!isInitialised) {
    return false;
  }
  
  int index = getSensorIndex(type);
  if (index < 0) {
    return false;
  }
  
  ActiveSensor &sensor = activeSensors[index];
  return sensor.isConnected && sensor.hasBatteryService;
}
#endif

// Discovery API functions
void BikeSensorServerStartDiscovery(uint8_t sensorTypeMask) {
  if (!isInitialised) {
    return;
  }
  
  // Delete all previously found sensors
  BikeSensorServerDeleteAllSensors();
  
  // Start discovery scan (pauseServerMode is called inside startDiscoveryScan)
  startDiscoveryScan(sensorTypeMask);
}

void BikeSensorServerStopDiscovery() {
  if (g_discoveryRunning) {
    stopDiscoveryScan();
  }
}

void BikeSensorServerDeleteAllSensors() {
  g_discoveredCount = 0;
BLE_LIB_LOGHOT_F("All discovered sensors deleted");
}

#if BBS_ENABLE_GETTERS
uint8_t BikeSensorServerGetSensorCount() {
  return g_discoveredCount;
}

uint8_t BikeSensorServerGetSensorCountByType(SensorType type) {
  uint8_t count = 0;
  for (int i = 0; i < g_discoveredCount; ++i) {
    if (g_discoveredSensors[i].getType() == type) {
      count++;
    }
  }
  return count;
}

bool BikeSensorServerGetSensor(uint8_t index, SensorType* type, char* name, size_t nameLen, uint8_t mac[6]) {
  if (index >= g_discoveredCount || !type || !name || !mac || nameLen == 0) {
    return false;
  }
  
  const DiscoveredSensor& sensor = g_discoveredSensors[index];
  *type = sensor.getType();
  
  // Copy name
  strncpy(name, sensor.name, nameLen - 1);
  name[nameLen - 1] = '\0';
  
  // Copy MAC as raw bytes (XX:XX:XX:XX:XX:XX)
  if (!parseMacString(sensor.mac, mac)) {
    memset(mac, 0, 6);
  }
  
  return true;
}

bool BikeSensorServerGetSensorByMac(const char* mac, SensorType* type, char* name, size_t nameLen) {
  if (!mac || !type || !name) {
    return false;
  }
  
  for (int i = 0; i < g_discoveredCount; ++i) {
    if (strcmp(g_discoveredSensors[i].mac, mac) == 0) {
      *type = g_discoveredSensors[i].getType();
      strncpy(name, g_discoveredSensors[i].name, nameLen - 1);
      name[nameLen - 1] = '\0';
      return true;
    }
  }
  return false;
}

const char* BikeSensorServerListSensors(SensorListFormat format) {
  static char buffer[512];
  buffer[0] = '\0';
  
  if (g_discoveredCount == 0) {
    strcpy(buffer, "No sensors found");
    return buffer;
  }
  
  size_t pos = 0;
  for (int i = 0; i < g_discoveredCount && pos < sizeof(buffer) - 1; i++) {
    const DiscoveredSensor& sensor = g_discoveredSensors[i];
    
    if (i > 0 && pos < sizeof(buffer) - 1) {
      buffer[pos++] = '\n';
    }
    
    if (format == LIST_FORMAT_NAME) {
      int written = snprintf(buffer + pos, sizeof(buffer) - pos, "%s", sensor.name);
      if (written > 0) pos += written;
    } else if (format == LIST_FORMAT_MAC) {
      int written = snprintf(buffer + pos, sizeof(buffer) - pos, "%s", sensor.mac);
      if (written > 0) pos += written;
    } else { // LIST_FORMAT_FULL
      // Build ART string (HR, CSC, PWR) using fixed buffer to avoid heap allocation
      char art[16] = "";
      bool first = true;
      if (sensor.hasHR) {
        strcat(art, first ? "HR" : "+HR");
        first = false;
      }
      if (sensor.hasCSC) {
        strcat(art, first ? "CSC" : "+CSC");
        first = false;
      }
      if (sensor.hasPWR) {
        strcat(art, first ? "PWR" : "+PWR");
        first = false;
      }
      if (first) {
        strcpy(art, "-");
      }
      
      // Include address type: 0=Public, 1=Random
      int written = snprintf(buffer + pos, sizeof(buffer) - pos, "[%s] %s (%s) %d", 
                            art, sensor.name, sensor.mac, sensor.addrType);
      if (written > 0) pos += written;
    }
  }
  
  buffer[pos] = '\0';
  return buffer;
}

const char* BikeSensorServerListSensorsByType(SensorType type, SensorListFormat format) {
  static char buffer[512];
  buffer[0] = '\0';
  
  bool found = false;
  size_t pos = 0;
  
  for (int i = 0; i < g_discoveredCount && pos < sizeof(buffer) - 1; i++) {
    const DiscoveredSensor& sensor = g_discoveredSensors[i];
    
    if (sensor.getType() != type) {
      continue;
    }
    
    found = true;
    
    if (pos > 0 && pos < sizeof(buffer) - 1) {
      buffer[pos++] = '\n';
    }
    
    if (format == LIST_FORMAT_NAME) {
      int written = snprintf(buffer + pos, sizeof(buffer) - pos, "%s", sensor.name);
      if (written > 0) pos += written;
    } else if (format == LIST_FORMAT_MAC) {
      int written = snprintf(buffer + pos, sizeof(buffer) - pos, "%s", sensor.mac);
      if (written > 0) pos += written;
    } else { // LIST_FORMAT_FULL
      // Build ART string (HR, CSC, PWR) using fixed buffer to avoid heap allocation
      char art[16] = "";
      bool first = true;
      if (sensor.hasHR) {
        strcat(art, first ? "HR" : "+HR");
        first = false;
      }
      if (sensor.hasCSC) {
        strcat(art, first ? "CSC" : "+CSC");
        first = false;
      }
      if (sensor.hasPWR) {
        strcat(art, first ? "PWR" : "+PWR");
        first = false;
      }
      if (first) {
        strcpy(art, "-");
      }
      
      // Include address type: 0=Public, 1=Random
      int written = snprintf(buffer + pos, sizeof(buffer) - pos, "[%s] %s (%s) %d", 
                            art, sensor.name, sensor.mac, sensor.addrType);
      if (written > 0) pos += written;
    }
  }
  
  if (!found) {
    strcpy(buffer, "No sensors of this type found");
  } else {
    buffer[pos] = '\0';
  }
  
  return buffer;
}
#endif

void BikeSensorServerSetDiscoveryDuration(uint32_t durationMs) {
  discoveryDuration = durationMs;
}

#if BBS_ENABLE_GETTERS
uint32_t BikeSensorServerGetDiscoveryDuration() {
  return discoveryDuration;
}

void BikeSensorServerSetMaxSensors(uint8_t max) {
  maxSensors = max;
}

uint8_t BikeSensorServerGetMaxSensors() {
  return maxSensors;
}
#endif

void BikeSensorServerPause() {
  if (!isInitialised) {
    return;
  }
  
  // Check if server is advertising
  if (pAdvertising) {
    g_serverWasAdvertisingBeforePause = pAdvertising->isAdvertising();
    if (g_serverWasAdvertisingBeforePause) {
      pAdvertising->stop();
      delay(200); // Delay to ensure advertising stops
BLE_LIB_LOGI_F("[BLESensorServer] Paused: Stopped advertising (Virtual Sensor Server)");
    } else {
BLE_LIB_LOGI_F("[BLESensorServer] Paused: Server was not advertising");
    }
  } else {
    g_serverWasAdvertisingBeforePause = false;
BLE_LIB_LOGI_F("[BLESensorServer] Paused: Server was not configured");
  }
  
  g_serverManuallyPaused = true;
}

void BikeSensorServerResume() {
  if (!isInitialised) {
    return;
  }
  
  if (!g_serverManuallyPaused) {
BLE_LIB_LOGI_F("[BLESensorServer] Resume: Server was not manually paused");
    return;
  }
  
  g_serverManuallyPaused = false;
  
  // Always restart advertising if it was running before pause
  // The relay server must remain advertising so central devices can connect
  if (g_serverWasAdvertisingBeforePause && pAdvertising && !pAdvertising->isAdvertising()) {
    pAdvertising->start(0); // 0 = advertise forever
    delay(200); // Delay to ensure advertising starts
BLE_LIB_LOGI_F("[BLESensorServer] Resumed: Restarted advertising (Virtual Sensor Server).");
  } else if (!g_serverWasAdvertisingBeforePause) {
BLE_LIB_LOGI_F("[BLESensorServer] Resumed: Server was not advertising before pause.");
  } else {
BLE_LIB_LOGI_F("[BLESensorServer] Resumed: Server already advertising.");
  }
}

#if BBS_ENABLE_GETTERS
bool BikeSensorServerIsAdvertising() {
  if (!isInitialised || !pAdvertising) {
    return false;
  }
  return pAdvertising->isAdvertising();
}
#endif

bool BikeSensorServerRestartAdvertising() {
  if (!isInitialised) {
BLE_LIB_LOGE_F("[RESTART_ADV] Cannot restart advertising - server not initialized");
    return false;
  }
  
  if (!server) {
BLE_LIB_LOGE_F("[RESTART_ADV] Cannot restart advertising - server object is null");
    return false;
  }
  
  // Check if there are active central connections
  // If there are, advertising may be temporarily stopped (normal BLE behavior)
  uint8_t connectedCount = server->getConnectedCount();
  if (connectedCount > 0) {
BLE_LIB_LOGHOT_F("[RESTART_ADV] Active central connections detected - advertising may be paused (normal behavior)");
    // Don't try to restart if there are active connections - this is expected
    // Advertising will resume automatically when connections close
    return true; // Return true since this is expected behavior, not an error
  }
  
  // Get advertising object (recreate if needed)
  pAdvertising = NimBLEDevice::getAdvertising();
  if (!pAdvertising) {
BLE_LIB_LOGE_F("[RESTART_ADV] Cannot restart advertising - failed to get advertising object");
    return false;
  }
  
  // If advertising is already running, we're good
  if (pAdvertising->isAdvertising()) {
BLE_LIB_LOGHOT_F("[RESTART_ADV] Advertising is already active");
    return true;
  }
  
  // Rebuild advertising data to ensure it's valid
  NimBLEAdvertisementData advData;
  advData.setName(advertisingName.c_str());
  
  // Add service UUIDs for compiled & enabled sensor types
#if BBS_COMPILE_HEART_RATE
  if (enabledSensorTypes & SENSOR_TYPE_HEART_RATE) {
    advData.addServiceUUID(HR_SERVICE_UUID);
  }
#endif
#if BBS_COMPILE_CSC
  if (enabledSensorTypes & SENSOR_TYPE_SPEED_CADENCE) {
    advData.addServiceUUID(SPEED_CADENCE_SERVICE_UUID);
  }
#endif
#if BBS_COMPILE_POWER
  if (enabledSensorTypes & SENSOR_TYPE_POWER_METER) {
    advData.addServiceUUID(POWER_SERVICE_UUID);
  }
#endif
  
  // Set the advertising data
  pAdvertising->setAdvertisementData(advData);
  
  // Clear scan response
  NimBLEAdvertisementData scanRespData;
  scanRespData.setName("");
  pAdvertising->setScanResponseData(scanRespData);
  
  delay(50); // Small delay to ensure data is set
  
  // Restart advertising
  if (!pAdvertising->start(0)) { // 0 = advertise forever
BLE_LIB_LOGE_F("[RESTART_ADV] start() returned false - BLE stack may be in invalid state");
    delay(200);
    // Try one more time after delay
    if (!pAdvertising->start(0)) {
BLE_LIB_LOGE_F("[RESTART_ADV] Second attempt also failed");
      return false;
    }
  }
  
  delay(200); // Delay to ensure advertising actually starts
  
  // Verify it started
  if (pAdvertising->isAdvertising()) {
    BLE_LIB_LOGI_F("[RESTART_ADV] Advertising restarted successfully for '%s'", advertisingName.c_str());
    return true;
  } else {
BLE_LIB_LOGE_F("[RESTART_ADV] Advertising start() succeeded but isAdvertising() returns false");
BLE_LIB_LOGE_F("[RESTART_ADV] BLE stack may need full restart - try BluetoothBikeSensorServerResetServer()");
    return false;
  }
}

void BikeSensorServerSetDiscoveryCompleteCallback(void (*callback)(uint8_t count)) {
  g_discoveryCompleteCallback = callback;
}

#if BBS_ENABLE_GETTERS
bool BikeSensorServerIsDiscoveryActive() {
  return g_discoveryRunning;
}

bool BikeSensorServerIsScanning() {
  return pBLEScan && pBLEScan->isScanning();
}
#endif

void BikeSensorServerSetPreserveMacOnDisconnect(bool preserve) {
  preserveMacOnDisconnect = preserve;
  BLE_LIB_LOGI_F("[CONFIG] Sensor MAC Preservation on disconnect: %s", preserve ? "enabled" : "disabled");
}

#if BBS_ENABLE_GETTERS
bool BikeSensorServerGetPreserveMacOnDisconnect() {
  return preserveMacOnDisconnect;
}

int BikeSensorServerGetLastDisconnectReason(SensorType type) {
  int index = getSensorIndex(type);
  if (index < 0) return 0;
  return activeSensors[index].lastDisconnectReason;
}

const char* BikeSensorServerGetDisconnectReasonString(int reason) {
  // BLE HCI disconnect reason codes (Bluetooth Core Spec Vol 2 Part D)
  switch (reason) {
    case 0x00: return "Success";
    case 0x01: return "Unknown Command";
    case 0x02: return "Unknown Connection";
    case 0x03: return "Hardware Failure";
    case 0x04: return "Page Timeout";
    case 0x05: return "Authentication Failure";
    case 0x06: return "PIN Missing";
    case 0x07: return "Memory Exceeded";
    case 0x08: return "Connection Timeout";
    case 0x09: return "Connection Limit Exceeded";
    case 0x0A: return "Sync Connection Limit Exceeded";
    case 0x0B: return "Connection Already Exists";
    case 0x0C: return "Command Disallowed";
    case 0x0D: return "Rejected: Limited Resources";
    case 0x0E: return "Rejected: Security Reasons";
    case 0x0F: return "Rejected: Unacceptable BD_ADDR";
    case 0x10: return "Accept Timeout";
    case 0x11: return "Unsupported Feature";
    case 0x12: return "Invalid Parameters";
    case 0x13: return "Remote User Terminated";  // Most common: peer closed connection
    case 0x14: return "Remote Low Resources";
    case 0x15: return "Remote Power Off";
    case 0x16: return "Local Host Terminated";   // We closed the connection
    case 0x17: return "Repeated Attempts";
    case 0x18: return "Pairing Not Allowed";
    case 0x1A: return "Unsupported Remote Feature";
    case 0x1F: return "Unspecified Error";
    case 0x20: return "Unsupported LMP Parameter";
    case 0x22: return "LMP Response Timeout";
    case 0x23: return "LMP Collision";
    case 0x24: return "LMP PDU Not Allowed";
    case 0x25: return "Encryption Mode Not Acceptable";
    case 0x26: return "Link Key Cannot Be Changed";
    case 0x27: return "Requested QoS Not Supported";
    case 0x28: return "Instant Passed";
    case 0x29: return "Pairing: Unit Key Not Supported";
    case 0x2A: return "Different Transaction Collision";
    case 0x2E: return "Channel Assessment Not Supported";
    case 0x2F: return "Insufficient Security";
    case 0x30: return "Parameter Out Of Range";
    case 0x3A: return "Controller Busy";
    case 0x3B: return "Unacceptable Connection Parameters";
    case 0x3C: return "Advertising Timeout";
    case 0x3D: return "Connection Terminated: MIC Failure";
    case 0x3E: return "Connection Failed To Establish";
    case 0x3F: return "MAC Connection Failed";
    default:   return "Unknown Reason";
  }
}
#endif
void BikeSensorServerSetDeviceNamePrefix(const char* prefix) {
  bbs::identity::SetDeviceNamePrefix(prefix);
  // Clear cached name so it will be regenerated
  // Update advertising name immediately if initialized
  if (isInitialised) {
    // v2.2.4.7: GetBleDeviceNameLower() returns const String& - use reference to avoid copy
    const String& newName = bbs::identity::GetBleDeviceNameLower();
    advertisingName = newName.c_str();
    
    // Update advertising if it's active
    if (pAdvertising && pAdvertising->isAdvertising()) {
      BLEAdvertisingManagerRefreshName(true, HR_SERVICE_UUID, SPEED_CADENCE_SERVICE_UUID, POWER_SERVICE_UUID);
    }
  }
}

bool BikeSensorServerRefreshAdvertisingWithIdentity(const char* deviceName, 
                                                     const uint8_t* manufacturerData, 
                                                     size_t manufacturerDataLength,
                                                     uint16_t primaryServiceUuid) {
  if (!isInitialised || !pAdvertising) {
BLE_LIB_LOGE_F("[REFRESH_ADV] Cannot refresh advertising - BLE not initialized or advertising object is null");
    return false;
  }
  
  if (!deviceName || !manufacturerData || manufacturerDataLength == 0) {
BLE_LIB_LOGE_F("[REFRESH_ADV] Invalid parameters");
    return false;
  }
  
BLE_LIB_LOGI_F("[REFRESH_ADV] Refreshing advertising with new identity...");
  
  // Stop advertising if it's running
  bool wasAdvertising = pAdvertising->isAdvertising();
  if (wasAdvertising) {
    pAdvertising->stop();
    delay(100); // Small delay to ensure stop completes
  }
  
  // Get fresh advertising object
  pAdvertising = NimBLEDevice::getAdvertising();
  if (!pAdvertising) {
BLE_LIB_LOGE_F("[REFRESH_ADV] Failed to get advertising object");
    return false;
  }
  
  // Build fresh advertising data
  NimBLEAdvertisementData advData;
  advData.setName(deviceName);
  
  // Add manufacturer data
  // NimBLE's setManufacturerData expects the full manufacturer data buffer
  // The manufacturer data should already include the company ID in the first 2 bytes (little-endian)
  if (manufacturerDataLength >= 2) {
    // Extract company ID for logging (first 2 bytes, little-endian)
    uint16_t companyId = manufacturerData[0] | (manufacturerData[1] << 8);
    // Pass the full manufacturer data array (including company ID)
    advData.setManufacturerData(manufacturerData, manufacturerDataLength);
    BLE_LIB_LOGI_F("[REFRESH_ADV] Added manufacturer data: company_id=0x%04X, total_length=%zu", 
           companyId, manufacturerDataLength);
  } else {
BLE_LIB_LOGW_F("[REFRESH_ADV] Manufacturer data too short (must be at least 2 bytes for company ID)");
  }
  
  // Add primary service UUID (only one, minimal/Tacx-style)
  advData.addServiceUUID(NimBLEUUID(primaryServiceUuid));
  BLE_LIB_LOGI_F("[REFRESH_ADV] Added primary service UUID: 0x%04X", primaryServiceUuid);
  
  // Set the advertising data
  pAdvertising->setAdvertisementData(advData);
  
  // Clear scan response
  NimBLEAdvertisementData scanRespData;
  scanRespData.setName("");
  pAdvertising->setScanResponseData(scanRespData);
  
  delay(50); // Small delay to ensure data is set
  
  // Restart advertising
  if (!pAdvertising->start(0)) { // 0 = advertise forever
BLE_LIB_LOGE_F("[REFRESH_ADV] Failed to start advertising");
    return false;
  }
  
  delay(200); // Delay to ensure advertising starts
  
  if (pAdvertising->isAdvertising()) {
BLE_LIB_LOGI_F("[REFRESH_ADV] Advertising refreshed successfully with new identity");
    return true;
  } else {
BLE_LIB_LOGE_F("[REFRESH_ADV] Advertising start() succeeded but isAdvertising() returns false");
    return false;
  }
}

