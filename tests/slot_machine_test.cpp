#include "BBSCore.h"
#include "BBSEventPush.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>

using BikeSensor::EventInfo;
using BikeSensor::EventType;
using BikeSensor::SensorType;
using BikeSensor::SlotState;
using BikeSensor::internal::BlePort;
using BikeSensor::internal::Core;
using BikeSensor::internal::GattWatchdog;
using BikeSensor::internal::ScanOwner;
namespace bs = BikeSensor::internal;

static const char kMacStr[] = "aa:bb:cc:dd:ee:01";
static const uint8_t kMac[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01};
static const char kOtherMacStr[] = "aa:bb:cc:dd:ee:02";
static const uint8_t kOtherMac[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x02};

struct FakePort final : BlePort {
  bool synced = true, discOn = false, connOn = false, connected = false;
  bool cancelLeavesDiscOn = false;
  int discStartRc = 0, discCancelRc = 0, connectErr = 0;
  bool connectOk = true, gattOk = true;
  uint8_t gattBattery = 80;
  const uint8_t* expectMac = kMac;
  std::function<void()> duringGatt;
  int discStarts = 0, discCancels = 0, connects = 0, cancelConnects = 0, disconnects = 0;
  int gattPasses = 0, timerStarts = 0, timerStops = 0;
  uint16_t lastEpoch = 0;
  uint32_t lastDurationMs = 0, timerMs = 0;
  uint8_t lastAddrType = 0xFF;

  bool hsSynced() override { return synced; }
  bool discActive() override { return discOn; }
  bool connActive() override { return connOn; }
  int discStart(uint16_t epoch, uint32_t durationMs) override {
    discStarts++;
    lastEpoch = epoch;
    lastDurationMs = durationMs;
    if (discOn) return bs::kHsEAlready;
    if (discStartRc == 0) discOn = true;
    return discStartRc;
  }
  int discCancel() override {
    discCancels++;
    if (!cancelLeavesDiscOn) discOn = false;
    return discCancelRc;
  }
  bool connect(uint8_t, const uint8_t mac[6], uint8_t addrType) override {
    connects++;
    assert(memcmp(mac, expectMac, 6) == 0);
    assert(!discOn && !connOn);       // §5.2 connect invariant
    lastAddrType = addrType;
    if (connectOk) connOn = true;
    return connectOk;
  }
  int lastError(uint8_t) override { return connectErr; }
  bool cancelConnect(uint8_t) override { cancelConnects++; return true; }
  bool disconnect(uint8_t) override { disconnects++; return true; }
  bool isConnected(uint8_t) override { return connected; }
  uint16_t connHandle(uint8_t) override { return 7; }
  bool gattPass(uint8_t, SensorType, uint8_t& battery) override {
    gattPasses++;
    if (duringGatt) duringGatt();
    battery = gattBattery;
    return gattOk;
  }
  void gattTimerStart(uint32_t ms) override { timerStarts++; timerMs = ms; }
  void gattTimerStop() override { timerStops++; }
};

struct Recorded {
  EventType type;
  SensorType sensorType;
  int reason;
  SlotState state;                                     // slot state when the callback ran
};
static Recorded g_events[16];
static int g_eventCount = 0;
static const Core* g_core = nullptr;

static void onEvent(const EventInfo& info) {
  assert(g_eventCount < 16);
  g_events[g_eventCount++] = {info.type, info.sensorType, info.reason, g_core->slot().state};
}

static int countEvents(EventType type) {
  int n = 0;
  for (int i = 0; i < g_eventCount; ++i) n += g_events[i].type == type ? 1 : 0;
  return n;
}

static void expectEvent(int i, EventType type, SlotState state, int reason = 0,
                        SensorType sensorType = SensorType::HEART_RATE) {
  assert(i < g_eventCount);
  assert(g_events[i].type == type && g_events[i].state == state);
  assert(g_events[i].reason == reason && g_events[i].sensorType == sensorType);
}

struct Rig {
  FakePort port;
  std::unique_ptr<Core> core{new Core(port)};
  uint32_t now = 1000;

  Rig() {
    core->setEventCallback(onEvent);
    g_core = core.get();
    g_eventCount = 0;
  }

  SlotState state() const { return core->slot().state; }
  uint16_t gen() const { return core->attemptGen(Core::kSlot); }
  void update() { core->update(now); }
  void at(uint32_t t) { now = t; update(); }
  void add() { assert(core->addByMac(SensorType::HEART_RATE, kMacStr, 0, "HRM", now)); }
  // Same-type re-add with kOtherMac (random address).
  void replace() {
    port.expectMac = kOtherMac;
    assert(core->addByMac(SensorType::HEART_RATE, kOtherMacStr, 1, "HRM2", now));
    assert(memcmp(core->slot().mac, kOtherMac, 6) == 0 && core->slot().addrType == 1);
    assert(strcmp(core->slot().name, "HRM2") == 0 && core->slot().failCount == 0);
    assert(core->scanFilterMatch(kOtherMac) && !core->scanFilterMatch(kMac));
  }
  void match(uint16_t epoch, const uint8_t* mac = kMac, uint8_t addrType = 0) {
    assert(bs::pushScanMatch(core->queue(), epoch, mac, addrType, "HRM", 3));
  }
  void connectedEv(uint16_t g) {
    port.connOn = false;
    port.connected = true;
    assert(bs::pushConnected(core->queue(), 0, g));
  }
  void failEv(uint16_t g, int16_t reason = 13) {
    port.connOn = false;
    assert(bs::pushConnectFail(core->queue(), 0, g, reason));
  }
  void discEv(uint16_t g, int16_t reason = 0x208) {
    port.connOn = false;
    port.connected = false;
    assert(bs::pushDisconnected(core->queue(), 0, g, reason));
  }
  // add → scan window → match → CONNECTING.
  void toConnecting() {
    add();
    update();
    assert(core->scan().owner() == ScanOwner::SLEEP_WINDOW);
    match(core->scan().epoch());
    update();
    assert(state() == SlotState::CONNECTING);
  }
  void toSubscribed() {
    toConnecting();
    connectedEv(gen());
    update();
    assert(state() == SlotState::SUBSCRIBED);
  }
};

static void testAddByMac() {
  Rig r;
  const char* bad[] = {nullptr, "", "aa:bb:cc:dd:ee", "aa:bb:cc:dd:ee:0g", "aa-bb-cc-dd-ee-01",
                       "aa:bb:cc:dd:ee:01 ", " aa:bb:cc:dd:ee:01"};
  for (const char* mac : bad) {
    assert(!r.core->addByMac(SensorType::HEART_RATE, mac, 0, nullptr, r.now));
  }
  assert(!r.core->addByMac(SensorType::HEART_RATE, kMacStr, 2, nullptr, r.now));
  assert(!r.core->addByMac(SensorType::HEART_RATE, kMacStr, -2, nullptr, r.now));
  assert(!r.core->addByMac(static_cast<SensorType>(0x08), kMacStr, 0, nullptr, r.now));
#if !BBS_COMPILE_POWER
  assert(!r.core->addByMac(SensorType::POWER, kMacStr, 0, nullptr, r.now));
#endif
  assert(r.state() == SlotState::EMPTY);
  assert(!r.core->scanFilterMatch(kMac));

  const char longName[] = "0123456789012345678901234567890123456789";
  assert(r.core->addByMac(SensorType::SPEED_CADENCE, "AA:BB:CC:DD:EE:01", -1, longName, r.now));
  assert(r.state() == SlotState::WAIT_SCAN);
  assert(r.core->slot().type == SensorType::SPEED_CADENCE);
  assert(memcmp(r.core->slot().mac, kMac, 6) == 0);
  assert(r.core->slot().addrType == 0);                  // 0xaa: top bits 10 → public
  assert(strlen(r.core->slot().name) == 31 && strncmp(r.core->slot().name, longName, 31) == 0);
  assert(r.core->scanFilterMatch(kMac) && !r.core->scanFilterMatch(kOtherMac));
  // Another type on the non-EMPTY slot, or a bad MAC on a replace: false, unchanged.
  assert(!r.core->addByMac(SensorType::HEART_RATE, kOtherMacStr, 0, nullptr, r.now));
  assert(!r.core->addByMac(SensorType::SPEED_CADENCE, "aa:bb:cc:dd:ee", 0, nullptr, r.now));
  assert(r.state() == SlotState::WAIT_SCAN && r.core->slot().type == SensorType::SPEED_CADENCE);
  assert(memcmp(r.core->slot().mac, kMac, 6) == 0 && r.core->scanFilterMatch(kMac));
  // Same type replaces: new identity, straight to FOUND.
  assert(r.core->addByMac(SensorType::SPEED_CADENCE, kOtherMacStr, -1, nullptr, r.now));
  assert(r.state() == SlotState::FOUND && r.core->slot().name[0] == '\0');
  assert(r.core->scanFilterMatch(kOtherMac) && !r.core->scanFilterMatch(kMac));

  r.core->remove(r.now);
  assert(r.state() == SlotState::EMPTY && !r.core->scanFilterMatch(kMac));
  assert(r.core->addByMac(SensorType::HEART_RATE, "c0:bb:cc:dd:ee:01", -1, nullptr, r.now));
  assert(r.core->slot().name[0] == '\0');
  assert(r.core->slot().addrType == 1);
  r.core->remove(r.now);
  assert(r.core->addByMac(SensorType::HEART_RATE, "c0:bb:cc:dd:ee:01", 0, nullptr, r.now));
  assert(r.core->slot().addrType == 0);                  // explicit type wins
}

