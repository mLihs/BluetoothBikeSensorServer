/*
 * BluetoothBikeSensorServer - Fast Reconnection Example
 * 
 * This example demonstrates the Sensor MAC Preservation feature for faster sensor reconnection.
 * When enabled, the library preserves the MAC address of disconnected sensors and
 * attempts direct reconnection without scanning, significantly reducing reconnection time.
 * 
 * IMPORTANT: This library requires NimBLE-Arduino 2.x (NOT 1.x compatible).
 * 
 * Benefits of Sensor MAC Preservation:
 * - Faster reconnection: Direct connection attempts (~1 second) vs scanning (~5-20 seconds)
 * - Lower power consumption: No continuous scanning during reconnection
 * - Better for stable sensors: Ideal for bike sensors with static MAC addresses
 * 
 * Default behavior (Sensor MAC Preservation disabled):
 * - Sensor MAC address is cleared on disconnect
 * - Library rescans to find device by name
 * - More conservative, ensures fresh device information
 */

#include <BluetoothBikeSensorServer.h>
#include <DebugLog.h>

// Sensor names - replace with your actual sensor names (exact match required)
const char* HR_SENSOR_NAME = "TICKR FIT C571";      // Replace with your HR sensor name
const char* CSC_SENSOR_NAME = "Ridesense";     // Replace with your CSC sensor name (optional)

// Data update callbacks
void onHeartRateChanged(uint16_t hr) {
  LOGI_F("[HR] Heart rate: %u bpm", hr);
}

void onSpeedCadenceChanged(uint16_t speed, uint16_t cadence) {
  LOGI_F("[CSC] Speed: %.1f km/h, Cadence: %u rpm", speed / 100.0 * 3.6, cadence);
}

// Sensor connection callbacks
void onHRConnected() {
  LOGI("[HR] ✓ Heart rate sensor connected!");
  
  // Get and display MAC address
  char mac[18];
  if (BikeSensorServerGetSensorMac(SENSOR_TYPE_HEART_RATE, mac, sizeof(mac))) {
    LOGI_F("[HR] MAC address: %s", mac);
  }
}

void onHRDisconnected() {
  LOGW("[HR] ✗ Heart rate sensor disconnected!");
  LOGI("[HR] Will attempt reconnection...");
  
  // Show Sensor MAC Preservation status
  if (BikeSensorServerGetPreserveMacOnDisconnect()) {
    LOGI("[HR] Fast reconnection enabled - Sensor MAC preserved, will reconnect directly");
  } else {
    LOGI("[HR] Standard reconnection - Sensor MAC cleared, will rescan");
  }
}

void onCSCConnected() {
  LOGI("[CSC] ✓ Speed/Cadence sensor connected!");
  
  // Get and display MAC address
  char mac[18];
  if (BikeSensorServerGetSensorMac(SENSOR_TYPE_SPEED_CADENCE, mac, sizeof(mac))) {
    LOGI_F("[CSC] MAC address: %s", mac);
  }
}

void onCSCDisconnected() {
  LOGW("[CSC] ✗ Speed/Cadence sensor disconnected!");
  
  if (BikeSensorServerGetPreserveMacOnDisconnect()) {
    LOGI("[CSC] Fast reconnection enabled - Sensor MAC preserved");
  } else {
    LOGI("[CSC] Standard reconnection - will rescan");
  }
}

// Central device callbacks
void onCentralConnected(uint8_t count) {
  LOGI_F("[Central] Device connected. Total: %u", count);
}

