#pragma once

#include <atomic>
#include <cstdint>

// §6a store-status rules, Arduino-free. With BBS_SENSOR_SECURITY the port
// installs them as NimBLEDeviceCallbacks::onStoreStatus (NimBLE host task,
// host lock not held, ble_store.c:102) and maps the verdict onto NimBLE's
// return code. Unlike ble_store_util_status_rr it never disconnects a peer.

namespace BikeSensor {
namespace internal {

// ble_store.h values (BBSNimBLEPort.h static_asserts that they match).
constexpr int kStoreEventOverflow = 1;
constexpr int kStoreEventFull = 2;
constexpr int kStoreObjOurSec = 1;
constexpr int kStoreObjPeerSec = 2;
constexpr int kStoreObjCccd = 3;
constexpr int kStoreObjPeerAddr = 6;
constexpr int kStoreObjCsfc = 8;

// PROCEED → 0, REJECT → BLE_HS_ESTORE_CAP, DELEGATE → ble_store_util_status_rr().
enum class StoreVerdict : uint8_t { PROCEED, REJECT, DELEGATE };

struct BondCounters {
  std::atomic<uint32_t> evictions{0};
  std::atomic<uint32_t> rejects{0};
};

// bonds: count() bonded peers in store order (as ble_store_util_bonded_peers
// returns it, like NimBLE's ble_store_util_status_rr); live(i): peer i has a
// connection; owns(i, objType): peer i has a stored record of that type;
// erase(i): deletes all of peer i's records, true on success.
template <typename Bonds>
StoreVerdict storeStatus(int eventCode, int objType, Bonds& bonds, BondCounters& counters) {
  // FULL comes before every pairing once the store is at its maximum; a
  // non-zero answer would abort all new pairing (ble_sm.c:1080–1109).
  if (eventCode == kStoreEventFull) return StoreVerdict::PROCEED;
  if (eventCode != kStoreEventOverflow) return StoreVerdict::DELEGATE;
  bool needsRecord;
  switch (objType) {
    case kStoreObjOurSec:
    case kStoreObjPeerSec:
    case kStoreObjPeerAddr:
      needsRecord = false;
      break;
    // Only a peer that owns such a record frees one; otherwise
    // ble_store_write's retry loop would wipe unrelated bonds (ble_store.c:53–71).
    case kStoreObjCccd:
    case kStoreObjCsfc:
      needsRecord = true;
      break;
    default:
      return StoreVerdict::DELEGATE;
  }
  const int n = bonds.count();
  for (int i = 0; i < n; ++i) {
    if (bonds.live(i) || (needsRecord && !bonds.owns(i, objType))) continue;
    // After a failed delete, 0 could make ble_store_write retry forever.
    if (!bonds.erase(i)) break;
    counters.evictions.fetch_add(1, std::memory_order_relaxed);
    return StoreVerdict::PROCEED;
  }
  counters.rejects.fetch_add(1, std::memory_order_relaxed);
  return StoreVerdict::REJECT;
}

}  // namespace internal
}  // namespace BikeSensor
