/**
 * @file WebUI_Integration.ino
 * @brief Example showing BluetoothBikeSensorServer with PulseFanSyncWebUis
 * 
 * This example demonstrates how to integrate BluetoothBikeSensorServer with
 * the WebUI using PulseFanSyncWebUi library. The WebUI provides a full
 * BLESensorManager interface for discovering, connecting, and managing
 * BLE bike sensors (Heart Rate, Speed/Cadence) through a
 * web browser.
 * 
 * Hardware: ESP32
 * 
 * Required libraries:
 * - BluetoothBikeSensorServer
 * - PulseFanSyncWebUi
 * - SmartMiFanAsync
 * - NVSUtilityLibrary
 * - WiFi (ESP32 Arduino Core)
 * - WiFiUDP (ESP32 Arduino Core)
 * - WiFiManager (optional, for WiFi setup)
 * - ArduinoJson
 * - NimBLE-Arduino (2.x required)
 * 
 * Setup:
 * 1. Update WiFi credentials below (or use WiFiManager)
 * 2. Upload to ESP32
 * 3. Connect to WiFi
 * 4. Open http://esp32-setup.local or the device IP in browser
 * 5. Use the "Setup Sensors" button in the WebUI to discover and manage sensors
 * 
 * Features demonstrated:
 * - Sensor discovery via WebUI
 * - Sensor connection management
 * - Battery level monitoring
 * - Real-time sensor data display
 * - Sensor removal
 * - Fan management via WebUI (SmartMiFanAsync)
 * - Fan token persistence in NVS
 * - Default fan configuration support
 */

#include <WiFi.h>
#include <WiFiUDP.h>
#include <PulseFanSyncWebUi.h>
#include <BluetoothBikeSensorServer.h>
#include <NVSConfigBus.h>
#include <SmartMiFanAsync.h>
#include <SmartMiFanAsync.h>




#define DEBUG_LOG_LEVEL 0;
#include <DebugLog.h>

// Include integrations
#include "integrations/BLESensorIntegration.h"
#include "integrations/FanManagerIntegration.h"

// ============================================================================
// Logging Configuration (Compile-time only, no runtime control)
// ============================================================================

// BLE Sensor Logging Configuration
#ifndef BLE_LOG_ENABLED
#define BLE_LOG_ENABLED 0  // Set to 0 to disable all BLE logging, 1 to enable
#endif

#ifndef BLE_LOG_LEVEL
#define BLE_LOG_LEVEL 3  // 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG
#endif

// Fan Control Logging Configuration
#ifndef FAN_LOG_ENABLED
#define FAN_LOG_ENABLED 0  // Set to 0 to disable all Fan logging, 1 to enable
#endif

#ifndef FAN_LOG_LEVEL
#define FAN_LOG_LEVEL 0  // 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG
#endif

// Logging helper macros for BLE
#if BLE_LOG_ENABLED
  #define BLE_LOG_ERROR(x) if(BLE_LOG_LEVEL >= 0) { LOGE(x); }
  #define BLE_LOG_WARN(x)  if(BLE_LOG_LEVEL >= 1) { LOGW(x); }
  #define BLE_LOG_INFO(x)  if(BLE_LOG_LEVEL >= 2) { LOGI(x); }
  #define BLE_LOG_DEBUG(x) if(BLE_LOG_LEVEL >= 3) { LOGD(x); }
#else
  #define BLE_LOG_ERROR(x)
  #define BLE_LOG_WARN(x)
  #define BLE_LOG_INFO(x)
  #define BLE_LOG_DEBUG(x)
#endif

// Logging helper macros for Fan
#if FAN_LOG_ENABLED
  #define FAN_LOG_ERROR(x) if(FAN_LOG_LEVEL >= 0) { LOGE(x); }
  #define FAN_LOG_WARN(x)  if(FAN_LOG_LEVEL >= 1) { LOGW(x); }
  #define FAN_LOG_INFO(x)  if(FAN_LOG_LEVEL >= 2) { LOGI(x); }
  #define FAN_LOG_DEBUG(x) if(FAN_LOG_LEVEL >= 3) { LOGD(x); }
#else
  #define FAN_LOG_ERROR(x)
  #define FAN_LOG_WARN(x)
  #define FAN_LOG_INFO(x)
  #define FAN_LOG_DEBUG(x)
#endif

// Optional: WiFiManager for WiFi setup
// Uncomment to use WiFiManager instead of hardcoded credentials
// #include <WiFiManager.h>
// WiFiManager wm;

// WiFi credentials (if not using WiFiManager)
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";


