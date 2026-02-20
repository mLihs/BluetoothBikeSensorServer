#pragma once
/**
 * @file BLETelemetry.h
 * @brief Telemetry system for measuring performance metrics
 * 
 * Phase 0: Baseline & Telemetrie
 * This module provides non-invasive performance monitoring for:
 * - Loop execution times (min/avg/max)
 * - Heap memory usage and fragmentation
 * - BLE operations (reconnects, scans, advertising refreshes)
 * 
 * IMPORTANT: Telemetry is DISABLED by default.
 * 
 * To enable telemetry, use ONE of these methods:
 * 
 * 1. Runtime (recommended for testing):
 *    BLETelemetry::setEnabled(true);  // Call after setup
 * 
 * 2. In your sketch setup():
 *    void setup() {
 *      BLETelemetry::setEnabled(true);  // Enable before Begin
 *      BluetoothBikeSensorServerBegin();
 *    }
 * 
 * Usage:
 *   // Telemetry is automatically integrated in BluetoothBikeSensorServerLoop()
 *   // Just enable it and metrics will be collected and printed every 30 seconds
 *   
 *   // Manual control:
 *   BLETelemetry::printMetrics();   // Print full report
 *   BLETelemetry::printSummary();   // Print one-line summary
 *   BLETelemetry::reset();          // Reset all metrics
 */

#include <Arduino.h>
#include <cstdint>

#include "config/BBSConfig.h"

#ifndef BLE_TELEMETRY_PRINT_INTERVAL_MS
  #define BLE_TELEMETRY_PRINT_INTERVAL_MS 120000
#endif

/**
 * @brief Loop timing metrics
 */
struct LoopMetrics {
  uint32_t maxLoopTimeMs = 0;        ///< Maximum loop time in ms
  uint32_t minLoopTimeMs = UINT32_MAX; ///< Minimum loop time in ms
  uint64_t totalLoopTimeMs = 0;      ///< Sum of all loop times (for avg calculation)
  uint32_t loopCount = 0;            ///< Number of loop iterations
  uint32_t lastLoopTimeMs = 0;       ///< Most recent loop time
  uint32_t over10msCount = 0;        ///< Count of loops > 10ms
  uint32_t over50msCount = 0;        ///< Count of loops > 50ms
  uint32_t over100msCount = 0;       ///< Count of loops > 100ms
  
  float getAvgLoopTimeMs() const {
    return loopCount > 0 ? (float)totalLoopTimeMs / loopCount : 0;
  }
  
  void reset() {
    maxLoopTimeMs = 0;
    minLoopTimeMs = UINT32_MAX;
    totalLoopTimeMs = 0;
    loopCount = 0;
    lastLoopTimeMs = 0;
    over10msCount = 0;
    over50msCount = 0;
    over100msCount = 0;
  }
};

/**
 * @brief Heap memory metrics (enhanced with fragmentation tracking)
 */
struct HeapMetrics {
  // Legacy metrics (ESP.getFreeHeap / ESP.getMaxAllocHeap)
  size_t currentFreeHeap = 0;        ///< Current free heap bytes
  size_t currentMaxAllocHeap = 0;    ///< Current largest allocatable block
  size_t minFreeHeap = SIZE_MAX;     ///< Minimum free heap observed
  size_t minMaxAllocHeap = SIZE_MAX; ///< Minimum max alloc heap observed
  size_t initialFreeHeap = 0;        ///< Free heap at start
  size_t initialMaxAllocHeap = 0;    ///< Max alloc heap at start
  uint32_t measurementCount = 0;     ///< Number of measurements
  
  // Enhanced metrics (heap_caps API for fragmentation tracking)
  size_t currentFree8bit = 0;        ///< heap_caps free (MALLOC_CAP_8BIT)
  size_t currentLargestBlock8bit = 0; ///< heap_caps largest block (8BIT)
  size_t minFree8bit = SIZE_MAX;     ///< Min free 8bit observed
  size_t minLargestBlock8bit = SIZE_MAX; ///< Min largest block 8bit observed
  
  size_t currentFreeInternal = 0;    ///< heap_caps free (MALLOC_CAP_INTERNAL)
  size_t currentLargestBlockInternal = 0; ///< heap_caps largest block (INTERNAL)
  size_t minFreeInternal = SIZE_MAX; ///< Min free internal observed
  size_t minLargestBlockInternal = SIZE_MAX; ///< Min largest block internal observed
  