static void testHappyPath() {
  Rig r;
  r.add();
  r.update();
  assert(r.port.discStarts == 1 && r.port.lastEpoch == 1);
  assert(r.port.lastDurationMs == BBS_SLEEP_SCAN_DURATION_MS);
  assert(r.core->stats().scansStarted == 1);
  assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW);

  r.now += 200;
  r.match(1, kMac, 1);
  r.update();                                          // FOUND → STOPPING → CONNECTING, one pass
  assert(r.state() == SlotState::CONNECTING);
  assert(r.port.discCancels == 1 && r.core->scan().owner() == ScanOwner::OFF);
  assert(r.port.connects == 1 && r.port.lastAddrType == 1);   // address type from the report
  assert(r.core->slot().addrType == 1 && r.gen() == 1);

  r.connectedEv(1);
  r.update();                                          // DISCOVERING → GATT pass, one pass
  assert(r.state() == SlotState::SUBSCRIBED);
  assert(r.port.gattPasses == 1 && r.port.timerStarts == 1 && r.port.timerStops == 1);
  assert(r.port.timerMs == BBS_GATT_BUDGET_MS);
  assert(r.core->slot().battery == 80);
  assert(r.core->stats().connects == 1 && r.core->stats().gattBudgetAborts == 0);
  r.update();
  assert(r.port.gattPasses == 1 && r.port.discStarts == 1);   // no rescan while subscribed
}

static void testScanMatchAddrTypeWins() {
  Rig r;                                               // configured random, advertiser public
  assert(r.core->addByMac(SensorType::HEART_RATE, kMacStr, 1, "HRM", r.now));
  assert(r.core->slot().addrType == 1);
  r.update();
  r.match(r.core->scan().epoch(), kMac, 0);
  r.update();
  assert(r.state() == SlotState::CONNECTING);
  assert(r.core->slot().addrType == 0 && r.port.lastAddrType == 0);
}

static void testAttemptGenStaleness() {
  Rig r;
  r.toConnecting();
  const uint16_t g = r.gen();
  r.connectedEv(static_cast<uint16_t>(g - 1));
  r.failEv(static_cast<uint16_t>(g + 1));
  r.discEv(static_cast<uint16_t>(g - 1));
  assert(bs::pushConnected(r.core->queue(), 1, g));    // other slot index
  r.update();
  assert(r.state() == SlotState::CONNECTING);

  r.connectedEv(g);
  r.update();
  assert(r.state() == SlotState::SUBSCRIBED);
  g_eventCount = 0;
  r.discEv(static_cast<uint16_t>(g - 1));
  r.failEv(static_cast<uint16_t>(g - 1));
  r.port.connected = true;
  r.update();
  assert(r.state() == SlotState::SUBSCRIBED && r.core->stats().disconnects == 0);

  r.port.connected = false;
  r.failEv(g, 0x3e);                                   // CONNECT_FAIL on a link = link loss
  r.update();
  assert(r.state() == SlotState::CONNECTING && r.gen() == g + 1);   // direct attempt
  assert(r.core->stats().disconnects == 1 && r.core->stats().connectFails == 0);
  assert(r.core->slot().lastDisconnectReason == 0x3e && r.core->slot().failCount == 0);
  assert(g_eventCount == 1);
  expectEvent(0, EventType::SENSOR_DISCONNECTED, SlotState::FOUND, 0x3e);
}

static void testConnectFailInDiscovering() {
  {
    Rig r;                                             // before the pass: neutral, no event
    r.toConnecting();
    g_eventCount = 0;
    const uint16_t g = r.gen();
    r.connectedEv(g);
    r.failEv(g, 0x3e);
    r.port.connected = false;
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.port.gattPasses == 0);
    assert(r.core->stats().disconnects == 1 && r.core->slot().failCount == 0);
    assert(r.core->slot().lastDisconnectReason == 0x3e && r.core->stats().connectFails == 0);
    assert(g_eventCount == 0);
  }
  {
    Rig r;                                             // after a failed pass: hard fail
    r.port.gattOk = false;
    r.toConnecting();
    r.connectedEv(r.gen());
    r.update();
    assert(r.state() == SlotState::DISCOVERING);
    g_eventCount = 0;
    r.failEv(r.gen(), 0x3e);
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.core->slot().failCount == 1);
    assert(r.core->stats().disconnects == 1 && r.core->stats().connectFails == 0);
    assert(g_eventCount == 1);
    expectEvent(0, EventType::SENSOR_FAILED, SlotState::WAIT_SCAN);
  }
  {
    Rig r;                                             // remove pending: EMPTY
    r.toSubscribed();
    r.core->remove(r.now);
    r.failEv(r.gen(), 0x3e);
    r.update();
    assert(r.state() == SlotState::EMPTY && r.core->stats().disconnects == 1);
  }
  {
    Rig r;                                             // DISCOVERING, remove pending: EMPTY, no event
    r.port.gattOk = false;
    r.toConnecting();
    r.connectedEv(r.gen());
    r.update();
    assert(r.state() == SlotState::DISCOVERING);
    r.core->remove(r.now);
    g_eventCount = 0;
    r.failEv(r.gen(), 0x3e);
    r.port.connected = false;
    r.update();
    assert(r.state() == SlotState::EMPTY && r.core->stats().disconnects == 1);
    assert(r.core->slot().failCount == 0 && r.core->stats().connectFails == 0);
    assert(g_eventCount == 0);
    r.at(r.now + 40000);
    assert(r.state() == SlotState::EMPTY && r.port.connects == 1 && g_eventCount == 0);
  }
}

static void testDisconnectDirectAttemptThenSleepScan() {
  Rig r;
  r.toSubscribed();
  const uint16_t g = r.gen();
  const int startsBefore = r.port.discStarts;
  r.discEv(g, 0x208);
  r.update();                                          // one direct attempt, no scan first
  assert(r.state() == SlotState::CONNECTING && r.gen() == g + 1);
  assert(r.port.discStarts == startsBefore);
  assert(r.core->slot().lastDisconnectReason == 0x208 && r.core->stats().disconnects == 1);

  r.failEv(r.gen());
  r.update();                                          // then sleep scan
  assert(r.state() == SlotState::WAIT_SCAN);
  assert(r.core->slot().failCount == 1 && r.core->stats().connectFails == 1);
  r.at(r.now);
  assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW);

  const uint32_t failedAt = r.now;
  r.match(r.core->scan().epoch());
  r.update();
  assert(r.state() == SlotState::FOUND);               // held by the 1 s backoff
  r.at(failedAt + 999);
  assert(r.state() == SlotState::FOUND);
  r.at(failedAt + 1000);
  assert(r.state() == SlotState::CONNECTING);

  r.connectedEv(r.gen());
  r.update();
  assert(r.state() == SlotState::SUBSCRIBED && r.core->slot().failCount == 0);
}