//const char* ssid = "YOUR_WIFI_SSID";
//const char* password = "YOUR_WIFI_PASSWORD";

// Create WebUI instance (owns WebServer)
PulseFanSyncWebUi webUI;

// Create NVS config bus for sensor and fan persistence
NVSConfigBus nvsConfigBus("appcfg");

// Create WiFiUDP instance for fan communication
WiFiUDP fanUdp;

// Initialize UDP socket (must be done after WiFi is connected)
// This will be called in setup() after WiFi connection

// Create BLE sensor integration instance
// Note: Must be created after webUI.begin() is called, so we'll create it in setup()
BLESensorIntegration* bleSensorIntegration = nullptr;

// Create Fan manager integration instance
// Note: Must be created after webUI.begin() is called, so we'll create it in setup()
FanManagerIntegration* fanManagerIntegration = nullptr;

// Heart rate to fan speed mapping configuration
uint32_t fanUpdateInterval = 3000;  // Update interval in milliseconds (3 seconds default)
uint32_t lastFanUpdateTime = 0;    // Last time fans were updated
uint16_t lastProcessedHR = 0;      // Last processed heart rate value

// Default sensors configuration
// These will be used if no sensors are found in NVS storage
// Format: { Name, Type, MAC, Connected }
// Note: MAC is optional and can be placeholder, Connected is always false initially
const struct {
  const char* Name;
  const char* Type;
  const char* MAC;
} DEFAULT_SENSORS[] = {
  //{ "HR Sensor", "HR", "00:00:00:00:00:01" },
  //{ "CSC Sensor", "CSC", "00:00:00:00:00:02" }
};
const size_t DEFAULT_SENSORS_COUNT = sizeof(DEFAULT_SENSORS) / sizeof(DEFAULT_SENSORS[0]);

// Default fans configuration
// These will be used if no fans are found in NVS storage
// Format: { token (32 hex chars), ip (optional, can be empty), connected (always false initially) }
// Can be empty or contain up to 4 fans
const struct {
  const char* token;      // 32 hex character token
  const char* ip;         // IP address (optional, can be empty string)
  bool connected;         // Initial connection status (always false)
} DEFAULT_FANS[] = {
  // Example (uncomment and modify as needed):
  // { "11111111111111111111111111111111", "192.168.1.100", false }
};
const size_t DEFAULT_FANS_COUNT = sizeof(DEFAULT_FANS) / sizeof(DEFAULT_FANS[0]);

// Helper function to calculate enabled sensor types from NVS before BLE initialization
// This ensures BLE only advertises services for sensors that are actually configured
uint8_t calculateEnabledSensorTypesFromNVS() {
  LOGI("Calculating enabled sensor types from NVS configuration...");
  
  // Create NVS config bus (doesn't require WebUI/WiFi)
  NVSConfigBus nvsConfigBus("appcfg");
  DynamicJsonDocument doc(2048);
  
  if (!nvsConfigBus.loadModuleConfig("blesensors", doc)) {
    LOGI("No sensors found in NVS, checking default sensors...");
    // No NVS config - check defaults
    uint8_t types = 0;
    for (size_t i = 0; i < DEFAULT_SENSORS_COUNT; i++) {
      if (strcmp(DEFAULT_SENSORS[i].Type, "HR") == 0) {
        types |= SENSOR_TYPE_HEART_RATE;
        LOGI("  Default sensor: HR");
      } else if (strcmp(DEFAULT_SENSORS[i].Type, "CSC") == 0) {
        types |= SENSOR_TYPE_SPEED_CADENCE;
        LOGI("  Default sensor: CSC");
      }
    }
    if (types == 0) {
      LOGI("  No default sensors, defaulting to HR only");
      return SENSOR_TYPE_HEART_RATE; // Default to HR if empty
    }
    return types;
  }
  
  // Parse sensors from NVS
  if (!doc.containsKey("sensors") || !doc["sensors"].is<JsonArray>()) {
    LOGI("Invalid NVS sensor data, defaulting to HR only");
    return SENSOR_TYPE_HEART_RATE; // Default
  }
  
  JsonArray sensors = doc["sensors"].as<JsonArray>();
  if (sensors.size() == 0) {
    LOGI("NVS sensors array is empty, defaulting to HR only");
    return SENSOR_TYPE_HEART_RATE; // Default
  }
  
  LOGI_F("Found %zu sensor(s) in NVS:", sensors.size());
  uint8_t types = 0;
  
  for (JsonObject sensor : sensors) {
    if (!sensor.containsKey("Type")) {
      continue;
    }
    String typeStr = sensor["Type"] | "";
    String name = sensor["Name"] | "";
    typeStr.toUpperCase();
    
    if (typeStr == "HR" || typeStr == "HEART_RATE") {
      types |= SENSOR_TYPE_HEART_RATE;
      LOGI_F("  Sensor: %s (HR)", name.c_str());
    } else if (typeStr == "CSC" || typeStr == "SPEED_CADENCE") {
      types |= SENSOR_TYPE_SPEED_CADENCE;
      LOGI_F("  Sensor: %s (CSC)", name.c_str());
    }
    // Note: PWR sensors are skipped (not supported in WebUI)
  }
  
  if (types == 0) {
    LOGI("  No valid sensors found, defaulting to HR only");
    return SENSOR_TYPE_HEART_RATE; // Default to HR if empty
  }
  
  LOGI_F("Calculated enabled sensor types: 0x%02X (HR=%s, CSC=%s)", 
         types,
         (types & SENSOR_TYPE_HEART_RATE) ? "yes" : "no",
         (types & SENSOR_TYPE_SPEED_CADENCE) ? "yes" : "no");
  
  return types;
}


