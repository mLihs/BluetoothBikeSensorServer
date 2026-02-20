/**
 * @file TelemetryExampleMacHRCSC.ino
 * @brief Telemetry example using MAC Sleep-Scan mode for HR and CSC sensors
 * 
 * This example demonstrates:
 * - MAC Sleep-Scan mode (BBS_MODE_MAC) - periodic short scans, lower power consumption
 * - Connecting to both Heart Rate and Speed/Cadence sensors simultaneously
 * - Telemetry monitoring for performance metrics
 * 
 * IMPORTANT: This library requires NimBLE-Arduino 2.x (NOT 1.x compatible).
 */

// Optional: Enable debug logging
#define BLE_LIB_DEBUG_GEN

#include <BluetoothBikeSensorServer.h>
#include <BLETelemetry.h>

// Sensor configuration (MAC Sleep-Scan mode - periodic short scans to find MAC)
// 
// IMPORTANT: BLE Address Type
// ===========================
// Each BLE device has an address type shown in Discovery output:
//   [HR] TICKR FIT (f0:13:c3:ee:56:97) 0  <- use 0
//   [CSC] Ridesense (fc:a6:1f:27:45:16) 1  <- use 1
//
// If your sensor doesn't connect, try the other address type (0 or 1)!
const char* HR_SENSOR_MAC = "f0:13:c3:ee:56:88";  // Replace with your HR sensor MAC f0:13:c3:ee:56:97
const int8_t HR_ADDR_TYPE = 0;  // Use 0 or 1 from Discovery output!

const char* CSC_SENSOR_MAC = "fc:a6:1f:27:45:16";  // Speed/Cadence sensor MAC
const int8_t CSC_ADDR_TYPE = 1;  // Use 0 or 1 from Discovery output!

void onDiscoveryComplete(uint8_t count) {
  Serial.printf("\n=== Discovery complete: %d sensors found ===\n", count);
  Serial.println(BikeSensorServerListSensors(LIST_FORMAT_FULL));
  
  // Print telemetry summary after discovery
  Serial.println("\n=== Telemetry after discovery ===");
  BLETelemetry::printSummary();
}

void onHeartRateChanged(uint16_t hr) {
  /*Serial.printf("Heart rate: %d bpm\n", hr);*/
}

void onSpeedCadenceChanged(uint16_t speed, uint16_t cadence) {
  /*float speedKmh = speed / 100.0 * 3.6;  // Convert to km/h
  Serial.printf("Speed: %.1f km/h, Cadence: %u rpm\n", speedKmh, cadence);*/
}

void onHRConnected() {
  Serial.println("[HR] ✓ Heart rate sensor connected!");
}

void onHRDisconnected() {
  Serial.println("[HR] ✗ Heart rate sensor disconnected!");
}

void onCSCConnected() {
  Serial.println("[CSC] ✓ Speed/Cadence sensor connected!");
}

void onCSCDisconnected() {
  Serial.println("[CSC] ✗ Speed/Cadence sensor disconnected!");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n");
  Serial.println("╔═══════════════════════════════════════════════════════╗");
  Serial.println("║  BluetoothBikeSensorServer Telemetry Example (MAC)   ║");
  Serial.println("║              HR + Speed/Cadence Sensors               ║");
  Serial.println("╚═══════════════════════════════════════════════════════╝");
  Serial.println();
  
  // *** ENABLE TELEMETRY ***
  BLETelemetry::setEnabled(true);
  
  // Initialize the BLE Sensor Server in MAC Sleep-Scan mode (v2.2.11.0+)
  // BBS_MODE_MAC = MAC Sleep-Scan mode (periodic short scans to find MAC)
  BluetoothBikeSensorServerBegin(
    "telemetry-mac-hrcsc",                    // Device name prefix
    SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_SPEED_CADENCE,  // Enabled sensor types
    BBS_MODE_MAC                              // Connection mode: MAC Sleep-Scan (periodic scans)
  );
  
  // Set data callbacks
  BluetoothBikeSensorServerSetHeartRateCallback(onHeartRateChanged);
  BluetoothBikeSensorServerSetSpeedCadenceCallback(onSpeedCadenceChanged);
  
  // Set per-sensor-type connect/disconnect callbacks
  BikeSensorServerSetSensorConn(SENSOR_TYPE_HEART_RATE, onHRConnected);
  BikeSensorServerSetSensorDisc(SENSOR_TYPE_HEART_RATE, onHRDisconnected);
  BikeSensorServerSetSensorConn(SENSOR_TYPE_SPEED_CADENCE, onCSCConnected);
  BikeSensorServerSetSensorDisc(SENSOR_TYPE_SPEED_CADENCE, onCSCDisconnected);
  
  // Set discovery callback
  BikeSensorServerSetDiscoveryCompleteCallback(onDiscoveryComplete);
  
  // Add sensors by MAC (Sleep-Scan will find and connect)
  // Use explicit address type from Discovery (see comments at top of file)
  Serial.println("Adding sensors by MAC address...");
  if (BikeSensorServerAddSensorByMac(SENSOR_TYPE_HEART_RATE, nullptr, HR_SENSOR_MAC, HR_ADDR_TYPE)) {
    Serial.printf("  ✓ HR sensor added: %s (addrType=%d)\n", HR_SENSOR_MAC, HR_ADDR_TYPE);
  } else {
    Serial.println("  ✗ Failed to add HR sensor");
  }
  
  if (BikeSensorServerAddSensorByMac(SENSOR_TYPE_SPEED_CADENCE, nullptr, CSC_SENSOR_MAC, CSC_ADDR_TYPE)) {
    Serial.printf("  ✓ CSC sensor added: %s (addrType=%d)\n", CSC_SENSOR_MAC, CSC_ADDR_TYPE);
  } else {
    Serial.println("  ✗ Failed to add CSC sensor");
  }
  
  Serial.println();
  Serial.println("Setup complete. Telemetry will print every 30 seconds.");
  Serial.println("Commands: m=metrics, s=summary, r=reset, d=discovery, h=help\n");
}

