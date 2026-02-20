/*
 * BluetoothBikeSensorServer - Homewind BLE Identity Example
 * 
 * This example demonstrates how to use the Homewind BLE Identity module
 * to configure BLE advertising with stable device names and beacon service data.
 * 
 * IMPORTANT: This library requires NimBLE-Arduino 2.x (NOT 1.x compatible).
 * 
 * Features demonstrated:
 * - Stable device name: "{prefix}-xxxxxxxx" from eFuse MAC lower 32-bit (WiFi-style suffix)
 * - Beacon Service UUID with Service Data payload (always advertised)
 * - Dynamic service advertising based on sensor mask
 * - Guard snapshot to rebuild advertising only when changed
 * - Integration with BluetoothBikeSensorServer library
 * - ZWIFT_CTRL service support
 * 
 * The example simulates sensor configuration changes and shows how
 * advertising is updated automatically only when needed.
 */

#include <BluetoothBikeSensorServer.h>
#include <DebugLog.h>
// Note: HomewindBleIdentity.h is automatically included via BluetoothBikeSensorServer.h
// You can use homewind:: functions directly without an extra include.

// Simulated firmware version (in real usage, get from your build system)
#define FW_MAJOR 1
#define FW_MINOR 0

// Global state
homewind::AdvSnapshot advSnapshot;
uint8_t currentSensorMask = 0x00;
uint8_t deviceState = 0x00;

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
  LOGI("Homewind BLE Identity Example");
  LOGI("========================================");
  LOGI("");
  
  // Display device identity information (default "{prefix}-xxxxxxxx")
  LOGI("Device Identity (default 'homewind' prefix):");
  LOGI_F("  BLE Device Name: %s", homewind::GetBleDeviceNameLower().c_str());
  LOGI_F("  mDNS Hostname: %s", homewind::GetMdnsHostnameLower().c_str());
  LOGI_F("  Instance ID: 0x%04X", homewind::GetInstanceId());
  LOGI("");
  
  // Demonstrate changing the device name prefix
  LOGI("=== Changing Device Name Prefix ===");
  LOGI("Setting prefix to 'mysensor'...");
  homewind::SetDeviceNamePrefix("mysensor");
  LOGI_F("  New BLE Device Name: %s", homewind::GetBleDeviceNameLower().c_str());
  LOGI_F("  New mDNS Hostname: %s", homewind::GetMdnsHostnameLower().c_str());
  LOGI("");
  
  // Change it back to "homewind" for the rest of the example
  homewind::SetDeviceNamePrefix("homewind");
  LOGI("Changed back to 'homewind' prefix");
  LOGI("");
  
  LOGI("Note: Use the hostname for mDNS:");
  LOGI_F("  MDNS.begin(\"%s\");", homewind::GetMdnsHostnameLower().c_str());
  LOGI("");
  
  // Initialize NimBLE (required before using identity module)
  NimBLEDevice::init("");
  
  // Get advertising object
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  
  // Initial configuration: HR sensor only
  LOGI("=== Initial Configuration ===");
  currentSensorMask = 0x01;  // HR only (bit 0)
  deviceState = 0x00;        // Normal state
  
  LOGI("Configuring advertising (HR sensor only)...");
  homewind::ConfigureAdvertising(adv, currentSensorMask, deviceState, 
                                 FW_MAJOR, FW_MINOR, false);
  
  // Initialize snapshot manually for first configuration
  advSnapshot.sensorMask = currentSensorMask;
  advSnapshot.state = deviceState;
  advSnapshot.fwMajor = FW_MAJOR;
  advSnapshot.fwMinor = FW_MINOR;
  advSnapshot.instanceId = homewind::GetInstanceId();
  advSnapshot.advertiseZwiftLegacyUuid = false;
  
  // Start advertising
  adv->start(0);  // 0 = advertise forever
  LOGI("✓ Advertising started");
  
  // Display what's advertised
  LOGI("");
  LOGI("Advertised Services:");
  LOGI("  - Beacon Service UUID (with Service Data) [ALWAYS]");
  if (currentSensorMask & 0x01) LOGI("  - HR Service (0x180D)");
  if (currentSensorMask & 0x02) LOGI("  - CSC Service (0x1816)");
  if (currentSensorMask & 0x04) LOGI("  - PWR Service (0x1818)");
  if (currentSensorMask & 0x08) LOGI("  - ZWIFT_CTRL Service (0xFC82)");
  LOGI("");
  
  delay(3000);
  
  // Simulate sensor configuration change: Add CSC sensor
  LOGI("=== Simulating Sensor Configuration Change ===");
  LOGI("Adding CSC sensor...");
  currentSensorMask = 0x03;  // HR + CSC (bits 0 and 1)
  
  // Use UpdateAdvertisingIfChanged - it will detect the change and rebuild
  if (homewind::UpdateAdvertisingIfChanged(adv, advSnapshot, currentSensorMask, 
                                           deviceState, FW_MAJOR, FW_MINOR, false)) {
    LOGI("✓ Advertising updated (sensor mask changed: 0x01 -> 0x03)");
  } else {
    LOGI("✗ No change detected (unexpected)");
  }
  
  LOGI("");
  LOGI("Advertised Services:");
  LOGI("  - Beacon Service UUID (with Service Data) [ALWAYS]");
  if (currentSensorMask & 0x01) LOGI("  - HR Service (0x180D)");
  if (currentSensorMask & 0x02) LOGI("  - CSC Service (0x1816)");
  if (currentSensorMask & 0x04) LOGI("  - PWR Service (0x1818)");
  if (currentSensorMask & 0x08) LOGI("  - ZWIFT_CTRL Service (0xFC82)");
  LOGI("");
  
  delay(3000);
  
  // Simulate adding ZWIFT_CTRL service
  LOGI("=== Adding ZWIFT_CTRL Service ===");
  LOGI("Enabling ZWIFT_CTRL service...");
  currentSensorMask = 0x0B;  // HR + CSC + ZWIFT_CTRL (bits 0, 1, 3)
  
  if (homewind::UpdateAdvertisingIfChanged(adv, advSnapshot, currentSensorMask,
                                           deviceState, FW_MAJOR, FW_MINOR, false)) {
    LOGI("✓ Advertising updated (ZWIFT_CTRL service added)");
  } else {
    LOGI("✗ No change detected (unexpected)");
  }
  
  LOGI("");
  LOGI("Advertised Services:");
  LOGI("  - Beacon Service UUID (with Service Data) [ALWAYS]");
  if (currentSensorMask & 0x01) LOGI("  - HR Service (0x180D)");
  if (currentSensorMask & 0x02) LOGI("  - CSC Service (0x1816)");
  if (currentSensorMask & 0x04) LOGI("  - PWR Service (0x1818)");
  if (currentSensorMask & 0x08) LOGI("  - ZWIFT_CTRL Service (0xFC82)");
  LOGI("");
  
  delay(3000);
  
  // Simulate firmware update
  LOGI("=== Simulating Firmware Update ===");
  LOGI("Firmware version: 1.0 -> 1.1...");
  
  // In real usage, you would get firmware version from build system
  uint8_t newFwMinor = 1;
  
  if (homewind::UpdateAdvertisingIfChanged(adv, advSnapshot, currentSensorMask,
                                           deviceState, FW_MAJOR, newFwMinor, false)) {
    LOGI("✓ Advertising updated (firmware version changed)");
  } else {
    LOGI("✗ No change detected (unexpected)");
  }
  LOGI("");
  
  delay(2000);
  
  // Demonstrate guard snapshot: try to update with same values (should skip)
  LOGI("=== Testing Guard Snapshot ===");
  LOGI("Attempting update with identical values (should skip rebuild)...");
  if (homewind::UpdateAdvertisingIfChanged(adv, advSnapshot, currentSensorMask,
                                           deviceState, FW_MAJOR, newFwMinor, false)) {
    LOGI("✗ Advertising was updated (unexpected - values are identical)");
  } else {
    LOGI("✓ No change detected - advertising NOT rebuilt (correct behavior)");
  }
  LOGI("");
  
  // Display current beacon service data
  LOGI("=== Current Beacon Service Data ===");
  uint8_t beaconData[8];
  homewind::BuildBeaconServiceData(beaconData, currentSensorMask, deviceState,
                                   FW_MAJOR, newFwMinor, homewind::GetInstanceId());
  
  char hexStr[32];
  for (int i = 0; i < 8; i++) {
    snprintf(hexStr + (i * 3), 4, "%02x ", beaconData[i]);
  }
  LOGI_F("Payload: %s", hexStr);
  LOGI("Format:");
  LOGI("  [0] = 0x01 (protocol version)");
  LOGI("  [1] = 0x01 (device type: SensorHub)");
  LOGI_F("  [2] = 0x%02X (sensor mask: HR=%s, CSC=%s, PWR=%s, ZWIFT_CTRL=%s)",
         currentSensorMask,
         (currentSensorMask & 0x01) ? "yes" : "no",
         (currentSensorMask & 0x02) ? "yes" : "no",
         (currentSensorMask & 0x04) ? "yes" : "no",
         (currentSensorMask & 0x08) ? "yes" : "no");
  LOGI_F("  [3] = 0x%02X (device state)", deviceState);
  LOGI_F("  [4] = 0x%02X (firmware major)", FW_MAJOR);
  LOGI_F("  [5] = 0x%02X (firmware minor)", newFwMinor);
  LOGI_F("  [6] = 0x%02X (instance ID LSB)", beaconData[6]);
  LOGI_F("  [7] = 0x%02X (instance ID MSB)", beaconData[7]);
  LOGI("");
  
  // Set central device callbacks (if using BluetoothBikeSensorServer)
  BikeSensorServerSetClientConn(onCentralConnected);
  BikeSensorServerSetClientDisc(onCentralDisconnected);
  
  LOGI("=== Example Setup Complete ===");
  LOGI("");
  LOGI("Check your BLE scanner app to verify:");
  LOGI_F("  - Device name: %s", homewind::GetBleDeviceNameLower().c_str());
  LOGI("  - Beacon Service UUID with Service Data (8 bytes)");
  LOGI("  - Advertised services based on sensor mask");
  LOGI("");
  LOGI("The device will continue advertising in the loop().");
  LOGI("Use the guard snapshot to update advertising only when needed.");
  LOGI("");
}

void loop() {
  // In a real application, you would:
  // 1. Call BluetoothBikeSensorServerLoop() if using sensor connections
  // 2. Periodically check for sensor configuration changes
  // 3. Update advertising when sensor mask or firmware changes
  
  static uint32_t lastStatusPrint = 0;
  if (millis() - lastStatusPrint >= 10000) {  // Every 10 seconds
    lastStatusPrint = millis();
    
    // Check advertising status
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    bool isAdvertising = adv && adv->isAdvertising();
    
    LOGI("=== Status ===");
    if (isAdvertising) {
      LOGI("✓ BLE advertising: ACTIVE");
    } else {
      LOGW("✗ BLE advertising: INACTIVE");
      // Attempt to restart if stopped
      if (adv) {
        adv->start(0);
        delay(100);
      }
    }
    
    LOGI_F("Device: %s", homewind::GetBleDeviceNameLower().c_str());
    LOGI_F("Sensor Mask: 0x%02X", currentSensorMask);
    LOGI_F("Firmware: %u.%u", FW_MAJOR, advSnapshot.fwMinor);
    LOGI("");
  }
  
  delay(100);
}
