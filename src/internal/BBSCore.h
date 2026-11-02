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

// One slot per compiled sensor type, one mode for all slots (§10 steps 3–4):
// slot machine (§5.2), event handling (§5.1), resync (§3.2), HOST_RESET and
// the app lifecycle events (§4), driven through BlePort. Arduino-free; the
// caller passes millis() as `now`. Everything except the members marked
// "any task" runs on the update() task.

namespace BikeSensor {
namespace internal {

constexpr uint8_t kSlotCount = BBS_COMPILED_SENSOR_COUNT;

// Compiled types in the order HR, CSC, Power; -1 if not compiled.
constexpr int slotIndex(SensorType type) {
  return type == SensorType::HEART_RATE      ? (BBS_COMPILE_HEART_RATE ? 0 : -1)
         : type == SensorType::SPEED_CADENCE ? (BBS_COMPILE_CSC ? BBS_COMPILE_HEART_RATE : -1)
         : type == SensorType::POWER
             ? (BBS_COMPILE_POWER ? BBS_COMPILE_HEART_RATE + BBS_COMPILE_CSC : -1)
             : -1;
}

constexpr uint32_t kConnectWatchdogMs = BBS_CONNECT_TIMEOUT_MS + 2000;
constexpr uint32_t kTransientRetryMs = 500;
constexpr uint32_t kBackoffMinMs = 1000;
constexpr uint32_t kBackoffMaxMs = 30000;

struct SensorSlot {
  SensorType type;                      // fixed by the slot index
  SlotState  state;
  char       name[32];
  uint8_t    mac[6];
  uint8_t    addrType;
  bool       hasAddr;                   // under tableMux: mac is set (configured or resolved)
  std::atomic<uint16_t> attemptGen{0};  // never reset; read by client callbacks
  uint8_t    battery;                   // 255 = unknown
  int        lastDisconnectReason;
  uint8_t    failCount;
  uint32_t   stateSinceMs;
  uint32_t   holdFromMs;                // FOUND connects not before holdFromMs + holdMs
  uint32_t   holdMs;
  bool       removing;                  // remove() in a link state: EMPTY on the outcome
  // Per-attempt flags. Set only during an attempt or link; leaveAttempt()
  // resets all three when it ends. clearSlot() also clears them; retarget()
  // clears failPending.
  bool       gattDone;                  // DISCOVERING: no pass (failed, or skipped by a replace)
  bool       directRetry;               // leaveAttempt → FOUND (one direct attempt), not WAIT_SCAN
  bool       failPending;               // watchdog cancel / failed GATT pass: outcome is a hard fail
};

class Core {
 public:
  // mode: the begin() mode, fixed for the lifetime of the Core (§4 rev 25).
  Core(BlePort& port, Mode mode) : port_(port), mode_(mode), scan_(mode) {
    const SensorType types[] = {SensorType::HEART_RATE, SensorType::SPEED_CADENCE,
                                SensorType::POWER};
    for (SensorType type : types) {
      if (slotIndex(type) >= 0) slots_[slotIndex(type)].type = type;
    }
    for (SensorSlot& s : slots_) clearSlot(s);
  }

  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;

  // MAC mode only. addrType: 0 public, 1 random, -1 auto-detect from the MAC
  // (2.x rule: top two bits 11 → random). The same type on a non-EMPTY slot
  // replaces the identity (§4); a MAC held by another slot returns false.
  bool addByMac(SensorType type, const char* mac, int8_t addrType, const char* name,
                uint32_t now) {
    const int i = slotIndex(type);
    uint8_t bytes[6];
    if (i < 0 || mode_ != Mode::MAC) return false;
    if (!parseMac(mac, bytes) || addrType < -1 || addrType > 1) return false;
    if (heldByOtherSlot(bytes, static_cast<uint8_t>(i))) return false;
    SensorSlot& s = slots_[i];
    const bool replace = s.state != SlotState::EMPTY;
    {
      MuxGuard guard(tableMux_);
      memcpy(s.mac, bytes, sizeof(bytes));
      copyName(s.name, name);
      s.hasAddr = true;
    }
    s.addrType = addrType >= 0 ? static_cast<uint8_t>(addrType)
                               : ((bytes[0] & 0xC0) == 0xC0 ? 1 : 0);
    if (replace) {
      retarget(static_cast<uint8_t>(i), now);
    } else {
      enter(s, SlotState::WAIT_SCAN, now);
    }
    return true;
  }

