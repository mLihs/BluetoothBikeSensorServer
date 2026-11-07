#pragma once

#include <atomic>
#include <cstdint>

#include "BBSBlePort.h"
#include "BBSConfig.h"
#include "BBSTypes.h"

// Raw-GAP scan owner (§5.3): NAME_SCAN windows (NAME mode), SLEEP windows
// (MAC mode) and the discovery (DISCOVERY_PENDING → DISCOVERY). A discovery
// ends only through Core::tryFinish(); advance() reports why it must end.
// update() task only, except activeDiscEpoch() and owner().

namespace BikeSensor {
namespace internal {

enum class ScanOwner : uint8_t { OFF, NAME_SCAN, SLEEP_WINDOW, DISCOVERY_PENDING, DISCOVERY };

// §5.3: the longest a pending discovery waits for a connect and its GATT pass.
constexpr uint32_t kDiscoveryPendingMaxMs = BBS_CONNECT_TIMEOUT_MS + 2000 + BBS_GATT_BUDGET_MS;
// §5.3: discovery durations stay below this (INT32_MAX is BLE_HS_FOREVER).
constexpr uint32_t kDiscoveryDurationLimitMs = INT32_MAX - BBS_SCAN_END_GRACE_MS;

class ScanController {
 public:
  explicit ScanController(Mode mode) : mode_(mode) {}

  ScanOwner owner() const { return owner_.load(std::memory_order_relaxed); }
  uint16_t epoch() const { return epoch_; }
  bool discoveryOwner() const {
    return owner_ == ScanOwner::DISCOVERY_PENDING || owner_ == ScanOwner::DISCOVERY;
  }

  // Any task (gapScanCb): the epoch of the running discovery scan, 0 if none.
  uint16_t activeDiscEpoch() const { return activeDiscEpoch_.load(std::memory_order_acquire); }

  // A SCAN_MATCH belongs to the running NAME/SLEEP window. After a stop the
  // owner has changed, so late events of that epoch are stale.
  bool isCurrent(uint16_t gen) const { return windowOwner() && gen == epoch_; }

  // SCAN_END of the current epoch: a NAME/SLEEP window goes OFF. True if it
  // ends the running discovery (the Core finishes it).
  bool onScanEnd(uint16_t gen) {
    if (gen != epoch_) return false;
    if (windowOwner()) owner_ = ScanOwner::OFF;
    return owner_ == ScanOwner::DISCOVERY;
  }

  // HOST_RESET, resync and safety exits: ends a NAME/SLEEP window.
  void forceOff() {
    if (windowOwner()) owner_ = ScanOwner::OFF;
  }

  // quiesce() step 4: cancels a running NAME/SLEEP window.
  void stopWindow(BlePort& port) {
    if (!windowOwner()) return;
    port.discCancel();
    owner_ = ScanOwner::OFF;
  }

  // Core::begin(): no window, discovery, hold or remainder; the epoch keeps
  // counting.
  void reset(Mode mode) {
    mode_ = mode;
    owner_ = ScanOwner::OFF;
    activeDiscEpoch_.store(0, std::memory_order_release);
    hold(0, 0);
    remainderMs_ = 0;
  }

  // Drop a remembered sleep-window remainder once a connect succeeds. Otherwise
  // a tiny leftover (e.g. 200 ms) survives a long SUBSCRIBED session and becomes
  // the first rescan after a later disconnect + failed direct attempt.
  void clearRemainder() { remainderMs_ = 0; }

  // startDiscovery() steps 2 and 4: cancel a NAME/SLEEP window, then pending.
  void beginDiscovery(BlePort& port, uint32_t now, uint32_t durationMs) {
    if (windowOwner()) port.discCancel();
    owner_ = ScanOwner::DISCOVERY_PENDING;
    pendingSinceMs_ = now;
    discDurationMs_ = durationMs;
    discHoldMs_ = 0;
  }

  // stopDiscovery(): a running discovery scan is cancelled before tryFinish().
  void cancelDiscovery(BlePort& port) {
    if (owner_ == ScanOwner::DISCOVERY) port.discCancel();
  }

  // Core::tryFinish(): owner OFF; reports of the discovery epoch are stale.
  void finishDiscovery() {
    activeDiscEpoch_.store(0, std::memory_order_release);
    owner_ = ScanOwner::OFF;
  }