static void testSleepRemainderClearedOnConnect() {
  Rig r;
  r.add();
  r.update();
  assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW);
  assert(r.port.lastDurationMs == BBS_SLEEP_SCAN_DURATION_MS);
  // Late match: stopForConnect remembers a tiny remainder.
  r.at(r.now + BBS_SLEEP_SCAN_DURATION_MS - 200);
  r.match(r.core->scan().epoch());
  r.update();
  assert(r.state() == SlotState::CONNECTING && r.port.discCancels == 1);
  r.connectedEv(r.gen());
  r.update();
  assert(r.state() == SlotState::SUBSCRIBED);

  // After a long SUBSCRIBED session, disconnect → direct fail → next sleep
  // window must be the full duration, not the 200 ms leftover.
  r.at(r.now + 60000);
  r.discEv(r.gen());
  r.update();
  assert(r.state() == SlotState::CONNECTING);
  const int startsBeforeFail = r.port.discStarts;
  r.failEv(r.gen());
  r.update();
  assert(r.state() == SlotState::WAIT_SCAN && r.core->slot().failCount == 1);
  // Slot backoff holds FOUND connects; WAIT_SCAN may start the sleep window at once.
  // Without clearRemainder that window would be 200 ms; with it, full duration.
  if (r.port.discStarts == startsBeforeFail) {
    r.at(r.now + 1000);
  }
  assert(r.port.discStarts == startsBeforeFail + 1);
  assert(r.port.lastDurationMs == BBS_SLEEP_SCAN_DURATION_MS);
  assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW);
}

static void testConnectLostBeforeOnConnect() {
  Rig r;
  r.toConnecting();
  r.discEv(r.gen(), 0x23e);
  r.update();
  assert(r.state() == SlotState::WAIT_SCAN && r.core->slot().failCount == 1);
}

static void testConnectErrorClassification() {
  const int transient[] = {bs::kHsEAlready, bs::kHsEBusy};
  for (int err : transient) {
    Rig r;
    r.port.connectOk = false;
    r.port.connectErr = err;
    r.add();
    r.update();
    r.match(r.core->scan().epoch());
    r.update();
    assert(r.state() == SlotState::FOUND && r.core->slot().failCount == 0);
    assert(r.core->stats().connectFails == 0 && r.port.connects == 1);
    const uint32_t t = r.now;
    r.at(t + 499);
    assert(r.port.connects == 1);
    r.port.connectOk = true;
    r.at(t + 500);
    assert(r.state() == SlotState::CONNECTING && r.port.connects == 2);
  }
  {
    Rig r;
    r.port.connectOk = false;
    r.port.connectErr = bs::kHsENotSynced;
    r.add();
    r.update();
    r.match(r.core->scan().epoch());
    r.update();
    assert(r.state() == SlotState::FOUND && r.core->slot().failCount == 0);
    const uint32_t t = r.now;
    r.at(t + BBS_SCAN_START_BACKOFF_MS - 1);
    assert(r.port.connects == 1);
    r.at(t + BBS_SCAN_START_BACKOFF_MS);
    assert(r.port.connects == 2);
  }
  {
    Rig r;
    r.port.connectOk = false;
    r.port.connectErr = 6;                             // BLE_HS_ENOMEM
    r.add();
    const uint32_t expected[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
    for (uint8_t i = 0; i < 7; ++i) {
      r.at(r.now + 40000);                             // past backoff and sleep interval
      assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW);
      r.match(r.core->scan().epoch());
      r.update();
      assert(r.state() == SlotState::WAIT_SCAN);
      assert(r.core->slot().failCount == i + 1 && r.core->stats().connectFails == i + 1u);
      assert(r.core->slot().holdMs == expected[i]);
    }
  }
}

static void testConnectWatchdogAndCancelling() {
  {
    Rig r;
    r.toConnecting();
    const uint32_t t = r.now;
    r.at(t + BBS_CONNECT_TIMEOUT_MS + 1999);
    assert(r.state() == SlotState::CONNECTING && r.port.cancelConnects == 0);
    r.at(t + BBS_CONNECT_TIMEOUT_MS + 2000);
    assert(r.state() == SlotState::CANCELLING && r.port.cancelConnects == 1);
    r.failEv(r.gen());
    r.update();                                        // watchdog cancel = hard fail
    assert(r.state() == SlotState::WAIT_SCAN && r.core->slot().failCount == 1);
    assert(r.core->stats().connectFails == 1 && r.core->slot().holdMs == 1000);
    assert(countEvents(EventType::SENSOR_FAILED) == 1);
  }
  {
    Rig r;                                             // CONNECTED while cancelling
    r.toConnecting();
    r.at(r.now + BBS_CONNECT_TIMEOUT_MS + 2000);
    assert(r.state() == SlotState::CANCELLING);
    r.connectedEv(r.gen());
    r.update();
    assert(r.state() == SlotState::CANCELLING && r.port.disconnects == 1);
    assert(r.port.gattPasses == 0);
    r.discEv(r.gen());
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.core->slot().failCount == 1);
    assert(r.core->stats().connectFails == 1 && r.core->stats().disconnects == 0);
    assert(countEvents(EventType::SENSOR_FAILED) == 1);
  }
  {
    Rig r;                                             // HOST_RESET during a watchdog cancel
    r.toConnecting();
    r.at(r.now + BBS_CONNECT_TIMEOUT_MS + 2000);
    assert(r.state() == SlotState::CANCELLING);
    g_eventCount = 0;
    r.port.connOn = false;
    assert(bs::pushHostReset(r.core->queue()));        // neutral: wait state, no fail
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.core->slot().failCount == 0);
    assert(r.core->stats().connectFails == 0 && r.core->slot().holdMs == 0);
    assert(g_eventCount == 1);
    expectEvent(0, EventType::HOST_RESET, SlotState::WAIT_SCAN, 0, SensorType{});
    r.failEv(r.gen());                                 // late outcome: no state to act on
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.core->slot().failCount == 0);
    assert(countEvents(EventType::SENSOR_FAILED) == 0);

    r.at(r.now + BBS_SCAN_START_BACKOFF_MS + 40000);   // flag cleared on the next attempt
    r.match(r.core->scan().epoch());
    r.update();
    assert(r.state() == SlotState::CONNECTING);
    const uint16_t g = r.gen();
    r.connectedEv(g);
    r.discEv(g, 0x08);                                 // link lost before the pass: neutral
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.core->slot().failCount == 0);
    assert(countEvents(EventType::SENSOR_FAILED) == 0);
    assert(countEvents(EventType::SENSOR_DISCONNECTED) == 0);
  }
}

static void testCancelSafety() {
  Rig r;
  r.toConnecting();
  r.at(r.now + BBS_CONNECT_TIMEOUT_MS + 2000);
  assert(r.state() == SlotState::CANCELLING);
  const uint32_t t = r.now;
  const uint16_t oldGen = r.gen();

  r.port.connOn = false;
  r.port.connected = false;
  r.at(t + BBS_CANCEL_SAFETY_MS - 1);
  assert(r.state() == SlotState::CANCELLING);
  r.port.connOn = true;                                // connect procedure still pending
  r.at(t + BBS_CANCEL_SAFETY_MS);
  assert(r.state() == SlotState::CANCELLING);
  r.port.connOn = false;
  r.port.connected = true;                             // link up
  r.at(t + BBS_CANCEL_SAFETY_MS + 1);
  assert(r.state() == SlotState::CANCELLING && r.core->stats().cancelSafetyExits == 0);

  r.port.connected = false;
  r.at(t + BBS_CANCEL_SAFETY_MS + 2);
  assert(r.state() == SlotState::WAIT_SCAN && r.core->stats().cancelSafetyExits == 1);
  assert(r.core->queue().takeResync());
  assert(r.core->slot().failCount == 1 && r.core->stats().connectFails == 1);   // watchdog cancel
  assert(countEvents(EventType::SENSOR_FAILED) == 1);

  r.at(r.now + 40000);
  r.match(r.core->scan().epoch());
  r.update();
  assert(r.state() == SlotState::CONNECTING && r.gen() == oldGen + 1);
  r.failEv(oldGen);                                    // late outcome of the old attempt
  r.connectedEv(oldGen);
  r.update();
  assert(r.state() == SlotState::CONNECTING);
}

