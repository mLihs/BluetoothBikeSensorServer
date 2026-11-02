#pragma once

#include <cstdint>

#include "BBSBlePort.h"
#include "BBSConfig.h"
#include "BBSTypes.h"

// Raw-GAP scan owner (§5.3): NAME_SCAN windows (NAME mode) and SLEEP windows
// (MAC mode); DISCOVERY_PENDING/DISCOVERY (step 6) are not built yet.
// update() task only.

namespace BikeSensor {
namespace internal {

enum class ScanOwner : uint8_t { OFF, NAME_SCAN, SLEEP_WINDOW };

class ScanController {
 public:
  explicit ScanController(Mode mode) : mode_(mode) {}

  ScanOwner owner() const { return owner_; }
  uint16_t epoch() const { return epoch_; }

  // A SCAN_MATCH/SCAN_END belongs to the running window. After a stop the
  // owner is OFF, so late events of that epoch are stale.
  bool isCurrent(uint16_t gen) const { return owner_ != ScanOwner::OFF && gen == epoch_; }

  void onScanEnd(uint16_t gen) {
    if (isCurrent(gen)) owner_ = ScanOwner::OFF;
  }

  // HOST_RESET, resync and safety exits.
  void forceOff() { owner_ = ScanOwner::OFF; }

  // Drop a remembered sleep-window remainder once a connect succeeds. Otherwise
  // a tiny leftover (e.g. 200 ms) survives a long SUBSCRIBED session and becomes
  // the first rescan after a later disconnect + failed direct attempt.
  void clearRemainder() { remainderMs_ = 0; }

  // needScan: NAME mode → a slot is IDLE; MAC mode → a slot is WAIT_SCAN.
  // slotBusy: a slot is FOUND, STOPPING_FOR_CONNECT, CONNECTING, CANCELLING or
  // DISCOVERING.
  void advance(BlePort& port, uint32_t now, bool needScan, bool slotBusy, Stats& stats) {
    if (owner_ != ScanOwner::OFF) {
      if (!needScan) {
        port.discCancel();
        owner_ = ScanOwner::OFF;
      } else if (now - windowStartMs_ >= windowMs_ + BBS_SCAN_END_GRACE_MS &&
                 !port.discActive()) {
        owner_ = ScanOwner::OFF;
      }
      return;
    }
    if (!needScan || slotBusy || now - holdFromMs_ < holdMs_ || port.connActive()) return;
    if (!port.hsSynced()) {
      hold(now, BBS_SCAN_START_BACKOFF_MS);
      return;
    }
    const bool name = mode_ == Mode::NAME;
    const uint32_t duration = name                ? BBS_NAME_SCAN_WINDOW_MS
                              : remainderMs_ != 0 ? remainderMs_
                                                  : BBS_SLEEP_SCAN_DURATION_MS;
    epoch_ = static_cast<uint16_t>(epoch_ + 1);
    if (epoch_ == 0) epoch_ = 1;
    if (port.discStart(epoch_, duration) != 0) {
      stats.scanStartFails++;
      hold(now, BBS_SCAN_START_BACKOFF_MS);
      return;
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
  void hold(uint32_t now, uint32_t ms) {
    holdFromMs_ = now;
    holdMs_ = ms;
  }

  const Mode mode_;
  ScanOwner owner_ = ScanOwner::OFF;
  uint16_t epoch_ = 0;                  // never 0 once a window started
  uint32_t holdFromMs_ = 0;             // next start not before holdFromMs_ + holdMs_
  uint32_t holdMs_ = 0;
  uint32_t windowStartMs_ = 0;
  uint32_t windowMs_ = 0;
  uint32_t remainderMs_ = 0;
};

}  // namespace internal
}  // namespace BikeSensor
