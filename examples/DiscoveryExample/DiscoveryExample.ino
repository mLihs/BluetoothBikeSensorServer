/*
 * BluetoothBikeSensorServer - Discovery Example
 * 
 * This example demonstrates how to use the discovery feature to find
 * Bluetooth sensors (Heart Rate, Speed/Cadence, Power Meter).
 * 
 * IMPORTANT: This library requires NimBLE-Arduino 2.x (NOT 1.x compatible).
 * 
 * Uses async discovery pattern - heartbeat shows during scan, results printed when complete.
 * After each discovery scan completes, the results are printed and
 * a new scan is manually restarted after a pause (matching BLE_BikeSensorScan_Async.ino pattern).
 * 
 * The library no longer auto-restarts discovery. Manual restart is implemented in the callback.
 * This gives you full control over when to restart discovery.
 */

// Enable library debug logging to see discovery results
#define BLE_LIB_DEBUG_GEN

#include <BluetoothBikeSensorServer.h>
#include <DebugLog.h>

// Pause duration between discovery scans (in milliseconds)
const uint32_t DISCOVERY_PAUSE_MS = 5000;

// Track when to restart discovery (non-blocking delay)
unsigned long nextDiscoveryStartMs = 0;
bool discoveryScheduled = false;

// Discovery complete callback - manually restarts discovery after pause
void onDiscoveryComplete(uint8_t count) {
  LOGI("");
  LOGI_F("=== Discovery complete: %u sensors found ===", count);
  
  if (count > 0) {
    // List all found sensors
    LOGI("All sensors:");
    LOGI(BikeSensorServerListSensors(LIST_FORMAT_FULL));
    
    // Show counts by type
    LOGI_F("HR sensors: %u", BikeSensorServerGetSensorCountByType(SENSOR_TYPE_HEART_RATE));
    LOGI_F("CSC sensors: %u", BikeSensorServerGetSensorCountByType(SENSOR_TYPE_SPEED_CADENCE));
    LOGI_F("Power sensors: %u", BikeSensorServerGetSensorCountByType(SENSOR_TYPE_POWER_METER));
    
    // List individual sensors
    LOGI("");
    LOGI("Individual sensors:");
    for (uint8_t i = 0; i < count; i++) {
      SensorType type;
      char name[64];
      uint8_t mac[6];
      char macStr[18];
      if (BikeSensorServerGetSensor(i, &type, name, sizeof(name), mac)) {
        snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        const char* typeStr = "";
        switch(type) {
          case SENSOR_TYPE_HEART_RATE: typeStr = "HR"; break;
          case SENSOR_TYPE_SPEED_CADENCE: typeStr = "CSC"; break;
          case SENSOR_TYPE_POWER_METER: typeStr = "Power"; break;
          default: typeStr = "Unknown"; break;
        }
        LOGI_F("  [%u] Type=%s, Name=%s, MAC=%s", i, typeStr, name, macStr);
      }
    }
  } else {
    LOGI("No sensors found");
  }
  LOGI("");
  
  // Schedule next discovery scan after the pause duration
  nextDiscoveryStartMs = millis() + DISCOVERY_PAUSE_MS;
  discoveryScheduled = true;
  
  LOGI_F("Next scan in %u seconds...", DISCOVERY_PAUSE_MS / 1000);
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  LOGI("");
  LOGI("=== Async BLE Sensor Discovery (HR / CSC / Power) ===");
  LOGI("");
  
  // Initialize the library (discovery doesn't require sensors to be enabled)
  // Device name is now MAC-based: "{prefix}-xxxxxxxx" (where xxxxxxxx is the WiFi-style suffix from eFuse MAC)
  // You can change the prefix using BikeSensorServerSetDeviceNamePrefix("yourprefix")
  BluetoothBikeSensorServerBegin(nullptr);  // nullptr = default "homewind" prefix
  
  // Set discovery callback - required for manual restart after each scan
  BikeSensorServerSetDiscoveryCompleteCallback(onDiscoveryComplete);
  
  // Set discovery duration (optional, default is 20 seconds)
  // Matching BLE_BikeSensorScan_Async.ino: DISCOVERY_DURATION_MS = 15000
  BikeSensorServerSetDiscoveryDuration(15000);
  
  // Set max sensors (optional, default is 12)
  // Matching BLE_BikeSensorScan_Async.ino: MAX_DEVICES = 50
  BikeSensorServerSetMaxSensors(50);
  
  LOGI("Discovery Configuration:");
  LOGI_F("Duration: %u ms", BikeSensorServerGetDiscoveryDuration());
  LOGI_F("Pause: %u ms", DISCOVERY_PAUSE_MS);
  LOGI_F("Max Sensors: %u", BikeSensorServerGetMaxSensors());
  LOGI("");
  
  // Start first discovery for all sensor types
  // Subsequent discoveries will be restarted manually in the callback
  BikeSensorServerStartDiscovery(SENSOR_TYPE_HEART_RATE); //| SENSOR_TYPE_SPEED_CADENCE | SENSOR_TYPE_POWER_METER
  LOGI("Discovery started. Heartbeat (.) will show during scan.");
  LOGI("Results will be printed when scan completes.");
  LOGI("");
}

void loop() {
  // Must call this regularly to handle BLE operations
  // This handles heartbeat and discovery completion
  BluetoothBikeSensorServerLoop();
  
  // Check if it's time to restart discovery (manual restart after pause)
  if (discoveryScheduled && millis() >= nextDiscoveryStartMs) {
    discoveryScheduled = false;
    LOGI("Restarting discovery...");
    BikeSensorServerStartDiscovery(SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_SPEED_CADENCE | SENSOR_TYPE_POWER_METER);
  }
  
  // Small delay like async example (matching BLE_BikeSensorScan_Async.ino line 193)
  delay(10);
}

