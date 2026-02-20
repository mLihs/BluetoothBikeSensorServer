/*
 * BluetoothBikeSensorServer - Manual Sensor Addition Example
 * 
 * This example demonstrates how to manually add sensors by device name
 * at runtime. Sensors can be added in setup() or during loop() execution.
 * 
 * IMPORTANT: This library requires NimBLE-Arduino 2.x (NOT 1.x compatible).
 * 
 * The library will automatically attempt to connect to added sensors
 * and keep retrying if the connection fails.
 * 
 * NOTE: This library uses name-only connection. MAC addresses are resolved
 * internally via scanning. Use the exact device name as shown in discovery.
 */

#include <BluetoothBikeSensorServer.h>
#include <DebugLog.h>

// Sensor names - replace with your actual sensor names (exact match required)
const char* HR_SENSOR_NAME = "TICKR FIT C571";      // Replace with your HR sensor name
const char* CSC_SENSOR_NAME = "Ridesense";     // Replace with your CSC sensor name
const char* POWER_SENSOR_NAME = "Power Meter"; // Replace with your Power sensor name

// Data update callbacks
void onHeartRateChanged(uint16_t hr) {
  LOGI_F("[HR] Heart rate: %u bpm", hr);
}

void onSpeedCadenceChanged(uint16_t speed, uint16_t cadence) {
  LOGI_F("[CSC] Speed: %.1f km/h, Cadence: %u rpm", speed / 100.0 * 3.6, cadence);
}

void onPowerChanged(int16_t power) {
  LOGI_F("[Power] Power: %d W", power);
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

void onPowerConnected() {
  LOGI("[Power] ✓ Power meter connected!");
}

void onPowerDisconnected() {
  LOGW("[Power] ✗ Power meter disconnected!");
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
  LOGI("Manual Sensor Addition Example");
  LOGI("(NimBLE-Arduino 2.x)");
  LOGI("========================================");
  LOGI("");
  
  // Initialize the library with all sensor types enabled
  // Device name is now MAC-based: "{prefix}-xxxxxxxx" (where xxxxxxxx is the WiFi-style suffix from eFuse MAC)
  // You can change the prefix using BikeSensorServerSetDeviceNamePrefix("yourprefix")
  // The library uses name-based connection - MAC addresses are resolved internally via scanning
  
  BluetoothBikeSensorServerBegin(nullptr, 
                                  SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_SPEED_CADENCE | SENSOR_TYPE_POWER_METER);  // nullptr = default "homewind" prefix
  
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
  
  LOGI("Adding sensors manually...");
  LOGI("");
  
  // Example 1: Add sensors in setup() - they will connect automatically
  LOGI("Adding Heart Rate sensor...");
  if (BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, HR_SENSOR_NAME)) {
    LOGI_F("  ✓ HR sensor added: %s", HR_SENSOR_NAME);
  } else {
    LOGE("  ✗ Failed to add HR sensor");
  }
  
  LOGI("Adding Speed/Cadence sensor...");
  if (BikeSensorServerAddSensor(SENSOR_TYPE_SPEED_CADENCE, CSC_SENSOR_NAME)) {
    LOGI_F("  ✓ CSC sensor added: %s", CSC_SENSOR_NAME);
  } else {
    LOGE("  ✗ Failed to add CSC sensor");
  }
  
  LOGI("Adding Power Meter sensor...");
  if (BikeSensorServerAddSensor(SENSOR_TYPE_POWER_METER, POWER_SENSOR_NAME)) {
    LOGI_F("  ✓ Power sensor added: %s", POWER_SENSOR_NAME);
  } else {
    LOGE("  ✗ Failed to add Power sensor");
  }
  
  LOGI("");
  LOGI("Sensors added. Library will attempt to connect automatically.");
  LOGI("You can also add sensors later in loop() if needed.");
  LOGI("");
}

void loop() {
  // Must call this regularly to handle BLE operations
  BluetoothBikeSensorServerLoop();
  
  // Example: Add a sensor dynamically based on some condition
  // (Uncomment and modify as needed)
  /*
  static bool sensorAdded = false;
  if (!sensorAdded && someCondition) {
    BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, "Sensor Name");
    sensorAdded = true;
  }
  */
  
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
          LOGI_F("HR: Connected (%s) - %s", mac, name);
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
          LOGI_F("CSC: Connected (%s) - %s", mac, name);
        } else {
          LOGI_F("CSC: Connected (%s)", mac);
        }
      }
    } else {
      LOGI("CSC: Not connected");
    }
    
    // Power Meter
    if (BikeSensorServerIsSensorConnected(SENSOR_TYPE_POWER_METER)) {
      char mac[18], name[64];
      if (BikeSensorServerGetSensorMac(SENSOR_TYPE_POWER_METER, mac, sizeof(mac))) {
        if (BikeSensorServerGetSensorName(SENSOR_TYPE_POWER_METER, name, sizeof(name))) {
          LOGI_F("Power: Connected (%s) - %s", mac, name);
        } else {
          LOGI_F("Power: Connected (%s)", mac);
        }
      }
    } else {
      LOGI("Power: Not connected");
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
    
    if (BluetoothBikeSensorServerHasPower()) {
      LOGI_F("Latest Power: %d W", BluetoothBikeSensorServerGetLastPower());
    }
    
    LOGI_F("Central devices: %u", BluetoothBikeSensorServerGetCentralCount());
    LOGI("");
  }
  
  delay(100); // Small delay to prevent overwhelming the serial output
}

/*
 * Example: How to change sensors at runtime
 * 
 * To replace a sensor with a different one:
 * 
 *   // Remove the old sensor
 *   BikeSensorServerRemoveSensor(SENSOR_TYPE_HEART_RATE);
 *   
 *   // Add the new sensor by name
 *   BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, "New Sensor Name");
 * 
 * To add a sensor discovered via discovery:
 * 
 *   // After discovery completes, you can add sensors from the discovered list
 *   for (uint8_t i = 0; i < BikeSensorServerGetSensorCount(); i++) {
 *     SensorType type;
 *     char name[64];
 *     uint8_t mac[6];
 *     if (BikeSensorServerGetSensor(i, &type, name, sizeof(name), mac)) {
 *       if (type == SENSOR_TYPE_HEART_RATE) {
 *         BikeSensorServerAddSensor(type, name);  // Use name from discovery
 *         break;
 *       }
 *     }
 *   }
 * 
 * Example: Dynamically change enabled sensor types at runtime
 * 
 *   // Change from all sensors to only HR + CSC (removes Power Meter)
 *   // This will:
 *   // - Update advertising to show only HR and CSC services
 *   // - Disconnect all connected central devices (forces them to rediscover)
 *   // - Create missing services if needed
 *   // Note: This function may not exist in the current API - check BluetoothBikeSensorServer.h
 *   // BikeSensorServerSetEnabledSensorTypes(SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_SPEED_CADENCE);
 * 
 * Note: When changing sensor configuration, the relay server's advertising identity
 * (services and manufacturer data) is updated. Central devices (like Zwift) may need
 * to reconnect or restart their scan to see updated services.
 */

