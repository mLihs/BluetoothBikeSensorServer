#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include "BBSBlePort.h"
#include "BBSCodec.h"
#include "BBSConfig.h"
#include "BBSEventQueue.h"
#include "BBSGattWatchdog.h"
#include "BBSMux.h"
#include "BBSScanController.h"
#include "BBSTypes.h"

// One MAC-mode slot (§10 step 3): slot machine (§5.2), event handling (§5.1),
// resync (§3.2), HOST_RESET and the app lifecycle events (§4), driven through
// BlePort. Arduino-free; the caller passes millis() as `now`. Everything
// except the members marked "any task" runs on the update() task.

namespace BikeSensor {
namespace internal {

constexpr uint8_t kCompiledTypes =
    (BBS_COMPILE_HEART_RATE ? typeBit(SensorType::HEART_RATE) : 0) |
    (BBS_COMPILE_CSC ? typeBit(SensorType::SPEED_CADENCE) : 0) |
    (BBS_COMPILE_POWER ? typeBit(SensorType::POWER) : 0);

constexpr uint32_t kConnectWatchdogMs = BBS_CONNECT_TIMEOUT_MS + 2000;
constexpr uint32_t kTransientRetryMs = 500;
constexpr uint32_t kBackoffMinMs = 1000;
constexpr uint32_t kBackoffMaxMs = 30000;

struct SensorSlot {
  SensorType type;
  SlotState  state;
  char       name[32];
  uint8_t    mac[6];
  uint8_t    addrType;
  std::atomic<uint16_t> attemptGen{0};  // never reset; read by client callbacks
  uint8_t    battery;                   // 255 = unknown
  int        lastDisconnectReason;
  uint8_t    failCount;
  uint32_t   stateSinceMs;
  uint32_t   holdFromMs;                // FOUND connects not before holdFromMs + holdMs
  uint32_t   holdMs;
  bool       removing;                  // remove() in a link state: EMPTY on the outcome
  bool       gattDone;                  // DISCOVERING: pass failed, waiting for DISCONNECTED
  bool       directRetry;               // endAttempt → FOUND (one direct attempt), not WAIT_SCAN
  bool       failPending;               // watchdog cancel / failed GATT pass: outcome is a hard fail
};

class Core {
 public:
  static constexpr uint8_t kSlot = 0;

  explicit Core(BlePort& port) : port_(port) { clearSlot(); }

  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;

  // addrType: 0 public, 1 random, -1 auto-detect from the MAC (2.x rule:
  // top two bits 11 → random). The same type on a non-EMPTY slot replaces the
  // identity (§4); another type returns false.
  bool addByMac(SensorType type, const char* mac, int8_t addrType, const char* name,
                uint32_t now) {
    uint8_t bytes[6];
    if (!(typeBit(type) & kCompiledTypes)) return false;
    const bool replace = slot_.state != SlotState::EMPTY;
    if (replace && type != slot_.type) return false;
    if (!parseMac(mac, bytes) || addrType < -1 || addrType > 1) return false;
    {
      MuxGuard guard(tableMux_);
      slot_.type = type;
      memcpy(slot_.mac, bytes, sizeof(bytes));
      size_t len = name != nullptr ? strlen(name) : 0;
      if (len > sizeof(slot_.name) - 1) len = sizeof(slot_.name) - 1;
      if (len > 0) memcpy(slot_.name, name, len);
      slot_.name[len] = '\0';
      configured_ = true;
    }
    slot_.addrType = addrType >= 0 ? static_cast<uint8_t>(addrType)
                                   : ((bytes[0] & 0xC0) == 0xC0 ? 1 : 0);
    if (replace) {
      retarget(now);
    } else {
      enter(SlotState::WAIT_SCAN, now);
    }
    return true;
  }

  // §5.2 remove() table. Link states finish on their outcome event.
  void remove(uint32_t now) {
    switch (slot_.state) {
      case SlotState::EMPTY:
        return;
      case SlotState::CONNECTING:
        port_.cancelConnect(kSlot);
        slot_.removing = true;
        enter(SlotState::CANCELLING, now);
        return;
      case SlotState::CANCELLING:
        slot_.removing = true;
        return;
      case SlotState::DISCOVERING:
      case SlotState::SUBSCRIBED:
        slot_.removing = true;
        port_.disconnect(kSlot);
        return;
      default:  // WAIT_SCAN, FOUND, STOPPING_FOR_CONNECT (the scan is already OFF)
        clearSlot();
        return;
    }
  }

