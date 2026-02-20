/**
 * @file BLETelemetry.cpp
 * @brief Implementation of telemetry system for performance monitoring
 * 
 * The telemetry system is always compiled, but controlled via the enabled_ flag.
 * This ensures the library works with Arduino's separate compilation model.
 * 
 * To enable telemetry:
 * - Call BLETelemetry::setEnabled(true) at runtime, OR
 * - Define BLE_LIB_TELEMETRY_AUTOSTART in a DebugConfig.h file
 */

#include "BLETelemetry.h"

#if BBS_ENABLE_TELEMETRY

#ifdef ESP32
  #include <esp_heap_caps.h>
#endif

// Static member initialization
LoopMetrics BLETelemetry::loopMetrics_;
HeapMetrics BLETelemetry::heapMetrics_;
BLEOperationMetrics BLETelemetry::bleMetrics_;
HealthMonitoringMetrics BLETelemetry::healthMetrics_;
uint32_t BLETelemetry::lastPrintTime_ = 0;
uint32_t BLETelemetry::startTimeMs_ = 0;
bool BLETelemetry::initialized_ = false;

// Default: disabled unless explicitly enabled
// Can be changed via setEnabled() or by defining BLE_LIB_TELEMETRY_AUTOSTART
#if defined(BLE_LIB_TELEMETRY) || defined(BLE_LIB_TELEMETRY_AUTOSTART)
bool BLETelemetry::enabled_ = true;
#else
bool BLETelemetry::enabled_ = false;
#endif

void BLETelemetry::begin() {
  if (initialized_) return;
  
  startTimeMs_ = millis();
  lastPrintTime_ = startTimeMs_;
  
  // Record initial heap state
  #ifdef ESP32
    // Legacy metrics
    heapMetrics_.initialFreeHeap = ESP.getFreeHeap();
    heapMetrics_.initialMaxAllocHeap = ESP.getMaxAllocHeap();
    heapMetrics_.currentFreeHeap = heapMetrics_.initialFreeHeap;
    heapMetrics_.currentMaxAllocHeap = heapMetrics_.initialMaxAllocHeap;
    heapMetrics_.minFreeHeap = heapMetrics_.initialFreeHeap;
    heapMetrics_.minMaxAllocHeap = heapMetrics_.initialMaxAllocHeap;
    
    // Enhanced metrics (heap_caps)
    heapMetrics_.initialFree8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    heapMetrics_.initialLargestBlock8bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    heapMetrics_.initialFreeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    heapMetrics_.initialLargestBlockInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    
    heapMetrics_.currentFree8bit = heapMetrics_.initialFree8bit;
    heapMetrics_.currentLargestBlock8bit = heapMetrics_.initialLargestBlock8bit;
    heapMetrics_.currentFreeInternal = heapMetrics_.initialFreeInternal;
    heapMetrics_.currentLargestBlockInternal = heapMetrics_.initialLargestBlockInternal;
    
    heapMetrics_.minFree8bit = heapMetrics_.initialFree8bit;
    heapMetrics_.minLargestBlock8bit = heapMetrics_.initialLargestBlock8bit;
    heapMetrics_.minFreeInternal = heapMetrics_.initialFreeInternal;
    heapMetrics_.minLargestBlockInternal = heapMetrics_.initialLargestBlockInternal;
  #endif
  
  initialized_ = true;
  
  if (enabled_) {
    Serial.println(F("[TELEMETRY] Initialized (enabled)"));
    Serial.printf("[TELEMETRY] Initial Heap: Free=%u, MaxAlloc=%u\n", 
                  heapMetrics_.initialFreeHeap, heapMetrics_.initialMaxAllocHeap);
    Serial.printf("[TELEMETRY] Initial 8BIT: Free=%u, Largest=%u\n",
                  heapMetrics_.initialFree8bit, heapMetrics_.initialLargestBlock8bit);
    Serial.printf("[TELEMETRY] Initial INTERNAL: Free=%u, Largest=%u\n",
                  heapMetrics_.initialFreeInternal, heapMetrics_.initialLargestBlockInternal);
  }
}

uint32_t BLETelemetry::startLoopMeasurement() {
  if (!enabled_) return 0;
  return millis();
}