// Optional: Sensor connection callbacks for Serial output
void onHRConnected() {
  BLE_LOG_INFO("[HR] ✓ Heart rate sensor connected!");
  
  // Display battery level if available
  if (BikeSensorServerHasBatteryLevel(SENSOR_TYPE_HEART_RATE)) {
    uint8_t batteryLevel;
    if (BikeSensorServerGetBatteryLevel(SENSOR_TYPE_HEART_RATE, &batteryLevel)) {
      BLE_LOG_INFO("[HR] Battery Level: " + String(batteryLevel) + "%");
    }
  }
}

void onHRDisconnected() {
  BLE_LOG_WARN("[HR] ✗ Heart rate sensor disconnected!");
  
  // Turn off all fans when HR sensor disconnects
  updateFansFromHeartRate(0);
  lastProcessedHR = 0;
}

void onCSCConnected() {
  BLE_LOG_INFO("[CSC] ✓ Speed/Cadence sensor connected!");
  
  // Display battery level if available
  if (BikeSensorServerHasBatteryLevel(SENSOR_TYPE_SPEED_CADENCE)) {
    uint8_t batteryLevel;
    if (BikeSensorServerGetBatteryLevel(SENSOR_TYPE_SPEED_CADENCE, &batteryLevel)) {
      BLE_LOG_INFO("[CSC] Battery Level: " + String(batteryLevel) + "%");
    }
  }
}

void onCSCDisconnected() {
  BLE_LOG_WARN("[CSC] ✗ Speed/Cadence sensor disconnected!");
}

// Helper functions for fan control callbacks
static bool g_fanPowerOn = false;
static uint8_t g_fanSpeedPercent = 0;

void controlFanCallback(const String& token, const IPAddress& ip) {
  // Validate IP address - must not be 0.0.0.0 (fan must be discovered)
  if (ip == IPAddress(0, 0, 0, 0)) {
    FAN_LOG_WARN("[FanControl] Fan not discovered yet (IP: 0.0.0.0), skipping control: " + token.substring(0, 8) + "...");
    return;
  }
  
  // Validate token
  if (token.length() != 32) {
    FAN_LOG_ERROR("[FanControl] Invalid token length: " + String(token.length()));
    return;
  }
  
  FAN_LOG_INFO("[FanControl] Controlling fan: " + token.substring(0, 8) + "... at " + ip.toString());
  
  // Use the same pattern as prepareFanContext() in SmartMiFanAsync
  // This matches how SmartMiFanAsync_setPowerAll() and setSpeedAll() work
  SmartMiFanAsync.attachUdp(fanUdp);
  if (!SmartMiFanAsync.setTokenFromHex(token.c_str())) {
    FAN_LOG_ERROR("[FanControl] Failed to set token for fan: " + token.substring(0, 8) + "...");
    return;
  }
  SmartMiFanAsync.setFanAddress(ip);
  
  FAN_LOG_INFO("[FanControl] Setting power: " + String(g_fanPowerOn ? "ON" : "OFF") + ", speed: " + String(g_fanSpeedPercent) + "%");
  
  // setPower() and setSpeed() will handle handshake internally
  bool powerResult = SmartMiFanAsync.setPower(g_fanPowerOn);
  if (!powerResult) {
    FAN_LOG_ERROR("[FanControl] Failed to set power for fan: " + token.substring(0, 8) + "... (handshake may have failed)");
    return;
  }
  
  FAN_LOG_INFO("[FanControl] Power set successfully");
  
  // Only set speed if fan is ON and speed is > 0
  // Don't send speed = 0 as it might turn the fan on
  if (g_fanPowerOn && g_fanSpeedPercent > 0) {
    bool speedResult = SmartMiFanAsync.setSpeed(g_fanSpeedPercent);
    if (!speedResult) {
      FAN_LOG_ERROR("[FanControl] Failed to set speed for fan: " + token.substring(0, 8) + "...");
    } else {
      FAN_LOG_INFO("[FanControl] Speed set successfully: " + String(g_fanSpeedPercent) + "%");
    }
  }
  // If fan is OFF, don't send speed command at all - just the power off is enough
}