  void update(uint32_t now) {
    if (queue_.takeResync()) resync(now);
    Event ev;
    for (uint8_t i = 0; i < BBS_EVENT_QUEUE_DEPTH && queue_.pop(ev); ++i) handle(ev, now);
    gattStep(now);
    advanceSlot(now);
    orphanSweep();
    scan_.advance(port_, now, slot_.state == SlotState::WAIT_SCAN, slotBusy(), stats_);
  }

  void setEventCallback(EventCallback cb) { eventCb_ = cb; }

  // Any task (NimBLE host task in gapScanCb): is `mac` a configured sensor?
  bool scanFilterMatch(const uint8_t mac[6]) {
    MuxGuard guard(tableMux_);
    return configured_ && memcmp(mac, slot_.mac, sizeof(slot_.mac)) == 0;
  }

  // Any task (client callbacks): the attempt an event belongs to.
  uint16_t attemptGen(uint8_t slot) const {
    return slot == kSlot ? slot_.attemptGen.load(std::memory_order_acquire) : 0;
  }

  EventQueue& queue() { return queue_; }
  GattWatchdog& watchdog() { return watchdog_; }
  const SensorSlot& slot() const { return slot_; }
  const ScanController& scan() const { return scan_; }
  const Stats& stats() const { return stats_; }

 private:
  void enter(SlotState state, uint32_t now) {
    slot_.state = state;
    slot_.stateSinceMs = now;
  }

  void hold(uint32_t now, uint32_t ms) {
    slot_.holdFromMs = now;
    slot_.holdMs = ms;
  }

  // Clears identity and per-link fields; attemptGen is kept so a late event
  // of an old attempt stays stale.
  void clearSlot() {
    {
      MuxGuard guard(tableMux_);
      configured_ = false;
      memset(slot_.name, 0, sizeof(slot_.name));
      memset(slot_.mac, 0, sizeof(slot_.mac));
    }
    slot_.type = static_cast<SensorType>(0);
    slot_.state = SlotState::EMPTY;
    slot_.addrType = 0;
    slot_.battery = 255;
    slot_.lastDisconnectReason = 0;
    slot_.failCount = 0;
    slot_.stateSinceMs = 0;
    slot_.holdFromMs = 0;
    slot_.holdMs = 0;
    slot_.removing = false;
    slot_.gattDone = false;
    slot_.directRetry = false;
    slot_.failPending = false;
  }

  // Wait state after an attempt or link ends: MAC → WAIT_SCAN, or FOUND for a
  // direct attempt; EMPTY if removing.
  void endAttempt(uint32_t now) {
    if (slot_.removing) {
      clearSlot();
      return;
    }
    slot_.gattDone = false;
    if (slot_.directRetry) {
      slot_.directRetry = false;
      hold(now, 0);
      enter(SlotState::FOUND, now);
    } else {
      enter(SlotState::WAIT_SCAN, now);
    }
  }

  // Same-type addByMac on a non-EMPTY slot: the new identity is already set.
  // Ends the old attempt or link, then one direct attempt to the new MAC.
  void retarget(uint32_t now) {
    slot_.failCount = 0;
    slot_.removing = false;
    slot_.failPending = false;
    switch (slot_.state) {
      case SlotState::CONNECTING:
        port_.cancelConnect(kSlot);
        enter(SlotState::CANCELLING, now);
        break;
      case SlotState::CANCELLING:
        break;
      case SlotState::DISCOVERING:
        slot_.gattDone = true;          // no pass on the old link
        port_.disconnect(kSlot);
        break;
      case SlotState::SUBSCRIBED:
        port_.disconnect(kSlot);
        break;
      case SlotState::STOPPING_FOR_CONNECT:
        return;                         // its connect uses the new identity
      default:                          // WAIT_SCAN, FOUND
        hold(now, 0);
        enter(SlotState::FOUND, now);
        return;
    }
    slot_.directRetry = true;
  }

  // §5.2 hard fail: backoff → WAIT_SCAN, SENSOR_FAILED. connectFails only if
  // the link never came up.
  void hardFail(uint32_t now, bool connectFail) {
    if (slot_.failCount < 255) slot_.failCount++;
    if (connectFail) stats_.connectFails++;
    const uint8_t shift = slot_.failCount > 6 ? 5 : static_cast<uint8_t>(slot_.failCount - 1);
    const uint32_t backoff = kBackoffMinMs << shift;
    hold(now, backoff > kBackoffMaxMs ? kBackoffMaxMs : backoff);
    enter(SlotState::WAIT_SCAN, now);
    emit(EventType::SENSOR_FAILED, slot_.type);
  }

