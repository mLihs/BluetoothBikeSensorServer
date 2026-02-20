/*
 * BluetoothBikeSensorServer - Heart Rate Only Example
 * 
 * This example demonstrates connecting to a single Heart Rate sensor using
 * name-based connection (NimBLE-Arduino 2.x).
 * 
 * IMPORTANT: This library requires NimBLE-Arduino 2.x (NOT 1.x compatible).
 * 
 * This is the simplest use case and demonstrates the name-only connection workflow.
 */

#include <BluetoothBikeSensorServer.h>
#include <DebugLog.h>

// Sensor name - replace with your actual sensor name (exact match required)
const char* HR_SENSOR_NAME = "TICKR FIT C571";  // Replace with your HR sensor name

// Data update callback
void onHeartRateChanged(uint16_t hr) {
  LOGI_F("Heart rate: %u bpm", hr);
}

// Sensor connection callbacks (per-type API)
void onSensorConnected() {
  LOGI("✓ Heart rate sensor connected!");
}

void onSensorDisconnected() {
  LOGW("✗ Heart rate sensor disconnected!");
}

// Central device callbacks
void onCentralConnected(uint8_t count) {
  LOGI_F("Central device connected. Total: %u", count);
}

void onCentralDisconnected(uint8_t count) {
  LOGI_F("Central device disconnected. Remaining: %u", count);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  LOGI("");
  LOGI("========================================");
  LOGI("BluetoothBikeSensorServer");
  LOGI("Heart Rate Only Example");
  LOGI("(NimBLE-Arduino 2.x)");
  LOGI("========================================");
  LOGI("");
  
  // Initialize the library with only Heart Rate sensor enabled
  // Device name is now MAC-based: "{prefix}-xxxxxxxx" (where xxxxxxxx is the WiFi-style suffix from eFuse MAC)
  // You can change the prefix using BikeSensorServerSetDeviceNamePrefix("yourprefix")
  // The library uses name-based connection - MAC addresses are resolved internally via scanning
  
  BluetoothBikeSensorServerBegin(nullptr, SENSOR_TYPE_HEART_RATE);  // nullptr = default "homewind" prefix
  
  // Set heart rate callback
  BluetoothBikeSensorServerSetHeartRateCallback(onHeartRateChanged);
  
  // Set sensor connection callbacks (per-type API)
  BikeSensorServerSetSensorConn(SENSOR_TYPE_HEART_RATE, onSensorConnected);
  BikeSensorServerSetSensorDisc(SENSOR_TYPE_HEART_RATE, onSensorDisconnected);
  
  // Set central device callbacks
  BikeSensorServerSetClientConn(onCentralConnected);
  BikeSensorServerSetClientDisc(onCentralDisconnected);
  
  LOGI("Adding Heart Rate sensor...");
  LOGI("");
  
  // Add Heart Rate sensor by device name (name-only API)
  if (BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, HR_SENSOR_NAME)) {
    LOGI_F("  ✓ HR sensor added: %s", HR_SENSOR_NAME);
    LOGI("  Library will scan and connect automatically.");
  } else {
    LOGE("  ✗ Failed to add HR sensor");
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
    
    if (BikeSensorServerIsSensorConnected(SENSOR_TYPE_HEART_RATE)) {
      char sensorName[64];
      if (BikeSensorServerGetSensorName(SENSOR_TYPE_HEART_RATE, sensorName, sizeof(sensorName))) {
        LOGI_F("Status: Connected to %s", sensorName);
      } else {
        LOGI("Status: Connected (name not available)");
      }
    } else {
      LOGI("Status: Not connected - scanning...");
    }
    LOGI("");
  }
  
  // Display latest heart rate data
  static uint32_t lastDataPrint = 0;
  if (millis() - lastDataPrint >= 2000) { // Every 2 seconds
    lastDataPrint = millis();
    
    if (BluetoothBikeSensorServerHasHeartRate()) {
      LOGI_F("Heart Rate: %u bpm", BluetoothBikeSensorServerGetLastHeartRate());
    } else {
      LOGI("Heart Rate: No data yet");
    }
    
    // Display battery level if available
    if (BikeSensorServerHasBatteryLevel(SENSOR_TYPE_HEART_RATE)) {
      uint8_t batteryLevel;
      if (BikeSensorServerGetBatteryLevel(SENSOR_TYPE_HEART_RATE, &batteryLevel)) {
        LOGI_F("Battery Level: %u%%", batteryLevel);
      }
    } else {
      LOGI("Battery Level: Not available");
    }
    
    LOGI_F("Central devices: %u", BluetoothBikeSensorServerGetCentralCount());
    LOGI("");
  }
  
  delay(100); // Small delay to prevent overwhelming the serial output
}

