/*
 * BluetoothBikeSensorServer - Heart Rate + Speed/Cadence Example
 * 
 * This example demonstrates connecting to both a Heart Rate sensor
 * and a Speed/Cadence sensor simultaneously using name-based connection.
 * 
 * IMPORTANT: This library requires NimBLE-Arduino 2.x (NOT 1.x compatible).
 */

#include <BluetoothBikeSensorServer.h>

#define DEBUG_LOG_LEVEL 3
#include <DebugLog.h>

// Sensor names - replace with your actual sensor names (exact match required)
const char* HR_SENSOR_NAME = "TICKR FIT C571";      // Replace with your HR sensor name
const char* CSC_SENSOR_NAME = "Ridesense";     // Replace with your CSC sensor name

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
}

void onHRDisconnected() {
  LOGW("[HR] ✗ Heart rate sensor disconnected!");
}

void onCSCConnected() {
  LOGI("[CSC] ✓ Speed/Cadence sensor connected!");
}

void onCSCDisconnected() {
  LOGW("[CSC] ✗ Speed/Cadence sensor disconnected!");
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
  LOGI("Heart Rate + Speed/Cadence Example");
  LOGI("(NimBLE-Arduino 2.x)");
  LOGI("========================================");
  LOGI("");
  
  // Initialize the library with Heart Rate and Speed/Cadence sensors enabled
  // Device name is now MAC-based: "{prefix}-xxxxxxxx" (where xxxxxxxx is the WiFi-style suffix from eFuse MAC)
  // You can change the prefix using BikeSensorServerSetDeviceNamePrefix("yourprefix")
  // The library uses name-based connection - MAC addresses are resolved internally via scanning
  
  BluetoothBikeSensorServerBegin(nullptr, SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_SPEED_CADENCE);  // nullptr = default "homewind" prefix
  
  // Set data callbacks
  BluetoothBikeSensorServerSetHeartRateCallback(onHeartRateChanged);
  BluetoothBikeSensorServerSetSpeedCadenceCallback(onSpeedCadenceChanged);
  
  // Set per-sensor-type connect/disconnect callbacks
  BikeSensorServerSetSensorConn(SENSOR_TYPE_HEART_RATE, onHRConnected);
  BikeSensorServerSetSensorDisc(SENSOR_TYPE_HEART_RATE, onHRDisconnected);
  BikeSensorServerSetSensorConn(SENSOR_TYPE_SPEED_CADENCE, onCSCConnected);
  BikeSensorServerSetSensorDisc(SENSOR_TYPE_SPEED_CADENCE, onCSCDisconnected);
  
  // Set central device callbacks
  BikeSensorServerSetClientConn(onCentralConnected);
  BikeSensorServerSetClientDisc(onCentralDisconnected);
  
  LOGI("Adding sensors...");
  LOGI("");
  
  // Add Heart Rate sensor by device name
  LOGI("Adding Heart Rate sensor...");
  if (BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, HR_SENSOR_NAME)) {
    LOGI_F("  ✓ HR sensor added: %s", HR_SENSOR_NAME);
  } else {
    LOGE("  ✗ Failed to add HR sensor");
  }
  
  // Add Speed/Cadence sensor by device name
  LOGI("Adding Speed/Cadence sensor...");
  if (BikeSensorServerAddSensor(SENSOR_TYPE_SPEED_CADENCE, CSC_SENSOR_NAME)) {
    LOGI_F("  ✓ CSC sensor added: %s", CSC_SENSOR_NAME);
  } else {
    LOGE("  ✗ Failed to add CSC sensor");
  }
  
  LOGI("");
  LOGI("Sensors added. Library will scan and connect automatically.");
  LOGI("");
}