  // Outcome of a cancel: hard fail after the watchdog; remove()/replace stay neutral.
  void endCancel(uint32_t now) {
    if (slot_.failPending && !slot_.removing) {
      hardFail(now, true);
    } else {
      endAttempt(now);
    }
  }

  void emit(EventType type, SensorType sensorType, int reason = 0) {
    if (eventCb_ == nullptr) return;
    EventInfo info{};
    info.type = type;
    info.sensorType = sensorType;
    info.reason = reason;
    eventCb_(info);
  }

  bool slotBusy() const {
    switch (slot_.state) {
      case SlotState::FOUND:
      case SlotState::STOPPING_FOR_CONNECT:
      case SlotState::CONNECTING:
      case SlotState::CANCELLING:
      case SlotState::DISCOVERING:
        return true;
      default:
        return false;
    }
  }

  void handle(const Event& ev, uint32_t now) {
    switch (ev.kind) {
      case Ev::SCAN_MATCH:
        if (scan_.isCurrent(ev.gen) && slot_.state == SlotState::WAIT_SCAN &&
            memcmp(ev.match.addr, slot_.mac, sizeof(slot_.mac)) == 0) {
          slot_.addrType = ev.match.addrType;
          enter(SlotState::FOUND, now);
          emit(EventType::SENSOR_FOUND, slot_.type);
        }
        break;
      case Ev::SCAN_END:
        scan_.onScanEnd(ev.gen);
        break;
      case Ev::HOST_RESET:
        onHostReset(now);
        break;
      case Ev::CONNECTED:
      case Ev::CONNECT_FAIL:
      case Ev::DISCONNECTED:
        if (ev.slot == kSlot && ev.gen == slot_.attemptGen.load(std::memory_order_relaxed)) {
          onClientEvent(ev, now);
        }
        break;
      default:  // NOTIFY, CENTRAL_*, SUBSCRIBE: not handled before §10 step 5
        break;
    }
  }

  void onClientEvent(const Event& ev, uint32_t now) {
    const SlotState s = slot_.state;
    if (ev.kind == Ev::CONNECTED) {
      if (s == SlotState::CONNECTING) {
        stats_.connects++;
        slot_.gattDone = false;
        scan_.clearRemainder();
        enter(SlotState::DISCOVERING, now);
      } else if (s == SlotState::CANCELLING) {
        port_.disconnect(kSlot);
      }
      return;
    }
    if (ev.kind == Ev::DISCONNECTED) slot_.lastDisconnectReason = ev.reason;
    switch (s) {
      case SlotState::CONNECTING:     // fail, or link lost before onConnect
        hardFail(now, true);
        break;
      case SlotState::CANCELLING:
        endCancel(now);
        break;
      case SlotState::DISCOVERING:
      case SlotState::SUBSCRIBED: {   // CONNECT_FAIL here is a link loss (NimBLE reason 0x3E)
        slot_.lastDisconnectReason = ev.reason;
        stats_.disconnects++;
        if (slot_.failPending && !slot_.removing) {
          hardFail(now, false);       // failed/aborted GATT pass
          break;
        }
        if (s == SlotState::DISCOVERING) {   // no SENSOR_CONNECTED yet: neutral, no event
          endAttempt(now);
          break;
        }
        // MAC retry after a link: one direct attempt, then sleep scan
        const SensorType type = slot_.type;
        slot_.directRetry = true;
        endAttempt(now);
        emit(EventType::SENSOR_DISCONNECTED, type, ev.reason);
        break;
      }
      default:
        break;
    }
  }

  void onHostReset(uint32_t now) {
    scan_.forceOff();
    if (slot_.state == SlotState::CONNECTING || slot_.state == SlotState::CANCELLING ||
        slot_.state == SlotState::DISCOVERING) {
      endAttempt(now);
    }
    stats_.hostResets++;
    emit(EventType::HOST_RESET, SensorType{});
  }

  // §3.2 steps 1, 2 and 4 (central count and advertising come with step 5).
  void resync(uint32_t now) {
    if (!port_.discActive()) scan_.forceOff();   // first: the slot repair may emit()
    const bool connected = port_.isConnected(kSlot);
    const SlotState s = slot_.state;
    if (s == SlotState::DISCOVERING || s == SlotState::SUBSCRIBED) {
      if (!connected) {
        // Missed DISCONNECTED: repair state. A failed pass is a hard fail; only
        // SUBSCRIBED had SENSOR_CONNECTED, so only then SENSOR_DISCONNECTED (reason 0).
        const SensorType type = slot_.type;
        stats_.disconnects++;
        if (slot_.failPending && !slot_.removing) {
          hardFail(now, false);
          return;
        }
        endAttempt(now);
        if (s == SlotState::SUBSCRIBED) emit(EventType::SENSOR_DISCONNECTED, type, 0);
      }
    } else if (connected) {
      port_.disconnect(kSlot);
    } else if ((s == SlotState::CONNECTING || s == SlotState::CANCELLING) &&
               !port_.connActive()) {
      endAttempt(now);
    }
  }