void BLETelemetry::endLoopMeasurement(uint32_t startTime) {
  if (!enabled_ || startTime == 0) return;
  
  uint32_t duration = millis() - startTime;
  
  loopMetrics_.lastLoopTimeMs = duration;
  loopMetrics_.totalLoopTimeMs += duration;
  loopMetrics_.loopCount++;
  
  if (duration > loopMetrics_.maxLoopTimeMs) {
    loopMetrics_.maxLoopTimeMs = duration;
  }
  if (duration < loopMetrics_.minLoopTimeMs) {
    loopMetrics_.minLoopTimeMs = duration;
  }
  
  // Count slow loops
  if (duration > 100) {
    loopMetrics_.over100msCount++;
    loopMetrics_.over50msCount++;
    loopMetrics_.over10msCount++;
  } else if (duration > 50) {
    loopMetrics_.over50msCount++;
    loopMetrics_.over10msCount++;
  } else if (duration > 10) {
    loopMetrics_.over10msCount++;
  }
}

void BLETelemetry::recordHeap() {
  if (!enabled_) return;
  
  #ifdef ESP32
    // Legacy metrics (kept for compatibility)
    heapMetrics_.currentFreeHeap = ESP.getFreeHeap();
    heapMetrics_.currentMaxAllocHeap = ESP.getMaxAllocHeap();
    heapMetrics_.measurementCount++;
    
    // Enhanced metrics: heap_caps API for fragmentation tracking
    heapMetrics_.currentFree8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    heapMetrics_.currentLargestBlock8bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    heapMetrics_.currentFreeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    heapMetrics_.currentLargestBlockInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    
    // Track minimums (legacy)
    if (heapMetrics_.currentFreeHeap < heapMetrics_.minFreeHeap) {
      heapMetrics_.minFreeHeap = heapMetrics_.currentFreeHeap;
    }
    if (heapMetrics_.currentMaxAllocHeap < heapMetrics_.minMaxAllocHeap) {
      heapMetrics_.minMaxAllocHeap = heapMetrics_.currentMaxAllocHeap;
    }
    
    // Track minimums (enhanced 8BIT)
    if (heapMetrics_.currentFree8bit < heapMetrics_.minFree8bit) {
      heapMetrics_.minFree8bit = heapMetrics_.currentFree8bit;
    }
    if (heapMetrics_.currentLargestBlock8bit < heapMetrics_.minLargestBlock8bit) {
      heapMetrics_.minLargestBlock8bit = heapMetrics_.currentLargestBlock8bit;
    }
    
    // Track minimums (enhanced INTERNAL)
    if (heapMetrics_.currentFreeInternal < heapMetrics_.minFreeInternal) {
      heapMetrics_.minFreeInternal = heapMetrics_.currentFreeInternal;
    }
    if (heapMetrics_.currentLargestBlockInternal < heapMetrics_.minLargestBlockInternal) {
      heapMetrics_.minLargestBlockInternal = heapMetrics_.currentLargestBlockInternal;
    }
  #endif
}

void BLETelemetry::recordReconnectAttempt() {
  if (enabled_) bleMetrics_.reconnectAttempts++;
}

void BLETelemetry::recordConnectionResult(bool success) {
  if (!enabled_) return;
  if (success) {
    bleMetrics_.connectionSuccesses++;
  } else {
    bleMetrics_.connectionFailures++;
  }
}

void BLETelemetry::recordDisconnection() {
  if (enabled_) bleMetrics_.disconnections++;
}

void BLETelemetry::recordScanStart() {
  if (enabled_) bleMetrics_.scanCycles++;
}

void BLETelemetry::recordScanStop() {
  if (enabled_) bleMetrics_.scanStops++;
}

void BLETelemetry::recordScanResult() {
  if (enabled_) bleMetrics_.scanResults++;
}

void BLETelemetry::setScanDuplicateFilter(bool enabled) {
  if (enabled_) bleMetrics_.scanDuplicateFilterEnabled = enabled;
}

void BLETelemetry::recordAdvertisingRefresh() {
  if (enabled_) bleMetrics_.advertisingRefreshes++;
}

void BLETelemetry::recordAdvertisingStart() {
  if (enabled_) bleMetrics_.advertisingStarts++;
}

void BLETelemetry::recordAdvertisingStop() {
  if (enabled_) bleMetrics_.advertisingStops++;
}

void BLETelemetry::recordDiscoveryStart() {
  if (enabled_) bleMetrics_.discoveryStarts++;
}

void BLETelemetry::recordDiscoveryComplete(uint8_t sensorsFound) {
  if (!enabled_) return;
  bleMetrics_.discoveryCompletes++;
  bleMetrics_.sensorsDiscovered += sensorsFound;
}