  // NAME mode only. The name is matched exactly and case-sensitively, stored
  // truncated to 31 bytes; a name another slot holds returns false. The same
  // type on a non-EMPTY slot replaces the identity and goes to IDLE (§4).
  bool addByName(SensorType type, const char* name, uint32_t now) {
    const int i = slotIndex(type);
    if (i < 0 || mode_ != Mode::NAME || name == nullptr || name[0] == '\0') return false;
    char stored[sizeof(SensorSlot::name)];
    copyName(stored, name);
    if (nameHeldByOtherSlot(stored, static_cast<uint8_t>(i))) return false;
    SensorSlot& s = slots_[i];
    const bool replace = s.state != SlotState::EMPTY;
    {
      MuxGuard guard(tableMux_);
      memcpy(s.name, stored, sizeof(stored));
      memset(s.mac, 0, sizeof(s.mac));
      s.hasAddr = false;
    }
    s.addrType = 0;
    if (replace) {
      retarget(static_cast<uint8_t>(i), now);
    } else {
      enter(s, SlotState::IDLE, now);
    }
    return true;
  }

  // §5.2 remove() table. Link states finish on their outcome event.
  void remove(SensorType type, uint32_t now) {
    const int i = slotIndex(type);
    if (i < 0) return;
    SensorSlot& s = slots_[i];
    switch (s.state) {
      case SlotState::EMPTY:
        return;
      case SlotState::CONNECTING:
        port_.cancelConnect(static_cast<uint8_t>(i));
        s.removing = true;
        enter(s, SlotState::CANCELLING, now);
        return;
      case SlotState::CANCELLING:
        s.removing = true;
        return;
      case SlotState::DISCOVERING:
      case SlotState::SUBSCRIBED:
        s.removing = true;
        port_.disconnect(static_cast<uint8_t>(i));
        return;
      default:  // IDLE, WAIT_SCAN, FOUND, STOPPING_FOR_CONNECT (its stop already set the scan OFF)
        clearSlot(s);
        return;
    }
  }

  void update(uint32_t now) {
    if (queue_.takeResync()) resync(now);
    Event ev;
    for (uint8_t i = 0; i < BBS_EVENT_QUEUE_DEPTH && queue_.pop(ev); ++i) handle(ev, now);
    gattStep(now);
    for (uint8_t i = 0; i < kSlotCount; ++i) advanceSlot(i, now);
    orphanSweep();
    scan_.advance(port_, now, needScan(), scanBlocked(), stats_);
  }

  void setEventCallback(EventCallback cb) { eventCb_ = cb; }

  // Any task (NimBLE host task in gapScanCb), MAC mode: is `mac` held by a slot?
  bool scanFilterMatch(const uint8_t mac[6]) {
    MuxGuard guard(tableMux_);
    for (const SensorSlot& s : slots_) {
      if (s.hasAddr && memcmp(mac, s.mac, sizeof(s.mac)) == 0) return true;
    }
    return false;
  }

  // Any task (gapScanCb), NAME mode: the slot whose stored name equals the
  // report's name (exact, case-sensitive; not NUL-terminated), or -1.
  int scanNameMatch(const char* name, size_t len) {
    if (name == nullptr || len == 0) return -1;
    MuxGuard guard(tableMux_);
    for (uint8_t i = 0; i < kSlotCount; ++i) {
      if (strlen(slots_[i].name) == len && memcmp(slots_[i].name, name, len) == 0) return i;
    }
    return -1;
  }

  Mode mode() const { return mode_; }

  // Any task (client callbacks): the attempt an event belongs to.
  uint16_t attemptGen(uint8_t slot) const {
    return slot < kSlotCount ? slots_[slot].attemptGen.load(std::memory_order_acquire) : 0;
  }

  EventQueue& queue() { return queue_; }
  GattWatchdog& watchdog() { return watchdog_; }
  const SensorSlot& slot(uint8_t i) const { return slots_[i]; }
  const ScanController& scan() const { return scan_; }
  const Stats& stats() const { return stats_; }

 private:
  enum class Outcome : uint8_t { NEUTRAL, CONNECT_FAIL, LINK_FAIL };