void turnOffFanCallback(const String& token, const IPAddress& ip) {
  // Validate IP address - must not be 0.0.0.0 (fan must be discovered)
  if (ip == IPAddress(0, 0, 0, 0)) {
    FAN_LOG_WARN("[FanControl] Fan not discovered yet (IP: 0.0.0.0), skipping turn off: " + token.substring(0, 8) + "...");
    return;
  }
  
  // Validate token
  if (token.length() != 32) {
    FAN_LOG_ERROR("[FanControl] Invalid token length: " + String(token.length()));
    return;
  }
  
  FAN_LOG_INFO("[FanControl] Turning off fan: " + token.substring(0, 8) + "... at " + ip.toString());
  
  // Use the same pattern as prepareFanContext() in SmartMiFanAsync
  SmartMiFanAsync.attachUdp(fanUdp);
  if (!SmartMiFanAsync.setTokenFromHex(token.c_str())) {
    FAN_LOG_ERROR("[FanControl] Failed to set token: " + token.substring(0, 8) + "...");
    return;
  }
  SmartMiFanAsync.setFanAddress(ip);
  
  // setPower() and setSpeed() will handle handshake internally
  // Only send power off - don't send speed = 0 as it might turn the fan on
  bool powerResult = SmartMiFanAsync.setPower(false);
  if (!powerResult) {
    FAN_LOG_ERROR("[FanControl] Failed to turn off fan: " + token.substring(0, 8) + "... (handshake may have failed)");
  } else {
    FAN_LOG_INFO("[FanControl] Fan turned off successfully");
  }
  // Don't send setSpeed(0) - just power off is enough
}

// Heart rate to fan speed mapping function
void updateFansFromHeartRate(uint16_t heartRate) {
  // Early return if fan manager not initialized
  if (!fanManagerIntegration) {
    return;
  }
  
  // CRITICAL: Only control fans if they are ready (discovered + handshaked)
  // This prevents crashes from trying to control fans before discovery completes
  if (!fanManagerIntegration->areFansReady()) {
    // Use simple Serial.print to avoid String concatenation stack issues
    static unsigned long lastWarning = 0;
    if (millis() - lastWarning > 5000) {  // Only warn every 5 seconds to avoid spam
      lastWarning = millis();
      Serial.println("[FanControl] Fans not ready yet, skipping control");
    }
    return;
  }
  
  // Check if HR sensor is connected
  bool hrSensorConnected = BikeSensorServerIsSensorConnected(SENSOR_TYPE_HEART_RATE);
  
  // If sensor disconnected or HR is 0, turn all fans off
  if (!hrSensorConnected || heartRate == 0) {
    FAN_LOG_INFO("[FanControl] HR sensor disconnected or HR=0, turning all fans off");
    // Use forEachConfiguredFan to control fans even if not marked as connected
    fanManagerIntegration->forEachConfiguredFan(turnOffFanCallback);
    return;
  }
  
  // Get min/max heart rate values
  if (!bleSensorIntegration) {
    return;  // BLE sensor integration not initialized
  }
  
  uint16_t minHR = bleSensorIntegration->getMinHeartRate();
  uint16_t maxHR = bleSensorIntegration->getMaxHeartRate();
  
  // Calculate fan power and speed based on heart rate
  bool powerOn = false;
  uint8_t speedPercent = 0;
  
  if (heartRate < minHR) {
    // HR below minimum: fan off
    powerOn = false;
    speedPercent = 0;
  } else if (heartRate >= minHR && heartRate <= maxHR) {
    // HR in range: map to 0-100%
    powerOn = true;
    speedPercent = map(heartRate, minHR, maxHR, 0, 100);
    // Clamp to valid range (0-100)
    if (speedPercent > 100) speedPercent = 100;
  } else {
    // HR above maximum: fan at 100%
    powerOn = true;
    speedPercent = 100;
  }
  
  // Store values in static variables for callback
  g_fanPowerOn = powerOn;
  g_fanSpeedPercent = speedPercent;
  
  FAN_LOG_INFO("[FanControl] HR: " + String(heartRate) + " bpm -> Power: " + String(powerOn ? "ON" : "OFF") + ", Speed: " + String(speedPercent) + "%");
  
  // Update all configured fans that have been discovered (have valid IP addresses)
  // Only control fans that have been discovered and have a valid IP (not 0.0.0.0)
  // The callbacks will validate IP addresses and skip fans that aren't discovered yet
  fanManagerIntegration->forEachConfiguredFan(controlFanCallback);
}