void onCentralDisconnected(uint8_t count) {
  LOGI_F("[Central] Device disconnected. Remaining: %u", count);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  LOGI("");
  LOGI("========================================");
  LOGI("BluetoothBikeSensorServer");
  LOGI("Fast Reconnection Example");
  LOGI("(Sensor MAC Preservation Feature)");
  LOGI("========================================");
  LOGI("");
  
  // Initialize the library
  // Device name is now MAC-based: "{prefix}-xxxxxxxx" (where xxxxxxxx is the WiFi-style suffix from eFuse MAC)
  // You can change the prefix using BikeSensorServerSetDeviceNamePrefix("yourprefix")
  BluetoothBikeSensorServerBegin(nullptr, 
                                 SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_SPEED_CADENCE);  // nullptr = default "homewind" prefix
  
  // ============================================
  // Enable Sensor MAC Preservation for fast reconnection
  // ============================================
  // This preserves the sensor's MAC address on disconnect and attempts direct reconnection
  // without scanning, reducing reconnection time from ~5-20 seconds to ~1 second
  //
  // Enable this if:
  // - Your sensors have stable MAC addresses (static/public, not privacy/random)
  // - You want faster reconnection times after temporary disconnections
  // - You're willing to trade a bit of conservatism for speed
  //
  // Keep disabled (default) if:
  // - Your sensors may change MAC addresses
  // - You want to ensure fresh device information on each reconnection
  // - You prefer more conservative behavior
  BikeSensorServerSetPreserveMacOnDisconnect(true);
  LOGI("✓ Fast reconnection enabled (Sensor MAC Preservation)");
  LOGI("");
  
  // Set data callbacks
  BluetoothBikeSensorServerSetHeartRateCallback(onHeartRateChanged);
  BluetoothBikeSensorServerSetSpeedCadenceCallback(onSpeedCadenceChanged);
  
  // Set sensor connection callbacks
  BikeSensorServerSetSensorConn(SENSOR_TYPE_HEART_RATE, onHRConnected);
  BikeSensorServerSetSensorDisc(SENSOR_TYPE_HEART_RATE, onHRDisconnected);
  BikeSensorServerSetSensorConn(SENSOR_TYPE_SPEED_CADENCE, onCSCConnected);
  BikeSensorServerSetSensorDisc(SENSOR_TYPE_SPEED_CADENCE, onCSCDisconnected);
  
  // Set central device callbacks
  BikeSensorServerSetClientConn(onCentralConnected);
  BikeSensorServerSetClientDisc(onCentralDisconnected);
  
  LOGI("Adding sensors...");
  
  // Add Heart Rate sensor
  if (BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, HR_SENSOR_NAME)) {
    LOGI_F("  ✓ HR sensor added: %s", HR_SENSOR_NAME);
  } else {
    LOGE("  ✗ Failed to add HR sensor");
  }
  
  // Add Speed/Cadence sensor (optional - comment out if you only have HR)
  if (BikeSensorServerAddSensor(SENSOR_TYPE_SPEED_CADENCE, CSC_SENSOR_NAME)) {
    LOGI_F("  ✓ CSC sensor added: %s", CSC_SENSOR_NAME);
  } else {
    LOGE("  ✗ Failed to add CSC sensor");
  }
  
  LOGI("");
  LOGI("Library will scan and connect automatically.");
  LOGI("After first connection, disconnections will trigger fast reconnection.");
  LOGI("");
  
  // Display current Sensor MAC Preservation setting
  LOGI_F("Sensor MAC Preservation: %s", 
         BikeSensorServerGetPreserveMacOnDisconnect() ? "ENABLED" : "DISABLED");
  LOGI("");
  
  // Check initial advertising status after a brief delay
  delay(500);
  if (BikeSensorServerIsAdvertising()) {
    LOGI("✓ Relay server is advertising and should be visible in BLE scanners/Zwift");
  } else {
    LOGW("⚠️  Relay server is NOT advertising - attempting to start...");
    if (BikeSensorServerRestartAdvertising()) {
      LOGI("✓ Advertising started successfully");
    } else {
      LOGE("✗ Failed to start advertising - check BLE initialization logs above");
      LOGE("  Look for [SERVER] and [INIT] messages to diagnose the issue");
    }
  }
  LOGI("");
}