  size_t initialFree8bit = 0;        ///< Initial free 8bit
  size_t initialLargestBlock8bit = 0; ///< Initial largest block 8bit
  size_t initialFreeInternal = 0;    ///< Initial free internal
  size_t initialLargestBlockInternal = 0; ///< Initial largest block internal
  
  int32_t getHeapDelta() const {
    return (int32_t)currentFreeHeap - (int32_t)initialFreeHeap;
  }
  
  int32_t getMaxAllocDelta() const {
    return (int32_t)currentMaxAllocHeap - (int32_t)initialMaxAllocHeap;
  }
  
  // Fragmentation calculations
  size_t getFragmentation8bit() const {
    return (currentFree8bit > currentLargestBlock8bit) 
      ? (currentFree8bit - currentLargestBlock8bit) : 0;
  }
  
  float getFragmentationPercent8bit() const {
    return (currentFree8bit > 0) 
      ? (float)getFragmentation8bit() * 100.0f / currentFree8bit : 0;
  }
  
  size_t getFragmentationInternal() const {
    return (currentFreeInternal > currentLargestBlockInternal)
      ? (currentFreeInternal - currentLargestBlockInternal) : 0;
  }
  
  float getFragmentationPercentInternal() const {
    return (currentFreeInternal > 0)
      ? (float)getFragmentationInternal() * 100.0f / currentFreeInternal : 0;
  }
  
  int32_t getLargestBlockDelta8bit() const {
    return (int32_t)currentLargestBlock8bit - (int32_t)initialLargestBlock8bit;
  }
  
  int32_t getFreeHeapDelta8bit() const {
    return (int32_t)currentFree8bit - (int32_t)initialFree8bit;
  }
  
  void reset() {
    currentFreeHeap = 0;
    currentMaxAllocHeap = 0;
    minFreeHeap = SIZE_MAX;
    minMaxAllocHeap = SIZE_MAX;
    initialFreeHeap = 0;
    initialMaxAllocHeap = 0;
    measurementCount = 0;
    
    // Reset enhanced metrics
    currentFree8bit = 0;
    currentLargestBlock8bit = 0;
    minFree8bit = SIZE_MAX;
    minLargestBlock8bit = SIZE_MAX;
    currentFreeInternal = 0;
    currentLargestBlockInternal = 0;
    minFreeInternal = SIZE_MAX;
    minLargestBlockInternal = SIZE_MAX;
    initialFree8bit = 0;
    initialLargestBlock8bit = 0;
    initialFreeInternal = 0;
    initialLargestBlockInternal = 0;
  }
};

/**
 * @brief BLE operation metrics
 */
struct BLEOperationMetrics {
  // Connection metrics
  uint32_t reconnectAttempts = 0;     ///< Total reconnection attempts
  uint32_t connectionSuccesses = 0;   ///< Successful connections
  uint32_t connectionFailures = 0;    ///< Failed connections
  uint32_t disconnections = 0;        ///< Total disconnections
  
  // Scan metrics
  uint32_t scanCycles = 0;            ///< Number of scan start events
  uint32_t scanStops = 0;             ///< Number of scan stop events
  uint32_t scanResults = 0;           ///< Number of onResult() callbacks
  bool scanDuplicateFilterEnabled = false; ///< Whether duplicate filter is enabled
  
  // Advertising metrics
  uint32_t advertisingRefreshes = 0;  ///< Advertising payload refreshes
  uint32_t advertisingStarts = 0;     ///< Advertising start events
  uint32_t advertisingStops = 0;      ///< Advertising stop events
  
  // Discovery metrics
  uint32_t discoveryStarts = 0;       ///< Discovery mode start events
  uint32_t discoveryCompletes = 0;    ///< Discovery mode completions
  uint32_t sensorsDiscovered = 0;     ///< Total sensors found in discoveries
  
  // Central device metrics
  uint32_t centralConnects = 0;       ///< Central device connections
  uint32_t centralDisconnects = 0;    ///< Central device disconnections
  
  float getConnectionSuccessRate() const {
    uint32_t total = connectionSuccesses + connectionFailures;
    return total > 0 ? (float)connectionSuccesses / total * 100.0f : 0;
  }
  