  static void enter(SensorSlot& s, SlotState state, uint32_t now) {
    s.state = state;
    s.stateSinceMs = now;
  }

  static void hold(SensorSlot& s, uint32_t now, uint32_t ms) {
    s.holdFromMs = now;
    s.holdMs = ms;
  }

  // §5.2 connect rules: at most one slot in these states.
  static bool inFlight(SlotState state) {
    return state == SlotState::STOPPING_FOR_CONNECT || state == SlotState::CONNECTING ||
           state == SlotState::CANCELLING || state == SlotState::DISCOVERING;
  }

  bool anyInFlight() const {
    for (const SensorSlot& s : slots_) {
      if (inFlight(s.state)) return true;
    }
    return false;
  }

  // §5.3 OFF: no scan start while a slot is FOUND or in flight.
  bool scanBlocked() const {
    for (const SensorSlot& s : slots_) {
      if (s.state == SlotState::FOUND || inFlight(s.state)) return true;
    }
    return false;
  }

  // Where a slot waits for a scan: NAME → IDLE, MAC → WAIT_SCAN.
  SlotState waitState() const {
    return mode_ == Mode::NAME ? SlotState::IDLE : SlotState::WAIT_SCAN;
  }

  // §5.3 needScan().
  bool needScan() const {
    for (const SensorSlot& s : slots_) {
      if (s.state == waitState()) return true;
    }
    return false;
  }

  // Truncated to 31 bytes with a guaranteed NUL; nullptr → "".
  static void copyName(char (&out)[sizeof(SensorSlot::name)], const char* name) {
    size_t len = name != nullptr ? strlen(name) : 0;
    if (len > sizeof(out) - 1) len = sizeof(out) - 1;
    if (len > 0) memcpy(out, name, len);
    out[len] = '\0';
  }

  // A slot holds an address once configured (MAC) or resolved (NAME), until
  // remove() or a new identity.
  bool heldByOtherSlot(const uint8_t mac[6], uint8_t self) const {
    for (uint8_t i = 0; i < kSlotCount; ++i) {
      if (i != self && slots_[i].hasAddr && memcmp(mac, slots_[i].mac, 6) == 0) return true;
    }
    return false;
  }

  bool nameHeldByOtherSlot(const char* name, uint8_t self) const {
    for (uint8_t i = 0; i < kSlotCount; ++i) {
      if (i != self && slots_[i].state != SlotState::EMPTY && strcmp(name, slots_[i].name) == 0) {
        return true;
      }
    }
    return false;
  }

  // Clears identity and per-link fields; type and attemptGen are kept, so a
  // late event of an old attempt stays stale.
  void clearSlot(SensorSlot& s) {
    {
      MuxGuard guard(tableMux_);
      s.hasAddr = false;
      memset(s.name, 0, sizeof(s.name));
      memset(s.mac, 0, sizeof(s.mac));
    }
    s.state = SlotState::EMPTY;
    s.addrType = 0;
    s.battery = 255;
    s.lastDisconnectReason = 0;
    s.failCount = 0;
    s.stateSinceMs = 0;
    s.holdFromMs = 0;
    s.holdMs = 0;
    s.removing = false;
    s.gattDone = false;
    s.directRetry = false;
    s.failPending = false;
  }

  // The only place attemptGen changes (§3.1). The per-attempt flags are clear
  // here: every earlier attempt ended through leaveAttempt().
  static void startAttempt(SensorSlot& s) {
    s.attemptGen.store(static_cast<uint16_t>(s.attemptGen.load() + 1),
                       std::memory_order_release);
  }

  // The single exit of an attempt or link. Resets the per-attempt flags, then:
  // removing → EMPTY; hard fail → backoff + SENSOR_FAILED; otherwise the wait
  // state, or FOUND for a pending direct attempt (MAC only).
  void leaveAttempt(uint8_t i, uint32_t now, Outcome outcome) {
    SensorSlot& s = slots_[i];
    const bool direct = s.directRetry;
    s.gattDone = false;
    s.directRetry = false;
    s.failPending = false;
    if (s.removing) {
      clearSlot(s);
    } else if (outcome != Outcome::NEUTRAL) {
      hardFail(s, now, outcome == Outcome::CONNECT_FAIL);
    } else if (direct) {
      hold(s, now, 0);
      enter(s, SlotState::FOUND, now);
    } else {
      enter(s, waitState(), now);
    }
  }