void loop() {
  // Must call this regularly to handle BLE operations
  BluetoothBikeSensorServerLoop();
  
  // Display sensor connection status periodically
  static uint32_t lastStatusPrint = 0;
  if (millis() - lastStatusPrint >= 5000) { // Every 5 seconds
    lastStatusPrint = millis();
    
    LOGI("=== Status ===");
    
    // Check and display relay server advertising status
    bool isAdvertising = BikeSensorServerIsAdvertising();
    if (isAdvertising) {
      LOGI("Relay Server: ✓ Advertising (visible to Zwift/BLE scanners)");
    } else {
      LOGW("Relay Server: ✗ NOT advertising - may not be visible!");
    }
    
    // Heart Rate sensor status
    if (BikeSensorServerIsSensorConnected(SENSOR_TYPE_HEART_RATE)) {
      char name[64], mac[18];
      if (BikeSensorServerGetSensorName(SENSOR_TYPE_HEART_RATE, name, sizeof(name))) {
        BikeSensorServerGetSensorMac(SENSOR_TYPE_HEART_RATE, mac, sizeof(mac));
        LOGI_F("HR: Connected to %s (%s)", name, mac);
      }
      
      // Display battery level if available
      if (BikeSensorServerHasBatteryLevel(SENSOR_TYPE_HEART_RATE)) {
        uint8_t battery;
        if (BikeSensorServerGetBatteryLevel(SENSOR_TYPE_HEART_RATE, &battery)) {
          LOGI_F("     Battery: %u%%", battery);
        }
      }
    } else {
      LOGI("HR: Not connected");
    }
    
    // Speed/Cadence sensor status
    if (BikeSensorServerIsSensorConnected(SENSOR_TYPE_SPEED_CADENCE)) {
      char name[64], mac[18];
      if (BikeSensorServerGetSensorName(SENSOR_TYPE_SPEED_CADENCE, name, sizeof(name))) {
        BikeSensorServerGetSensorMac(SENSOR_TYPE_SPEED_CADENCE, mac, sizeof(mac));
        LOGI_F("CSC: Connected to %s (%s)", name, mac);
      }
    } else {
      LOGI("CSC: Not connected");
    }
    
    LOGI_F("Central devices: %u", BluetoothBikeSensorServerGetCentralCount());
    LOGI("");
  }
  
  // Advertising diagnostic check (every 10 seconds)
  static uint32_t lastAdvCheck = 0;
  if (millis() - lastAdvCheck >= 10000) {
    lastAdvCheck = millis();
    
    // Check if advertising stopped unexpectedly
    // Note: Advertising may be temporarily stopped when central devices are connected (normal BLE behavior)
    uint8_t centralCount = BluetoothBikeSensorServerGetCentralCount();
    bool isAdvertising = BikeSensorServerIsAdvertising();
    
    if (!isAdvertising && centralCount == 0) {
      // Only report as an issue if there are no active connections
      // When centrals are connected, advertising is often paused (normal behavior)
      LOGW("[DIAG] ⚠️  Advertising is NOT active and no central connections. Attempting restart...");
      // Force restart advertising
      if (BikeSensorServerRestartAdvertising()) {
        LOGI("[DIAG] ✓ Advertising restarted successfully");
      } else {
        LOGE("[DIAG] ✗ Failed to restart advertising - server may need reinitialization");
        LOGI("[DIAG] Try calling BluetoothBikeSensorServerResetServer() or restart the ESP32");
      }
    } else if (!isAdvertising && centralCount > 0) {
      // Advertising stopped but there are connections - this is normal, don't log as error
      LOGD_F("[DIAG] Advertising paused (normal - active central connections: %u)", centralCount);
    }
  }
  
  // Display latest sensor data periodically
  static uint32_t lastDataPrint = 0;
  if (millis() - lastDataPrint >= 2000) { // Every 2 seconds
    lastDataPrint = millis();
    
    if (BluetoothBikeSensorServerHasHeartRate()) {
      LOGI_F("Heart Rate: %u bpm", BluetoothBikeSensorServerGetLastHeartRate());
    }
    
    if (BluetoothBikeSensorServerHasSpeedCadence()) {
      uint16_t speed, cadence;
      if (BluetoothBikeSensorServerGetLastSpeedCadence(&speed, &cadence)) {
        LOGI_F("Speed: %.1f km/h, Cadence: %u rpm", speed / 100.0 * 3.6, cadence);
      }
    }
    
    LOGI("");
  }
  
  delay(100); // Small delay to prevent overwhelming the serial output
}