static void testStoppingSafety() {
  {
    Rig r;                                             // cancel rc ∉ {0, EALREADY}
    r.port.discCancelRc = 3;
    r.add();
    r.update();
    r.match(r.core->scan().epoch());
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.port.connects == 0);
    assert(r.core->stats().cancelSafetyExits == 1);
    assert(r.core->queue().takeResync());
    assert(r.port.discStarts == 2);                    // owner went OFF; the rest resumes
  }
  {
    Rig r;                                             // EALREADY is fine
    r.port.discCancelRc = bs::kHsEAlready;
    r.add();
    r.update();
    r.match(r.core->scan().epoch());
    r.update();
    assert(r.state() == SlotState::CONNECTING && r.core->stats().cancelSafetyExits == 0);
  }
  {
    Rig r;                                             // disc stays active after cancel
    r.port.cancelLeavesDiscOn = true;
    r.add();
    r.update();
    r.match(r.core->scan().epoch());
    r.update();
    assert(r.state() == SlotState::STOPPING_FOR_CONNECT);
    const uint32_t t = r.now;
    r.at(t + BBS_STOPPING_SAFETY_MS - 1);
    assert(r.state() == SlotState::STOPPING_FOR_CONNECT && r.port.connects == 0);
    r.at(t + BBS_STOPPING_SAFETY_MS);
    assert(r.state() == SlotState::WAIT_SCAN && r.core->stats().cancelSafetyExits == 1);
    assert(r.core->scan().owner() == ScanOwner::OFF);  // restart refused: disc still active
    assert(r.core->stats().scanStartFails == 1);
  }
  {
    Rig r;                                             // disc goes idle late: connect then
    r.port.cancelLeavesDiscOn = true;
    r.add();
    r.update();
    r.match(r.core->scan().epoch());
    r.update();
    r.port.discOn = false;
    r.at(r.now + 10);
    assert(r.state() == SlotState::CONNECTING && r.core->stats().cancelSafetyExits == 0);
  }
  {
    Rig r;                                             // a pending connect blocks connect()
    r.add();
    r.update();
    r.match(r.core->scan().epoch());
    r.port.connOn = true;
    r.update();
    assert(r.state() == SlotState::STOPPING_FOR_CONNECT && r.port.connects == 0);
    r.port.connOn = false;
    r.at(r.now + 10);
    assert(r.state() == SlotState::CONNECTING && r.port.connects == 1);
  }
}

static void testScanController() {
  {
    Rig r;                                             // start conditions
    r.update();
    assert(r.port.discStarts == 0);                    // nothing configured
    r.add();
    r.port.connOn = true;
    r.update();
    assert(r.port.discStarts == 0);
    r.port.connOn = false;
    r.port.synced = false;
    r.update();
    assert(r.port.discStarts == 0);
    r.port.synced = true;
    r.at(r.now + BBS_SCAN_START_BACKOFF_MS - 1);
    assert(r.port.discStarts == 0);
    r.port.discStartRc = 2;
    r.at(r.now + 1);
    assert(r.port.discStarts == 1 && r.core->stats().scanStartFails == 1);
    assert(r.core->scan().owner() == ScanOwner::OFF);
    r.port.discStartRc = 0;
    r.at(r.now + BBS_SCAN_START_BACKOFF_MS - 1);
    assert(r.port.discStarts == 1);
    r.at(r.now + 1);
    assert(r.port.discStarts == 2 && r.core->stats().scansStarted == 1);
    assert(r.port.lastEpoch == 2);                     // every start bumps the epoch
  }
  {
    Rig r;                                             // epoch staleness and interval
    r.add();
    r.update();
    const uint32_t start = r.now;
    const uint16_t e = r.core->scan().epoch();
    r.match(static_cast<uint16_t>(e - 1));
    assert(bs::pushScanEnd(r.core->queue(), static_cast<uint16_t>(e + 1), 0));
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN);
    assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW);
    r.port.discOn = false;
    assert(bs::pushScanEnd(r.core->queue(), e, 0));
    r.match(e);                                        // after its own SCAN_END: stale
    r.at(start + BBS_SLEEP_SCAN_DURATION_MS);
    assert(r.core->scan().owner() == ScanOwner::OFF && r.state() == SlotState::WAIT_SCAN);
    r.at(start + BBS_SLEEP_SCAN_DURATION_MS + BBS_SLEEP_SCAN_INTERVAL_MS - 1);
    assert(r.port.discStarts == 1);
    r.at(start + BBS_SLEEP_SCAN_DURATION_MS + BBS_SLEEP_SCAN_INTERVAL_MS);
    assert(r.port.discStarts == 2 && r.core->scan().epoch() == e + 1);
    r.match(r.core->scan().epoch(), kOtherMac);        // gapScanCb filters; update() rechecks
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN);
  }
  {
    Rig r;                                             // missing SCAN_END
    r.add();
    r.update();
    const uint32_t end = r.now + BBS_SLEEP_SCAN_DURATION_MS + BBS_SCAN_END_GRACE_MS;
    r.port.discOn = false;
    r.at(end - 1);
    assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW);
    r.port.discOn = true;
    r.at(end);
    assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW);   // disc still active
    r.port.discOn = false;
    r.at(end + 1);
    assert(r.core->scan().owner() == ScanOwner::OFF);
  }
  {
    Rig r;                                             // interrupted window resumes its rest
    r.add();
    r.update();
    const uint32_t start = r.now;
    r.now = start + 1000;
    r.match(r.core->scan().epoch());
    r.update();
    assert(r.state() == SlotState::CONNECTING);
    const uint16_t stopped = r.core->scan().epoch();
    r.match(stopped);                                  // queued before the cancel: stale now
    r.failEv(r.gen());
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.port.discStarts == 2);
    assert(r.port.lastDurationMs == BBS_SLEEP_SCAN_DURATION_MS - 1000);
    assert(r.core->scan().epoch() == stopped + 1);
  }
  {
    Rig r;                                             // !needScan cancels the window
    r.add();
    r.update();
    r.core->remove(r.now);
    r.update();
    assert(r.port.discCancels == 1 && r.core->scan().owner() == ScanOwner::OFF);
  }
  {
    Rig r;                                             // epoch skips 0 on wrap
    r.add();
    for (uint32_t i = 0; i < 65536u + 2u; ++i) {
      r.update();
      assert(r.core->scan().epoch() != 0);
      assert(bs::pushScanEnd(r.core->queue(), r.core->scan().epoch(), 0));
      r.port.discOn = false;
      r.update();
      r.now += BBS_SLEEP_SCAN_DURATION_MS + BBS_SLEEP_SCAN_INTERVAL_MS;
    }
    assert(r.port.discStarts == 65538);
    assert(r.core->scan().epoch() == 3);
  }
}

static void testGattWatchdogUnit() {
  GattWatchdog wd;
  assert(!wd.onTimer());                               // never armed
  const uint32_t g1 = wd.arm(5);
  assert(wd.connHandle() == 5);
  wd.disarm();
  assert(!wd.onTimer() && !wd.fired(g1));              // fires after the stop: ignored
  const uint32_t g2 = wd.arm(9);
  assert(g2 != g1);
  assert(wd.onTimer() && wd.connHandle() == 9);
  wd.disarm();
  assert(wd.fired(g2) && !wd.fired(g1));
  const uint32_t g3 = wd.arm(9);
  wd.disarm();
  assert(!wd.fired(g3));
}