  // needScan: NAME mode → a slot is IDLE; MAC mode → a slot is WAIT_SCAN.
  // slotBusy: a slot is FOUND, STOPPING_FOR_CONNECT, CONNECTING, CANCELLING or
  // DISCOVERING. linkBusy: a slot is CONNECTING, CANCELLING or DISCOVERING.
  // sleepUnfiltered: a SLEEP window started now runs without the duplicate
  // filter (a name-fallback slot waits; its name may be in SCAN_RSP only).
  // Returns true if the discovery must finish with `finish`.
  bool advance(BlePort& port, uint32_t now, bool needScan, bool slotBusy, bool linkBusy,
               Stats& stats, FinishReason& finish, bool sleepUnfiltered = false) {
    switch (owner_) {
      case ScanOwner::DISCOVERY_PENDING:
        return advancePending(port, now, linkBusy, stats, finish);
      case ScanOwner::DISCOVERY: {
        // Overflow-safe for any duration: elapsed is compared in two steps.
        const uint32_t elapsed = now - windowStartMs_;
        if (elapsed < windowMs_ || elapsed - windowMs_ < BBS_SCAN_END_GRACE_MS) return false;
        port.discCancel();
        stats.discoveryTimeouts++;
        finish = FinishReason::TIMEOUT;
        return true;
      }
      case ScanOwner::NAME_SCAN:
      case ScanOwner::SLEEP_WINDOW:
        if (!needScan) {
          port.discCancel();
          owner_ = ScanOwner::OFF;
        } else if (now - windowStartMs_ >= windowMs_ + BBS_SCAN_END_GRACE_MS &&
                   !port.discActive()) {
          owner_ = ScanOwner::OFF;
        }
        return false;
      case ScanOwner::OFF:
        break;
    }
    if (!needScan || slotBusy || now - holdFromMs_ < holdMs_ || port.connActive()) return false;
    if (!port.hsSynced()) {
      hold(now, BBS_SCAN_START_BACKOFF_MS);
      return false;
    }
    const bool name = mode_ == Mode::NAME;
    const uint32_t duration = name                ? BBS_NAME_SCAN_WINDOW_MS
                              : remainderMs_ != 0 ? remainderMs_
                                                  : BBS_SLEEP_SCAN_DURATION_MS;
    nextEpoch();
    if (port.discStart(epoch_, duration, !name && !sleepUnfiltered) != 0) {
      stats.scanStartFails++;
      hold(now, BBS_SCAN_START_BACKOFF_MS);
      return false;
    }
    stats.scansStarted++;
    owner_ = name ? ScanOwner::NAME_SCAN : ScanOwner::SLEEP_WINDOW;
    windowStartMs_ = now;
    windowMs_ = duration;
    if (name) {
      hold(now, 0);                     // windows repeat back to back
    } else {
      remainderMs_ = 0;
      hold(now, duration + BBS_SLEEP_SCAN_INTERVAL_MS);
    }
    return false;
  }

  // FOUND → STOPPING_FOR_CONNECT. Cancels a running window. A SLEEP window
  // remembers its remainder, so a failed connect resumes scanning at once for
  // that rest. Returns the cancel rc (0 if nothing was running).
  int stopForConnect(BlePort& port, uint32_t now) {
    if (owner_ == ScanOwner::OFF) return 0;
    const int rc = port.discCancel();
    const bool sleep = owner_ == ScanOwner::SLEEP_WINDOW;
    owner_ = ScanOwner::OFF;
    const uint32_t elapsed = now - windowStartMs_;
    if (sleep && elapsed < windowMs_) {
      remainderMs_ = windowMs_ - elapsed;
      hold(now, 0);
    }
    return rc;
  }

 private:
  bool windowOwner() const {
    return owner_ == ScanOwner::NAME_SCAN || owner_ == ScanOwner::SLEEP_WINDOW;
  }

  void nextEpoch() {
    epoch_ = static_cast<uint16_t>(epoch_ + 1);
    if (epoch_ == 0) epoch_ = 1;
  }

  // DISCOVERY_PENDING: start once no link procedure is left and NimBLE neither
  // scans nor connects. The NAME/SLEEP hold does not apply; only a !synced
  // backoff does.
  bool advancePending(BlePort& port, uint32_t now, bool linkBusy, Stats& stats,
                      FinishReason& finish) {
    if (now - pendingSinceMs_ >= kDiscoveryPendingMaxMs) {
      finish = FinishReason::START_FAILED;
      return true;
    }
    if (linkBusy || port.discActive() || port.connActive() ||
        now - discHoldFromMs_ < discHoldMs_) {
      return false;
    }
    if (!port.hsSynced()) {
      discHoldFromMs_ = now;
      discHoldMs_ = BBS_SCAN_START_BACKOFF_MS;
      return false;
    }
    nextEpoch();
    // Published before the start: the first report can arrive before
    // discStart() returns here.
    activeDiscEpoch_.store(epoch_, std::memory_order_release);
    if (port.discStart(epoch_, discDurationMs_, false) != 0) {   // tryFinish() clears the epoch
      stats.scanStartFails++;
      hold(now, BBS_SCAN_START_BACKOFF_MS);
      finish = FinishReason::START_FAILED;
      return true;
    }
    stats.scansStarted++;
    owner_ = ScanOwner::DISCOVERY;
    windowStartMs_ = now;
    windowMs_ = discDurationMs_;
    return false;
  }

  void hold(uint32_t now, uint32_t ms) {
    holdFromMs_ = now;
    holdMs_ = ms;
  }

  Mode mode_;
  std::atomic<ScanOwner> owner_{ScanOwner::OFF};   // read by scanning() (§3.4 owner snapshot)
  uint16_t epoch_ = 0;                  // never 0 once a scan started
  std::atomic<uint16_t> activeDiscEpoch_{0};   // non-zero only for the discovery scan
  uint32_t holdFromMs_ = 0;             // next NAME/SLEEP start not before holdFromMs_ + holdMs_
  uint32_t holdMs_ = 0;
  uint32_t windowStartMs_ = 0;
  uint32_t windowMs_ = 0;
  uint32_t remainderMs_ = 0;
  uint32_t pendingSinceMs_ = 0;
  uint32_t discDurationMs_ = 0;
  uint32_t discHoldFromMs_ = 0;         // pending start not before discHoldFromMs_ + discHoldMs_
  uint32_t discHoldMs_ = 0;
};

}  // namespace internal
}  // namespace BikeSensor
