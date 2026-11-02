#pragma once

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "BBSBlePort.h"
#include "BBSCodec.h"
#include "BBSConfig.h"
#include "BBSEventQueue.h"
#include "BBSGattWatchdog.h"
#include "BBSMux.h"
#include "BBSScanController.h"
#include "BBSTypes.h"

// One slot per compiled sensor type, one mode for all slots (§10 steps 3–8):
// slot machine (§5.2), event handling (§5.1), resync (§3.2), HOST_RESET, the
// app lifecycle, DATA and central events (§4), HR/CSC/Power data callbacks,
// cadence source (§6b), relay, server name and advertising desired state
// (§5.4), discovery (§3.3, §5.3), pause/quiesce and begin()/end() (§4),
// driven through BlePort. Arduino-free; the caller passes millis() as `now`.
// Everything except the members marked "any task" runs on the update() task.

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

constexpr uint8_t kCompiledMask =
    (BBS_COMPILE_HEART_RATE ? typeBit(SensorType::HEART_RATE) : 0) |
    (BBS_COMPILE_CSC ? typeBit(SensorType::SPEED_CADENCE) : 0) |
    (BBS_COMPILE_POWER ? typeBit(SensorType::POWER) : 0);

constexpr size_t kServerNameSize = 48;                  // 2.x BBSIdentity buffer
constexpr size_t kNamePrefixMax = kServerNameSize - 1;  // more cannot reach the name

// 2.x BBSIdentity prefix rule: lowercase, every byte outside [a-z0-9-] → '-',
// runs of '-' collapsed, leading and trailing '-' trimmed, empty → "homewind".
// Keeps the first kNamePrefixMax bytes of that result.
inline void sanitizeNamePrefix(const char* in, char (&out)[kNamePrefixMax + 1]) {
  size_t n = 0;
  bool dash = false;                    // a '-' run, written only before [a-z0-9]
  for (const char* p = in; p != nullptr && *p != '\0'; ++p) {
    char c = *p;
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
      dash = n > 0;
      continue;
    }
    if (dash && n < kNamePrefixMax) out[n++] = '-';
    dash = false;
    if (n < kNamePrefixMax) out[n++] = c;
  }
  if (n == 0) {
    memcpy(out, "homewind", sizeof("homewind"));
    return;
  }
  out[n] = '\0';
}

// §5.4 budget: flags 3 + name (2 + len) + 16-bit UUID list (2 + 2n) ≤ 31.
// Checked before NimBLE: it would move UUIDs that do not fit into the
// (disabled) scan response and still report success.
inline bool advertisingFits(size_t nameLen, uint8_t typeMask) {
  size_t uuids = 0;
  for (uint8_t bits = typeMask; bits != 0; bits &= static_cast<uint8_t>(bits - 1)) ++uuids;
  return 3 + 2 + nameLen + (uuids > 0 ? 2 + 2 * uuids : 0) <= 31;
}

constexpr uint32_t kConnectWatchdogMs = BBS_CONNECT_TIMEOUT_MS + 2000;
constexpr uint32_t kEndDisconnectMs = 3000;   // §4 end() step 4
constexpr uint32_t kTransientRetryMs = 500;
constexpr uint32_t kBackoffMinMs = 1000;
constexpr uint32_t kBackoffMaxMs = 30000;

// §5.4 local Feature/Location defaults, and the sensor bits the relay copies
// (it has no Control Point).
constexpr uint32_t kCscFeatureDefault = 0x0002;         // crank data
constexpr uint32_t kCscFeatureMask = 0x0003;
constexpr uint32_t kCpFeatureDefault = 0;
constexpr uint32_t kCpFeatureMask = 0x000301FF;
constexpr uint8_t kCpLocationDefault = 0x0D;            // rear hub

