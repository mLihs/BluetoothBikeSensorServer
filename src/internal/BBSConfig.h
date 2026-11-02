#pragma once

// BikeSensor 3.0 compile-time defaults (design §3). Every 3.0 header includes
// this one, so all TUs see one Event layout. The 2.x library keeps using
// src/config/BBSConfig.h until cutover.

#ifndef BBS_MAX_DISCOVERED
#define BBS_MAX_DISCOVERED 16
#endif
#ifndef BBS_CONNECT_TIMEOUT_MS
#define BBS_CONNECT_TIMEOUT_MS 5000
#endif
#ifndef BBS_GATT_BUDGET_MS
#define BBS_GATT_BUDGET_MS 8000
#endif
#ifndef BBS_SLEEP_SCAN_DURATION_MS
#define BBS_SLEEP_SCAN_DURATION_MS 5000
#endif
#ifndef BBS_SLEEP_SCAN_INTERVAL_MS
#define BBS_SLEEP_SCAN_INTERVAL_MS 30000
#endif
#ifndef BBS_NAME_SCAN_WINDOW_MS
#define BBS_NAME_SCAN_WINDOW_MS 10000
#endif
#ifndef BBS_EVENT_QUEUE_DEPTH
#define BBS_EVENT_QUEUE_DEPTH 24
#endif
#ifndef BBS_NOTIFY_MAX_LEN
#define BBS_NOTIFY_MAX_LEN 36               // full CPS measurement is 34 B (§6)
#endif
#ifndef BBS_SENSOR_SECURITY
#define BBS_SENSOR_SECURITY BBS_COMPILE_POWER
#endif
#ifndef BBS_POWER_STALE_MS
#define BBS_POWER_STALE_MS 3000             // cadence → 0 without a new crank event
#endif
#ifndef BBS_SCAN_END_GRACE_MS
#define BBS_SCAN_END_GRACE_MS 3000
#endif
#ifndef BBS_SCAN_START_BACKOFF_MS
#define BBS_SCAN_START_BACKOFF_MS 1000
#endif
#ifndef BBS_CANCEL_SAFETY_MS
#define BBS_CANCEL_SAFETY_MS 10000
#endif
#ifndef BBS_STOPPING_SAFETY_MS
#define BBS_STOPPING_SAFETY_MS BBS_SCAN_END_GRACE_MS   // escape if disc cancel fails (§5.2)
#endif
// Compile-time sensor selection (kept from 2.x; defaults match 2.2.21.0).
#ifndef BBS_COMPILE_HEART_RATE
#define BBS_COMPILE_HEART_RATE 1
#endif
#ifndef BBS_COMPILE_CSC
#define BBS_COMPILE_CSC 1
#endif
#ifndef BBS_COMPILE_POWER
#define BBS_COMPILE_POWER 0
#endif
#ifndef BBS_COMPILED_SENSOR_COUNT
#define BBS_COMPILED_SENSOR_COUNT (BBS_COMPILE_HEART_RATE + BBS_COMPILE_CSC + BBS_COMPILE_POWER)
#endif
static_assert(BBS_COMPILED_SENSOR_COUNT >= 1 && BBS_COMPILED_SENSOR_COUNT <= 3, "");
static_assert(BBS_MAX_DISCOVERED >= 1 && BBS_MAX_DISCOVERED <= 255, "discoveredCount() is uint8_t");
static_assert(BBS_NOTIFY_MAX_LEN >= 20 && BBS_NOTIFY_MAX_LEN <= 38, "Event must stay <= 48 B");
#if BBS_SENSOR_SECURITY && !BBS_COMPILE_POWER
#error "BBS_SENSOR_SECURITY is only used for Power sensors"
#endif
#if CONFIG_BT_NIMBLE_EXT_ADV
#error "BikeSensor 3.0 uses legacy ble_gap_disc events; disable CONFIG_BT_NIMBLE_EXT_ADV"
#endif
