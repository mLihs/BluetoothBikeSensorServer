/**
 * BLEScanManager.cpp
 * 
 * Implementation of BLE scan management facade
 * Phase 1: Lightweight wrapper around existing scan logic
 * Phase 2: Full extraction with own callbacks and state
 */

#include "BLEScanManager.h"
#include "../BluetoothBikeSensorServer.h"
#include "../BLETelemetry.h"

// External references to existing implementation
// (defined in BluetoothBikeSensorServer.cpp)
extern NimBLEScan *pBLEScan;
extern bool isInitialised;
extern bool g_discoveryRunning;
extern void startScan();
extern void startDiscoveryScan(uint8_t sensorTypeMask);
extern void stopDiscoveryScan();

// Namespace implementation
namespace BLEScanManager {

void begin() {
  // Initialization is currently handled in BluetoothBikeSensorServerBegin()
  // This function is a placeholder for future initialization needs
  BLE_LIB_LOGI_F("[BLEScanManager] Initialized (wrapper mode)");
}

void startServerModeScan() {
  // Delegate to existing implementation
  // The existing startScan() already handles:
  // - Safety checks (isInitialised, g_discoveryRunning)
  // - Scan parameter configuration
  // - Callback restoration (critical fix for discovery race condition)
  // - Telemetry recording
  startScan();
}

void startDiscoveryScan(uint8_t sensorTypeMask, uint8_t durationSeconds) {
  // Delegate to existing implementation
  // Note: Current implementation uses hardcoded 5s duration
  // Future: Pass durationSeconds parameter through
  (void)durationSeconds; // Suppress unused warning
  startDiscoveryScan(sensorTypeMask);
}

void stopScan() {
  if (pBLEScan && pBLEScan->isScanning()) {
    pBLEScan->stop();
    BLE_LIB_LOGI_F("[BLEScanManager] Scan stopped");
    BLETelemetry::recordScanStop();
  }
}

void stopDiscoveryScan() {
  // Delegate to existing implementation
  ::stopDiscoveryScan();
}

bool isScanning() {
  return pBLEScan && pBLEScan->isScanning();
}

NimBLEScan* getScanInstance() {
  return pBLEScan;
}

void configureScanParameters(NimBLEScan* pScan, ScanMode mode) {
  if (!pScan) return;
  
  pScan->setActiveScan(true);   // Active scan for device names

  if (mode == SCAN_MODE_DISCOVERY) {
    // Discovery: faster interval/window (matching async example)
    pScan->setInterval(45);       // Scan interval (units of 0.625ms)
    pScan->setWindow(30);         // Scan window (units of 0.625ms)
  } else {
    // Server / Sleep-Scan: standard parameters
    pScan->setInterval(97);       // Scan interval (units of 0.625ms)
    pScan->setWindow(67);         // Scan window (units of 0.625ms)
  }

  // v2.2.4.9 OPT: Enable duplicate filter to reduce onResult() calls
  // This reduces heap allocations from frequent duplicate device reports
  pScan->setDuplicateFilter(true);
  
  BLE_LIB_LOGHOT_F("[BLEScanManager] Scan params: mode=%s, interval=%d, window=%d, dupFilter=true",
    mode == SCAN_MODE_DISCOVERY ? "DISCOVERY" : "SERVER",
    mode == SCAN_MODE_DISCOVERY ? 45 : 97,
    mode == SCAN_MODE_DISCOVERY ? 30 : 67);
}

} // namespace BLEScanManager
