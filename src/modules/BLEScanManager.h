/**
 * BLEScanManager.h
 * 
 * Facade/wrapper for BLE scanning operations
 * Provides clean API around existing scan logic without full extraction
 * 
 * Phase 1 - Task 2.1: Organize scan logic (lightweight approach)
 * Phase 2 will do full extraction with callbacks
 */

#ifndef BLE_SCAN_MANAGER_H
#define BLE_SCAN_MANAGER_H

#include <NimBLEDevice.h>

/**
 * BLEScanManager - Scan Management Facade
 * 
 * Organizes scan-related operations into a clean API.
 * Current implementation wraps existing functions.
 * Future: Full extraction of callbacks and state.
 */
namespace BLEScanManager {
  
  /**
   * Initialize scan manager (sets up scan instance and parameters)
   */
  void begin();
  
  /**
   * Start server mode scanning (continuous scan for sensor connection)
   * - Scans indefinitely (0 duration)
   * - Checks sensors by name and service UUID
   * - Sets doConnect flag when match found
   * - Restores server mode callbacks if needed
   */
  void startServerModeScan();
  
  /**
   * Start discovery mode scanning (user-initiated search)
   * @param sensorTypeMask Bitmask of sensor types to discover
   * @param durationSeconds Scan duration (default 5s)
   * 
   * - Pauses server mode
   * - Uses discovery callbacks
   * - Populates discovered sensors array
   * - Calls onScanEnd when complete
   */
  void startDiscoveryScan(uint8_t sensorTypeMask, uint8_t durationSeconds = 5);
  
  /**
   * Stop any active scan
   */
  void stopScan();
  
  /**
   * Stop discovery scan specifically
   * (calls internal stopDiscoveryScan())
   */
  void stopDiscoveryScan();
  
  /**
   * Check if scan is currently active
   */
  bool isScanning();
  
  /**
   * Get the NimBLEScan instance
   * @return Pointer to NimBLEScan (may be nullptr if not initialized)
   */
  NimBLEScan* getScanInstance();
  
  /**
   * Scan mode determines which parameter set is used.
   * SERVER  = normal + sleep-scan (interval=97, window=67)
   * DISCOVERY = discovery scan    (interval=45, window=30)
   */
  enum ScanMode { SCAN_MODE_SERVER, SCAN_MODE_DISCOVERY };

  /**
   * Configure scan parameters (interval, window, active scan, duplicate filter).
   * Called automatically by begin() and start functions.
   * @param pScan  NimBLEScan instance
   * @param mode   SCAN_MODE_SERVER (default) or SCAN_MODE_DISCOVERY
   */
  void configureScanParameters(NimBLEScan* pScan, ScanMode mode = SCAN_MODE_SERVER);
}

#endif // BLE_SCAN_MANAGER_H