static void testGattAbortAndFailure() {
  {
    Rig r;                                             // budget abort during the pass
    r.port.gattOk = false;
    r.port.duringGatt = [&r] { assert(r.core->watchdog().onTimer()); };
    r.toConnecting();
    r.connectedEv(r.gen());
    r.update();
    assert(r.state() == SlotState::DISCOVERING);
    assert(r.core->stats().gattBudgetAborts == 1 && r.port.disconnects == 1);
    r.update();
    assert(r.port.gattPasses == 1);                    // no second pass on the same link
    r.discEv(r.gen(), 0x216);
    r.update();                                        // hard fail, link was up: no connectFails
    assert(r.state() == SlotState::WAIT_SCAN && r.core->stats().disconnects == 1);
    assert(r.core->slot().failCount == 1 && r.core->stats().connectFails == 0);
    assert(r.core->slot().holdMs == 1000 && r.core->slot().lastDisconnectReason == 0x216);
  }
  {
    Rig r;                                             // pass "ok" but the timer cut it
    r.port.duringGatt = [&r] { assert(r.core->watchdog().onTimer()); };
    r.toConnecting();
    r.connectedEv(r.gen());
    r.update();
    assert(r.state() == SlotState::DISCOVERING && r.core->stats().gattBudgetAborts == 1);
    assert(countEvents(EventType::SENSOR_CONNECTED) == 0);
    r.discEv(r.gen(), 0x216);
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.core->slot().failCount == 1);
  }
  {
    Rig r;                                             // plain failure: no abort count
    r.port.gattOk = false;
    r.toConnecting();
    r.connectedEv(r.gen());
    r.update();
    assert(r.state() == SlotState::DISCOVERING && r.port.disconnects == 1);
    assert(r.core->stats().gattBudgetAborts == 0);
    assert(!r.core->watchdog().onTimer());             // disarmed after the pass
    r.discEv(r.gen());
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.core->slot().failCount == 1);
    assert(r.core->stats().connectFails == 0 && countEvents(EventType::SENSOR_FAILED) == 1);
    // Backoff holds the next connect; a good pass resets failCount.
    const uint32_t failedAt = r.now;
    r.port.gattOk = true;
    assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW);
    r.match(r.core->scan().epoch());
    r.update();
    assert(r.state() == SlotState::FOUND);
    r.at(failedAt + 999);
    assert(r.state() == SlotState::FOUND);
    r.at(failedAt + 1000);
    assert(r.state() == SlotState::CONNECTING);
    r.connectedEv(r.gen());
    r.update();
    assert(r.state() == SlotState::SUBSCRIBED && r.core->slot().failCount == 0);
  }
}

static void testGattFailBackoff() {
  Rig r;                                               // same 1 s → 30 s table as connect errors
  r.port.gattOk = false;
  r.add();
  const uint32_t expected[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
  for (uint8_t i = 0; i < 7; ++i) {
    r.at(r.now + 40000);                               // past backoff and sleep interval
    assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW);
    r.match(r.core->scan().epoch());
    r.update();
    assert(r.state() == SlotState::CONNECTING);
    r.connectedEv(r.gen());
    r.update();
    assert(r.state() == SlotState::DISCOVERING && r.port.gattPasses == i + 1);
    r.discEv(r.gen(), 0x216);
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.core->slot().failCount == i + 1);
    assert(r.core->slot().holdMs == expected[i] && r.core->stats().connectFails == 0);
    assert(r.core->stats().disconnects == i + 1u && countEvents(EventType::SENSOR_FAILED) == i + 1);
  }
}

static void testHostReset() {
  {
    Rig r;
    r.add();
    r.update();
    assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW);
    const uint16_t e = r.core->scan().epoch();
    assert(bs::pushScanEnd(r.core->queue(), e, 0));    // host reset delivers SCAN_END first
    assert(bs::pushHostReset(r.core->queue()));
    r.port.discOn = false;
    r.update();
    assert(r.core->scan().owner() == ScanOwner::OFF && r.port.discStarts == 1);
    assert(g_eventCount == 1 && r.core->stats().hostResets == 1);
    expectEvent(0, EventType::HOST_RESET, SlotState::WAIT_SCAN, 0, SensorType{});
  }
  {
    Rig r;                                             // HOST_RESET alone ends the window
    r.add();
    r.update();
    assert(bs::pushHostReset(r.core->queue()));
    r.port.discOn = false;
    r.update();
    assert(r.core->scan().owner() == ScanOwner::OFF);
  }
  {
    Rig r;
    r.toConnecting();
    r.port.connOn = false;
    assert(bs::pushHostReset(r.core->queue()));
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.core->stats().hostResets == 1);
  }
  {
    Rig r;
    r.toSubscribed();
    assert(bs::pushHostReset(r.core->queue()));
    r.update();
    assert(r.state() == SlotState::SUBSCRIBED);        // its DISCONNECTED ends the link
  }
  {
    Rig r;
    r.toConnecting();
    r.core->remove(r.now);
    assert(r.state() == SlotState::CANCELLING);
    assert(bs::pushHostReset(r.core->queue()));
    r.update();
    assert(r.state() == SlotState::EMPTY);
  }
  {
    Rig r;                                             // DISCOVERING after a failed pass: neutral
    r.port.gattOk = false;
    r.toConnecting();
    r.connectedEv(r.gen());
    r.update();
    assert(r.state() == SlotState::DISCOVERING && r.core->slot().failPending);
    g_eventCount = 0;
    r.port.connected = false;
    assert(bs::pushHostReset(r.core->queue()));
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.core->slot().failCount == 0);
    assert(r.core->slot().holdMs == 0 && r.core->stats().connectFails == 0);
    assert(r.core->stats().disconnects == 0);
    assert(g_eventCount == 1);
    expectEvent(0, EventType::HOST_RESET, SlotState::WAIT_SCAN, 0, SensorType{});
    r.discEv(r.gen(), 0x16);                           // late outcome: no state to act on
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.core->slot().failCount == 0);
    assert(r.core->stats().disconnects == 0 && g_eventCount == 1);

    r.at(r.now + BBS_SCAN_START_BACKOFF_MS + 40000);   // flags cleared on the next attempt
    r.match(r.core->scan().epoch());
    r.update();
    assert(r.state() == SlotState::CONNECTING);
    const uint16_t g = r.gen();
    r.connectedEv(g);
    r.discEv(g, 0x08);                                 // link lost before the pass: neutral
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.core->slot().failCount == 0);
    assert(countEvents(EventType::SENSOR_FAILED) == 0);
  }
}

static void fillQueueToResync(Core& core) {
  for (uint8_t i = 0; i < BBS_EVENT_QUEUE_DEPTH; ++i) bs::pushCentralConnect(core.queue());
  assert(!bs::pushCentralConnect(core.queue()));
}

static void testResync() {
  {
    Rig r;                                             // connected outside DISCOVERING/SUBSCRIBED
    r.toConnecting();
    r.port.connected = true;                           // the orphan sweep never does this
    fillQueueToResync(*r.core);
    r.update();
    assert(r.port.disconnects == 1 && r.state() == SlotState::CONNECTING);
  }
  {
    Rig r;                                             // CONNECTING with nothing pending
    r.toConnecting();
    r.port.connOn = false;
    r.port.connected = false;
    fillQueueToResync(*r.core);
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN);
  }
  {
    Rig r;                                             // CONNECTING, connect still pending
    r.toConnecting();
    fillQueueToResync(*r.core);
    r.update();
    assert(r.state() == SlotState::CONNECTING);
  }
  {
    Rig r;                                             // lost DISCONNECTED from SUBSCRIBED
    r.toSubscribed();
    g_eventCount = 0;                                  // drop FOUND/CONNECTED from setup
    r.port.connected = false;
    fillQueueToResync(*r.core);
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN);
    assert(r.core->stats().disconnects == 1);
    assert(g_eventCount == 1);
    expectEvent(0, EventType::SENSOR_DISCONNECTED, SlotState::WAIT_SCAN, 0);
  }
  {
    Rig r;                                             // lost link after a failed pass: hard fail
    r.port.gattOk = false;
    r.toConnecting();
    r.connectedEv(r.gen());
    r.update();
    assert(r.state() == SlotState::DISCOVERING);
    g_eventCount = 0;
    r.port.connected = false;
    fillQueueToResync(*r.core);
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.core->slot().failCount == 1);
    assert(r.core->slot().holdMs == 1000 && r.core->stats().disconnects == 1);
    assert(r.core->stats().connectFails == 0);
    assert(g_eventCount == 1);
    expectEvent(0, EventType::SENSOR_FAILED, SlotState::WAIT_SCAN);
  }
  {
    Rig r;                                             // pass skipped by a replace: neutral
    r.port.gattOk = false;
    r.toConnecting();
    r.connectedEv(r.gen());
    r.update();
    r.replace();
    g_eventCount = 0;
    r.port.connected = false;
    fillQueueToResync(*r.core);
    r.update();                                        // direct attempt to the new MAC
    assert(r.state() == SlotState::CONNECTING && r.core->slot().failCount == 0);
    assert(r.core->stats().disconnects == 1 && g_eventCount == 0);
  }
  {
    Rig r;                                             // failed pass, remove pending: EMPTY
    r.port.gattOk = false;
    r.toConnecting();
    r.connectedEv(r.gen());
    r.update();
    r.core->remove(r.now);
    g_eventCount = 0;
    r.port.connected = false;
    fillQueueToResync(*r.core);
    r.update();
    assert(r.state() == SlotState::EMPTY && r.core->slot().failCount == 0);
    assert(g_eventCount == 0);
  }
  {
    Rig r;                                             // scan owner OFF if disc idle
    r.add();
    r.update();
    assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW);
    r.port.discOn = false;
    r.core->queue().requestResync();
    r.port.connOn = true;                              // keep advance() from restarting
    r.update();
    assert(r.core->scan().owner() == ScanOwner::OFF);
  }
}