// §6b. Kept in the Core (info().providesCadence is §10 step 9).
enum class CadenceSource : uint8_t { NONE, CSC, POWER };

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
  CrankState crank;                     // CSC and Power; reset whenever an attempt or link ends
  // Last measurement (HR bpm, CSC cadence, Power sample), under tableMux. Set
  // only in SUBSCRIBED and cleared whenever an attempt or link ends.
  bool       hasValue;
  uint16_t   value;
  uint32_t   valueMs;                   // `now` of the packet that produced it
  PowerSample power;                    // Power: stale rule applied at valueMs
  uint32_t   crankMs;                   // Power: crank.lastCrankEventMs, for the getters
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
  // mode and typeMask: the begin() arguments (§4 rev 25); begin() sets them
  // again. Types outside the compiled ones are ignored.
  Core(BlePort& port, Mode mode, uint8_t typeMask)
      : port_(port), mode_(mode), enabled_(typeMask & kCompiledMask), scan_(mode) {
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
  // replaces the identity (§4); a MAC held by another slot, a type outside
  // the begin() mask or quiesce returns false.
  bool addByMac(SensorType type, const char* mac, int8_t addrType, const char* name,
                uint32_t now) {
    const int i = enabledSlot(type);
    uint8_t bytes[6];
    if (i < 0 || quiesced_ || mode_ != Mode::MAC) return false;
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
    refreshCadenceSource();
    return true;
  }

  // NAME mode only. The name is matched exactly and case-sensitively, stored
  // truncated to 31 bytes; a name another slot holds, a type outside the
  // begin() mask or quiesce returns false. The same type on a non-EMPTY slot
  // replaces the identity and goes to IDLE (§4).
  bool addByName(SensorType type, const char* name, uint32_t now) {
    const int i = enabledSlot(type);
    if (i < 0 || quiesced_ || mode_ != Mode::NAME || name == nullptr || name[0] == '\0') {
      return false;
    }
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
    refreshCadenceSource();
    return true;
  }

  // §5.2 remove() table. Link states finish on their outcome event. The local
  // Feature/Location of the type return to their defaults (§5.4).
  void remove(SensorType type, uint32_t now) {
    const int i = slotIndex(type);
    if (i < 0) return;
    SensorSlot& s = slots_[i];
    if (s.state != SlotState::EMPTY) copyFeature(type, GattResult{});
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
        refreshCadenceSource();         // remove(CSC) hands back to Power at once
        return;
    }
  }

  // §4 startServer(), device part: local services and advertising data for
  // the enabled types. Advertising follows the desired state from the next
  // update() on. False if the name and UUIDs do not fit (nothing is created
  // then), the stack is not idle before the first server start (rev 28 guard;
  // NimBLE asserts otherwise), the server or the advertising data cannot be
  // set up, or end() has torn the stack down.
  bool startServer() {
    char name[kServerNameSize];
    serverName(name, sizeof(name));
    if (down_ || !advertisingFits(strlen(name), localTypes())) return false;
    if (!gattStarted_ && !stackIdle()) return false;
    if (!port_.serverStart(localTypes())) return false;
    gattStarted_ = true;
    if (!port_.advSetData(name, localTypes())) return false;
    serverStarted_ = true;
    return true;
  }

  // §5.4: rebuilds the advertising data (name + UUIDs) and restarts
  // advertising under the same rules as the enforcement (desired state, start
  // hold). True if the new data was applied; false under quiesce, if the
  // server is not started, the data does not fit (previous data and
  // advertising stay untouched) or NimBLE refused a field. A failed restart
  // is not a failure.
  bool refreshAdvertising(uint32_t now) {
    if (!serverStarted_ || quiesced_) return false;
    char name[kServerNameSize];
    serverName(name, sizeof(name));
    if (!advertisingFits(strlen(name), localTypes())) return false;
    if (port_.advActive()) {
      advHeld_ = false;                 // a start has succeeded since the failure
      port_.advStop();
    }
    // Unreachable after the budget check in NimBLE 2.5.2 (setName truncates
    // only past 29 bytes); if it happens, advertising restarts with partial data.
    const bool dataOk = port_.advSetData(name, localTypes());
    enforceAdvertising(now);
    return dataOk;
  }

  // Takes effect through refreshAdvertising().
  void setNamePrefix(const char* prefix) {
    char clean[kNamePrefixMax + 1];
    sanitizeNamePrefix(prefix, clean);
    MuxGuard guard(tableMux_);
    memcpy(prefix_, clean, sizeof(clean));
  }

  // Any task. "<prefix>-%08lx" with the chip ID (2.x BBSIdentity); truncated
  // to len - 1 bytes with a NUL.
  void serverName(char* out, size_t len) {
    if (out == nullptr || len == 0) return;
    char prefix[kNamePrefixMax + 1];
    {
      MuxGuard guard(tableMux_);
      memcpy(prefix, prefix_, sizeof(prefix));
    }
    snprintf(out, len, "%s-%08" PRIx32, prefix, port_.chipId());
  }

  void setHeartRateCallback(void (*cb)(uint16_t bpm)) { heartRateCb_ = cb; }
  void setCadenceCallback(void (*cb)(uint16_t rpm)) { cadenceCb_ = cb; }
  // Never called without BBS_COMPILE_POWER.
  void setPowerCallback(void (*cb)(const PowerSample&)) { powerCb_ = cb; }

  // Any task; now: the caller's millis(). False while the source slot is not
  // SUBSCRIBED or has not delivered since entering it (§4 rev 27); the outputs
  // are then unchanged. ageMs: since the packet that produced the value.
  bool lastHeartRate(uint16_t& bpm, uint32_t& ageMs, uint32_t now) {
    return lastValue(slotIndex(SensorType::HEART_RATE), bpm, ageMs, now);
  }
  // §6b source. Power: false until a crank delta (hasCadence); the stale rule
  // applies and ageMs is the time since the last crank event.
  bool lastCadence(uint16_t& rpm, uint32_t& ageMs, uint32_t now) {
    CadenceSource source;
    {
      MuxGuard guard(tableMux_);
      source = source_;
    }
    if (source == CadenceSource::CSC) {
      return lastValue(slotIndex(SensorType::SPEED_CADENCE), rpm, ageMs, now);
    }
    PowerSample sample;
    uint32_t at, crankMs;
    if (source != CadenceSource::POWER || !readPower(sample, at, crankMs, now) ||
        !sample.hasCadence) {
      return false;
    }
    rpm = sample.cadence;
    ageMs = now - crankMs;
    return true;
  }
  // Always false without BBS_COMPILE_POWER. The stale rule (§6) applies at now.
  bool lastPower(PowerSample& out, uint32_t& ageMs, uint32_t now) {
    uint32_t at, crankMs;
    if (!readPower(out, at, crankMs, now)) return false;
    ageMs = now - at;
    return true;
  }
  // Any task. §6b: Power is the cadence source.
  bool providesCadence() {
    MuxGuard guard(tableMux_);
    return source_ == CadenceSource::POWER;
  }

  // §5.3. False under quiesce, while a discovery is pending or open, or for a
  // duration of 0 or >= kDiscoveryDurationLimitMs. Otherwise the new discovery waits as DISCOVERY_PENDING for a running
  // connect/GATT pass; FOUND/STOPPING_FOR_CONNECT slots go back to their wait
  // state, and no slot connects until DISCOVERY_DONE. Results replace the
  // table on DISCOVERY_DONE.
  bool startDiscovery(uint8_t typeMask, uint32_t durationMs, uint32_t now) {
    if (quiesced_ || scan_.discoveryOwner() || durationMs == 0 ||
        durationMs >= kDiscoveryDurationLimitMs) {
      return false;
    }
    {
      MuxGuard guard(tableMux_);
      discCount_ = 0;
      discMask_ = typeMask & kCompiledMask;
      discGen_++;
      discOpen_ = true;
    }
    scan_.beginDiscovery(port_, now, durationMs);
    for (SensorSlot& s : slots_) {
      if (s.state == SlotState::FOUND || s.state == SlotState::STOPPING_FOR_CONNECT) {
        enter(s, waitState(), now);
      }
    }
    return true;
  }

  // §5.3: finishes a pending or running discovery with MANUAL; collected
  // matches are kept. No-op otherwise.
  void stopDiscovery() {
    if (!scan_.discoveryOwner()) return;
    scan_.cancelDiscovery(port_);
    tryFinish(FinishReason::MANUAL);
  }

  // Any task. True from startDiscovery() until DISCOVERY_DONE.
  bool discovering() {
    MuxGuard guard(tableMux_);
    return discOpen_;
  }

  // Any task. The table of the last finished discovery, named entries first.
  uint8_t discoveredCount() {
    MuxGuard guard(tableMux_);
    return discTableCount_;
  }
  bool discovered(uint8_t i, DiscoveredSensor& out) {
    MuxGuard guard(tableMux_);
    if (i >= discTableCount_) return false;
    out = discTable_[i];
    return true;
  }

  // Any task (gapScanCb): upserts one discovery report by address (§3.3).
  // Only reports of the running discovery scan count. A new address needs an
  // advertised type in the requested ∩ compiled mask, then later reports
  // (SCAN_RSP) merge their types and name: a complete name replaces a
  // shortened one, never the reverse. New addresses are dropped once
  // BBS_MAX_DISCOVERED are stored.
  void discoveryReport(uint16_t epoch, const uint8_t mac[6], uint8_t addrType,
                       const AdFields& ad) {
    MuxGuard guard(tableMux_);
    // Under the lock: closeDiscovery() clears the epoch in the same section as
    // the sort, so no report lands after it.
    if (epoch == 0 || epoch != scan_.activeDiscEpoch()) return;
    const uint8_t types = ad.typeMask & discMask_;
    uint8_t i = 0;
    while (i < discCount_ && memcmp(discBuf_[i].mac, mac, sizeof(discBuf_[i].mac)) != 0) ++i;
    if (i == discCount_) {
      if (types == 0 || discCount_ == BBS_MAX_DISCOVERED) return;
      discBuf_[i] = DiscoveredSensor{};
      memcpy(discBuf_[i].mac, mac, sizeof(discBuf_[i].mac));
      discBuf_[i].addrType = addrType;
      discNameComplete_[i] = false;
      discCount_++;
    }
    DiscoveredSensor& d = discBuf_[i];
    d.typeMask |= types;
    if (ad.name != nullptr && ad.nameLen > 0 &&
        (d.name[0] == '\0' || (ad.nameComplete && !discNameComplete_[i]))) {
      const size_t len = ad.nameLen < sizeof(d.name) - 1 ? ad.nameLen : sizeof(d.name) - 1;
      memcpy(d.name, ad.name, len);
      d.name[len] = '\0';
      discNameComplete_[i] = ad.nameComplete;
    }
  }

  // §4 initStack(): NimBLE init with the GAP name "<prefix>-%08lx", prefix
  // from deviceName. Skipped, prefix unchanged, if the stack is up.
  bool initStack(const char* deviceName) {
    if (port_.stackReady()) return true;
    setNamePrefix(deviceName);
    char name[kServerNameSize];
    serverName(name, sizeof(name));
    return port_.stackInit(name);
  }

  // §4 begin(): stack init if needed, clients and GATT timer, server. A fresh
  // session: every slot EMPTY, no queued events, the given mode and types.
  // False while begun and while quiesced without a successful end(), so the
  // latch is cleared only after end() returned true. A name and UUID list that
  // do not fit (§5.4), a failed stack init or open() return false before the
  // session changes; only the prefix follows deviceName as in initStack().
  bool begin(const char* deviceName, uint8_t typeMask, Mode mode) {
    if (begun_ || (quiesced_ && !down_)) return false;
    if (!port_.stackReady()) setNamePrefix(deviceName);
    char name[kServerNameSize];
    serverName(name, sizeof(name));
    if (!advertisingFits(strlen(name), localTypes(typeMask)) || !initStack(deviceName) ||
        !port_.open()) {
      return false;
    }
    mode_ = mode;
    enabled_ = typeMask & kCompiledMask;
    scan_.reset(mode);
    for (SensorSlot& s : slots_) clearSlot(s);
    refreshCadenceSource();
    cadenceArmed_ = false;
    Event ev;
    while (queue_.pop(ev)) {}
    queue_.takeResync();
    {
      MuxGuard guard(tableMux_);
      quiesced_ = false;
      discOpen_ = false;
      centralCount_ = 0;
    }
    down_ = false;
    advHeld_ = false;
    begun_ = startServer();
    return begun_;
  }

  // §4 end(), update() task only, never the NimBLE host task, and never from
  // an event or data callback (it runs update() itself). False (not torn
  // down, reboot) after a timeout in step 2 or 4 or a failed teardown; the
  // quiesce latch stays either way.
  bool end() {
    if (down_) return true;
    quiesce(port_.nowMs());
    if (!driveUntil(kConnectWatchdogMs, [this] { return quiesced(); })) return false;
    // Every client with a handle, also one DISCONNECTING in a wait state:
    // deinit(true) cannot delete it (§4 step 4). Its DISCONNECTED is queued
    // before the handle clears, so one more pass handles it.
    for (uint8_t i = 0; i < kSlotCount; ++i) {
      if (port_.connHandle(i) != kConnHandleNone) port_.disconnect(i);
    }
    if (!driveUntil(kEndDisconnectMs, [this] { return !anyConnHandle(); })) return false;
    update(port_.nowMs());
    if (!port_.close()) return false;
    serverStarted_ = gattStarted_ = begun_ = false;
    down_ = true;
    return true;
  }

  // §5.5, 2.x semantics: advertising only; scans, connects and discovery go on.
  void pause() {
    {
      MuxGuard guard(tableMux_);
      paused_ = true;
    }
    if (port_.advActive()) port_.advStop();
  }

  // Advertising follows the desired state from the next update(). No-op under quiesce.
  void resume() {
    MuxGuard guard(tableMux_);
    if (!quiesced_) paused_ = false;
  }

  // §4 quiesce(): one-way until end() + begin(), idempotent. DISCOVERY_DONE
  // (QUIESCED) follows all five steps. A DISCOVERING slot stays there until
  // its DISCONNECTED, without a GATT pass.
  void quiesce(uint32_t now) {
    if (quiesced_) return;
    {
      MuxGuard guard(tableMux_);
      quiesced_ = true;
    }
    if (port_.advActive()) port_.advStop();
    const bool discovery = scan_.discoveryOwner();
    DiscoveryDone done{};
    if (discovery) {
      scan_.cancelDiscovery(port_);
      done = closeDiscovery(FinishReason::QUIESCED);
    }
    scan_.stopWindow(port_);
    for (uint8_t i = 0; i < kSlotCount; ++i) {
      SensorSlot& s = slots_[i];
      if (s.state == SlotState::FOUND || s.state == SlotState::STOPPING_FOR_CONNECT) {
        enter(s, waitState(), now);
      } else if (s.state == SlotState::CONNECTING) {
        port_.cancelConnect(i);
        enter(s, SlotState::CANCELLING, now);
      } else if (s.state == SlotState::DISCOVERING) {
        port_.disconnect(i);
      }
    }
    if (discovery) emitDone(done);
  }

  // §4 quiesced(). update() task: reads the slot states.
  bool quiesced() {
    return quiesced_ && scan_.owner() == ScanOwner::OFF && !port_.discActive() &&
           !port_.advActive() && !linkBusy();
  }

  // Any task.
  bool scanning() { return scan_.owner() != ScanOwner::OFF || port_.discActive(); }

  // A copy, with the event queue's drop counters (§3.2). update() task.
  Stats stats() {
    Stats out = stats_;
    const QueueCounters q = queue_.counters();
    out.eventsDropped = q.eventsDropped;
    out.notifiesDropped = q.notifiesDropped;
    out.matchesDropped = q.matchesDropped;
    return out;
  }

  // Under quiesce: drain, advertising off, cancellations and disconnects; no
  // GATT pass, connect or scan. Nothing after a successful end().
  void update(uint32_t now) {
    if (down_) return;
    if (queue_.takeResync()) resync(now);
    Event ev;
    for (uint8_t i = 0; i < BBS_EVENT_QUEUE_DEPTH && queue_.pop(ev); ++i) handle(ev, now);
    enforceAdvertising(now);
    if (!quiesced_) gattStep(now);
    for (uint8_t i = 0; i < kSlotCount; ++i) advanceSlot(i, now);
    advanceCadence(now);
    orphanSweep();
    if (quiesced_) return;
    FinishReason finish;
    if (scan_.advance(port_, now, needScan(), scanBlocked(), linkBusy(), stats_, finish)) {
      tryFinish(finish);
    }
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

  // Any task.
  uint8_t centralCount() {
    MuxGuard guard(tableMux_);
    return centralCount_;
  }

  // Any task (client callbacks): the attempt an event belongs to.
  uint16_t attemptGen(uint8_t slot) const {
    return slot < kSlotCount ? slots_[slot].attemptGen.load(std::memory_order_acquire) : 0;
  }

  EventQueue& queue() { return queue_; }
  GattWatchdog& watchdog() { return watchdog_; }
  const SensorSlot& slot(uint8_t i) const { return slots_[i]; }
  const ScanController& scan() const { return scan_; }

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

  // §5.3 DISCOVERY_PENDING waits for these. A slot that becomes FOUND later
  // (direct retry, MAC replace) does not block it; it waits for DISCOVERY_DONE.
  bool linkBusy() const {
    for (const SensorSlot& s : slots_) {
      if (s.state == SlotState::CONNECTING || s.state == SlotState::CANCELLING ||
          s.state == SlotState::DISCOVERING) {
        return true;
      }
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
      clearValue(s);
    }
    s.state = SlotState::EMPTY;
    s.addrType = 0;
    s.battery = 255;
    s.lastDisconnectReason = 0;
    s.failCount = 0;
    s.stateSinceMs = 0;
    s.holdFromMs = 0;
    s.holdMs = 0;
    resetCrank(s.crank);
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

  // Caller holds tableMux_.
  static void clearValue(SensorSlot& s) {
    s.hasValue = false;
    s.value = 0;
    s.valueMs = 0;
    s.power = PowerSample{};
    s.crankMs = 0;
  }

  // The single exit of an attempt or link. Resets the per-attempt flags, the
  // crank state and the last measurement (§3.1), then: removing → EMPTY; hard
  // fail → backoff + SENSOR_FAILED; otherwise the wait state, or FOUND for a
  // pending direct attempt (MAC only).
  void leaveAttempt(uint8_t i, uint32_t now, Outcome outcome) {
    SensorSlot& s = slots_[i];
    const bool direct = s.directRetry;
    resetCrank(s.crank);
    {
      MuxGuard guard(tableMux_);
      clearValue(s);
    }
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

  void emit(EventType type, SensorType sensorType, int reason = 0, uint8_t count = 0,
            uint32_t generation = 0) {
    if (eventCb_ == nullptr) return;
    EventInfo info{};
    info.type = type;
    info.sensorType = sensorType;
    info.reason = reason;
    info.count = count;
    info.generation = generation;
    eventCb_(info);
  }

  struct DiscoveryDone {
    uint32_t generation;
    uint8_t count;
    FinishReason reason;
  };

  // §5.3 tryFinish(), state part (caller checked discoveryOwner()): owner OFF,
  // later reports stale, discBuf sorted into discTable.
  DiscoveryDone closeDiscovery(FinishReason reason) {
    MuxGuard guard(tableMux_);
    scan_.finishDiscovery();
    sortDiscovered();
    discOpen_ = false;
    return {discGen_, discTableCount_, reason};
  }

  void emitDone(const DiscoveryDone& done) {
    emit(EventType::DISCOVERY_DONE, SensorType{}, static_cast<int>(done.reason), done.count,
         done.generation);
  }

  // Wins once per discovery: afterwards the owner is OFF.
  void tryFinish(FinishReason reason) {
    if (scan_.discoveryOwner()) emitDone(closeDiscovery(reason));
  }

  // Caller holds tableMux_. Stable insertion sort: named entries by strcmp,
  // then unnamed ones in arrival order.
  void sortDiscovered() {
    uint8_t n = 0;
    for (uint8_t k = 0; k < discCount_; ++k) {
      const char* name = discBuf_[k].name;
      uint8_t j = n;
      while (j > 0 && name[0] != '\0' &&
             (discTable_[j - 1].name[0] == '\0' || strcmp(discTable_[j - 1].name, name) > 0)) {
        discTable_[j] = discTable_[j - 1];
        --j;
      }
      discTable_[j] = discBuf_[k];
      ++n;
    }
    discTableCount_ = n;
  }

  void handle(const Event& ev, uint32_t now) {
    switch (ev.kind) {
      case Ev::SCAN_MATCH:
        if (scan_.isCurrent(ev.gen)) onScanMatch(ev, now);
        break;
      case Ev::SCAN_END:
        if (scan_.onScanEnd(ev.gen)) tryFinish(FinishReason::NATURAL);
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
      case Ev::NOTIFY:
        if (ev.slot < kSlotCount && slots_[ev.slot].state == SlotState::SUBSCRIBED) {
          onNotify(ev.slot, ev, now);
        }
        break;
      case Ev::CENTRAL_CONNECT:
        emit(EventType::CENTRAL_CONNECTED, SensorType{}, 0, refreshCentralCount());
        break;
      case Ev::CENTRAL_DISCONNECT:
        advHeld_ = false;
        emit(EventType::CENTRAL_DISCONNECTED, SensorType{}, ev.reason, refreshCentralCount());
        break;
      case Ev::SUBSCRIBE:
        if (ev.slot < kSlotCount) {
          emit(ev.sub.subValue != 0 ? EventType::CENTRAL_SUBSCRIBED
                                    : EventType::CENTRAL_UNSUBSCRIBED,
               slots_[ev.slot].type);
        }
        break;
    }
  }

  // §5.1 NOTIFY on a SUBSCRIBED slot. A cut packet contradicts its own flags:
  // neither parsed nor relayed. A malformed one is still relayed raw, without
  // data callbacks or DATA.
  void onNotify(uint8_t i, const Event& ev, uint32_t now) {
    SensorSlot& s = slots_[i];
    stats_.notifies++;
    if (ev.reason > ev.notify.len) {
      stats_.notifiesTruncated++;
      return;
    }
    if (s.type == SensorType::POWER) {
      onPowerNotify(i, ev, now);
      return;
    }
    uint16_t value = 0;
    bool hasValue = false;
    const bool ok = parseMeasurement(s, ev.notify.data, ev.notify.len, now, value, hasValue);
    if (!ok) stats_.notifiesMalformed++;
    port_.relay(i, ev.notify.data, ev.notify.len);
    if (!ok) return;
    if (hasValue) {
      MuxGuard guard(tableMux_);
      s.hasValue = true;
      s.value = value;
      s.valueMs = now;
    }
    const SensorType type = s.type;
    if (type == SensorType::HEART_RATE) {
      if (heartRateCb_ != nullptr) heartRateCb_(value);
    } else if (hasValue && currentCadenceSource() == CadenceSource::CSC &&
               cadenceCb_ != nullptr) {
      cadenceCb_(value);
    }
    emit(EventType::DATA, type);
  }

  // §6, §6b. The stored sample carries cadence over packets without a crank
  // event. Relay → power callback → cadence callback (Power source, new crank
  // delta only) → DATA.
  void onPowerNotify(uint8_t i, const Event& ev, uint32_t now) {
    SensorSlot& s = slots_[i];
    PowerSample sample = s.power;       // only this task writes it
    bool crankUpdated = false;
    const bool ok = parsePower(ev.notify.data, ev.notify.len, now, s.crank, sample, crankUpdated);
    if (!ok) stats_.notifiesMalformed++;
    port_.relay(i, ev.notify.data, ev.notify.len);
    if (!ok) return;
    {
      MuxGuard guard(tableMux_);
      s.power = sample;
      s.hasValue = true;
      s.valueMs = now;
      s.crankMs = s.crank.lastCrankEventMs;
    }
    if (powerCb_ != nullptr) powerCb_(sample);
    if (crankUpdated && currentCadenceSource() == CadenceSource::POWER) {
      cadenceArmed_ = sample.cadence != 0;
      if (cadenceCb_ != nullptr) cadenceCb_(sample.cadence);
    }
    emit(EventType::DATA, SensorType::POWER);
  }

  // Any task: the Power slot's sample with the stale rule at now (moved up to
  // the packet time if read before update() stored it). out is written only on true.
  bool readPower(PowerSample& out, uint32_t& at, uint32_t& crankMs, uint32_t& now) {
    const int p = slotIndex(SensorType::POWER);
    if (p < 0) return false;
    {
      MuxGuard guard(tableMux_);
      if (!slots_[p].hasValue) return false;
      out = slots_[p].power;
      at = slots_[p].valueMs;
      crankMs = slots_[p].crankMs;
    }
    if (static_cast<int32_t>(now - at) < 0) now = at;
    CrankState crank{};
    crank.lastCrankEventMs = crankMs;
    applyPowerStale(out, crank, now);
    return true;
  }

  // HR: bpm. CSC: cadence, hasValue only when the packet reports one (§6:
  // none without crank data or for the first crank packet after a reset).
  static bool parseMeasurement(SensorSlot& s, const uint8_t* data, uint8_t len, uint32_t now,
                               uint16_t& value, bool& hasValue) {
    if (s.type == SensorType::HEART_RATE) {
      hasValue = parseHeartRate(data, len, value);
      return hasValue;
    }
    return parseCsc(data, len, now, s.crank, value, hasValue);
  }

  // hasValue is set only in SUBSCRIBED and cleared on every exit, so it alone
  // answers "SUBSCRIBED and delivered" without reading the update-task state.
  // A now read just before update() stored the value gives age 0, not a wrap.
  bool lastValue(int i, uint16_t& value, uint32_t& ageMs, uint32_t now) {
    if (i < 0) return false;
    uint16_t v;
    uint32_t at;
    {
      MuxGuard guard(tableMux_);
      if (!slots_[i].hasValue) return false;
      v = slots_[i].value;
      at = slots_[i].valueMs;
    }
    const uint32_t age = now - at;
    value = v;
    ageMs = static_cast<int32_t>(age) < 0 ? 0 : age;
    return true;
  }

  // §6b source rule (rev 31): a configured (non-EMPTY) CSC slot, CSC compiled
  // and in the begin() mask, always wins; otherwise a SUBSCRIBED Power slot;
  // otherwise none.
  CadenceSource currentCadenceSource() const {
    const int csc = enabledSlot(SensorType::SPEED_CADENCE);
    if (csc >= 0 && slots_[csc].state != SlotState::EMPTY) return CadenceSource::CSC;
    const int p = slotIndex(SensorType::POWER);
    if (p >= 0 && slots_[p].state == SlotState::SUBSCRIBED) return CadenceSource::POWER;
    return CadenceSource::NONE;
  }

  // The copy the any-task getters read.
  void refreshCadenceSource() {
    const CadenceSource source = currentCadenceSource();
    MuxGuard guard(tableMux_);
    source_ = source;
  }

  // §5.1 step 5, §6b: after non-zero Power cadence callbacks, one 0 when no
  // crank event came for BBS_POWER_STALE_MS or the source left Power.
  void advanceCadence(uint32_t now) {
    refreshCadenceSource();
    if (!cadenceArmed_) return;
    const int p = slotIndex(SensorType::POWER);
    if (p >= 0 && currentCadenceSource() == CadenceSource::POWER &&
        now - slots_[p].crank.lastCrankEventMs <= BBS_POWER_STALE_MS) {
      return;
    }
    cadenceArmed_ = false;
    if (cadenceCb_ != nullptr) cadenceCb_(0);
  }

  // The server's count is authoritative: a +1/-1 per event would count twice
  // the CENTRAL_* events still queued behind a resync that read it.
  uint8_t refreshCentralCount() {
    const uint8_t count = port_.centralCount();
    MuxGuard guard(tableMux_);
    centralCount_ = count;
    return count;
  }

  // §5.4 desired state, enforced on every update(), after HOST_RESET and after
  // resync. Also stops NimBLE's own restarts (§0.9).
  bool advertisingDesired() const { return serverStarted_ && !paused_ && !quiesced_; }

  // ble_gatts_mutable(): no advertising, discovery or connect procedure and no
  // established connection.
  bool stackIdle() {
    if (port_.advActive() || port_.discActive() || port_.connActive()) return false;
    for (uint8_t i = 0; i < kSlotCount; ++i) {
      if (port_.isConnected(i)) return false;
    }
    return true;
  }

  // Slot of a type in the begin() mask (rev 28), or -1.
  int enabledSlot(SensorType type) const {
    return (enabled_ & typeBit(type)) != 0 ? slotIndex(type) : -1;
  }

  // begin() mask ∩ compiled; each has a local service, created once per
  // begin() (rev 31), and its UUID in the advertising.
  uint8_t localTypes() const { return localTypes(enabled_); }
  static uint8_t localTypes(uint8_t typeMask) { return typeMask & kCompiledMask; }

  // §5.4: the sensor's Feature without the bits that need a Control Point (CP
  // also the Sensor Location); a missing read gives the default. HR: no-op.
  void copyFeature(SensorType type, const GattResult& r) {
    if (type == SensorType::SPEED_CADENCE) {
      port_.setLocalFeature(type, r.hasFeature ? r.feature & kCscFeatureMask : kCscFeatureDefault,
                            0);
    } else if (type == SensorType::POWER) {
      port_.setLocalFeature(type, r.hasFeature ? r.feature & kCpFeatureMask : kCpFeatureDefault,
                            r.hasLocation ? r.location : kCpLocationDefault);
    }
  }

  // Rev 28: a failed start holds the next one for BBS_SCAN_START_BACKOFF_MS
  // (all connections in use → BLE_HS_ENOMEM on every attempt). Stops are never
  // held; a connection freed, HOST_RESET and resync end the hold.
  void enforceAdvertising(uint32_t now) {
    const bool desired = advertisingDesired();
    const bool active = port_.advActive();
    if (active) advHeld_ = false;       // a start has succeeded since the failure
    if (desired && !active) {
      if (advHeld_ && now - advHeldSinceMs_ < BBS_SCAN_START_BACKOFF_MS) return;
      startAdvertising(now);
    } else if (!desired && active) {
      port_.advStop();
      stats_.advertisingEnforced++;
    }
  }

  void startAdvertising(uint32_t now) {
    advHeld_ = !port_.advStart();
    advHeldSinceMs_ = now;
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
      advHeld_ = false;                 // a connection is free again
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

  // A pending or open discovery finishes with HOST_RESET (matches kept);
  // DISCOVERY_DONE follows the state changes and precedes HOST_RESET.
  void onHostReset(uint32_t now) {
    const bool discovery = scan_.discoveryOwner();
    DiscoveryDone done{};
    if (discovery) {
      done = closeDiscovery(FinishReason::HOST_RESET);
    } else {
      scan_.forceOff();
    }
    for (uint8_t i = 0; i < kSlotCount; ++i) {
      const SlotState state = slots_[i].state;
      if (state == SlotState::CONNECTING || state == SlotState::CANCELLING ||
          state == SlotState::DISCOVERING) {
        leaveAttempt(i, now, Outcome::NEUTRAL);
      }
    }
    advHeld_ = false;
    enforceAdvertising(now);
    stats_.hostResets++;
    if (discovery) emitDone(done);
    emit(EventType::HOST_RESET, SensorType{});
  }

  // §3.2 resync.
  void resync(uint32_t now) {
    // Step 4 first: the slot repair may emit(). DISCOVERY_DONE goes out before
    // it, so a discovery started from a later callback cannot precede it.
    if (!port_.discActive()) {
      if (scan_.discoveryOwner()) {
        tryFinish(FinishReason::RESYNC);
      } else {
        scan_.forceOff();
      }
    }
    for (uint8_t i = 0; i < kSlotCount; ++i) resyncSlot(i, now);
    refreshCentralCount();
    advHeld_ = false;
    enforceAdvertising(now);
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
      GattResult result;
      const uint32_t gen = watchdog_.arm(port_.connHandle(i));
      port_.gattTimerStart(BBS_GATT_BUDGET_MS);
      const bool ok = port_.gattPass(i, s.type, result);
      watchdog_.disarm();
      port_.gattTimerStop();
      const bool aborted = watchdog_.fired(gen);
      if (ok && !aborted) {
        s.battery = result.battery;
        copyFeature(s.type, result);
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
        // §5.2: no connect while a discovery is pending or open, or under quiesce.
        if (quiesced_ || now - s.holdFromMs < s.holdMs || anyInFlight() ||
            scan_.discoveryOwner()) {
          return;
        }
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

  // end(): update() passes until done() or ms elapsed, one tick apart.
  template <typename Done>
  bool driveUntil(uint32_t ms, Done done) {
    const uint32_t start = port_.nowMs();
    for (;;) {
      const uint32_t now = port_.nowMs();
      update(now);
      if (done()) return true;
      if (now - start >= ms) return false;
      port_.waitTick();
    }
  }

  bool anyConnHandle() {
    for (uint8_t i = 0; i < kSlotCount; ++i) {
      if (port_.connHandle(i) != kConnHandleNone) return true;
    }
    return false;
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
  Mode mode_;
  uint8_t enabled_;                     // begin() typeMask ∩ compiled
  EventQueue queue_;
  GattWatchdog watchdog_;
  ScanController scan_;
  Mux tableMux_;                        // slot identity read by gapScanCb, last measurements, central count, name prefix, discovery (§3.4)
  SensorSlot slots_[kSlotCount];
  // Discovery (§3.3), under tableMux_: discBuf_ is written by gapScanCb while
  // the discovery scan runs, discTable_ is the sorted copy the getters read.
  DiscoveredSensor discBuf_[BBS_MAX_DISCOVERED];
  bool discNameComplete_[BBS_MAX_DISCOVERED];   // per discBuf_ entry: name from AD 0x09
  uint8_t discCount_ = 0;
  DiscoveredSensor discTable_[BBS_MAX_DISCOVERED];
  uint8_t discTableCount_ = 0;
  uint8_t discMask_ = 0;                // requested ∩ compiled
  uint32_t discGen_ = 0;                // one per accepted startDiscovery()
  bool discOpen_ = false;               // startDiscovery() until DISCOVERY_DONE
  Stats stats_{};
  EventCallback eventCb_ = nullptr;
  void (*heartRateCb_)(uint16_t) = nullptr;
  void (*cadenceCb_)(uint16_t) = nullptr;
  void (*powerCb_)(const PowerSample&) = nullptr;
  CadenceSource source_ = CadenceSource::NONE;     // under tableMux_; refreshCadenceSource()
  bool cadenceArmed_ = false;           // §6b "once": a non-zero Power cadence callback since the last 0
  char prefix_[kNamePrefixMax + 1] = "homewind";   // under tableMux_
  uint8_t centralCount_ = 0;            // under tableMux_
  bool serverStarted_ = false;
  bool gattStarted_ = false;            // port_.serverStart() succeeded once
  bool advHeld_ = false;                // last advStart() failed (rev 28 hold)
  uint32_t advHeldSinceMs_ = 0;
  bool paused_ = false;                 // pause latch (§5.5), written under tableMux_
  bool quiesced_ = false;               // quiesce latch (§4), written under tableMux_
  bool begun_ = false;                  // begin() succeeded, no successful end() since
  bool down_ = false;                   // end() tore the stack down; begin() not yet
};

}  // namespace internal
}  // namespace BikeSensor