  // Outcome of a cancel or of a link that ends before SENSOR_CONNECTED: hard
  // after a watchdog cancel or a failed pass; remove()/replace stay neutral.
  static Outcome pendingOutcome(const SensorSlot& s, Outcome hard) {
    return s.failPending ? hard : Outcome::NEUTRAL;
  }

  // Same-type addBy* on a non-EMPTY slot: the new identity is already set.
  // Ends the old attempt or link, then MAC: one direct attempt to the new MAC;
  // NAME: IDLE (the next window resolves the address).
  void retarget(uint8_t i, uint32_t now) {
    SensorSlot& s = slots_[i];
    s.failCount = 0;
    s.removing = false;
    s.failPending = false;              // a replace-driven end is neutral
    switch (s.state) {
      case SlotState::CONNECTING:
        port_.cancelConnect(i);
        enter(s, SlotState::CANCELLING, now);
        break;
      case SlotState::CANCELLING:
        break;
      case SlotState::DISCOVERING:
        s.gattDone = true;              // no pass on the old link
        port_.disconnect(i);
        break;
      case SlotState::SUBSCRIBED:
        port_.disconnect(i);
        break;
      case SlotState::STOPPING_FOR_CONNECT:
        if (mode_ == Mode::MAC) return; // its connect uses the new identity
        hold(s, now, 0);                // NAME: no address yet
        enter(s, SlotState::IDLE, now);
        return;
      default:                          // IDLE, WAIT_SCAN, FOUND
        hold(s, now, 0);
        enter(s, mode_ == Mode::MAC ? SlotState::FOUND : SlotState::IDLE, now);
        return;
    }
    if (mode_ == Mode::MAC) s.directRetry = true;
  }

  // §5.2 hard fail: backoff → wait state, SENSOR_FAILED. connectFails only if
  // the link never came up. Only leaveAttempt() calls this.
  void hardFail(SensorSlot& s, uint32_t now, bool connectFail) {
    if (s.failCount < 255) s.failCount++;
    if (connectFail) stats_.connectFails++;
    const uint8_t shift = s.failCount > 6 ? 5 : static_cast<uint8_t>(s.failCount - 1);
    const uint32_t backoff = kBackoffMinMs << shift;
    hold(s, now, backoff > kBackoffMaxMs ? kBackoffMaxMs : backoff);
    enter(s, waitState(), now);
    emit(EventType::SENSOR_FAILED, s.type);
  }

  void emit(EventType type, SensorType sensorType, int reason = 0) {
    if (eventCb_ == nullptr) return;
    EventInfo info{};
    info.type = type;
    info.sensorType = sensorType;
    info.reason = reason;
    eventCb_(info);
  }

  void handle(const Event& ev, uint32_t now) {
    switch (ev.kind) {
      case Ev::SCAN_MATCH:
        if (scan_.isCurrent(ev.gen)) onScanMatch(ev, now);
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
        if (ev.slot < kSlotCount &&
            ev.gen == slots_[ev.slot].attemptGen.load(std::memory_order_relaxed)) {
          onClientEvent(ev.slot, ev, now);
        }
        break;
      default:  // NOTIFY, CENTRAL_*, SUBSCRIBE: not handled before §10 step 5
        break;
    }
  }

  // §5.1 SCAN_MATCH (epoch already checked). The report's address and
  // addrType replace the slot's.
  void onScanMatch(const Event& ev, uint32_t now) {
    const int i = matchTarget(ev);
    if (i < 0) return;
    if (heldByOtherSlot(ev.match.addr, static_cast<uint8_t>(i))) return;   // §4 runtime guard
    SensorSlot& s = slots_[i];
    {
      MuxGuard guard(tableMux_);
      memcpy(s.mac, ev.match.addr, sizeof(s.mac));
      s.hasAddr = true;
    }
    s.addrType = ev.match.addrType;
    enter(s, SlotState::FOUND, now);
    emit(EventType::SENSOR_FOUND, s.type);
  }

