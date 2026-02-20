/**
 * @file TelemetryExampleMac.ino
 * @brief Telemetry example using direct MAC connection
 */

// Optional: Enable debug logging
#define BLE_LIB_DEBUG_GEN

#include <BluetoothBikeSensorServer.h>
#include <BLETelemetry.h>

// Sensor configuration (MAC mode - no name needed, just MAC address)
// 
// IMPORTANT: BLE Address Type
// ===========================
// Each BLE device has an address type shown in Discovery output:
//   [HR] TICKR FIT (f0:13:c3:ee:56:97) 0  <- use 0
//   [CSC] Speed Sensor (fc:a6:1f:27:45:16) 1  <- use 1
//
// If your sensor doesn't connect, try the other address type (0 or 1)!
const char* HR_SENSOR_MAC = "f0:13:c3:ee:56:97";
const int8_t HR_ADDR_TYPE = 0;  // Use 0 or 1 from Discovery output!

void onDiscoveryComplete(uint8_t count) {
  Serial.printf("\n=== Discovery complete: %d sensors found ===\n", count);
  Serial.println(BikeSensorServerListSensors(LIST_FORMAT_FULL));
  
  // Print telemetry summary after discovery
  Serial.println("\n=== Telemetry after discovery ===");
  BLETelemetry::printSummary();
}

void onHeartRateChanged(uint16_t hr) {
  //(void)hr;
  Serial.printf("Heart rate: %d bpm\n", hr);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n");
  Serial.println("╔═══════════════════════════════════════════════════════╗");
  Serial.println("║   BluetoothBikeSensorServer Telemetry Example (MAC)   ║");
  Serial.println("╚═══════════════════════════════════════════════════════╝");
  Serial.println();
  
  // *** ENABLE TELEMETRY ***
  BLETelemetry::setEnabled(true);
  
  // Initialize the BLE Sensor Server in MAC Sleep-Scan mode (v2.2.11.0+)
  // BBS_MODE_MAC = MAC Sleep-Scan mode (periodic short scans to find MAC)
  BluetoothBikeSensorServerBegin(
    "telemetry-mac",           // Device name prefix
    SENSOR_TYPE_HEART_RATE,    // Enabled sensor types
    BBS_MODE_MAC               // Connection mode: MAC Sleep-Scan (periodic scans)
  );
  
  // Set callbacks
  BluetoothBikeSensorServerSetHeartRateCallback(onHeartRateChanged);
  BikeSensorServerSetDiscoveryCompleteCallback(onDiscoveryComplete);
  
  // Add sensor by MAC (Sleep-Scan will find and connect)
  // Use explicit address type from Discovery (see comments above)
  BikeSensorServerAddSensorByMac(SENSOR_TYPE_HEART_RATE, nullptr, HR_SENSOR_MAC, HR_ADDR_TYPE);
  
  Serial.println("Setup complete. Telemetry will print every 30 seconds.");
  Serial.println("Commands: m=metrics, s=summary, r=reset, d=discovery, h=help\n");
}

void loop() {
  // Main library loop (telemetry is integrated here)
  BluetoothBikeSensorServerLoop();
  
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
        
      case 'd': // Start discovery
        Serial.println("Starting discovery...");
        BikeSensorServerStartDiscovery(SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_SPEED_CADENCE);
        break;
        
      case 't': // Toggle telemetry
        BLETelemetry::setEnabled(!BLETelemetry::isEnabled());
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

void printHelp() {
  Serial.println("\n=== Telemetry Commands ===");
  Serial.println("  m - Print full metrics report");
  Serial.println("  s - Print summary line");
  Serial.println("  r - Reset metrics");
  Serial.println("  d - Start discovery scan");
  Serial.println("  t - Toggle telemetry on/off");
  Serial.println("  h - Show this help");
  Serial.printf("  Telemetry is: %s\n", BLETelemetry::isEnabled() ? "ENABLED" : "DISABLED");
  Serial.println("===========================\n");
}
