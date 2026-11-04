#pragma once

// ============================================================================
// SENSOR TYPE FLAGS - Compile-Time Sensor Exclusion
// ============================================================================
// Set to 0 to completely exclude a sensor type from compilation.
// This saves Flash (~2-4KB per type) and RAM (~200 bytes per type).
// Default: all enabled (1). Override via build flags, e.g.:
//   -DBBS_COMPILE_HEART_RATE=0   → exclude HR code entirely
//   -DBBS_COMPILE_CSC=0          → exclude CSC code entirely
//   -DBBS_COMPILE_POWER=0        → exclude Power code entirely
// Note: The runtime enabledSensorTypes bitmask is still respected for
//       compiled-in types. These flags only control what is in the binary.

#ifndef BBS_COMPILE_HEART_RATE
  #define BBS_COMPILE_HEART_RATE 1
#endif

#ifndef BBS_COMPILE_CSC
  #define BBS_COMPILE_CSC 1
#endif

#ifndef BBS_COMPILE_POWER
  #define BBS_COMPILE_POWER 0
#endif

// Derived: number of compiled sensor types (for static array sizing)
#define BBS_COMPILED_SENSOR_COUNT \
  (BBS_COMPILE_HEART_RATE + BBS_COMPILE_CSC + BBS_COMPILE_POWER)

// ============================================================================
// FEATURE FLAGS - Compile-Time Configuration
// ============================================================================
// Set these flags to 0/1 to enable/disable features at compile time.
// Disabled features should be removed from binary (zero overhead).

#ifndef BBS_ENABLE_TELEMETRY
  #define BBS_ENABLE_TELEMETRY 1
#endif

#ifndef BBS_ENABLE_NAME_DISCOVERY
  #define BBS_ENABLE_NAME_DISCOVERY 1
#endif

#ifndef BBS_ENABLE_VERBOSE_LOG
  #define BBS_ENABLE_VERBOSE_LOG 0
#endif

#ifndef BBS_ENABLE_GETTERS
  #define BBS_ENABLE_GETTERS 1
#endif

#ifndef BBS_ENABLE_CENTRAL_CB
  #define BBS_ENABLE_CENTRAL_CB 1
#endif

// ============================================================================
// Diagnostics
// ============================================================================
// Enable very small heap logs during init phases (Serial).
// Useful to pinpoint which sub-step causes largest_free_block drops.
// 0 = off (default), 1 = on.
#ifndef BBS_ENABLE_INIT_HEAP_LOG
  #define BBS_ENABLE_INIT_HEAP_LOG 0
#endif

#ifndef BBS_CONNECT_TIMEOUT_MS
  #define BBS_CONNECT_TIMEOUT_MS 5000
#endif

#ifndef BBS_DISCOVERY_END_GRACE_MS
  #define BBS_DISCOVERY_END_GRACE_MS 3000
#endif

// ============================================================================
// SLEEP SCAN MODE CONFIGURATION (BBS_MODE_MAC)
// ============================================================================
// Sleep Scan Mode scans periodically instead of continuously.
// This saves power while still finding sensors by MAC address.

// How long each scan runs (in milliseconds)
// Default: 2000ms (2 seconds) - increase if sensors are not found
#ifndef BBS_SLEEP_SCAN_DURATION_MS
  #define BBS_SLEEP_SCAN_DURATION_MS 5000
#endif

// Interval between scans (in milliseconds)
// Default: 30000ms (30 seconds) - decrease for faster reconnection
#ifndef BBS_SLEEP_SCAN_INTERVAL_MS
  #define BBS_SLEEP_SCAN_INTERVAL_MS 30000
#endif