  // §5.1 step 4: one blocking pass under the watchdog.
  void gattStep(uint32_t now) {
    if (slot_.state != SlotState::DISCOVERING || slot_.gattDone || slot_.removing) return;
    uint8_t battery = 255;
    const uint32_t gen = watchdog_.arm(port_.connHandle(kSlot));
    port_.gattTimerStart(BBS_GATT_BUDGET_MS);
    const bool ok = port_.gattPass(kSlot, slot_.type, battery);
    watchdog_.disarm();
    port_.gattTimerStop();
    const bool aborted = watchdog_.fired(gen);
    if (ok && !aborted) {
      slot_.battery = battery;
      slot_.failCount = 0;
      enter(SlotState::SUBSCRIBED, now);
      emit(EventType::SENSOR_CONNECTED, slot_.type);
      return;
    }
    if (aborted) stats_.gattBudgetAborts++;
    slot_.gattDone = true;
    slot_.failPending = true;
    port_.disconnect(kSlot);
  }

  void advanceSlot(uint32_t now) {
    switch (slot_.state) {
      case SlotState::FOUND: {
        if (now - slot_.holdFromMs < slot_.holdMs) return;
        const int rc = scan_.stopForConnect(port_, now);
        if (rc != 0 && rc != kHsEAlready) {
          stoppingSafetyExit(now);
          return;
        }
        enter(SlotState::STOPPING_FOR_CONNECT, now);
        tryConnect(now);
        return;
      }
      case SlotState::STOPPING_FOR_CONNECT:
        tryConnect(now);
        return;
      case SlotState::CONNECTING:
        if (now - slot_.stateSinceMs >= kConnectWatchdogMs) {
          port_.cancelConnect(kSlot);
          slot_.failPending = true;
          enter(SlotState::CANCELLING, now);
        }
        return;
      case SlotState::CANCELLING:
        if (now - slot_.stateSinceMs >= BBS_CANCEL_SAFETY_MS && !port_.connActive() &&
            !port_.isConnected(kSlot)) {
          queue_.requestResync();
          stats_.cancelSafetyExits++;
          endCancel(now);
        }
        return;
      default:
        return;
    }
  }

  void stoppingSafetyExit(uint32_t now) {
    queue_.requestResync();
    stats_.cancelSafetyExits++;
    scan_.forceOff();
    enter(SlotState::WAIT_SCAN, now);
  }

  // STOPPING_FOR_CONNECT: connect only under the §5.2 connect invariant.
  void tryConnect(uint32_t now) {
    if (port_.discActive()) {
      if (now - slot_.stateSinceMs >= BBS_STOPPING_SAFETY_MS) stoppingSafetyExit(now);
      return;
    }
    if (port_.connActive()) return;
    slot_.failPending = false;
    slot_.attemptGen.store(static_cast<uint16_t>(slot_.attemptGen.load() + 1),
                           std::memory_order_release);
    if (port_.connect(kSlot, slot_.mac, slot_.addrType)) {
      enter(SlotState::CONNECTING, now);
      return;
    }
    const int err = port_.lastError(kSlot);
    if (err == kHsEAlready || err == kHsEBusy) {
      hold(now, kTransientRetryMs);
      enter(SlotState::FOUND, now);
    } else if (err == kHsENotSynced) {
      hold(now, BBS_SCAN_START_BACKOFF_MS);
      enter(SlotState::FOUND, now);
    } else {
      hardFail(now, true);
    }
  }

  // §5.1 step 6. Never in CONNECTING (NimBLE reports connected before onConnect).
  void orphanSweep() {
    const SlotState s = slot_.state;
    if ((s == SlotState::EMPTY || s == SlotState::WAIT_SCAN || s == SlotState::FOUND) &&
        port_.isConnected(kSlot)) {
      port_.disconnect(kSlot);
    }
  }

  BlePort& port_;
  EventQueue queue_;
  GattWatchdog watchdog_;
  ScanController scan_;
  Mux tableMux_;                        // slot identity read by gapScanCb (§3.4)
  bool configured_ = false;             // under tableMux_
  SensorSlot slot_;
  Stats stats_{};
  EventCallback eventCb_ = nullptr;
};

}  // namespace internal
}  // namespace BikeSensor