static void testRemoveFromEveryState() {
  {
    Rig r;
    r.core->remove(r.now);                             // EMPTY: no-op
    assert(r.state() == SlotState::EMPTY);
    r.add();
    r.core->remove(r.now);                             // WAIT_SCAN
    assert(r.state() == SlotState::EMPTY);
  }
  {
    Rig r;                                             // FOUND (held)
    r.port.connectOk = false;
    r.port.connectErr = bs::kHsEBusy;
    r.add();
    r.update();
    r.match(r.core->scan().epoch());
    r.update();
    assert(r.state() == SlotState::FOUND);
    r.core->remove(r.now);
    assert(r.state() == SlotState::EMPTY);
    r.at(r.now + 1000);
    assert(r.port.connects == 1);
  }
  {
    Rig r;                                             // STOPPING_FOR_CONNECT
    r.port.cancelLeavesDiscOn = true;
    r.add();
    r.update();
    r.match(r.core->scan().epoch());
    r.update();
    assert(r.state() == SlotState::STOPPING_FOR_CONNECT);
    r.core->remove(r.now);
    assert(r.state() == SlotState::EMPTY && r.core->scan().owner() == ScanOwner::OFF);
    r.port.discOn = false;
    r.update();
    assert(r.port.connects == 0);
  }
  const bool viaFail[] = {true, false};
  for (bool fail : viaFail) {
    Rig r;                                             // CONNECTING → CANCELLING → EMPTY
    r.toConnecting();
    r.core->remove(r.now);
    assert(r.state() == SlotState::CANCELLING && r.port.cancelConnects == 1);
    if (fail) {
      r.failEv(r.gen());
    } else {
      r.connectedEv(r.gen());
      r.update();
      assert(r.state() == SlotState::CANCELLING && r.port.disconnects == 1);
      r.discEv(r.gen());
    }
    r.update();
    assert(r.state() == SlotState::EMPTY && r.core->slot().failCount == 0);
  }
  {
    Rig r;                                             // CANCELLING (watchdog), then remove
    r.toConnecting();
    r.at(r.now + BBS_CONNECT_TIMEOUT_MS + 2000);
    r.core->remove(r.now);
    assert(r.state() == SlotState::CANCELLING);
    r.failEv(r.gen());
    r.update();
    assert(r.state() == SlotState::EMPTY && r.core->stats().connectFails == 0);
    assert(countEvents(EventType::SENSOR_FAILED) == 0);
  }
  for (bool watchdog : viaFail) {
    Rig r;                                             // CANCELLING safety exit while removing
    r.toConnecting();
    if (watchdog) r.at(r.now + BBS_CONNECT_TIMEOUT_MS + 2000);
    r.core->remove(r.now);
    r.port.connOn = false;
    r.at(r.now + BBS_CANCEL_SAFETY_MS);
    assert(r.state() == SlotState::EMPTY && r.core->stats().cancelSafetyExits == 1);
    assert(r.core->stats().connectFails == 0 && countEvents(EventType::SENSOR_FAILED) == 0);
  }
  {
    Rig r;                                             // DISCOVERING (failed pass)
    r.port.gattOk = false;
    r.toConnecting();
    r.connectedEv(r.gen());
    r.update();
    assert(r.state() == SlotState::DISCOVERING);
    r.core->remove(r.now);
    assert(r.state() == SlotState::DISCOVERING && r.port.disconnects == 2);
    r.discEv(r.gen());
    r.update();
    assert(r.state() == SlotState::EMPTY && countEvents(EventType::SENSOR_FAILED) == 0);
    assert(countEvents(EventType::SENSOR_DISCONNECTED) == 0);   // never SENSOR_CONNECTED
  }
  {
    Rig r;                                             // SUBSCRIBED
    r.toSubscribed();
    const uint16_t g = r.gen();
    r.core->remove(r.now);
    assert(r.state() == SlotState::SUBSCRIBED && r.port.disconnects == 1);
    r.discEv(g);
    r.update();
    assert(r.state() == SlotState::EMPTY && r.port.connects == 1);
    assert(!r.core->scanFilterMatch(kMac));
    r.add();                                           // attemptGen is never reused
    r.update();
    r.match(r.core->scan().epoch());
    r.update();
    assert(r.state() == SlotState::CONNECTING && r.gen() == g + 1);
    r.connectedEv(g);
    r.update();
    assert(r.state() == SlotState::CONNECTING);
  }
}

static void testOrphanSweep() {
  Rig r;
  r.add();
  r.port.connected = true;
  r.port.connOn = true;                                // no scan start, stay in WAIT_SCAN
  r.update();
  assert(r.port.disconnects == 1);
  r.port.connOn = false;
  r.port.connected = false;
  r.update();
  r.match(r.core->scan().epoch());
  r.update();
  assert(r.state() == SlotState::CONNECTING);
  r.port.connected = true;                             // NimBLE: connected before onConnect
  r.update();
  r.update();
  assert(r.port.disconnects == 1);
}

static void testDrainBound() {
  Rig r;
  r.toConnecting();
  for (uint8_t i = 0; i < BBS_EVENT_QUEUE_DEPTH - 1; ++i) bs::pushCentralConnect(r.core->queue());
  r.connectedEv(r.gen());
  assert(r.core->queue().size() == BBS_EVENT_QUEUE_DEPTH);
  r.update();
  assert(r.core->queue().size() == 0 && r.state() == SlotState::SUBSCRIBED);
}