  void reset() {
    reconnectAttempts = 0;
    connectionSuccesses = 0;
    connectionFailures = 0;
    disconnections = 0;
    scanCycles = 0;
    scanStops = 0;
    scanResults = 0;
    scanDuplicateFilterEnabled = false;
    advertisingRefreshes = 0;
    advertisingStarts = 0;
    advertisingStops = 0;
    discoveryStarts = 0;
    discoveryCompletes = 0;
    sensorsDiscovered = 0;
    centralConnects = 0;
    centralDisconnects = 0;
  }
};

/**
 * @brief Health monitoring metrics (Phase 1)
 * 
 * Tracks heap health and recovery operations
 * Based on CLEANUP_STRATEGIES_ALWAYS_ON.md
 */
struct HealthMonitoringMetrics {
  // Current health state
  uint32_t currentFreeHeap = 0;        ///< Current free heap
  uint32_t currentMaxAlloc = 0;        ///< Current largest allocatable block
  uint32_t startMaxAlloc = 0;          ///< Baseline maxAlloc at startup
  float maxAllocPercent = 1.0f;        ///< Current maxAlloc as % of start (0.0-1.0)
  char healthStatus[16] = "UNKNOWN";   ///< Health status: "OK", "WARN", "CRITICAL"
  
  // Recovery statistics
  uint32_t softRecoveryCount = 0;      ///< Number of soft recoveries performed
  uint32_t mediumRecoveryCount = 0;    ///< Number of medium recoveries performed
  uint32_t hardCleanupCount = 0;       ///< Number of hard cleanups performed
  
  // Timestamps
  unsigned long lastHealthCheckTime = 0;     ///< Last health check timestamp
  unsigned long lastSoftRecoveryTime = 0;    ///< Last soft recovery timestamp
  unsigned long lastHardCleanupTime = 0;     ///< Last hard cleanup timestamp
  
  void reset() {
    currentFreeHeap = 0;
    currentMaxAlloc = 0;
    startMaxAlloc = 0;
    maxAllocPercent = 1.0f;
    strncpy(healthStatus, "UNKNOWN", sizeof(healthStatus));
    softRecoveryCount = 0;
    mediumRecoveryCount = 0;
    hardCleanupCount = 0;
    lastHealthCheckTime = 0;
    lastSoftRecoveryTime = 0;
    lastHardCleanupTime = 0;
  }
};

/**
 * @brief Telemetry class for performance monitoring
 * 
 * Singleton class that collects and reports performance metrics.
 * All methods are static for easy access.
 * 
 * Telemetry is disabled by default. Enable via setEnabled(true).
 * When disabled, all recording methods return immediately (minimal overhead).
 */
#if BBS_ENABLE_TELEMETRY
class BLETelemetry {
public:
  /**
   * @brief Initialize telemetry system
   * Called automatically by BluetoothBikeSensorServerBegin()
   */
  static void begin();
  
  // ===== Loop Timing =====
  
  /**
   * @brief Start loop time measurement
   * @return Start time in milliseconds (use as parameter for endLoopMeasurement)
   */
  static uint32_t startLoopMeasurement();
  
  /**
   * @brief End loop time measurement
   * @param startTime Value returned from startLoopMeasurement()
   */
  static void endLoopMeasurement(uint32_t startTime);
  
  // ===== Heap Monitoring =====
  
  /**
   * @brief Record current heap state
   * Call periodically (e.g., every 1-10 seconds)
   */
  static void recordHeap();
  
  // ===== BLE Operation Recording =====
  
  /** @brief Record a reconnection attempt */
  static void recordReconnectAttempt();
  
  /** @brief Record connection result */
  static void recordConnectionResult(bool success);
  
  /** @brief Record sensor disconnection */
  static void recordDisconnection();
  
  /** @brief Record scan start */
  static void recordScanStart();
  
  /** @brief Record scan stop */
  static void recordScanStop();

  /** @brief Record scan onResult() callback */
  static void recordScanResult();

  /** @brief Record scan duplicate-filter setting */
  static void setScanDuplicateFilter(bool enabled);
  
  /** @brief Record advertising refresh */
  static void recordAdvertisingRefresh();
  
  /** @brief Record advertising start */
  static void recordAdvertisingStart();
  
  /** @brief Record advertising stop */
  static void recordAdvertisingStop();
  