void BLETelemetry::recordCentralConnect() {
  if (enabled_) bleMetrics_.centralConnects++;
}

void BLETelemetry::recordCentralDisconnect() {
  if (enabled_) bleMetrics_.centralDisconnects++;
}

void BLETelemetry::printIfDue(uint32_t intervalMs) {
  if (!enabled_) return;
  
  uint32_t now = millis();
  if (now - lastPrintTime_ >= intervalMs) {
    lastPrintTime_ = now;
    printMetrics();
  }
}

void BLETelemetry::printMetrics() {
  if (!enabled_) return;
  
  uint32_t uptime = (millis() - startTimeMs_) / 1000;
  
  Serial.println(F(""));
  Serial.println(F("╔════════════════════════════════════════════════════════════════╗"));
  Serial.println(F("║                    BLE TELEMETRY REPORT                        ║"));
  Serial.println(F("╠════════════════════════════════════════════════════════════════╣"));
  Serial.printf("║ Uptime: %lu seconds                                            \n", uptime);
  Serial.println(F("╠════════════════════════════════════════════════════════════════╣"));
  
  // Loop Metrics
  Serial.println(F("║ LOOP METRICS:                                                  ║"));
  Serial.printf("║   Iterations: %lu                                              \n", loopMetrics_.loopCount);
  Serial.printf("║   Time (min/avg/max): %lu / %.1f / %lu ms                      \n", 
                loopMetrics_.minLoopTimeMs == UINT32_MAX ? 0 : loopMetrics_.minLoopTimeMs,
                loopMetrics_.getAvgLoopTimeMs(),
                loopMetrics_.maxLoopTimeMs);
  Serial.printf("║   Last: %lu ms                                                 \n", loopMetrics_.lastLoopTimeMs);
  Serial.printf("║   Slow loops: >10ms=%lu, >50ms=%lu, >100ms=%lu                 \n",
                loopMetrics_.over10msCount, loopMetrics_.over50msCount, loopMetrics_.over100msCount);
  
  // Heap Metrics (Legacy)
  Serial.println(F("╠════════════════════════════════════════════════════════════════╣"));
  Serial.println(F("║ HEAP METRICS (Legacy):                                         ║"));
  Serial.printf("║   Current Free: %u bytes                                       \n", heapMetrics_.currentFreeHeap);
  Serial.printf("║   Current MaxAlloc: %u bytes                                   \n", heapMetrics_.currentMaxAllocHeap);
  Serial.printf("║   Min Free: %u bytes                                           \n", 
                heapMetrics_.minFreeHeap == SIZE_MAX ? 0 : heapMetrics_.minFreeHeap);
  Serial.printf("║   Min MaxAlloc: %u bytes                                       \n",
                heapMetrics_.minMaxAllocHeap == SIZE_MAX ? 0 : heapMetrics_.minMaxAllocHeap);
  Serial.printf("║   Delta from start: Free=%+d, MaxAlloc=%+d                     \n",
                heapMetrics_.getHeapDelta(), heapMetrics_.getMaxAllocDelta());
  
  // Enhanced Heap Metrics (heap_caps API - fragmentation tracking)
  Serial.println(F("╠════════════════════════════════════════════════════════════════╣"));
  Serial.println(F("║ HEAP FRAGMENTATION (8BIT):                                     ║"));
  Serial.printf("║   Total Free: %u bytes (start=%u, delta=%+d)                   \n",
                heapMetrics_.currentFree8bit,
                heapMetrics_.initialFree8bit,
                heapMetrics_.getFreeHeapDelta8bit());
  Serial.printf("║   Largest Block: %u bytes (start=%u, delta=%+d)                \n",
                heapMetrics_.currentLargestBlock8bit,
                heapMetrics_.initialLargestBlock8bit,
                heapMetrics_.getLargestBlockDelta8bit());
  Serial.printf("║   Min Largest Block: %u bytes                                  \n",
                heapMetrics_.minLargestBlock8bit == SIZE_MAX ? 0 : heapMetrics_.minLargestBlock8bit);
  Serial.printf("║   Fragmentation: %u bytes (%.1f%%)                              \n",
                heapMetrics_.getFragmentation8bit(),
                heapMetrics_.getFragmentationPercent8bit());
  
  Serial.println(F("╠════════════════════════════════════════════════════════════════╣"));
  Serial.println(F("║ HEAP FRAGMENTATION (INTERNAL):                                 ║"));
  Serial.printf("║   Total Free: %u bytes                                         \n",
                heapMetrics_.currentFreeInternal);
  Serial.printf("║   Largest Block: %u bytes                                      \n",
                heapMetrics_.currentLargestBlockInternal);
  Serial.printf("║   Min Largest Block: %u bytes                                  \n",
                heapMetrics_.minLargestBlockInternal == SIZE_MAX ? 0 : heapMetrics_.minLargestBlockInternal);
  Serial.printf("║   Fragmentation: %u bytes (%.1f%%)                              \n",
                heapMetrics_.getFragmentationInternal(),
                heapMetrics_.getFragmentationPercentInternal());
  
  // BLE Metrics
  Serial.println(F("╠════════════════════════════════════════════════════════════════╣"));
  Serial.println(F("║ BLE METRICS:                                                   ║"));
  Serial.printf("║   Connections: Success=%lu, Failed=%lu (%.1f%% success)        \n",
                bleMetrics_.connectionSuccesses, bleMetrics_.connectionFailures,
                bleMetrics_.getConnectionSuccessRate());
  Serial.printf("║   Reconnect attempts: %lu                                      \n", bleMetrics_.reconnectAttempts);
  Serial.printf("║   Disconnections: %lu                                          \n", bleMetrics_.disconnections);
  Serial.printf("║   Scans: Start=%lu, Stop=%lu                                   \n", 
                bleMetrics_.scanCycles, bleMetrics_.scanStops);
  float scanResultsPerHour = uptime > 0 ? (bleMetrics_.scanResults * 3600.0f / uptime) : 0.0f;
  Serial.printf("║   Scan results: %lu (dupFilter=%s, perHour=%.1f)               \n",
               bleMetrics_.scanResults,
               bleMetrics_.scanDuplicateFilterEnabled ? "on" : "off",
               scanResultsPerHour);
  Serial.printf("║   Advertising: Start=%lu, Stop=%lu, Refresh=%lu                \n",
                bleMetrics_.advertisingStarts, bleMetrics_.advertisingStops, bleMetrics_.advertisingRefreshes);
  Serial.printf("║   Discovery: %lu times, %lu sensors found                      \n",
                bleMetrics_.discoveryCompletes, bleMetrics_.sensorsDiscovered);
  Serial.printf("║   Centrals: Connect=%lu, Disconnect=%lu                        \n",
                bleMetrics_.centralConnects, bleMetrics_.centralDisconnects);
  
  // Health Monitoring (Phase 1)
  if (healthMetrics_.startMaxAlloc > 0) {
    printHealthMetrics();
  }
  
  Serial.println(F("╚════════════════════════════════════════════════════════════════╝"));
  Serial.println(F(""));
}