static void testLifecycleEvents() {
  {
    Rig r;                                             // found, connected, lost, retry fails
    r.toSubscribed();
    assert(g_eventCount == 2);
    expectEvent(0, EventType::SENSOR_FOUND, SlotState::FOUND);
    expectEvent(1, EventType::SENSOR_CONNECTED, SlotState::SUBSCRIBED);
    r.discEv(r.gen(), 0x208);
    r.update();
    assert(g_eventCount == 3);
    expectEvent(2, EventType::SENSOR_DISCONNECTED, SlotState::FOUND, 0x208);
    r.failEv(r.gen());
    r.update();
    assert(g_eventCount == 4);
    expectEvent(3, EventType::SENSOR_FAILED, SlotState::WAIT_SCAN);
  }
  {
    Rig r;                                             // link lost before onConnect: fail only
    r.toConnecting();
    r.discEv(r.gen(), 0x23e);
    r.update();
    assert(g_eventCount == 2);
    expectEvent(1, EventType::SENSOR_FAILED, SlotState::WAIT_SCAN);
  }
  {
    Rig r;                                             // hard connect() error; transient: none
    r.port.connectOk = false;
    r.port.connectErr = bs::kHsEBusy;
    r.add();
    r.update();
    r.match(r.core->scan().epoch());
    r.update();
    assert(g_eventCount == 1 && r.state() == SlotState::FOUND);
    r.port.connectErr = 6;                             // BLE_HS_ENOMEM
    r.at(r.now + 500);
    assert(g_eventCount == 2);
    expectEvent(1, EventType::SENSOR_FAILED, SlotState::WAIT_SCAN);
  }
  {
    Rig r;                                             // watchdog cancel outcome: failed
    r.toConnecting();
    r.at(r.now + BBS_CONNECT_TIMEOUT_MS + 2000);
    r.failEv(r.gen());
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && g_eventCount == 2);
    expectEvent(1, EventType::SENSOR_FAILED, SlotState::WAIT_SCAN);
  }
  {
    Rig r;                                             // failed GATT pass: failed only
    r.port.gattOk = false;
    r.toConnecting();
    r.connectedEv(r.gen());
    r.update();
    assert(g_eventCount == 1);
    r.discEv(r.gen(), 0x216);
    r.update();
    assert(g_eventCount == 2);
    expectEvent(1, EventType::SENSOR_FAILED, SlotState::WAIT_SCAN);
  }
  {
    Rig r;                                             // remove-driven teardown keeps the type
    r.toSubscribed();
    r.core->remove(r.now);
    r.discEv(r.gen(), 0x16);
    r.update();
    assert(g_eventCount == 3 && countEvents(EventType::SENSOR_DISCONNECTED) == 1);
    expectEvent(2, EventType::SENSOR_DISCONNECTED, SlotState::EMPTY, 0x16);
  }
  {
    Rig r;                                             // HOST_RESET while subscribed: only HOST_RESET
    r.toSubscribed();
    assert(bs::pushHostReset(r.core->queue()));
    r.update();
    assert(g_eventCount == 3);
    expectEvent(2, EventType::HOST_RESET, SlotState::SUBSCRIBED, 0, SensorType{});
  }
}

static Core* g_removeCore = nullptr;
static EventType g_removeOn = EventType::SENSOR_FAILED;
static uint32_t g_removeNow = 0;

static void onEventRemoving(const EventInfo& info) {
  onEvent(info);
  if (info.type == g_removeOn) g_removeCore->remove(g_removeNow);
}

// emit() runs after the state change, so the app may remove() from the callback.
static void testRemoveFromCallback() {
  {
    Rig r;                                             // SENSOR_FAILED (watchdog cancel)
    r.core->setEventCallback(onEventRemoving);
    g_removeCore = r.core.get();
    g_removeOn = EventType::SENSOR_FAILED;
    r.toConnecting();
    r.at(r.now + BBS_CONNECT_TIMEOUT_MS + 2000);
    g_removeNow = r.now;
    r.failEv(r.gen());
    r.update();
    expectEvent(1, EventType::SENSOR_FAILED, SlotState::WAIT_SCAN);
    assert(r.state() == SlotState::EMPTY && !r.core->scanFilterMatch(kMac));
    assert(r.core->scan().owner() == ScanOwner::OFF);
    r.at(r.now + 40000);
    assert(r.state() == SlotState::EMPTY && r.port.connects == 1 && g_eventCount == 2);
  }
  {
    Rig r;                                             // SENSOR_DISCONNECTED (direct retry pending)
    r.core->setEventCallback(onEventRemoving);
    g_removeCore = r.core.get();
    g_removeOn = EventType::SENSOR_DISCONNECTED;
    r.toSubscribed();
    g_removeNow = r.now;
    r.discEv(r.gen(), 0x208);
    r.update();
    expectEvent(2, EventType::SENSOR_DISCONNECTED, SlotState::FOUND, 0x208);
    assert(r.state() == SlotState::EMPTY && r.port.connects == 1);
    assert(r.core->slot().failCount == 0 && !r.core->slot().directRetry);
    r.at(r.now + 40000);
    assert(r.state() == SlotState::EMPTY && r.port.connects == 1 && g_eventCount == 3);
  }
  {
    Rig r;                                             // SENSOR_FOUND: no connect
    r.core->setEventCallback(onEventRemoving);
    g_removeCore = r.core.get();
    g_removeOn = EventType::SENSOR_FOUND;
    r.add();
    r.update();
    g_removeNow = r.now;
    r.match(r.core->scan().epoch());
    r.update();
    expectEvent(0, EventType::SENSOR_FOUND, SlotState::FOUND);
    assert(r.state() == SlotState::EMPTY && r.port.connects == 0);
    assert(r.core->scan().owner() == ScanOwner::OFF && !r.core->scanFilterMatch(kMac));
    r.at(r.now + 40000);
    assert(r.state() == SlotState::EMPTY && r.port.connects == 0 && g_eventCount == 1);
  }
  {
    Rig r;                                             // SENSOR_CONNECTED: teardown, no retry
    r.core->setEventCallback(onEventRemoving);
    g_removeCore = r.core.get();
    g_removeOn = EventType::SENSOR_CONNECTED;
    r.toConnecting();
    g_removeNow = r.now;
    r.connectedEv(r.gen());
    r.update();
    expectEvent(1, EventType::SENSOR_CONNECTED, SlotState::SUBSCRIBED);
    assert(r.state() == SlotState::SUBSCRIBED && r.port.disconnects == 1);
    r.discEv(r.gen(), 0x16);
    r.update();
    expectEvent(2, EventType::SENSOR_DISCONNECTED, SlotState::EMPTY, 0x16);
    assert(r.state() == SlotState::EMPTY && r.port.connects == 1);
    r.at(r.now + 40000);
    assert(r.state() == SlotState::EMPTY && r.port.connects == 1 && g_eventCount == 3);
  }
  {
    Rig r;                                             // HOST_RESET mid-attempt: late outcome ignored
    r.core->setEventCallback(onEventRemoving);
    g_removeCore = r.core.get();
    g_removeOn = EventType::HOST_RESET;
    r.toConnecting();
    const uint16_t g = r.gen();
    g_removeNow = r.now;
    r.port.connOn = false;
    assert(bs::pushHostReset(r.core->queue()));
    r.update();
    expectEvent(1, EventType::HOST_RESET, SlotState::WAIT_SCAN, 0, SensorType{});
    assert(r.state() == SlotState::EMPTY && !r.core->scanFilterMatch(kMac));
    r.failEv(g);
    r.update();
    r.at(r.now + 40000);
    assert(r.state() == SlotState::EMPTY && r.port.connects == 1 && g_eventCount == 2);
    assert(r.core->slot().failCount == 0 && r.core->stats().connectFails == 0);
  }
  {
    Rig r;                                             // HOST_RESET on a link: EMPTY on DISCONNECTED
    r.core->setEventCallback(onEventRemoving);
    g_removeCore = r.core.get();
    g_removeOn = EventType::HOST_RESET;
    r.toSubscribed();
    g_removeNow = r.now;
    assert(bs::pushHostReset(r.core->queue()));
    r.update();
    expectEvent(2, EventType::HOST_RESET, SlotState::SUBSCRIBED, 0, SensorType{});
    assert(r.state() == SlotState::SUBSCRIBED && r.port.disconnects == 1);
    r.discEv(r.gen(), 0x16);
    r.update();
    expectEvent(3, EventType::SENSOR_DISCONNECTED, SlotState::EMPTY, 0x16);
    assert(r.state() == SlotState::EMPTY && r.port.connects == 1);
    r.at(r.now + 40000);
    assert(r.state() == SlotState::EMPTY && r.port.connects == 1 && g_eventCount == 4);
  }
}

static Rig* g_replaceRig = nullptr;
static EventType g_replaceOn = EventType::SENSOR_FAILED;
static bool g_replaceArmed = false;

static void onEventReplacing(const EventInfo& info) {
  onEvent(info);
  if (g_replaceArmed && info.type == g_replaceOn) {
    g_replaceArmed = false;
    g_replaceRig->replace();
  }
}