  /** @brief Record discovery start */
  static void recordDiscoveryStart();
  
  /** @brief Record discovery complete with sensor count */
  static void recordDiscoveryComplete(uint8_t sensorsFound);
  
  /** @brief Record central device connection */
  static void recordCentralConnect();
  
  /** @brief Record central device disconnection */
  static void recordCentralDisconnect();
  
  // ===== Health Monitoring (Phase 1) =====
  
  /** 
   * @brief Record health check result
   * @param freeHeap Current free heap
   * @param maxAlloc Current max allocatable block
   * @param status Health status string ("OK", "WARN", "CRITICAL")
   */
  static void recordHealthCheck(uint32_t freeHeap, uint32_t maxAlloc, const char* status);
  
  /**
   * @brief Record recovery event
   * @param level Recovery level: "SOFT", "MEDIUM", or "HARD"
   * @param success Whether recovery was successful
   */
  static void recordRecovery(const char* level, bool success);
  
  /**
   * @brief Print health monitoring metrics
   */
  static void printHealthMetrics();
  
  // ===== Output =====
  
  /**
   * @brief Print metrics if interval elapsed
   * @param intervalMs Print interval in milliseconds (default: 10s)
   */
  static void printIfDue(uint32_t intervalMs = 10000);
  
  /**
   * @brief Force print all metrics now
   */
  static void printMetrics();
  
  /**
   * @brief Print a single-line summary
   */
  static void printSummary();
  
  // ===== Getters =====
  
  static const LoopMetrics& getLoopMetrics() { return loopMetrics_; }
  static const HeapMetrics& getHeapMetrics() { return heapMetrics_; }
  static const BLEOperationMetrics& getBLEMetrics() { return bleMetrics_; }
  static const HealthMonitoringMetrics& getHealthMetrics() { return healthMetrics_; }
  
  // ===== Control =====
  
  /**
   * @brief Reset all metrics to initial state
   */
  static void reset();
  
  /**
   * @brief Enable or disable telemetry at runtime
   * @param enabled true to enable, false to disable
   * 
   * When disabled, all recording methods return immediately with minimal overhead.
   * Default: disabled
   */
  static void setEnabled(bool enabled);
  
  /**
   * @brief Check if telemetry is enabled
   * @return true if enabled
   */
  static bool isEnabled() { return enabled_; }

private:
  static LoopMetrics loopMetrics_;
  static HeapMetrics heapMetrics_;
  static BLEOperationMetrics bleMetrics_;
  static HealthMonitoringMetrics healthMetrics_;
  static uint32_t lastPrintTime_;
  static uint32_t startTimeMs_;
  static bool initialized_;
  static bool enabled_;
};
#else
class BLETelemetry {
public:
  static void begin() {}
  static uint32_t startLoopMeasurement() { return 0; }
  static void endLoopMeasurement(uint32_t) {}
  static void recordHeap() {}
  static void recordReconnectAttempt() {}
  static void recordConnectionResult(bool) {}
  static void recordDisconnection() {}
  static void recordScanStart() {}
  static void recordScanStop() {}
  static void recordScanResult() {}
  static void setScanDuplicateFilter(bool) {}
  static void recordAdvertisingRefresh() {}
  static void recordAdvertisingStart() {}
  static void recordAdvertisingStop() {}
  static void recordDiscoveryStart() {}
  static void recordDiscoveryComplete(uint8_t) {}
  static void recordCentralConnect() {}
  static void recordCentralDisconnect() {}
  static void recordHealthCheck(uint32_t, uint32_t, const char*) {}
  static void recordRecovery(const char*, bool) {}
  static void printHealthMetrics() {}
  static void printIfDue(uint32_t = 10000) {}
  static void printMetrics() {}
  static void printSummary() {}
  static const LoopMetrics& getLoopMetrics() { static LoopMetrics metrics; return metrics; }
  static const HeapMetrics& getHeapMetrics() { static HeapMetrics metrics; return metrics; }
  static const BLEOperationMetrics& getBLEMetrics() { static BLEOperationMetrics metrics; return metrics; }
  static const HealthMonitoringMetrics& getHealthMetrics() { static HealthMonitoringMetrics metrics; return metrics; }
  static void reset() {}
  static void setEnabled(bool) {}
  static bool isEnabled() { return false; }
};
#endif