void BLETelemetry::printSummary() {
  if (!enabled_) return;
  
  // Record heap before printing
  recordHeap();
  
  Serial.printf("[TELEM] Loop: %lu iter, max=%lums | Heap: %u/%u | BLE: conn=%lu/%lu, reconn=%lu, scan=%lu\n",
                loopMetrics_.loopCount,
                loopMetrics_.maxLoopTimeMs,
                heapMetrics_.currentFreeHeap,
                heapMetrics_.currentMaxAllocHeap,
                bleMetrics_.connectionSuccesses,
                bleMetrics_.connectionSuccesses + bleMetrics_.connectionFailures,
               bleMetrics_.reconnectAttempts,
               bleMetrics_.scanResults);
  
  // Enhanced heap metrics (KEY for fragmentation detection!)
  Serial.printf("[TELEM] 8BIT: Free=%u, Largest=%u (delta=%+d), Frag=%u (%.1f%%) | MinLargest=%u\n",
                heapMetrics_.currentFree8bit,
                heapMetrics_.currentLargestBlock8bit,
                heapMetrics_.getLargestBlockDelta8bit(),
                heapMetrics_.getFragmentation8bit(),
                heapMetrics_.getFragmentationPercent8bit(),
                heapMetrics_.minLargestBlock8bit == SIZE_MAX ? 0 : heapMetrics_.minLargestBlock8bit);
}

