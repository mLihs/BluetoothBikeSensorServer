/**
 * @file TelemetryExample.ino
 * @brief Example showing how to use the BLE Telemetry system
 * 
 * This example demonstrates how to enable and use the telemetry system
 * for monitoring performance metrics of the BluetoothBikeSensorServer library.
 * 
 * The telemetry system collects:
 * - Loop timing (min/avg/max, slow loop counts)
 * - Heap memory usage
 * - BLE operations (connections, scans, advertising)
 * 
 * Telemetry is DISABLED by default. Enable it via:
 *   BLETelemetry::setEnabled(true);
 * 
 * @note Telemetry adds minimal overhead when disabled (just a flag check).
 */

// Optional: Enable debug logging
 #define BLE_LIB_DEBUG_GEN

#include <BluetoothBikeSensorServer.h>
#include <BLETelemetry.h>

// Sensor configuration - change these to your sensor names
const char* HR_SENSOR_NAME = "TICKR FIT C571";//"Polar H10 0D8A9336";//"TICKR FIT C571";
const char* CSC_SENSOR_NAME = "Ridesense";

void onDiscoveryComplete(uint8_t count) {
  Serial.printf("\n=== Discovery complete: %d sensors found ===\n", count);
  Serial.println(BikeSensorServerListSensors(LIST_FORMAT_FULL));
  
  // Print telemetry summary after discovery
  Serial.println("\n=== Telemetry after discovery ===");
  BLETelemetry::printSummary();
}


void onHeartRateChanged(uint16_t hr) {
  Serial.printf("Heart rate: %d bpm\n", hr);
}


void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n");
  Serial.println("╔═══════════════════════════════════════════════════════╗");
  Serial.println("║     BluetoothBikeSensorServer Telemetry Example       ║");
  Serial.println("╚═══════════════════════════════════════════════════════╝");
  Serial.println();
  
  // *** ENABLE TELEMETRY ***
  // This MUST be called to enable telemetry (disabled by default)
  BLETelemetry::setEnabled(true);
  
  // Initialize the BLE Sensor Server
  // Telemetry begin() is automatically called
  // Using scan-based mode (default) - sensors are found by device name
  BluetoothBikeSensorServerBegin(
    "telemetry-test",
    SENSOR_TYPE_HEART_RATE, //| SENSOR_TYPE_SPEED_CADENCE
    BBS_MODE_SCAN           // Connection mode: scan-based (default)
  );
  
  // Note: For MAC Sleep-Scan mode (periodic short scans), use:
  // BluetoothBikeSensorServerBegin("telemetry-test", SENSOR_TYPE_HEART_RATE, BBS_MODE_MAC);
  // Then use: BikeSensorServerAddSensorByMac(SENSOR_TYPE_HEART_RATE, "aa:bb:cc:dd:ee:ff", -1);
  
  // Set callbacks
  BluetoothBikeSensorServerSetHeartRateCallback(onHeartRateChanged);
  BikeSensorServerSetDiscoveryCompleteCallback(onDiscoveryComplete);
  
  // Add sensors
  BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, HR_SENSOR_NAME);
  //BikeSensorServerAddSensor(SENSOR_TYPE_SPEED_CADENCE, CSC_SENSOR_NAME);
  
  Serial.println("Setup complete. Telemetry will print every 30 seconds.");
  Serial.println("Commands: m=metrics, s=summary, r=reset, d=discovery, h=help\n");
}

void loop() {
  // Main library loop (telemetry is integrated here)
  BluetoothBikeSensorServerLoop();
  
    // Alive indicator every 100ms
  static unsigned long lastAliveMs = 0;
  unsigned long now = millis();
  if (now - lastAliveMs >= 1000) {
    Serial.println(".");
    lastAliveMs = now;
  }

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