// Optional: Data update callbacks for Serial output
void onHeartRateChanged(uint16_t hr) {
  BLE_LOG_INFO("[HR] Heart rate: " + String(hr) + " bpm");
  
  // Update WebUI with heart rate
  if (bleSensorIntegration) {
    bleSensorIntegration->updateHeartRate(hr);
  }
  
  // Update fans based on heart rate (with throttling)
  uint32_t currentTime = millis();
  if (currentTime - lastFanUpdateTime >= fanUpdateInterval) {
    // Only update if HR has changed or enough time has passed
    if (hr != lastProcessedHR || currentTime - lastFanUpdateTime >= fanUpdateInterval) {
      updateFansFromHeartRate(hr);
      lastFanUpdateTime = currentTime;
      lastProcessedHR = hr;
    }
  }
}

void onSpeedCadenceChanged(uint16_t speed, uint16_t cadence) {
  BLE_LOG_INFO("[CSC] Speed: " + String(speed / 100.0 * 3.6, 1) + " km/h, Cadence: " + String(cadence) + " rpm");
}


// Central device callbacks
void onCentralConnected(uint8_t count) {
  BLE_LOG_INFO("[Central] Device connected. Total: " + String(count));
}

void onCentralDisconnected(uint8_t count) {
  BLE_LOG_INFO("[Central] Device disconnected. Remaining: " + String(count));
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  LOGI("");
  LOGI("========================================");
  LOGI("BluetoothBikeSensorServer");
  LOGI("WebUI Integration Example");
  LOGI("(NimBLE-Arduino 2.x)");
  LOGI("========================================");
  LOGI("");
  
  // CRITICAL: Calculate enabled sensor types from NVS BEFORE BLE initialization
  // This ensures BLE only advertises services for sensors that are actually configured
  // This prevents advertising HR+CSC when only HR is configured (avoids Zwift confusion)
  uint8_t enabledSensorTypes = calculateEnabledSensorTypesFromNVS();
  
  // CRITICAL: Initialize BLE FIRST before WiFi
  // On ESP32, WiFi and BLE share the same radio. Initializing BLE first
  // ensures proper coexistence and prevents interference.
  // This matches the working HR_CSC_Example pattern (no WiFi = BLE first)
  LOGI("Initializing BLE sensor server with calculated sensor types (before WiFi)...");
  
  // Initialize BLE with:
  // - Device name
  // - Enabled sensor types (determines which services to advertise)
  BluetoothBikeSensorServerBegin("Bike_Fan_Snyc", enabledSensorTypes);
  

  LOGI("BLE Sensor Server initialized");
  
  // Small delay to ensure BLE is fully initialized
  delay(200);
  
  // Configure discovery settings (IMPORTANT: Without these, discovery may not find sensors!)
  // These settings match the DiscoveryExample for optimal results
  // Defaults are: Duration=20000ms, Pause=5000ms, MaxSensors=12
  // DiscoveryExample uses: Duration=15000ms, Pause=5000ms, MaxSensors=50
  BikeSensorServerSetDiscoveryDuration(15000);  // 15 seconds scan duration
  BikeSensorServerSetDiscoveryPause(5000);      // 5 seconds pause between scans
  BikeSensorServerSetMaxSensors(50);            // Allow up to 50 sensors
  
  LOGI("Discovery Configuration:");
  LOGI_F("  Duration: %u ms", BikeSensorServerGetDiscoveryDuration());
  LOGI_F("  Pause: %u ms", BikeSensorServerGetDiscoveryPause());
  LOGI_F("  Max Sensors: %u", BikeSensorServerGetMaxSensors());
  LOGI("");

  // Set data callbacks (optional - for Serial output)
  BluetoothBikeSensorServerSetHeartRateCallback(onHeartRateChanged);
  BluetoothBikeSensorServerSetSpeedCadenceCallback(onSpeedCadenceChanged);
  
  // Set per-sensor-type connect/disconnect callbacks (optional)
  BikeSensorServerSetSensorConn(SENSOR_TYPE_HEART_RATE, onHRConnected);
  BikeSensorServerSetSensorDisc(SENSOR_TYPE_HEART_RATE, onHRDisconnected);
  BikeSensorServerSetSensorConn(SENSOR_TYPE_SPEED_CADENCE, onCSCConnected);
  BikeSensorServerSetSensorDisc(SENSOR_TYPE_SPEED_CADENCE, onCSCDisconnected);
  
  // Set central device callbacks (optional)
  BikeSensorServerSetClientConn(onCentralConnected);
  BikeSensorServerSetClientDisc(onCentralDisconnected);

  // IMPORTANT: Add sensors IMMEDIATELY after BLE initialization
  // This matches the working HR_CSC_Example pattern where sensors are added right after Begin()
  // Note: BLE was initialized with sensor types calculated from NVS (or defaults)
  // We'll load from NVS later (which will replace these defaults if NVS has sensors)
  // For now, add default sensors to get BLE scanning started (if defaults are defined)
  LOGI("Adding default sensors to start BLE scanning...");
  if (DEFAULT_SENSORS_COUNT > 0) {
    for (size_t i = 0; i < DEFAULT_SENSORS_COUNT; i++) {
      SensorType type;
      if (strcmp(DEFAULT_SENSORS[i].Type, "HR") == 0) {
        type = SENSOR_TYPE_HEART_RATE;
      } else if (strcmp(DEFAULT_SENSORS[i].Type, "CSC") == 0) {
        type = SENSOR_TYPE_SPEED_CADENCE;
      } else {
        continue;
      }
      
      BikeSensorServerAddSensor(type, DEFAULT_SENSORS[i].Name);
      LOGI_F("  Added default sensor: %s (%s)", DEFAULT_SENSORS[i].Name, DEFAULT_SENSORS[i].Type);
    }
    LOGI("Sensors added. Library will scan and connect automatically.");
  } else {
    LOGI("No default sensors defined. Sensors will be loaded from NVS or added via WebUI.");
  }
  LOGI("");
  
  // Now initialize WiFi (after BLE is fully set up)
  LOGI_F("Connecting to WiFi: %s", ssid);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  // Alternative: Use WiFiManager (uncomment if using)
  // wm.setConfigPortalTimeout(180);
  // if (!wm.autoConnect("ESP32-Sensor-Setup")) {
  //   LOGE("Failed to connect, restarting...");
  //   delay(3000);
  //   ESP.restart();
  // }
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    LOGV(".");
    // Keep BLE running during WiFi connection
    BluetoothBikeSensorServerLoop();
  }
  
  LOGI("");
  IPAddress localIP = WiFi.localIP();
  LOGI_F("WiFi connected! IP address: %d.%d.%d.%d", localIP[0], localIP[1], localIP[2], localIP[3]);
  LOGI("");

  // Initialize WebUI (handles mDNS, WebServer, HTML serving)
  // NOTE: Initialize WebUI BEFORE loading sensors to set up NVS config bus
  if (webUI.begin("esp32-setup")) {
    LOGI("WebUI initialized successfully");
  } else {
    LOGE("WebUI initialization failed!");
    return;  // Can't continue without WebUI
  }

  // Create BLE sensor integration (must be after webUI.begin())
  static BLESensorIntegration bleIntegration(webUI.getServer());
  bleSensorIntegration = &bleIntegration;
  
  // Set NVS config bus for sensor persistence
  bleSensorIntegration->setNVSConfigBus(&nvsConfigBus);
  
  // Create Fan manager integration (must be after webUI.begin())
  static FanManagerIntegration fanIntegration(webUI.getServer(), fanUdp);
  fanManagerIntegration = &fanIntegration;
  
  // Set NVS config bus for fan persistence
  fanManagerIntegration->setNVSConfigBus(&nvsConfigBus);

  // Register BLE sensor integration with WebUI
  webUI.registerBLESensorIntegration(bleSensorIntegration);
  
  // Enable BLESensorManager in WebUI (enabled by default, but explicit for clarity)
  webUI.enableBLESensorManager(true);
  
  // Register Fan manager integration with WebUI
  webUI.registerFanManagerIntegration(fanManagerIntegration);
  
  // Enable FanManager in WebUI (enabled by default, but explicit for clarity)
  webUI.enableFanManager(true);
  
  // Initialize FanManagerIntegration HTTP endpoints
  fanManagerIntegration->begin();
  
  // Load sensors from NVS storage (if available) and replace defaults
  // Note: Default sensors were already added above to start BLE scanning immediately
  // Now we check NVS and replace them if NVS has saved sensors
  LOGI("Loading sensors from NVS storage...");
  if (bleSensorIntegration->loadSensorsFromNVS()) {
    LOGI("Sensors loaded from NVS successfully (replaced defaults)");
  } else {
    LOGI("No sensors found in NVS, using default sensors (already added above)");
    // Save default sensors to NVS for next time
    if (bleSensorIntegration->saveSensorsToNVS()) {
      LOGI("Default sensors saved to NVS");
    } else {
      LOGW("WARNING: Failed to save default sensors to NVS");
    }
  }
  
  // The BLE relay server is already advertising after BluetoothBikeSensorServerBegin()
  // setupLocalBLEServer() is called by Begin(), which starts advertising automatically
  // No need to call Resume() here - it only works if the server was previously paused
  LOGI("BLE Relay Server ready (advertising)");
  LOGI("Library will scan and connect to sensors automatically (matching HR_CSC_Example behavior)");

  // Load fans from NVS storage (if available)
  // If no fans found in NVS, load default fans
  LOGI("Loading fans from NVS storage...");
  if (!fanManagerIntegration->loadFansFromNVS()) {
    LOGI("No fans found in NVS, loading default fans...");
    // Load default fans
    for (size_t i = 0; i < DEFAULT_FANS_COUNT; i++) {
      String token = String(DEFAULT_FANS[i].token);
      if (token.length() == 32) {  // Validate token length
        // Parse IP address if provided
        IPAddress ip(0, 0, 0, 0);
        if (strlen(DEFAULT_FANS[i].ip) > 0) {
          if (ip.fromString(DEFAULT_FANS[i].ip)) {
            LOGI_F("  Default fan IP: %s", DEFAULT_FANS[i].ip);
          } else {
            LOGW_F("  Invalid IP address for default fan: %s", DEFAULT_FANS[i].ip);
          }
        }
        
        // Add fan with IP address in one call
        if (fanManagerIntegration->addFanToken(token, ip)) {
          LOGI_F("  ✓ Added default fan: %s", token.c_str());
        } else {
          LOGW_F("  ✗ Failed to add default fan (max reached or duplicate): %s", token.c_str());
        }
      } else {
        LOGW_F("  ✗ Invalid default fan token (must be 32 chars): %s", token.c_str());
      }
    }
    
    // Save default fans to NVS
    if (fanManagerIntegration->saveFansToNVS()) {
      LOGI("Default fans saved to NVS");
    } else {
      LOGW("WARNING: Failed to save default fans to NVS");
    }
  } else {
    LOGI("Fans loaded from NVS successfully");
  }
  
  // Start automatic fan discovery and handshake
  // This follows the same procedure as MultipleFansAsync example:
  // 1. Start discovery with fan tokens
  // 2. Wait for discovery to complete (handled in loop() via updateDiscovery())
  // 3. Perform handshake (handled automatically by checkFanConnections())
  // 4. Then fans can be controlled
  if (fanManagerIntegration && fanManagerIntegration->getFanCount() > 0) {
    LOGI("Starting automatic fan discovery...");
    if (fanManagerIntegration->startAutoDiscovery(5000)) {
      LOGI("Fan discovery started successfully (5 second timeout)");
      LOGI("Fans will be discovered and handshaked automatically");
    } else {
      LOGW("Failed to start fan discovery");
    }
  } else {
    LOGI("No fans configured, skipping discovery");
  }
  
  // Restore fans to WebUI via JavaScript injection
  // The WebUI will fetch fans via the /api/fans/list endpoint on page load
  // This is handled automatically by the FanManagerIntegration
  LOGI("Fans will be restored to WebUI on page load via API");

  // Load min/max heart rate values from NVS (merge with defaults)
  LOGI("Loading heart rate range from NVS...");
  uint16_t minHR, maxHR;
  const uint16_t DEFAULT_MIN_HR = 80;
  const uint16_t DEFAULT_MAX_HR = 160;
  
  if (bleSensorIntegration->loadHeartRateRange(DEFAULT_MIN_HR, DEFAULT_MAX_HR, &minHR, &maxHR)) {
    LOGI_F("  Min Heart Rate: %u (default: %u)", minHR, DEFAULT_MIN_HR);
    LOGI_F("  Max Heart Rate: %u (default: %u)", maxHR, DEFAULT_MAX_HR);
    
    // Inject initial values into WebUI
    // Use snprintf to avoid String concatenation stack issues
    char jsBuffer[256];
    snprintf(jsBuffer, sizeof(jsBuffer),
      "if(typeof window.HeartRateManager !== 'undefined') {"
      "  const hrMinEl = document.getElementById('hr-min-value');"
      "  const hrMaxEl = document.getElementById('hr-max-value');"
      "  if(hrMinEl) hrMinEl.textContent = %u;"
      "  if(hrMaxEl) hrMaxEl.textContent = %u;"
      "}", minHR, maxHR);
    webUI.injectJavaScript(String(jsBuffer));
  } else {
    LOGW("  WARNING: Failed to load heart rate range, using defaults");
  }

  // Note: Root routes (/ and /setup) are automatically registered by webUI.begin()
  // BLESensorManager is accessible via the "Setup Sensors" button in the WebUI

  LOGI("");
  LOGI("========================================");
  LOGI("Setup complete!");
  LOGI("========================================");
  LOGI("");
  LOGI("WebUI is now available at:");
  LOGI("  http://esp32-setup.local (mDNS)");
  IPAddress ipAddr = WiFi.localIP();
  LOGI_F("  http://%d.%d.%d.%d (IP address)", ipAddr[0], ipAddr[1], ipAddr[2], ipAddr[3]);
  LOGI("");
  LOGI("To manage sensors:");
  LOGI("  1. Open the WebUI in your browser");
  LOGI("  2. Click the 'Setup Sensors' button");
  LOGI("  3. Use the BLESensorManager to discover and connect sensors");
  LOGI("");
  LOGI("Sensor status will be displayed in Serial Monitor");
  LOGI("========================================");
  LOGI("");
}