void loop() {
  // Must call this regularly to handle BLE operations
  BluetoothBikeSensorServerLoop();
  
  // Display sensor connection status periodically
  static uint32_t lastStatusPrint = 0;
  if (millis() - lastStatusPrint >= 5000) { // Every 5 seconds
    lastStatusPrint = millis();
    
    LOGI("--- Sensor Status ---");
    
    // Heart Rate
    if (BikeSensorServerIsSensorConnected(SENSOR_TYPE_HEART_RATE)) {
      char mac[18], name[64];
      if (BikeSensorServerGetSensorMac(SENSOR_TYPE_HEART_RATE, mac, sizeof(mac))) {
        if (BikeSensorServerGetSensorName(SENSOR_TYPE_HEART_RATE, name, sizeof(name))) {
          // Add battery level if available
          if (BikeSensorServerHasBatteryLevel(SENSOR_TYPE_HEART_RATE)) {
            uint8_t batteryLevel;
            if (BikeSensorServerGetBatteryLevel(SENSOR_TYPE_HEART_RATE, &batteryLevel)) {
              LOGI_F("HR: Connected (%s) - %s - Battery: %u%%", mac, name, batteryLevel);
            } else {
              LOGI_F("HR: Connected (%s) - %s", mac, name);
            }
          } else {
            LOGI_F("HR: Connected (%s) - %s", mac, name);
          }
        } else {
          LOGI_F("HR: Connected (%s)", mac);
        }
      }
    } else {
      LOGI("HR: Not connected");
    }
    
    // Speed/Cadence
    if (BikeSensorServerIsSensorConnected(SENSOR_TYPE_SPEED_CADENCE)) {
      char mac[18], name[64];
      if (BikeSensorServerGetSensorMac(SENSOR_TYPE_SPEED_CADENCE, mac, sizeof(mac))) {
        if (BikeSensorServerGetSensorName(SENSOR_TYPE_SPEED_CADENCE, name, sizeof(name))) {
          // Add battery level if available
          if (BikeSensorServerHasBatteryLevel(SENSOR_TYPE_SPEED_CADENCE)) {
            uint8_t batteryLevel;
            if (BikeSensorServerGetBatteryLevel(SENSOR_TYPE_SPEED_CADENCE, &batteryLevel)) {
              LOGI_F("CSC: Connected (%s) - %s - Battery: %u%%", mac, name, batteryLevel);
            } else {
              LOGI_F("CSC: Connected (%s) - %s", mac, name);
            }
          } else {
            LOGI_F("CSC: Connected (%s) - %s", mac, name);
          }
        } else {
          LOGI_F("CSC: Connected (%s)", mac);
        }
      }
    } else {
      LOGI("CSC: Not connected");
    }
    
    LOGI("");
  }
  
  // Display latest sensor data
  static uint32_t lastDataPrint = 0;
  if (millis() - lastDataPrint >= 2000) { // Every 2 seconds
    lastDataPrint = millis();
    
    if (BluetoothBikeSensorServerHasHeartRate()) {
      LOGI_F("Latest HR: %u bpm", BluetoothBikeSensorServerGetLastHeartRate());
    }
    
    if (BluetoothBikeSensorServerHasSpeedCadence()) {
      uint16_t speed, cadence;
      if (BluetoothBikeSensorServerGetLastSpeedCadence(&speed, &cadence)) {
        LOGI_F("Latest Speed: %.1f km/h, Cadence: %u rpm", speed / 100.0 * 3.6, cadence);
      }
    }
    
    // Display battery levels
    if (BikeSensorServerHasBatteryLevel(SENSOR_TYPE_HEART_RATE)) {
      uint8_t batteryLevel;
      if (BikeSensorServerGetBatteryLevel(SENSOR_TYPE_HEART_RATE, &batteryLevel)) {
        LOGI_F("HR Battery: %u%%", batteryLevel);
      }
    }
    
    if (BikeSensorServerHasBatteryLevel(SENSOR_TYPE_SPEED_CADENCE)) {
      uint8_t batteryLevel;
      if (BikeSensorServerGetBatteryLevel(SENSOR_TYPE_SPEED_CADENCE, &batteryLevel)) {
        LOGI_F("CSC Battery: %u%%", batteryLevel);
      }
    }
    
    LOGI_F("Central devices: %u", BluetoothBikeSensorServerGetCentralCount());
    LOGI("");
  }
  
  delay(100); // Small delay to prevent overwhelming the serial output
}