  // NAME: the slot gapScanCb matched, if it is still IDLE under that name.
  // MAC: the WAIT_SCAN slot with the report's address.
  int matchTarget(const Event& ev) const {
    if (mode_ == Mode::NAME) {
      if (ev.slot >= kSlotCount) return -1;
      const SensorSlot& s = slots_[ev.slot];
      return s.state == SlotState::IDLE && strcmp(ev.match.name, s.name) == 0 ? ev.slot : -1;
    }
    for (uint8_t i = 0; i < kSlotCount; ++i) {
      const SensorSlot& s = slots_[i];
      if (s.state == SlotState::WAIT_SCAN && s.hasAddr &&
          memcmp(ev.match.addr, s.mac, sizeof(s.mac)) == 0) {
        return i;
      }
    }
    return -1;
  }

  void onClientEvent(uint8_t i, const Event& ev, uint32_t now) {
    SensorSlot& s = slots_[i];
    const SlotState state = s.state;
    if (ev.kind == Ev::CONNECTED) {
      if (state == SlotState::CONNECTING) {
        stats_.connects++;
        scan_.clearRemainder();
        enter(s, SlotState::DISCOVERING, now);
      } else if (state == SlotState::CANCELLING) {
        port_.disconnect(i);
      }
      return;
    }
    if (ev.kind == Ev::DISCONNECTED || state == SlotState::DISCOVERING ||
        state == SlotState::SUBSCRIBED) {
      s.lastDisconnectReason = ev.reason;
    }
    // CONNECT_FAIL in DISCOVERING/SUBSCRIBED is a link loss (NimBLE reason 0x3E).
    switch (state) {
      case SlotState::CONNECTING:       // fail, or link lost before onConnect
        leaveAttempt(i, now, Outcome::CONNECT_FAIL);
        break;
      case SlotState::CANCELLING:
        leaveAttempt(i, now, pendingOutcome(s, Outcome::CONNECT_FAIL));
        break;
      case SlotState::DISCOVERING:      // no SENSOR_CONNECTED yet: no SENSOR_DISCONNECTED
        stats_.disconnects++;
        leaveAttempt(i, now, pendingOutcome(s, Outcome::LINK_FAIL));
        break;
      case SlotState::SUBSCRIBED:       // MAC: one direct attempt, then sleep scan; NAME: IDLE
        stats_.disconnects++;
        s.directRetry = mode_ == Mode::MAC;
        leaveAttempt(i, now, Outcome::NEUTRAL);
        emit(EventType::SENSOR_DISCONNECTED, s.type, ev.reason);
        break;
      default:
        break;
    }
  }

  void onHostReset(uint32_t now) {
    scan_.forceOff();
    for (uint8_t i = 0; i < kSlotCount; ++i) {
      const SlotState state = slots_[i].state;
      if (state == SlotState::CONNECTING || state == SlotState::CANCELLING ||
          state == SlotState::DISCOVERING) {
        leaveAttempt(i, now, Outcome::NEUTRAL);
      }
    }
    stats_.hostResets++;
    emit(EventType::HOST_RESET, SensorType{});
  }

  // §3.2 steps 1, 2 and 4 (central count and advertising come with step 5).
  void resync(uint32_t now) {
    if (!port_.discActive()) scan_.forceOff();   // first: the slot repair may emit()
    for (uint8_t i = 0; i < kSlotCount; ++i) resyncSlot(i, now);
  }

  void resyncSlot(uint8_t i, uint32_t now) {
    SensorSlot& s = slots_[i];
    const bool connected = port_.isConnected(i);
    const SlotState state = s.state;
    if (state == SlotState::DISCOVERING || state == SlotState::SUBSCRIBED) {
      if (connected) return;
      // Missed DISCONNECTED: repair state. A failed pass is a hard fail; only
      // SUBSCRIBED had SENSOR_CONNECTED, so only then SENSOR_DISCONNECTED (reason 0).
      stats_.disconnects++;
      leaveAttempt(i, now, pendingOutcome(s, Outcome::LINK_FAIL));
      if (state == SlotState::SUBSCRIBED) emit(EventType::SENSOR_DISCONNECTED, s.type, 0);
    } else if (connected) {
      port_.disconnect(i);
    } else if ((state == SlotState::CONNECTING || state == SlotState::CANCELLING) &&
               !port_.connActive()) {
      leaveAttempt(i, now, Outcome::NEUTRAL);
    }
  }