void loop() {
  // Main library loop (telemetry is integrated here)
  BluetoothBikeSensorServerLoop();
  
  // Alive indicator every 100ms
  /*static unsigned long lastAliveMs = 0;
  unsigned long now = millis();
  if (now - lastAliveMs >= 1000) {
    Serial.println(".");
    lastAliveMs = now;
  }*/

  // Handle serial commands
  if (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'm': // Print full metrics
        BLETelemetry::printMetrics();
        break;
        
      case 's': // Print summary
        BLETelemetry::printSummary();
        break;
        
      case 'r': // Reset metrics
        BLETelemetry::reset();
        Serial.println("Telemetry reset.");
        break;
        
      case 'd': // Start discovery (search for all enabled sensor types)
        Serial.println("Starting discovery...");
        BikeSensorServerStartDiscovery(SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_SPEED_CADENCE);
        break;
        
      case 't': // Toggle telemetry
        BLETelemetry::setEnabled(!BLETelemetry::isEnabled());
        Serial.printf("Telemetry: %s\n", BLETelemetry::isEnabled() ? "ENABLED" : "DISABLED");
        break;
        
      case 'c': // Show connection status
        printConnectionStatus();
        break;
        
      case 'h': // Help
        printHelp();
        break;
        
      default:
        break;
    }
  }
  
  // Small delay to prevent watchdog issues
  delay(10);
}

void printConnectionStatus() {
  Serial.println("\n=== Connection Status ===");
  
  // Connection mode
  BBSConnectionMode mode = BikeSensorServerGetConnectionMode();
  Serial.printf("Connection Mode: %s\n", mode == BBS_MODE_MAC ? "MAC Sleep-Scan" : "Scan-based");
  
  // HR Sensor
  if (BikeSensorServerIsSensorConnected(SENSOR_TYPE_HEART_RATE)) {
    char mac[18], name[64];
    if (BikeSensorServerGetSensorMac(SENSOR_TYPE_HEART_RATE, mac, sizeof(mac))) {
      if (BikeSensorServerGetSensorName(SENSOR_TYPE_HEART_RATE, name, sizeof(name))) {
        Serial.printf("HR: Connected - %s (%s)\n", name, mac);
      } else {
        Serial.printf("HR: Connected - %s\n", mac);
      }
    } else {
      Serial.println("HR: Connected (MAC unknown)");
    }
  } else {
    Serial.println("HR: Not connected");
  }
  
  // CSC Sensor
  if (BikeSensorServerIsSensorConnected(SENSOR_TYPE_SPEED_CADENCE)) {
    char mac[18], name[64];
    if (BikeSensorServerGetSensorMac(SENSOR_TYPE_SPEED_CADENCE, mac, sizeof(mac))) {
      if (BikeSensorServerGetSensorName(SENSOR_TYPE_SPEED_CADENCE, name, sizeof(name))) {
        Serial.printf("CSC: Connected - %s (%s)\n", name, mac);
      } else {
        Serial.printf("CSC: Connected - %s\n", mac);
      }
    } else {
      Serial.println("CSC: Connected (MAC unknown)");
    }
  } else {
    Serial.println("CSC: Not connected");
  }
  
  // Scanning status (should be false in MAC mode)
  Serial.printf("Scanning: %s\n", BikeSensorServerIsScanning() ? "YES" : "NO");
  Serial.printf("Central devices: %u\n", BluetoothBikeSensorServerGetCentralCount());
  Serial.println("==========================\n");
}

void printHelp() {
  Serial.println("\n=== Telemetry Commands ===");
  Serial.println("  m - Print full metrics report");
  Serial.println("  s - Print summary line");
  Serial.println("  r - Reset metrics");
  Serial.println("  d - Start discovery scan");
  Serial.println("  t - Toggle telemetry on/off");
  Serial.println("  c - Show connection status");
  Serial.println("  h - Show this help");
  Serial.printf("  Telemetry is: %s\n", BLETelemetry::isEnabled() ? "ENABLED" : "DISABLED");
  Serial.printf("  Connection Mode: %s\n", 
                BikeSensorServerGetConnectionMode() == BBS_MODE_MAC ? "MAC Sleep-Scan" : "Scan-based");
  Serial.println("===========================\n");
}