void BLETelemetry::reset() {
  loopMetrics_.reset();
  heapMetrics_.reset();
  bleMetrics_.reset();
  
  #ifdef ESP32
    heapMetrics_.initialFreeHeap = ESP.getFreeHeap();
    heapMetrics_.initialMaxAllocHeap = ESP.getMaxAllocHeap();
    heapMetrics_.currentFreeHeap = heapMetrics_.initialFreeHeap;
    heapMetrics_.currentMaxAllocHeap = heapMetrics_.initialMaxAllocHeap;
    heapMetrics_.minFreeHeap = heapMetrics_.initialFreeHeap;
    heapMetrics_.minMaxAllocHeap = heapMetrics_.initialMaxAllocHeap;
  #endif
  
  startTimeMs_ = millis();
  lastPrintTime_ = startTimeMs_;
  
  if (enabled_) {
    Serial.println(F("[TELEMETRY] Reset"));
  }
}

void BLETelemetry::setEnabled(bool enabled) {
  bool wasEnabled = enabled_;
  enabled_ = enabled;
  
  // If enabling for the first time, initialize
  if (enabled && !wasEnabled && !initialized_) {
    begin();
  }
  
  Serial.printf("[TELEMETRY] %s\n", enabled ? "Enabled" : "Disabled");
}

// ===== Health Monitoring (Phase 1) =====

void BLETelemetry::recordHealthCheck(uint32_t freeHeap, uint32_t maxAlloc, const char* status) {
  if (!enabled_) return;
  
  healthMetrics_.currentFreeHeap = freeHeap;
  healthMetrics_.currentMaxAlloc = maxAlloc;
  healthMetrics_.lastHealthCheckTime = millis();
  
  // Update status
  strncpy(healthMetrics_.healthStatus, status, sizeof(healthMetrics_.healthStatus) - 1);
  healthMetrics_.healthStatus[sizeof(healthMetrics_.healthStatus) - 1] = '\0';
  
  // Calculate percentage if we have a baseline
  if (healthMetrics_.startMaxAlloc > 0) {
    healthMetrics_.maxAllocPercent = (float)maxAlloc / (float)healthMetrics_.startMaxAlloc;
  } else {
    // First time - set baseline
    healthMetrics_.startMaxAlloc = maxAlloc;
    healthMetrics_.maxAllocPercent = 1.0f;
  }
}

void BLETelemetry::recordRecovery(const char* level, bool success) {
  if (!enabled_) return;
  
  unsigned long now = millis();
  
  if (strcmp(level, "SOFT") == 0) {
    healthMetrics_.softRecoveryCount++;
    healthMetrics_.lastSoftRecoveryTime = now;
    Serial.printf("[TELEMETRY] Soft recovery %s\n", success ? "succeeded" : "failed");
  } else if (strcmp(level, "MEDIUM") == 0) {
    healthMetrics_.mediumRecoveryCount++;
    Serial.printf("[TELEMETRY] Medium recovery %s\n", success ? "succeeded" : "failed");
  } else if (strcmp(level, "HARD") == 0) {
    healthMetrics_.hardCleanupCount++;
    healthMetrics_.lastHardCleanupTime = now;
    
    // After hard cleanup, reset baseline
    #ifdef ESP32
      healthMetrics_.startMaxAlloc = ESP.getMaxAllocHeap();
      healthMetrics_.maxAllocPercent = 1.0f;
    #endif
    
    Serial.printf("[TELEMETRY] Hard cleanup %s - baseline reset\n", success ? "succeeded" : "failed");
  }
}

void BLETelemetry::printHealthMetrics() {
  if (!enabled_) return;
  
  Serial.println("╠════════════════════════════════════════════════════════════════╣");
  Serial.println("║ HEALTH MONITORING (Phase 1):                                  ║");
  Serial.printf("║   Status: %-52s║\n", healthMetrics_.healthStatus);
  Serial.printf("║   Free Heap: %u bytes %-37s║\n", healthMetrics_.currentFreeHeap, "");
  Serial.printf("║   Max Alloc: %u bytes (%.1f%% of start) %-19s║\n", 
                healthMetrics_.currentMaxAlloc, 
                healthMetrics_.maxAllocPercent * 100.0f, "");
  Serial.printf("║   Baseline Max Alloc: %u bytes %-30s║\n", 
                healthMetrics_.startMaxAlloc, "");
  
  Serial.println("╠════════════════════════════════════════════════════════════════╣");
  Serial.println("║ RECOVERY STATISTICS:                                           ║");
  Serial.printf("║   Soft Recoveries: %-44u║\n", healthMetrics_.softRecoveryCount);
  Serial.printf("║   Medium Recoveries: %-42u║\n", healthMetrics_.mediumRecoveryCount);
  Serial.printf("║   Hard Cleanups: %-46u║\n", healthMetrics_.hardCleanupCount);
}

#endif