void loop() {
  // Handle WebServer requests (compatible with WiFiManager)
  webUI.handleClient();
  
  // Handle BLE sensor server operations
  BluetoothBikeSensorServerLoop();
  
  // Update fan discovery if in progress
  if (fanManagerIntegration) {
    fanManagerIntegration->updateDiscovery();
    
    // Periodically check fan connections (every 30 seconds)
    // This ensures connection status stays up-to-date even without discovery
    // Note: checkFanConnections() performs handshakes, so we don't want to call it too frequently
    static uint32_t lastFanConnectionCheck = 0;
    if (millis() - lastFanConnectionCheck >= 30000) {
      lastFanConnectionCheck = millis();
      // Only check if fans are configured
      // We check discovery state internally in checkFanConnections() to avoid conflicts
      if (fanManagerIntegration->getFanCount() > 0) {
        FAN_LOG_DEBUG("[FanControl] Periodic fan connection check...");
        fanManagerIntegration->checkFanConnections();
      }
    }
  }
  
  // Optional: Display sensor status periodically (every 10 seconds)
  static uint32_t lastStatusPrint = 0;
  if (millis() - lastStatusPrint >= 10000) {
    lastStatusPrint = millis();
    
    LOGI("--- Sensor Status ---");
    
    // Heart Rate
    if (BikeSensorServerIsSensorConnected(SENSOR_TYPE_HEART_RATE)) {
      char name[64];
      String status = "HR: Connected - ";
      if (BikeSensorServerGetSensorName(SENSOR_TYPE_HEART_RATE, name, sizeof(name))) {
        status += String(name);
        if (BikeSensorServerHasBatteryLevel(SENSOR_TYPE_HEART_RATE)) {
          uint8_t batteryLevel;
          if (BikeSensorServerGetBatteryLevel(SENSOR_TYPE_HEART_RATE, &batteryLevel)) {
            status += " (Battery: " + String(batteryLevel) + "%)";
          }
        }
        LOGI(status);
      }
    } else {
      LOGI("HR: Not connected");
    }
    
    // Speed/Cadence
    if (BikeSensorServerIsSensorConnected(SENSOR_TYPE_SPEED_CADENCE)) {
      char name[64];
      String status = "CSC: Connected - ";
      if (BikeSensorServerGetSensorName(SENSOR_TYPE_SPEED_CADENCE, name, sizeof(name))) {
        status += String(name);
        if (BikeSensorServerHasBatteryLevel(SENSOR_TYPE_SPEED_CADENCE)) {
          uint8_t batteryLevel;
          if (BikeSensorServerGetBatteryLevel(SENSOR_TYPE_SPEED_CADENCE, &batteryLevel)) {
            status += " (Battery: " + String(batteryLevel) + "%)";
          }
        }
        LOGI(status);
      }
    } else {
      LOGI("CSC: Not connected");
    }
    
    LOGI_F("Central devices: %u", BluetoothBikeSensorServerGetCentralCount());
    LOGI("");
  }
  
  // If using WiFiManager, also call:
  // wm.process();
  
  delay(10); // Small delay to prevent overwhelming the system
}