// After a replace from the callback: one direct attempt to the new MAC; its
// fail goes to the sleep scan.
static void expectOneDirectAttempt(Rig& r, uint16_t oldGen, int startsBefore) {
  assert(!g_replaceArmed);
  assert(r.state() == SlotState::CONNECTING && r.gen() == oldGen + 1);
  assert(r.port.connects == 2 && r.port.lastAddrType == 1);
  assert(r.port.discStarts == startsBefore && !r.core->slot().directRetry);
  assert(r.core->slot().failCount == 0);
  r.failEv(r.gen());
  r.update();
  assert(r.state() == SlotState::WAIT_SCAN && r.core->slot().failCount == 1);
  r.at(r.now + 40000);
  assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW && r.port.connects == 2);
}

static void testReplaceFromCallback() {
  {
    Rig r;                                             // SENSOR_DISCONNECTED (direct retry pending)
    r.core->setEventCallback(onEventReplacing);
    g_replaceRig = &r;
    g_replaceOn = EventType::SENSOR_DISCONNECTED;
    g_replaceArmed = true;
    r.toSubscribed();
    const uint16_t g = r.gen();
    const int startsBefore = r.port.discStarts;
    r.discEv(g, 0x208);
    r.update();
    expectEvent(2, EventType::SENSOR_DISCONNECTED, SlotState::FOUND, 0x208);
    expectOneDirectAttempt(r, g, startsBefore);
  }
  {
    Rig r;                                             // SENSOR_FAILED (backoff pending)
    r.core->setEventCallback(onEventReplacing);
    g_replaceRig = &r;
    g_replaceOn = EventType::SENSOR_FAILED;
    g_replaceArmed = true;
    r.toConnecting();
    const uint16_t g = r.gen();
    const int startsBefore = r.port.discStarts;
    r.failEv(g);
    r.update();
    expectEvent(1, EventType::SENSOR_FAILED, SlotState::WAIT_SCAN);
    assert(r.core->slot().holdMs == 0);
    expectOneDirectAttempt(r, g, startsBefore);
  }
}

static void testReplace() {
  {
    Rig r;                                             // WAIT_SCAN: direct attempt, no new scan
    r.add();
    r.update();
    assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW);
    r.replace();
    assert(r.state() == SlotState::FOUND);
    r.update();
    assert(r.state() == SlotState::CONNECTING && r.port.connects == 1);
    assert(r.port.lastAddrType == 1 && r.port.discCancels == 1 && r.port.discStarts == 1);
  }
  {
    Rig r;                                             // WAIT_SCAN in backoff: backoff cleared
    r.port.connectOk = false;
    r.port.connectErr = 6;
    r.add();
    r.update();
    r.match(r.core->scan().epoch());
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.core->slot().failCount == 1);
    r.port.connectOk = true;
    r.replace();
    r.update();
    assert(r.state() == SlotState::CONNECTING && r.port.connects == 2);
  }
  {
    Rig r;                                             // FOUND (held): hold cleared
    r.port.connectOk = false;
    r.port.connectErr = bs::kHsEBusy;
    r.add();
    r.update();
    r.match(r.core->scan().epoch());
    r.update();
    assert(r.state() == SlotState::FOUND);
    r.port.connectOk = true;
    r.replace();
    r.update();
    assert(r.state() == SlotState::CONNECTING && r.port.connects == 2);
  }
  {
    Rig r;                                             // STOPPING_FOR_CONNECT keeps going
    r.port.cancelLeavesDiscOn = true;
    r.add();
    r.update();
    r.match(r.core->scan().epoch());
    r.update();
    assert(r.state() == SlotState::STOPPING_FOR_CONNECT);
    r.replace();
    assert(r.state() == SlotState::STOPPING_FOR_CONNECT);
    r.port.discOn = false;
    r.at(r.now + 10);
    assert(r.state() == SlotState::CONNECTING && r.port.lastAddrType == 1);
  }
  const bool viaRemove[] = {false, true};
  for (bool removeFirst : viaRemove) {
    Rig r;                                             // CONNECTING (or remove pending) → cancel
    r.toConnecting();
    const uint16_t g = r.gen();
    if (removeFirst) r.core->remove(r.now);
    r.replace();
    assert(r.state() == SlotState::CANCELLING && r.port.cancelConnects == 1);
    r.failEv(g);
    r.update();
    assert(r.state() == SlotState::CONNECTING && r.gen() == g + 1 && r.port.connects == 2);
    assert(countEvents(EventType::SENSOR_FAILED) == 0);
  }
  {
    Rig r;                                             // CANCELLING (watchdog)
    r.toConnecting();
    r.at(r.now + BBS_CONNECT_TIMEOUT_MS + 2000);
    r.replace();
    assert(r.state() == SlotState::CANCELLING && r.port.cancelConnects == 1);
    r.connectedEv(r.gen());
    r.update();
    assert(r.port.disconnects == 1 && r.port.gattPasses == 0);
    r.discEv(r.gen());
    r.update();
    assert(r.state() == SlotState::CONNECTING && r.port.connects == 2);
    assert(r.core->slot().failCount == 0 && r.core->stats().connectFails == 0);
    assert(countEvents(EventType::SENSOR_FAILED) == 0);
  }
  {
    Rig r;                                             // DISCOVERING (failed pass)
    r.port.gattOk = false;
    r.toConnecting();
    r.connectedEv(r.gen());
    r.update();
    assert(r.state() == SlotState::DISCOVERING && r.port.disconnects == 1);
    r.port.gattOk = true;
    r.replace();
    assert(r.port.disconnects == 2);
    r.update();
    assert(r.state() == SlotState::DISCOVERING && r.port.gattPasses == 1);
    r.discEv(r.gen());
    r.update();
    assert(r.state() == SlotState::CONNECTING && r.port.connects == 2);
    assert(r.core->slot().failCount == 0 && countEvents(EventType::SENSOR_FAILED) == 0);
    assert(countEvents(EventType::SENSOR_DISCONNECTED) == 0);
  }
  for (bool removeFirst : viaRemove) {
    Rig r;                                             // SUBSCRIBED (or remove pending)
    r.toSubscribed();
    const int startsBefore = r.port.discStarts;
    if (removeFirst) r.core->remove(r.now);
    r.replace();
    assert(r.state() == SlotState::SUBSCRIBED && r.port.disconnects >= 1);
    r.discEv(r.gen(), 0x16);
    r.update();
    assert(r.state() == SlotState::CONNECTING && r.port.connects == 2);
    assert(r.port.discStarts == startsBefore && r.port.lastAddrType == 1);
    expectEvent(2, EventType::SENSOR_DISCONNECTED, SlotState::FOUND, 0x16);
    r.connectedEv(r.gen());
    r.update();
    assert(r.state() == SlotState::SUBSCRIBED && countEvents(EventType::SENSOR_CONNECTED) == 2);
  }
  {
    Rig r;                                             // same identity again: controlled reconnect
    r.toSubscribed();
    assert(r.core->addByMac(SensorType::HEART_RATE, kMacStr, 0, "HRM", r.now));
    assert(r.port.disconnects == 1);
    r.discEv(r.gen());
    r.update();
    assert(r.state() == SlotState::CONNECTING && r.port.connects == 2);
  }
  {
    Rig r;                                             // another type: no teardown
    r.toSubscribed();
    assert(!r.core->addByMac(SensorType::SPEED_CADENCE, kOtherMacStr, 0, nullptr, r.now));
    assert(r.state() == SlotState::SUBSCRIBED && r.port.disconnects == 0);
    assert(r.core->scanFilterMatch(kMac));
  }
}

int main() {
  testAddByMac();
  testHappyPath();
  testScanMatchAddrTypeWins();
  testAttemptGenStaleness();
  testConnectFailInDiscovering();
  testDisconnectDirectAttemptThenSleepScan();
  testSleepRemainderClearedOnConnect();
  testConnectLostBeforeOnConnect();
  testConnectErrorClassification();
  testConnectWatchdogAndCancelling();
  testCancelSafety();
  testStoppingSafety();
  testScanController();
  testGattWatchdogUnit();
  testGattAbortAndFailure();
  testGattFailBackoff();
  testHostReset();
  testResync();
  testRemoveFromEveryState();
  testOrphanSweep();
  testDrainBound();
  testLifecycleEvents();
  testRemoveFromCallback();
  testReplaceFromCallback();
  testReplace();
  return 0;
}