  // §5.1 step 4: one blocking pass under the watchdog, for the one DISCOVERING slot.
  void gattStep(uint32_t now) {
    for (uint8_t i = 0; i < kSlotCount; ++i) {
      SensorSlot& s = slots_[i];
      if (s.state != SlotState::DISCOVERING || s.gattDone || s.removing) continue;
      uint8_t battery = 255;
      const uint32_t gen = watchdog_.arm(port_.connHandle(i));
      port_.gattTimerStart(BBS_GATT_BUDGET_MS);
      const bool ok = port_.gattPass(i, s.type, battery);
      watchdog_.disarm();
      port_.gattTimerStop();
      const bool aborted = watchdog_.fired(gen);
      if (ok && !aborted) {
        s.battery = battery;
        s.failCount = 0;
        enter(s, SlotState::SUBSCRIBED, now);
        emit(EventType::SENSOR_CONNECTED, s.type);
        return;
      }
      if (aborted) stats_.gattBudgetAborts++;
      s.gattDone = true;
      s.failPending = true;
      port_.disconnect(i);
      return;
    }
  }

  void advanceSlot(uint8_t i, uint32_t now) {
    SensorSlot& s = slots_[i];
    switch (s.state) {
      case SlotState::FOUND: {
        if (now - s.holdFromMs < s.holdMs || anyInFlight()) return;
        const int rc = scan_.stopForConnect(port_, now);
        if (rc != 0 && rc != kHsEAlready) {
          stoppingSafetyExit(s, now);
          return;
        }
        enter(s, SlotState::STOPPING_FOR_CONNECT, now);
        tryConnect(i, now);
        return;
      }
      case SlotState::STOPPING_FOR_CONNECT:
        tryConnect(i, now);
        return;
      case SlotState::CONNECTING:
        if (now - s.stateSinceMs >= kConnectWatchdogMs) {
          port_.cancelConnect(i);
          s.failPending = true;
          enter(s, SlotState::CANCELLING, now);
        }
        return;
      case SlotState::CANCELLING:
        if (now - s.stateSinceMs >= BBS_CANCEL_SAFETY_MS && !port_.connActive() &&
            !port_.isConnected(i)) {
          queue_.requestResync();
          stats_.cancelSafetyExits++;
          leaveAttempt(i, now, pendingOutcome(s, Outcome::CONNECT_FAIL));
        }
        return;
      default:
        return;
    }
  }

  void stoppingSafetyExit(SensorSlot& s, uint32_t now) {
    queue_.requestResync();
    stats_.cancelSafetyExits++;
    scan_.forceOff();
    enter(s, waitState(), now);
  }

  // STOPPING_FOR_CONNECT: connect only under the §5.2 connect invariant.
  void tryConnect(uint8_t i, uint32_t now) {
    SensorSlot& s = slots_[i];
    if (port_.discActive()) {
      if (now - s.stateSinceMs >= BBS_STOPPING_SAFETY_MS) stoppingSafetyExit(s, now);
      return;
    }
    if (port_.connActive()) return;
    startAttempt(s);
    if (port_.connect(i, s.mac, s.addrType)) {
      enter(s, SlotState::CONNECTING, now);
      return;
    }
    const int err = port_.lastError(i);
    if (err == kHsEAlready || err == kHsEBusy) {
      hold(s, now, kTransientRetryMs);
      enter(s, SlotState::FOUND, now);
    } else if (err == kHsENotSynced) {
      hold(s, now, BBS_SCAN_START_BACKOFF_MS);
      enter(s, SlotState::FOUND, now);
    } else {
      leaveAttempt(i, now, Outcome::CONNECT_FAIL);
    }
  }

  // §5.1 step 6. Never in CONNECTING (NimBLE reports connected before onConnect).
  void orphanSweep() {
    for (uint8_t i = 0; i < kSlotCount; ++i) {
      const SlotState state = slots_[i].state;
      if ((state == SlotState::EMPTY || state == SlotState::IDLE ||
           state == SlotState::WAIT_SCAN || state == SlotState::FOUND) &&
          port_.isConnected(i)) {
        port_.disconnect(i);
      }
    }
  }

  BlePort& port_;
  const Mode mode_;
  EventQueue queue_;
  GattWatchdog watchdog_;
  ScanController scan_;
  Mux tableMux_;                        // slot identity read by gapScanCb (§3.4)
  SensorSlot slots_[kSlotCount];
  Stats stats_{};
  EventCallback eventCb_ = nullptr;
};

}  // namespace internal
}  // namespace BikeSensor
