#include "BBSCore.h"
#include "BBSEventPush.h"

#include <atomic>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <thread>

using BikeSensor::DiscoveredSensor;
using BikeSensor::EventInfo;
using BikeSensor::EventType;
using BikeSensor::FinishReason;
using BikeSensor::Mode;
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
static const char kCscMacStr[] = "aa:bb:cc:dd:ee:03";
static const uint8_t kCscMac[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x03};

static const uint8_t kHr = static_cast<uint8_t>(bs::slotIndex(SensorType::HEART_RATE));
static const uint8_t kCsc = static_cast<uint8_t>(bs::slotIndex(SensorType::SPEED_CADENCE));

struct FakePort final : BlePort {
  bool synced = true, discOn = false, connOn = false;
  bool linkUp[bs::kSlotCount] = {};
  bool& connected = linkUp[kHr];                       // the one-slot tests use HR
  bool cancelLeavesDiscOn = false;
  int discStartRc = 0, discCancelRc = 0, connectErr = 0;
  bool connectOk = true, gattOk = true;
  uint8_t gattBattery = 80;
  const uint8_t* expect[bs::kSlotCount] = {};
  const uint8_t*& expectMac = expect[kHr];
  std::function<void()> duringGatt;
  int discStarts = 0, discCancels = 0, connects = 0, cancelConnects = 0, disconnects = 0;
  int gattPasses = 0, timerStarts = 0, timerStops = 0;
  int slotConnects[bs::kSlotCount] = {}, slotDisconnects[bs::kSlotCount] = {};
  uint16_t lastEpoch = 0;
  uint32_t lastDurationMs = 0, timerMs = 0;
  bool lastFilterDuplicates = false;
  uint8_t lastAddrType = 0xFF, lastConnectSlot = 0xFF, lastGattSlot = 0xFF;
  // Local server.
  bool serverOk = true, advOn = false, advStartOk = true, advDataOk = true;
  uint8_t centrals = 0;
  uint8_t serverMask = 0xFF, advMask = 0xFF;
  uint32_t chip = 0xda4eb580;
  char advName[64] = {};
  int serverStarts = 0, advStarts = 0, advStops = 0, advDataSets = 0;
  bool advOnAtDataSet = false;
  int relays[bs::kSlotCount] = {};
  uint8_t relayData[BBS_NOTIFY_MAX_LEN] = {};
  uint8_t relayLen = 0xFF;
  const Core* core = nullptr;                          // set by Rig
  uint16_t activeAtStart = 0;                          // activeDiscEpoch() seen by discStart()

  FakePort() {
    expect[kHr] = kMac;
    expect[kCsc] = kCscMac;
    for (int64_t& f : localFeature) f = -1;
  }

  bool hsSynced() override { return synced; }
  bool discActive() override { return discOn; }
  bool connActive() override { return connOn; }
  int discStart(uint16_t epoch, uint32_t durationMs, bool filterDuplicates) override {
    discStarts++;
    lastEpoch = epoch;
    if (core != nullptr) activeAtStart = core->scan().activeDiscEpoch();
    lastDurationMs = durationMs;
    lastFilterDuplicates = filterDuplicates;
    if (discOn) return bs::kHsEAlready;
    if (discStartRc == 0) discOn = true;
    return discStartRc;
  }
  int discCancel() override {
    discCancels++;
    if (!cancelLeavesDiscOn) discOn = false;
    return discCancelRc;
  }
  bool connect(uint8_t slot, const uint8_t mac[6], uint8_t addrType) override {
    connects++;
    slotConnects[slot]++;
    assert(memcmp(mac, expect[slot], 6) == 0);
    assert(!discOn && !connOn);       // §5.2 connect invariant
    assert(core == nullptr || !core->scan().discoveryOwner());   // §5.2: none while pending/open
    lastAddrType = addrType;
    lastConnectSlot = slot;
    if (connectOk) connOn = true;
    return connectOk;
  }
  int lastError(uint8_t) override { return connectErr; }
  bool cancelConnect(uint8_t) override { cancelConnects++; return true; }
  bool disconnect(uint8_t slot) override {
    disconnects++;
    slotDisconnects[slot]++;
    return true;
  }
  bool isConnected(uint8_t slot) override { return linkUp[slot]; }
  // NimBLE keeps the handle while DISCONNECTING (isConnected() false); discEv clears it.
  bool disconnecting[bs::kSlotCount] = {};
  std::function<void(uint8_t)> onConnHandle;           // the host task, racing a handle read
  uint16_t connHandle(uint8_t slot) override {
    if (onConnHandle) onConnHandle(slot);
    return linkUp[slot] || disconnecting[slot] ? 7 : bs::kConnHandleNone;
  }
  bs::GattResult gattRead;                             // Feature/Location the sensor returns
  int securePasses = 0;
  bool lastSecure = false;
  bool gattPass(uint8_t slot, SensorType type, bool secure, bs::GattResult& out) override {
    assert(bs::slotIndex(type) == slot);
    assert(!secure || (BBS_SENSOR_SECURITY && type == SensorType::POWER));   // §6a
    gattPasses++;
    securePasses += secure ? 1 : 0;
    lastSecure = secure;
    lastGattSlot = slot;
    if (duringGatt) duringGatt();
    out = gattRead;
    out.battery = gattBattery;
    return gattOk;
  }
  // Local Feature/Location per slot; -1 = never written.
  int64_t localFeature[bs::kSlotCount];
  int localLocation = -1, featureSets = 0;
  void setLocalFeature(SensorType type, uint32_t feature, uint8_t location) override {
    const int slot = bs::slotIndex(type);
    assert(slot >= 0 && type != SensorType::HEART_RATE);
    featureSets++;
    localFeature[slot] = feature;
    if (type == SensorType::POWER) localLocation = location;
  }
  void gattTimerStart(uint32_t ms) override { timerStarts++; timerMs = ms; }
  void gattTimerStop() override { timerStops++; }
  bool serverStart(uint8_t typeMask) override {
    serverStarts++;
    serverMask = typeMask;
    return serverOk;
  }
  bool advSetData(const char* name, uint8_t typeMask) override {
    advDataSets++;
    assert(strlen(name) < sizeof(advName));
    strcpy(advName, name);
    advMask = typeMask;
    advOnAtDataSet = advOn;
    return advDataOk;
  }
  uint32_t chipId() override { return chip; }
  uint8_t centralCount() override { return centrals; }
  bool advActive() override { return advOn; }
  bool advStart() override {
    advStarts++;
    if (advStartOk) advOn = true;
    return advStartOk;
  }
  bool advStop() override { advStops++; advOn = false; return true; }
  void relay(uint8_t slot, const uint8_t* data, uint8_t len) override {
    assert(len <= BBS_NOTIFY_MAX_LEN);
    relays[slot]++;
    relayLen = len;
    if (len > 0) memcpy(relayData, data, len);
  }
  // Lifecycle.
  bool stackUp = false, stackInitOk = true, openOk = true, closeOk = true;
  int stackInits = 0, opens = 0, closes = 0, ticks = 0;
  char gapName[64] = {};
  uint32_t clock = 0;
  std::function<void()> onTick;                        // the host task between update() passes
  bool stackReady() override { return stackUp; }
  bool stackInit(const char* name) override {
    stackInits++;
    assert(strlen(name) < sizeof(gapName));
    strcpy(gapName, name);
    stackUp = stackInitOk;
    return stackInitOk;
  }
  bool open() override {
    assert(stackUp);
    opens++;
    return openOk;
  }
  bool close() override {
    closes++;
    for (uint8_t i = 0; i < bs::kSlotCount; ++i) {
      if (linkUp[i] || disconnecting[i]) return false;   // as NimBLEPort::close()
    }
    if (closeOk) stackUp = false;
    return closeOk;
  }
  bool bondsOk = true;
  int bondDeletes = 0;
  bool deleteAllBonds() override {
    bondDeletes++;
    return bondsOk;
  }
  uint32_t nowMs() override { return clock; }
  void waitTick() override {
    ticks++;
    clock += 10;
    if (onTick) onTick();
  }
};

struct Recorded {
  EventType type;
  SensorType sensorType;
  int reason;
  SlotState state;                                     // its slot's state when the callback ran
  uint8_t count;
  uint32_t generation;
  bool discovering;                                    // discovering() when the callback ran
  uint8_t tableCount;                                  // discoveredCount() when the callback ran
  bool providesCadence;                                // providesCadence() when the callback ran
};
static Recorded g_events[32];
static int g_eventCount = 0;
static Core* g_core = nullptr;

static void onEvent(const EventInfo& info) {
  assert(g_eventCount < 32);
  const int slot = bs::slotIndex(info.sensorType);     // HOST_RESET, CENTRAL_*, DISCOVERY_DONE: HR slot
  const SlotState state = g_core->slot(slot >= 0 ? static_cast<uint8_t>(slot) : kHr).state;
  g_events[g_eventCount++] = {info.type, info.sensorType, info.reason, state, info.count,
                              info.generation, g_core->discovering(), g_core->discoveredCount(),
                              g_core->providesCadence()};
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
  std::unique_ptr<Core> core;
  uint32_t now = 1000;

  explicit Rig(Mode mode = Mode::MAC, uint8_t typeMask = 0x07)
      : core(new Core(port, mode, typeMask)) {
    core->setEventCallback(onEvent);
    g_core = core.get();
    port.core = core.get();
    g_eventCount = 0;
  }

  const bs::SensorSlot& hr() const { return core->slot(kHr); }
  const bs::SensorSlot& csc() const { return core->slot(kCsc); }
  SlotState state() const { return hr().state; }
  SlotState cscState() const { return csc().state; }
  uint16_t gen() const { return core->attemptGen(kHr); }
  uint16_t cscGen() const { return core->attemptGen(kCsc); }
  // §5.2: at most one slot in STOPPING_FOR_CONNECT/CONNECTING/CANCELLING/DISCOVERING.
  void checkAtMostOneInFlight() const {
    int n = 0;
    for (uint8_t i = 0; i < bs::kSlotCount; ++i) {
      const SlotState s = core->slot(i).state;
      n += (s == SlotState::STOPPING_FOR_CONNECT || s == SlotState::CONNECTING ||
            s == SlotState::CANCELLING || s == SlotState::DISCOVERING) ? 1 : 0;
    }
    assert(n <= 1);
  }
  void update() {
    core->update(now);
    checkAtMostOneInFlight();
  }
  void at(uint32_t t) { now = t; update(); }
  void add() { assert(core->addByMac(SensorType::HEART_RATE, kMacStr, 0, "HRM", now)); }
  void addCsc() { assert(core->addByMac(SensorType::SPEED_CADENCE, kCscMacStr, 0, "CSC", now)); }
  void remove() { core->remove(SensorType::HEART_RATE, now); }
  // NAME mode: what gapScanCb does with one report. False if no slot has the name.
  bool report(const uint8_t* mac, const char* name, uint8_t addrType = 0) {
    const int slot = core->scanNameMatch(name, strlen(name));
    if (slot < 0) return false;
    assert(bs::pushScanMatch(core->queue(), core->scan().epoch(), mac, addrType, name,
                             strlen(name), static_cast<uint8_t>(slot)));
    return true;
  }
  // NAME mode: HR "HRM" → window → report from kMac → SUBSCRIBED.
  void nameToSubscribed() {
    assert(core->addByName(SensorType::HEART_RATE, "HRM", now));
    update();
    assert(core->scan().owner() == ScanOwner::NAME_SCAN);
    assert(report(kMac, "HRM"));
    update();
    assert(state() == SlotState::CONNECTING);
    connectedEv(gen());
    update();
    assert(state() == SlotState::SUBSCRIBED);
  }
  // Same-type re-add with kOtherMac (random address).
  void replace() {
    port.expectMac = kOtherMac;
    assert(core->addByMac(SensorType::HEART_RATE, kOtherMacStr, 1, "HRM2", now));
    assert(memcmp(hr().mac, kOtherMac, 6) == 0 && hr().addrType == 1);
    assert(strcmp(hr().name, "HRM2") == 0 && hr().failCount == 0);
    assert(core->scanFilterMatch(kOtherMac) && !core->scanFilterMatch(kMac));
  }
  void match(uint16_t epoch, const uint8_t* mac = kMac, uint8_t addrType = 0) {
    assert(bs::pushScanMatch(core->queue(), epoch, mac, addrType, "HRM", 3));
  }
  void connectedEv(uint16_t g, uint8_t slot = kHr) {
    port.connOn = false;
    port.linkUp[slot] = true;
    assert(bs::pushConnected(core->queue(), slot, g));
  }
  void failEv(uint16_t g, int16_t reason = 13, uint8_t slot = kHr) {
    port.connOn = false;
    assert(bs::pushConnectFail(core->queue(), slot, g, reason));
  }
  void discEv(uint16_t g, int16_t reason = 0x208, uint8_t slot = kHr) {
    port.connOn = false;
    port.linkUp[slot] = false;
    port.disconnecting[slot] = false;
    assert(bs::pushDisconnected(core->queue(), slot, g, reason));
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
  assert(r.cscState() == SlotState::WAIT_SCAN && r.state() == SlotState::EMPTY);
  assert(r.csc().type == SensorType::SPEED_CADENCE && r.hr().type == SensorType::HEART_RATE);
  assert(memcmp(r.csc().mac, kMac, 6) == 0);
  assert(r.csc().addrType == 0);                       // 0xaa: top bits 10 → public
  assert(strlen(r.csc().name) == 31 && strncmp(r.csc().name, longName, 31) == 0);
  assert(r.core->scanFilterMatch(kMac) && !r.core->scanFilterMatch(kOtherMac));
  // A bad MAC on a replace: false, unchanged.
  assert(!r.core->addByMac(SensorType::SPEED_CADENCE, "aa:bb:cc:dd:ee", 0, nullptr, r.now));
  assert(r.cscState() == SlotState::WAIT_SCAN);
  assert(memcmp(r.csc().mac, kMac, 6) == 0 && r.core->scanFilterMatch(kMac));
  // Same type replaces: new identity, straight to FOUND.
  assert(r.core->addByMac(SensorType::SPEED_CADENCE, kOtherMacStr, -1, nullptr, r.now));
  assert(r.cscState() == SlotState::FOUND && r.csc().name[0] == '\0');
  assert(r.core->scanFilterMatch(kOtherMac) && !r.core->scanFilterMatch(kMac));

  r.core->remove(SensorType::SPEED_CADENCE, r.now);
  assert(r.cscState() == SlotState::EMPTY && !r.core->scanFilterMatch(kOtherMac));
  assert(r.core->addByMac(SensorType::HEART_RATE, "c0:bb:cc:dd:ee:01", -1, nullptr, r.now));
  assert(r.hr().name[0] == '\0');
  assert(r.hr().addrType == 1);
  r.remove();
  assert(r.core->addByMac(SensorType::HEART_RATE, "c0:bb:cc:dd:ee:01", 0, nullptr, r.now));
  assert(r.hr().addrType == 0);                  // explicit type wins
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
  assert(r.hr().addrType == 1 && r.gen() == 1);

  r.connectedEv(1);
  r.update();                                          // DISCOVERING → GATT pass, one pass
  assert(r.state() == SlotState::SUBSCRIBED);
  assert(r.port.gattPasses == 1 && r.port.timerStarts == 1 && r.port.timerStops == 1);
  assert(r.port.timerMs == BBS_GATT_BUDGET_MS);
  assert(r.hr().battery == 80);
  assert(r.core->stats().connects == 1 && r.core->stats().gattBudgetAborts == 0);
  r.update();
  assert(r.port.gattPasses == 1 && r.port.discStarts == 1);   // no rescan while subscribed
}

static void testScanMatchAddrTypeWins() {
  Rig r;                                               // configured random, advertiser public
  assert(r.core->addByMac(SensorType::HEART_RATE, kMacStr, 1, "HRM", r.now));
  assert(r.hr().addrType == 1);
  r.update();
  r.match(r.core->scan().epoch(), kMac, 0);
  r.update();
  assert(r.state() == SlotState::CONNECTING);
  assert(r.hr().addrType == 0 && r.port.lastAddrType == 0);
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
  assert(r.hr().lastDisconnectReason == 0x3e && r.hr().failCount == 0);
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
    assert(r.core->stats().disconnects == 1 && r.hr().failCount == 0);
    assert(r.hr().lastDisconnectReason == 0x3e && r.core->stats().connectFails == 0);
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
    assert(r.state() == SlotState::WAIT_SCAN && r.hr().failCount == 1);
    assert(r.core->stats().disconnects == 1 && r.core->stats().connectFails == 0);
    assert(g_eventCount == 1);
    expectEvent(0, EventType::SENSOR_FAILED, SlotState::WAIT_SCAN);
  }
  {
    Rig r;                                             // remove pending: EMPTY
    r.toSubscribed();
    r.remove();
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
    r.remove();
    g_eventCount = 0;
    r.failEv(r.gen(), 0x3e);
    r.port.connected = false;
    r.update();
    assert(r.state() == SlotState::EMPTY && r.core->stats().disconnects == 1);
    assert(r.hr().failCount == 0 && r.core->stats().connectFails == 0);
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
  assert(r.hr().lastDisconnectReason == 0x208 && r.core->stats().disconnects == 1);

  r.failEv(r.gen());
  r.update();                                          // then sleep scan
  assert(r.state() == SlotState::WAIT_SCAN);
  assert(r.hr().failCount == 1 && r.core->stats().connectFails == 1);
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
  assert(r.state() == SlotState::SUBSCRIBED && r.hr().failCount == 0);
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
  assert(r.state() == SlotState::WAIT_SCAN && r.hr().failCount == 1);
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
  assert(r.state() == SlotState::WAIT_SCAN && r.hr().failCount == 1);
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
    assert(r.state() == SlotState::FOUND && r.hr().failCount == 0);
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
    assert(r.state() == SlotState::FOUND && r.hr().failCount == 0);
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
      assert(r.hr().failCount == i + 1 && r.core->stats().connectFails == i + 1u);
      assert(r.hr().holdMs == expected[i]);
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
    assert(r.state() == SlotState::WAIT_SCAN && r.hr().failCount == 1);
    assert(r.core->stats().connectFails == 1 && r.hr().holdMs == 1000);
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
    assert(r.state() == SlotState::WAIT_SCAN && r.hr().failCount == 1);
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
    assert(r.state() == SlotState::WAIT_SCAN && r.hr().failCount == 0);
    assert(r.core->stats().connectFails == 0 && r.hr().holdMs == 0);
    assert(g_eventCount == 1);
    expectEvent(0, EventType::HOST_RESET, SlotState::WAIT_SCAN, 0, SensorType{});
    r.failEv(r.gen());                                 // late outcome: no state to act on
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.hr().failCount == 0);
    assert(countEvents(EventType::SENSOR_FAILED) == 0);

    r.at(r.now + BBS_SCAN_START_BACKOFF_MS + 40000);   // next attempt: no failPending left over
    r.match(r.core->scan().epoch());
    r.update();
    assert(r.state() == SlotState::CONNECTING);
    const uint16_t g = r.gen();
    r.connectedEv(g);
    r.discEv(g, 0x08);                                 // link lost before the pass: neutral
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.hr().failCount == 0);
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
  assert(r.hr().failCount == 1 && r.core->stats().connectFails == 1);   // watchdog cancel
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
    r.remove();
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
    assert(r.hr().failCount == 1 && r.core->stats().connectFails == 0);
    assert(r.hr().holdMs == 1000 && r.hr().lastDisconnectReason == 0x216);
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
    assert(r.state() == SlotState::WAIT_SCAN && r.hr().failCount == 1);
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
    assert(r.state() == SlotState::WAIT_SCAN && r.hr().failCount == 1);
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
    assert(r.state() == SlotState::SUBSCRIBED && r.hr().failCount == 0);
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
    assert(r.state() == SlotState::WAIT_SCAN && r.hr().failCount == i + 1);
    assert(r.hr().holdMs == expected[i] && r.core->stats().connectFails == 0);
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
    r.remove();
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
    assert(r.state() == SlotState::DISCOVERING && r.hr().failPending);
    g_eventCount = 0;
    r.port.connected = false;
    assert(bs::pushHostReset(r.core->queue()));
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.hr().failCount == 0);
    assert(r.hr().holdMs == 0 && r.core->stats().connectFails == 0);
    assert(r.core->stats().disconnects == 0);
    assert(g_eventCount == 1);
    expectEvent(0, EventType::HOST_RESET, SlotState::WAIT_SCAN, 0, SensorType{});
    r.discEv(r.gen(), 0x16);                           // late outcome: no state to act on
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.hr().failCount == 0);
    assert(r.core->stats().disconnects == 0 && g_eventCount == 1);

    r.at(r.now + BBS_SCAN_START_BACKOFF_MS + 40000);   // flags cleared on the next attempt
    r.match(r.core->scan().epoch());
    r.update();
    assert(r.state() == SlotState::CONNECTING);
    const uint16_t g = r.gen();
    r.connectedEv(g);
    r.discEv(g, 0x08);                                 // link lost before the pass: neutral
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.hr().failCount == 0);
    assert(countEvents(EventType::SENSOR_FAILED) == 0);
  }
}

// Protected events that change nothing: SCAN_END of epoch 0 is always stale.
static void fillQueueToResync(Core& core) {
  for (uint8_t i = 0; i < BBS_EVENT_QUEUE_DEPTH; ++i) bs::pushScanEnd(core.queue(), 0, 0);
  assert(!bs::pushScanEnd(core.queue(), 0, 0));
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
    Rig r;                                             // the repair sets lastDisconnectReason 0
    r.toSubscribed();
    r.discEv(r.gen(), 0x13);
    r.update();
    r.connectedEv(r.gen());
    r.update();
    assert(r.state() == SlotState::SUBSCRIBED && r.hr().lastDisconnectReason == 0x13);
    r.port.connected = false;
    fillQueueToResync(*r.core);
    r.update();
    BikeSensor::SensorInfo info{};
    assert(r.core->info(SensorType::HEART_RATE, info) && info.state == SlotState::WAIT_SCAN);
    assert(info.lastDisconnectReason == 0);
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
    assert(r.state() == SlotState::WAIT_SCAN && r.hr().failCount == 1);
    assert(r.hr().holdMs == 1000 && r.core->stats().disconnects == 1);
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
    assert(r.state() == SlotState::CONNECTING && r.hr().failCount == 0);
    assert(r.core->stats().disconnects == 1 && g_eventCount == 0);
  }
  {
    Rig r;                                             // failed pass, remove pending: EMPTY
    r.port.gattOk = false;
    r.toConnecting();
    r.connectedEv(r.gen());
    r.update();
    r.remove();
    g_eventCount = 0;
    r.port.connected = false;
    fillQueueToResync(*r.core);
    r.update();
    assert(r.state() == SlotState::EMPTY && r.hr().failCount == 0);
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
    r.remove();                             // EMPTY: no-op
    assert(r.state() == SlotState::EMPTY);
    r.add();
    r.remove();                             // WAIT_SCAN
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
    r.remove();
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
    r.remove();
    assert(r.state() == SlotState::EMPTY && r.core->scan().owner() == ScanOwner::OFF);
    r.port.discOn = false;
    r.update();
    assert(r.port.connects == 0);
  }
  const bool viaFail[] = {true, false};
  for (bool fail : viaFail) {
    Rig r;                                             // CONNECTING → CANCELLING → EMPTY
    r.toConnecting();
    r.remove();
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
    assert(r.state() == SlotState::EMPTY && r.hr().failCount == 0);
  }
  {
    Rig r;                                             // CANCELLING (watchdog), then remove
    r.toConnecting();
    r.at(r.now + BBS_CONNECT_TIMEOUT_MS + 2000);
    r.remove();
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
    r.remove();
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
    r.remove();
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
    r.remove();
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
  for (uint8_t i = 0; i < BBS_EVENT_QUEUE_DEPTH - 1; ++i) bs::pushScanEnd(r.core->queue(), 0, 0);
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
    r.remove();
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
static SensorType g_removeType = SensorType::HEART_RATE;
static uint32_t g_removeNow = 0;

static void onEventRemoving(const EventInfo& info) {
  onEvent(info);
  if (info.type == g_removeOn) g_removeCore->remove(g_removeType, g_removeNow);
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
    assert(r.hr().failCount == 0 && !r.hr().directRetry);
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
    assert(r.hr().failCount == 0 && r.core->stats().connectFails == 0);
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
  assert(r.port.discStarts == startsBefore && !r.hr().directRetry);
  assert(r.hr().failCount == 0);
  r.failEv(r.gen());
  r.update();
  assert(r.state() == SlotState::WAIT_SCAN && r.hr().failCount == 1);
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
    assert(r.hr().holdMs == 0);
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
    assert(r.state() == SlotState::WAIT_SCAN && r.hr().failCount == 1);
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
    if (removeFirst) r.remove();
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
    assert(r.hr().failCount == 0 && r.core->stats().connectFails == 0);
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
    assert(r.hr().failCount == 0 && countEvents(EventType::SENSOR_FAILED) == 0);
    assert(countEvents(EventType::SENSOR_DISCONNECTED) == 0);
  }
  for (bool removeFirst : viaRemove) {
    Rig r;                                             // SUBSCRIBED (or remove pending)
    r.toSubscribed();
    const int startsBefore = r.port.discStarts;
    if (removeFirst) r.remove();
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
    Rig r;                                             // another type: own slot, no teardown
    r.toSubscribed();
    assert(!r.core->addByMac(SensorType::SPEED_CADENCE, kMacStr, 0, nullptr, r.now));
    assert(r.cscState() == SlotState::EMPTY);
    r.addCsc();
    assert(r.state() == SlotState::SUBSCRIBED && r.cscState() == SlotState::WAIT_SCAN);
    assert(r.port.disconnects == 0 && memcmp(r.hr().mac, kMac, 6) == 0);
  }
}

// ---- Multi-slot (§10 step 4) ----

static void testSlotIndex() {
  Rig r;
  assert(bs::kSlotCount == BBS_COMPILED_SENSOR_COUNT);
  assert(kHr == 0 && kCsc == 1);
#if BBS_COMPILE_POWER
  assert(bs::slotIndex(SensorType::POWER) == 2);
#else
  assert(bs::slotIndex(SensorType::POWER) == -1);
#endif
  assert(bs::slotIndex(static_cast<SensorType>(0)) == -1);
  assert(bs::slotIndex(static_cast<SensorType>(0x03)) == -1);
  for (uint8_t i = 0; i < bs::kSlotCount; ++i) {
    assert(bs::slotIndex(r.core->slot(i).type) == i);
    assert(r.core->slot(i).state == SlotState::EMPTY);
  }
  r.core->remove(static_cast<SensorType>(0x03), r.now);   // not a slot: no-op
  assert(r.core->attemptGen(bs::kSlotCount) == 0);
}

static void testDuplicateMac() {
  {
    Rig r;
    r.add();
    assert(!r.core->addByMac(SensorType::SPEED_CADENCE, kMacStr, 0, "CSC", r.now));
    assert(!r.core->addByMac(SensorType::SPEED_CADENCE, "AA:BB:CC:DD:EE:01", 1, nullptr, r.now));
    assert(r.cscState() == SlotState::EMPTY && !r.csc().hasAddr);
    r.addCsc();
    // Replace with a MAC held by the other slot: false, identity and state unchanged.
    assert(!r.core->addByMac(SensorType::HEART_RATE, kCscMacStr, 0, "X", r.now));
    assert(r.state() == SlotState::WAIT_SCAN && memcmp(r.hr().mac, kMac, 6) == 0);
    assert(strcmp(r.hr().name, "HRM") == 0);
    assert(!r.core->addByMac(SensorType::SPEED_CADENCE, kMacStr, 0, "X", r.now));
    assert(r.cscState() == SlotState::WAIT_SCAN && memcmp(r.csc().mac, kCscMac, 6) == 0);
    // Its own MAC again is a same-type replace.
    assert(r.core->addByMac(SensorType::HEART_RATE, kMacStr, 0, "HRM", r.now));
    assert(r.state() == SlotState::FOUND);
    // Freed by remove().
    r.core->remove(SensorType::SPEED_CADENCE, r.now);
    assert(r.core->addByMac(SensorType::HEART_RATE, kCscMacStr, 0, nullptr, r.now));
    assert(r.core->scanFilterMatch(kCscMac) && !r.core->scanFilterMatch(kMac));
  }
  {
    Rig r;                                             // a removing slot holds its MAC until EMPTY
    r.toSubscribed();
    r.remove();
    assert(!r.core->addByMac(SensorType::SPEED_CADENCE, kMacStr, 0, nullptr, r.now));
    r.discEv(r.gen());
    r.update();
    assert(r.state() == SlotState::EMPTY);
    assert(r.core->addByMac(SensorType::SPEED_CADENCE, kMacStr, 0, nullptr, r.now));
  }
}

static void testTwoMatchesOneWindow() {
  Rig r;
  r.add();
  r.addCsc();
  r.update();
  assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW && r.port.discStarts == 1);
  assert(r.port.lastFilterDuplicates);                 // SLEEP: controller filter on (§5.3)
  const uint16_t e = r.core->scan().epoch();
  r.match(e, kCscMac, 1);
  r.match(e, kMac);
  r.update();                                          // both FOUND; HR (lower index) first
  assert(r.state() == SlotState::CONNECTING && r.cscState() == SlotState::FOUND);
  assert(r.port.connects == 1 && r.port.lastConnectSlot == kHr && r.port.discCancels == 1);
  assert(countEvents(EventType::SENSOR_FOUND) == 2);
  expectEvent(0, EventType::SENSOR_FOUND, SlotState::FOUND, 0, SensorType::SPEED_CADENCE);
  expectEvent(1, EventType::SENSOR_FOUND, SlotState::FOUND);
  r.at(r.now + 100);                                   // CSC waits; no scan while a slot is FOUND
  assert(r.cscState() == SlotState::FOUND && r.port.connects == 1 && r.port.discStarts == 1);

  r.connectedEv(r.gen());
  r.update();                                          // HR subscribes, CSC connects, same pass
  assert(r.state() == SlotState::SUBSCRIBED && r.cscState() == SlotState::CONNECTING);
  assert(r.port.connects == 2 && r.port.lastConnectSlot == kCsc);
  assert(r.csc().addrType == 1 && r.port.lastAddrType == 1);
  r.connectedEv(r.cscGen(), kCsc);
  r.update();
  assert(r.cscState() == SlotState::SUBSCRIBED && r.port.lastGattSlot == kCsc);
  assert(r.port.gattPasses == 2 && r.core->stats().connects == 2);
  r.at(r.now + 60000);
  assert(r.port.discStarts == 1);                      // nothing waits: no scan
  assert(g_eventCount == 4);
  expectEvent(2, EventType::SENSOR_CONNECTED, SlotState::SUBSCRIBED);
  expectEvent(3, EventType::SENSOR_CONNECTED, SlotState::SUBSCRIBED, 0,
              SensorType::SPEED_CADENCE);
}

static void testMatchAfterStopAndAcrossWindows() {
  Rig r;
  r.add();
  r.addCsc();
  r.update();
  const uint16_t e = r.core->scan().epoch();
  r.now += 1000;
  r.match(e, kMac);
  r.update();                                          // HR stops the window and connects
  assert(r.state() == SlotState::CONNECTING && r.core->scan().owner() == ScanOwner::OFF);
  r.match(e, kCscMac);                                 // queued before the cancel: stale
  r.update();
  assert(r.cscState() == SlotState::WAIT_SCAN && countEvents(EventType::SENSOR_FOUND) == 1);
  r.at(r.now + 1000);
  assert(r.port.discStarts == 1);                      // no scan while HR is in flight

  r.connectedEv(r.gen());
  r.update();                                          // remainder cleared: full window for CSC
  assert(r.state() == SlotState::SUBSCRIBED);
  assert(r.port.discStarts == 2 && r.port.lastDurationMs == BBS_SLEEP_SCAN_DURATION_MS);
  assert(r.core->scan().epoch() == e + 1);
  r.match(e, kCscMac);                                 // previous window's epoch: stale
  r.update();
  assert(r.cscState() == SlotState::WAIT_SCAN);
  r.match(r.core->scan().epoch(), kMac);               // HR is not waiting: ignored
  r.update();
  assert(r.state() == SlotState::SUBSCRIBED && countEvents(EventType::SENSOR_FOUND) == 1);
  r.match(r.core->scan().epoch(), kCscMac);
  r.update();
  assert(r.cscState() == SlotState::CONNECTING && r.port.lastConnectSlot == kCsc);
}

static void testInFlightBlocksOtherSlots() {
  Rig r;
  r.add();
  r.addCsc();
  r.update();
  r.match(r.core->scan().epoch(), kMac);
  r.match(r.core->scan().epoch(), kCscMac);
  r.update();
  assert(r.state() == SlotState::CONNECTING && r.cscState() == SlotState::FOUND);
  r.at(r.now + BBS_CONNECT_TIMEOUT_MS + 2000);         // HR watchdog cancel: CSC still waits
  assert(r.state() == SlotState::CANCELLING && r.cscState() == SlotState::FOUND);
  assert(r.port.connects == 1);
  r.failEv(r.gen());
  r.update();                                          // HR hard fail; CSC connects
  assert(r.state() == SlotState::WAIT_SCAN && r.hr().failCount == 1);
  assert(r.cscState() == SlotState::CONNECTING && r.port.lastConnectSlot == kCsc);

  r.port.gattOk = false;
  r.connectedEv(r.cscGen(), kCsc);
  r.update();                                          // failed pass: CSC stays in flight
  assert(r.cscState() == SlotState::DISCOVERING && r.port.slotDisconnects[kCsc] == 1);
  r.at(r.now + 40000);
  assert(r.port.discStarts == 1 && r.state() == SlotState::WAIT_SCAN);   // HR waits for CSC
  r.discEv(r.cscGen(), 0x16, kCsc);
  r.update();                                          // CSC hard fail; the scan resumes for both
  assert(r.cscState() == SlotState::WAIT_SCAN && r.csc().failCount == 1);
  assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW && r.port.discStarts == 2);
  assert(countEvents(EventType::SENSOR_FAILED) == 2);
}

static void testHeldFoundDoesNotBlockOtherSlot() {
  Rig r;
  r.add();
  r.addCsc();
  r.update();
  r.port.connectOk = false;
  r.port.connectErr = 6;                               // BLE_HS_ENOMEM: HR hard fail, 1 s backoff
  r.match(r.core->scan().epoch(), kMac);
  r.update();
  const uint32_t failedAt = r.now;
  assert(r.state() == SlotState::WAIT_SCAN && r.hr().holdMs == 1000);
  assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW);   // the remainder resumes
  r.port.connectOk = true;
  r.match(r.core->scan().epoch(), kMac);
  r.match(r.core->scan().epoch(), kCscMac);
  r.update();                                          // HR held, so CSC goes first
  assert(r.state() == SlotState::FOUND && r.cscState() == SlotState::CONNECTING);
  r.connectedEv(r.cscGen(), kCsc);
  r.update();
  assert(r.cscState() == SlotState::SUBSCRIBED && r.state() == SlotState::FOUND);
  r.at(failedAt + 999);
  assert(r.state() == SlotState::FOUND);
  r.at(failedAt + 1000);
  assert(r.state() == SlotState::CONNECTING && r.port.lastConnectSlot == kHr);
}

static void testClientEventRouting() {
  Rig r;
  r.addCsc();
  r.toConnecting();
  const uint16_t g = r.gen();
  assert(r.cscGen() == 0);
  r.connectedEv(g, kCsc);                              // CSC slot: wrong gen, wrong state
  r.failEv(0, 13, kCsc);                               // CSC gen, but CSC is not in an attempt
  assert(bs::pushConnected(r.core->queue(), bs::kSlotCount, g));   // no such slot
  r.port.linkUp[kCsc] = false;
  r.update();
  assert(r.state() == SlotState::CONNECTING && r.cscState() == SlotState::WAIT_SCAN);
  assert(r.core->stats().connectFails == 0 && countEvents(EventType::SENSOR_FAILED) == 0);
  r.connectedEv(g);
  r.update();
  assert(r.state() == SlotState::SUBSCRIBED && r.cscState() == SlotState::WAIT_SCAN);
}

static void testNeedScanAcrossSlots() {
  Rig r;
  r.addCsc();
  r.toSubscribed();
  assert(r.cscState() == SlotState::WAIT_SCAN);
  assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW);   // CSC still needs the scan
  r.match(r.core->scan().epoch(), kCscMac);
  r.update();
  r.connectedEv(r.cscGen(), kCsc);
  r.update();
  assert(r.cscState() == SlotState::SUBSCRIBED);
  const int starts = r.port.discStarts;
  r.at(r.now + 60000);
  assert(r.port.discStarts == starts);                 // all subscribed: no scan

  r.discEv(r.gen());
  r.update();                                          // HR: direct attempt, no scan
  assert(r.state() == SlotState::CONNECTING && r.port.discStarts == starts);
  r.failEv(r.gen());
  r.update();                                          // HR waits: scan while CSC stays up
  assert(r.state() == SlotState::WAIT_SCAN && r.cscState() == SlotState::SUBSCRIBED);
  r.at(r.now + 40000);
  assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW && r.port.discStarts == starts + 1);
  r.remove();                                          // last waiting slot gone: cancel
  r.update();
  assert(r.core->scan().owner() == ScanOwner::OFF && r.port.slotDisconnects[kCsc] == 0);
  assert(r.cscState() == SlotState::SUBSCRIBED);
}

static void testHostResetAndResyncAcrossSlots() {
  {
    Rig r;                                             // HOST_RESET: only in-flight slots move
    r.addCsc();
    r.update();
    r.match(r.core->scan().epoch(), kCscMac);
    r.update();
    r.connectedEv(r.cscGen(), kCsc);
    r.update();
    assert(r.cscState() == SlotState::SUBSCRIBED);
    r.add();
    r.update();
    r.match(r.core->scan().epoch(), kMac);
    r.update();
    assert(r.state() == SlotState::CONNECTING);
    g_eventCount = 0;
    r.port.connOn = false;
    assert(bs::pushHostReset(r.core->queue()));
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.cscState() == SlotState::SUBSCRIBED);
    assert(g_eventCount == 1 && r.core->stats().hostResets == 1);
  }
  {
    Rig r;                                             // resync repairs only the lost link
    r.add();
    r.addCsc();
    r.update();
    r.match(r.core->scan().epoch(), kMac);
    r.match(r.core->scan().epoch(), kCscMac);
    r.update();
    r.connectedEv(r.gen());
    r.update();
    r.connectedEv(r.cscGen(), kCsc);
    r.update();
    assert(r.state() == SlotState::SUBSCRIBED && r.cscState() == SlotState::SUBSCRIBED);
    g_eventCount = 0;
    r.port.connected = false;                          // HR link gone, CSC still up
    r.port.connOn = true;                              // keep the scan from starting
    fillQueueToResync(*r.core);
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.cscState() == SlotState::SUBSCRIBED);
    assert(r.core->stats().disconnects == 1 && r.port.disconnects == 0);
    assert(g_eventCount == 1);
    expectEvent(0, EventType::SENSOR_DISCONNECTED, SlotState::WAIT_SCAN, 0);
  }
  {
    Rig r;                                             // orphan sweep per slot
    r.toSubscribed();
    r.port.linkUp[kCsc] = true;                        // CSC EMPTY with a connected client
    r.update();
    assert(r.port.slotDisconnects[kCsc] == 1 && r.port.slotDisconnects[kHr] == 0);
  }
}

static void testCallbackTouchesOtherSlot() {
  Rig r;                                               // HR connected → app removes the FOUND CSC
  r.core->setEventCallback(onEventRemoving);
  g_removeCore = r.core.get();
  g_removeOn = EventType::SENSOR_CONNECTED;
  g_removeType = SensorType::SPEED_CADENCE;
  r.add();
  r.addCsc();
  r.update();
  r.match(r.core->scan().epoch(), kMac);
  r.match(r.core->scan().epoch(), kCscMac);
  r.update();
  g_removeNow = r.now;
  r.connectedEv(r.gen());
  r.update();
  assert(r.state() == SlotState::SUBSCRIBED && r.cscState() == SlotState::EMPTY);
  r.at(r.now + 60000);
  assert(r.port.connects == 1 && r.port.discStarts == 1);
  g_removeType = SensorType::HEART_RATE;
}

static std::function<void(const EventInfo&)> g_hook;

static void onEventHook(const EventInfo& info) {
  onEvent(info);
  if (g_hook) g_hook(info);
}

static void testCallbackReplacesOtherSlot() {
  Rig r;                                               // HR fails inside advanceSlot → app replaces CSC
  r.core->setEventCallback(onEventHook);
  r.add();
  r.addCsc();
  r.update();
  r.port.connectOk = false;
  r.port.connectErr = 6;                               // BLE_HS_ENOMEM: hard fail in tryConnect
  g_hook = [&r](const EventInfo& info) {
    if (info.type != EventType::SENSOR_FAILED) return;
    r.port.connectOk = true;
    r.port.expect[kCsc] = kOtherMac;
    assert(r.core->addByMac(SensorType::SPEED_CADENCE, kOtherMacStr, 1, "CSC2", r.now));
    assert(r.cscState() == SlotState::FOUND);
    r.checkAtMostOneInFlight();
  };
  r.match(r.core->scan().epoch(), kMac);
  r.update();                                          // CSC's direct attempt in the same pass
  g_hook = nullptr;
  assert(r.state() == SlotState::WAIT_SCAN && r.hr().failCount == 1);
  assert(r.cscState() == SlotState::CONNECTING && r.port.lastConnectSlot == kCsc);
  assert(r.port.connects == 2 && r.port.lastAddrType == 1);
  assert(r.core->scan().owner() == ScanOwner::OFF && r.port.discStarts == 1);
  r.failEv(r.cscGen(), 13, kCsc);
  r.update();                                          // CSC hard fail; the scan resumes for both
  assert(r.cscState() == SlotState::WAIT_SCAN && r.csc().failCount == 1);
  assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW && r.port.discStarts == 2);
}

static void testBothSubscribedDropOnePass() {
  Rig r;
  r.add();
  r.addCsc();
  r.update();
  r.match(r.core->scan().epoch(), kMac);
  r.match(r.core->scan().epoch(), kCscMac);
  r.update();
  r.connectedEv(r.gen());
  r.update();
  r.connectedEv(r.cscGen(), kCsc);
  r.update();
  assert(r.state() == SlotState::SUBSCRIBED && r.cscState() == SlotState::SUBSCRIBED);
  const int starts = r.port.discStarts;
  g_eventCount = 0;
  r.discEv(r.cscGen(), 0x08, kCsc);                    // CSC's event first: the slot order decides
  r.discEv(r.gen(), 0x08);
  r.update();
  assert(r.state() == SlotState::CONNECTING && r.port.lastConnectSlot == kHr);
  assert(r.cscState() == SlotState::FOUND && r.port.connects == 3);
  assert(r.core->scan().owner() == ScanOwner::OFF && r.port.discStarts == starts);
  assert(g_eventCount == 2);
  expectEvent(0, EventType::SENSOR_DISCONNECTED, SlotState::FOUND, 0x08,
              SensorType::SPEED_CADENCE);
  expectEvent(1, EventType::SENSOR_DISCONNECTED, SlotState::FOUND, 0x08);
  r.at(r.now + 100);
  assert(r.cscState() == SlotState::FOUND && r.port.discStarts == starts);

  r.connectedEv(r.gen());
  r.update();                                          // HR subscribes; CSC's direct attempt
  assert(r.state() == SlotState::SUBSCRIBED && r.cscState() == SlotState::CONNECTING);
  assert(r.port.lastConnectSlot == kCsc && r.port.connects == 4);
  r.connectedEv(r.cscGen(), kCsc);
  r.update();
  assert(r.cscState() == SlotState::SUBSCRIBED && r.port.discStarts == starts);
}

#if BBS_COMPILE_POWER
static const char kPowerMacStr[] = "aa:bb:cc:dd:ee:04";
static const uint8_t kPowerMac[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x04};

// Two FOUND slots wait while a third is in flight; they connect in slot order.
static void testThreeSlotsOrdering() {
  const uint8_t kPower = static_cast<uint8_t>(bs::slotIndex(SensorType::POWER));
  Rig r;
  r.port.expect[kPower] = kPowerMac;
  r.add();
  r.addCsc();
  assert(r.core->addByMac(SensorType::POWER, kPowerMacStr, 0, "PWR", r.now));
  r.update();
  const uint16_t e = r.core->scan().epoch();
  r.match(e, kPowerMac);                               // reverse slot order
  r.match(e, kCscMac);
  r.match(e, kMac);
  r.update();
  assert(r.state() == SlotState::CONNECTING && r.port.lastConnectSlot == kHr);
  assert(r.cscState() == SlotState::FOUND && r.core->slot(kPower).state == SlotState::FOUND);
  expectEvent(0, EventType::SENSOR_FOUND, SlotState::FOUND, 0, SensorType::POWER);
  expectEvent(1, EventType::SENSOR_FOUND, SlotState::FOUND, 0, SensorType::SPEED_CADENCE);
  expectEvent(2, EventType::SENSOR_FOUND, SlotState::FOUND);

  r.at(r.now + BBS_CONNECT_TIMEOUT_MS + 2000);         // HR watchdog cancel: both still wait
  assert(r.state() == SlotState::CANCELLING && r.port.connects == 1);
  assert(r.cscState() == SlotState::FOUND && r.core->slot(kPower).state == SlotState::FOUND);
  r.failEv(r.gen());
  r.update();                                          // HR hard fail; CSC (lower index) next
  assert(r.state() == SlotState::WAIT_SCAN && r.cscState() == SlotState::CONNECTING);
  assert(r.core->slot(kPower).state == SlotState::FOUND);
  assert(r.port.lastConnectSlot == kCsc && r.port.connects == 2);
  r.connectedEv(r.cscGen(), kCsc);
  r.update();                                          // CSC subscribes; Power connects, same pass
  assert(r.cscState() == SlotState::SUBSCRIBED);
  assert(r.core->slot(kPower).state == SlotState::CONNECTING);
  assert(r.port.lastConnectSlot == kPower && r.port.connects == 3);
  r.connectedEv(r.core->attemptGen(kPower), kPower);
  r.update();
  assert(r.core->slot(kPower).state == SlotState::SUBSCRIBED && r.port.lastGattSlot == kPower);
  assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW && r.port.discStarts == 2);
}
#endif

// ---- NAME mode (§4 rev 25, §5.3) ----

static void testCrossModeAdd() {
  {
    Rig r;                                             // MAC mode: no addByName
    assert(!r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
    assert(r.state() == SlotState::EMPTY && r.core->mode() == Mode::MAC);
  }
  Rig r(Mode::NAME);                                   // NAME mode: no addByMac, no empty name
  assert(!r.core->addByMac(SensorType::HEART_RATE, kMacStr, 0, "HRM", r.now));
  assert(!r.core->addByName(SensorType::HEART_RATE, nullptr, r.now));
  assert(!r.core->addByName(SensorType::HEART_RATE, "", r.now));
  assert(!r.core->addByName(static_cast<SensorType>(0x08), "HRM", r.now));
#if !BBS_COMPILE_POWER
  assert(!r.core->addByName(SensorType::POWER, "HRM", r.now));
#endif
  assert(r.state() == SlotState::EMPTY && r.cscState() == SlotState::EMPTY);
  r.update();
  assert(r.port.discStarts == 0);
}

static void testAddByNameAndDuplicates() {
  Rig r(Mode::NAME);
  assert(r.core->addByName(SensorType::HEART_RATE, "Polar H10", r.now));
  assert(r.state() == SlotState::IDLE && strcmp(r.hr().name, "Polar H10") == 0);
  assert(!r.hr().hasAddr);
  assert(!r.core->addByName(SensorType::SPEED_CADENCE, "Polar H10", r.now));
  assert(r.cscState() == SlotState::EMPTY);
  assert(r.core->addByName(SensorType::SPEED_CADENCE, "polar h10", r.now));   // case-sensitive
  r.core->remove(SensorType::SPEED_CADENCE, r.now);
  assert(r.cscState() == SlotState::EMPTY);

  // Names equal in their first 31 bytes collide after truncation.
  const char longA[] = "0123456789012345678901234567890-A";
  const char longB[] = "0123456789012345678901234567890-B";
  r.remove();
  assert(r.core->addByName(SensorType::HEART_RATE, longA, r.now));
  assert(strlen(r.hr().name) == 31 && strncmp(r.hr().name, longA, 31) == 0);
  assert(!r.core->addByName(SensorType::SPEED_CADENCE, longB, r.now));
  assert(r.core->addByName(SensorType::SPEED_CADENCE, "Speed", r.now));
  // Replace with the other slot's name: false, unchanged. Own name again: replace.
  assert(!r.core->addByName(SensorType::HEART_RATE, "Speed", r.now));
  assert(strncmp(r.hr().name, longA, 31) == 0 && r.state() == SlotState::IDLE);
  assert(r.core->addByName(SensorType::HEART_RATE, longB, r.now));
  assert(r.state() == SlotState::IDLE && r.hr().name[30] == '0');

  // gapScanCb lookup: exact, case-sensitive, whole length, stored (truncated) name.
  assert(r.core->scanNameMatch("Speed", 5) == kCsc);
  assert(r.core->scanNameMatch("Spee", 4) == -1);
  assert(r.core->scanNameMatch("Speedo", 6) == -1);
  assert(r.core->scanNameMatch("speed", 5) == -1);
  assert(r.core->scanNameMatch("Speed\0", 6) == -1);
  assert(r.core->scanNameMatch(nullptr, 0) == -1);
  assert(r.core->scanNameMatch(longB, 31) == kHr);
  assert(r.core->scanNameMatch(longB, sizeof(longB) - 1) == -1);
  r.core->remove(SensorType::SPEED_CADENCE, r.now);
  assert(r.core->scanNameMatch("Speed", 5) == -1);
}

static void testNameWindows() {
  Rig r(Mode::NAME);
  r.update();
  assert(r.port.discStarts == 0);                      // nothing IDLE
  assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
  r.update();
  assert(r.core->scan().owner() == ScanOwner::NAME_SCAN && r.port.discStarts == 1);
  assert(r.port.lastDurationMs == BBS_NAME_SCAN_WINDOW_MS);
  assert(!r.port.lastFilterDuplicates);                // the name may be in SCAN_RSP only
  const uint16_t e1 = r.core->scan().epoch();

  r.at(r.now + BBS_NAME_SCAN_WINDOW_MS);               // natural end: next window at once
  r.port.discOn = false;
  assert(bs::pushScanEnd(r.core->queue(), e1, 0));
  r.update();
  assert(r.core->scan().owner() == ScanOwner::NAME_SCAN && r.port.discStarts == 2);
  assert(r.core->scan().epoch() == e1 + 1 && r.port.lastDurationMs == BBS_NAME_SCAN_WINDOW_MS);
  assert(r.core->stats().scansStarted == 2);

  assert(bs::pushScanMatch(r.core->queue(), e1, kMac, 0, "HRM", 3, kHr));   // previous window
  assert(bs::pushScanEnd(r.core->queue(), e1, 0));
  r.update();
  assert(r.state() == SlotState::IDLE && r.core->scan().owner() == ScanOwner::NAME_SCAN);
  assert(!r.report(kMac, "HRM2"));                     // another name: nothing pushed

  // Missing SCAN_END: OFF after the grace, then a new window.
  const uint32_t start = r.now;
  r.port.discOn = false;
  r.at(start + BBS_NAME_SCAN_WINDOW_MS + BBS_SCAN_END_GRACE_MS - 1);
  assert(r.core->scan().epoch() == e1 + 1);
  r.at(start + BBS_NAME_SCAN_WINDOW_MS + BBS_SCAN_END_GRACE_MS);
  assert(r.core->scan().owner() == ScanOwner::OFF);
  r.update();
  assert(r.core->scan().owner() == ScanOwner::NAME_SCAN && r.core->scan().epoch() == e1 + 2);

  r.remove();                                          // !needScan cancels
  r.update();
  assert(r.core->scan().owner() == ScanOwner::OFF && r.port.discCancels == 1);
}

static void testNameHappyPath() {
  Rig r(Mode::NAME);
  assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
  r.update();
  assert(r.report(kMac, "HRM", 1));
  r.update();                                          // FOUND → STOPPING → CONNECTING
  assert(r.state() == SlotState::CONNECTING && r.port.lastConnectSlot == kHr);
  assert(r.port.lastAddrType == 1 && r.hr().addrType == 1);
  assert(r.hr().hasAddr && memcmp(r.hr().mac, kMac, 6) == 0);
  assert(r.core->scan().owner() == ScanOwner::OFF && r.port.discCancels == 1);
  expectEvent(0, EventType::SENSOR_FOUND, SlotState::FOUND);
  r.connectedEv(r.gen());
  r.update();
  assert(r.state() == SlotState::SUBSCRIBED);
  expectEvent(1, EventType::SENSOR_CONNECTED, SlotState::SUBSCRIBED);
  r.at(r.now + 60000);
  assert(r.port.discStarts == 1);                      // nothing IDLE: no scan
}

static void testNameDropRefoundNewAddress() {
  Rig r(Mode::NAME);
  r.nameToSubscribed();
  const int connects = r.port.connects;
  const int starts = r.port.discStarts;
  g_eventCount = 0;
  r.discEv(r.gen(), 0x208);
  r.update();                                          // no direct attempt: IDLE, NAME window
  assert(r.state() == SlotState::IDLE && r.port.connects == connects);
  assert(r.core->scan().owner() == ScanOwner::NAME_SCAN && r.port.discStarts == starts + 1);
  assert(g_eventCount == 1);
  expectEvent(0, EventType::SENSOR_DISCONNECTED, SlotState::IDLE, 0x208);
  assert(r.hr().hasAddr && memcmp(r.hr().mac, kMac, 6) == 0);   // kept until re-resolved
  assert(r.hr().failCount == 0 && r.hr().holdMs == 0);

  r.port.expectMac = kOtherMac;
  assert(r.report(kOtherMac, "HRM", 1));
  r.update();
  assert(r.state() == SlotState::CONNECTING && r.port.lastAddrType == 1);
  assert(memcmp(r.hr().mac, kOtherMac, 6) == 0);
  r.connectedEv(r.gen());
  r.update();
  assert(r.state() == SlotState::SUBSCRIBED);
}

static void testNameHardFailIdleBackoff() {
  {
    Rig r(Mode::NAME);                                 // hard connect() error
    r.port.connectOk = false;
    r.port.connectErr = 6;                             // BLE_HS_ENOMEM
    assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
    r.update();
    assert(r.report(kMac, "HRM"));
    r.update();
    const uint32_t failedAt = r.now;
    assert(r.state() == SlotState::IDLE && r.hr().failCount == 1 && r.hr().holdMs == 1000);
    assert(r.core->stats().connectFails == 1);
    expectEvent(1, EventType::SENSOR_FAILED, SlotState::IDLE);
    assert(r.core->scan().owner() == ScanOwner::NAME_SCAN);   // the next window at once
    r.port.connectOk = true;
    assert(r.report(kMac, "HRM"));
    r.update();
    assert(r.state() == SlotState::FOUND);             // held by the backoff
    assert(r.core->scan().owner() == ScanOwner::OFF);  // nothing IDLE: window cancelled
    r.at(failedAt + 999);
    assert(r.state() == SlotState::FOUND);
    r.at(failedAt + 1000);
    assert(r.state() == SlotState::CONNECTING);
  }
  {
    Rig r(Mode::NAME);                                 // failed GATT pass
    r.port.gattOk = false;
    assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
    r.update();
    assert(r.report(kMac, "HRM"));
    r.update();
    r.connectedEv(r.gen());
    r.update();
    assert(r.state() == SlotState::DISCOVERING);
    r.discEv(r.gen(), 0x216);
    r.update();
    assert(r.state() == SlotState::IDLE && r.hr().failCount == 1);
    assert(r.core->stats().connectFails == 0 && countEvents(EventType::SENSOR_FAILED) == 1);
    assert(countEvents(EventType::SENSOR_DISCONNECTED) == 0);
  }
  {
    Rig r(Mode::NAME);                                 // watchdog cancel
    assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
    r.update();
    assert(r.report(kMac, "HRM"));
    r.update();
    r.at(r.now + BBS_CONNECT_TIMEOUT_MS + 2000);
    assert(r.state() == SlotState::CANCELLING);
    r.failEv(r.gen());
    r.update();
    assert(r.state() == SlotState::IDLE && r.hr().failCount == 1);
    assert(r.core->stats().connectFails == 1);
  }
}

static void testNameRuntimeGuard() {
  Rig r(Mode::NAME);
  assert(r.core->addByName(SensorType::HEART_RATE, "Foo", r.now));
  assert(r.core->addByName(SensorType::SPEED_CADENCE, "FooBar", r.now));
  r.update();
  assert(r.report(kCscMac, "FooBar"));
  r.update();
  r.connectedEv(r.cscGen(), kCsc);
  r.update();
  assert(r.cscState() == SlotState::SUBSCRIBED && r.state() == SlotState::IDLE);
  assert(r.core->scan().owner() == ScanOwner::NAME_SCAN);

  assert(r.report(kCscMac, "Foo"));                    // CSC's device, shortened name
  r.update();
  assert(r.state() == SlotState::IDLE && countEvents(EventType::SENSOR_FOUND) == 1);

  r.discEv(r.cscGen(), 0x208, kCsc);                   // CSC IDLE still holds its address
  r.update();
  assert(r.cscState() == SlotState::IDLE);
  assert(r.report(kCscMac, "Foo"));
  r.update();
  assert(r.state() == SlotState::IDLE && countEvents(EventType::SENSOR_FOUND) == 1);

  assert(r.report(kMac, "Foo"));                       // a match queued, then HR renamed
  assert(r.core->addByName(SensorType::HEART_RATE, "Baz", r.now));
  r.update();
  assert(r.state() == SlotState::IDLE && countEvents(EventType::SENSOR_FOUND) == 1);
  assert(r.core->addByName(SensorType::HEART_RATE, "Foo", r.now));

  r.core->remove(SensorType::SPEED_CADENCE, r.now);    // freed by remove()
  r.port.expect[kHr] = kCscMac;
  assert(r.report(kCscMac, "Foo"));
  r.update();
  assert(r.state() == SlotState::CONNECTING && memcmp(r.hr().mac, kCscMac, 6) == 0);
}

static void testNameReplace() {
  {
    Rig r(Mode::NAME);                                 // SUBSCRIBED → teardown → IDLE
    r.nameToSubscribed();
    assert(r.core->addByName(SensorType::HEART_RATE, "New", r.now));
    assert(r.port.disconnects == 1 && !r.hr().hasAddr && strcmp(r.hr().name, "New") == 0);
    r.discEv(r.gen(), 0x16);
    r.update();
    assert(r.state() == SlotState::IDLE && r.port.connects == 1);
    expectEvent(2, EventType::SENSOR_DISCONNECTED, SlotState::IDLE, 0x16);
    assert(r.core->scan().owner() == ScanOwner::NAME_SCAN);
    assert(!r.report(kMac, "HRM"));                    // the old name no longer matches
    assert(r.report(kMac, "New"));
  }
  {
    Rig r(Mode::NAME);                                 // same name again: also a replace
    r.nameToSubscribed();
    assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
    assert(r.port.disconnects == 1);
    r.discEv(r.gen());
    r.update();
    assert(r.state() == SlotState::IDLE && r.port.connects == 1);
  }
  {
    Rig r(Mode::NAME);                                 // FOUND (held) → IDLE, hold cleared
    r.port.connectOk = false;
    r.port.connectErr = bs::kHsEBusy;
    assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
    r.update();
    assert(r.report(kMac, "HRM"));
    r.update();
    assert(r.state() == SlotState::FOUND && r.hr().holdMs == 500);
    assert(r.core->addByName(SensorType::HEART_RATE, "New", r.now));
    assert(r.state() == SlotState::IDLE && r.hr().holdMs == 0);
    r.at(r.now + 1000);
    assert(r.port.connects == 1 && r.core->scan().owner() == ScanOwner::NAME_SCAN);
  }
  {
    Rig r(Mode::NAME);                                 // STOPPING_FOR_CONNECT → IDLE, no connect
    r.port.cancelLeavesDiscOn = true;
    assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
    r.update();
    assert(r.report(kMac, "HRM"));
    r.update();
    assert(r.state() == SlotState::STOPPING_FOR_CONNECT);
    assert(r.core->addByName(SensorType::HEART_RATE, "New", r.now));
    assert(r.state() == SlotState::IDLE);
    r.port.discOn = false;
    r.update();
    assert(r.port.connects == 0);
  }
  {
    Rig r(Mode::NAME);                                 // CONNECTING → CANCELLING → IDLE, neutral
    assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
    r.update();
    assert(r.report(kMac, "HRM"));
    r.update();
    const uint16_t g = r.gen();
    assert(r.core->addByName(SensorType::HEART_RATE, "New", r.now));
    assert(r.state() == SlotState::CANCELLING && r.port.cancelConnects == 1);
    r.failEv(g);
    r.update();
    assert(r.state() == SlotState::IDLE && r.port.connects == 1 && r.hr().failCount == 0);
    assert(countEvents(EventType::SENSOR_FAILED) == 0);
  }
}

static void testNameTwoMatchesOneWindow() {
  Rig r(Mode::NAME);
  assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
  assert(r.core->addByName(SensorType::SPEED_CADENCE, "CSC", r.now));
  r.update();
  assert(r.report(kCscMac, "CSC"));
  assert(r.report(kMac, "HRM"));
  r.update();
  assert(r.state() == SlotState::CONNECTING && r.cscState() == SlotState::FOUND);
  r.connectedEv(r.gen());
  r.update();
  assert(r.state() == SlotState::SUBSCRIBED && r.cscState() == SlotState::CONNECTING);
  r.connectedEv(r.cscGen(), kCsc);
  r.update();
  assert(r.cscState() == SlotState::SUBSCRIBED && r.port.discStarts == 1);
}

static void testNameSafetyHostResetOrphan() {
  {
    Rig r(Mode::NAME);                                 // STOPPING safety → IDLE
    r.port.discCancelRc = 3;
    assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
    r.update();
    assert(r.report(kMac, "HRM"));
    r.update();
    assert(r.state() == SlotState::IDLE && r.core->stats().cancelSafetyExits == 1);
    assert(r.port.connects == 0);
  }
  {
    Rig r(Mode::NAME);                                 // HOST_RESET → IDLE, neutral
    assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
    r.update();
    assert(r.report(kMac, "HRM"));
    r.update();
    r.port.connOn = false;
    assert(bs::pushHostReset(r.core->queue()));
    r.update();
    assert(r.state() == SlotState::IDLE && r.hr().failCount == 0);
  }
  {
    Rig r(Mode::NAME);                                 // orphan sweep includes IDLE
    assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
    r.port.connected = true;
    r.update();
    assert(r.port.slotDisconnects[kHr] == 1);
  }
}

static void testNameCallbackAddsOtherSlot() {
  Rig r(Mode::NAME);                                   // HR drops → app adds CSC
  r.core->setEventCallback(onEventHook);
  r.nameToSubscribed();
  const int starts = r.port.discStarts;
  g_hook = [&r](const EventInfo& info) {
    if (info.type != EventType::SENSOR_DISCONNECTED) return;
    assert(!r.core->addByName(SensorType::SPEED_CADENCE, "HRM", r.now));   // HR's name
    assert(r.core->addByName(SensorType::SPEED_CADENCE, "CSC", r.now));
    assert(r.cscState() == SlotState::IDLE);
  };
  r.discEv(r.gen(), 0x208);
  r.update();                                          // both IDLE: one window for both
  g_hook = nullptr;
  assert(r.state() == SlotState::IDLE && r.cscState() == SlotState::IDLE);
  assert(r.core->scan().owner() == ScanOwner::NAME_SCAN && r.port.discStarts == starts + 1);
  assert(r.report(kCscMac, "CSC"));
  assert(r.report(kMac, "HRM"));
  r.update();
  assert(r.state() == SlotState::CONNECTING && r.cscState() == SlotState::FOUND);
  r.connectedEv(r.gen());
  r.update();
  assert(r.state() == SlotState::SUBSCRIBED && r.cscState() == SlotState::CONNECTING);
}

static void testNameResync() {
  {
    Rig r(Mode::NAME);                                 // lost SUBSCRIBED link: IDLE, no direct attempt
    r.nameToSubscribed();
    g_eventCount = 0;
    const int connects = r.port.connects;
    r.port.connected = false;
    fillQueueToResync(*r.core);
    r.update();
    assert(r.state() == SlotState::IDLE && r.port.connects == connects);
    assert(r.core->stats().disconnects == 1 && r.hr().failCount == 0 && r.hr().holdMs == 0);
    assert(g_eventCount == 1);
    expectEvent(0, EventType::SENSOR_DISCONNECTED, SlotState::IDLE, 0);
    assert(r.core->scan().owner() == ScanOwner::NAME_SCAN);
  }
  {
    Rig r(Mode::NAME);                                 // DISCOVERING after a failed pass: hard fail
    r.port.gattOk = false;
    assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
    r.update();
    assert(r.report(kMac, "HRM"));
    r.update();
    r.connectedEv(r.gen());
    r.update();
    assert(r.state() == SlotState::DISCOVERING && r.port.disconnects == 1);
    g_eventCount = 0;
    r.port.connected = false;
    fillQueueToResync(*r.core);
    r.update();
    assert(r.state() == SlotState::IDLE && r.hr().failCount == 1 && r.hr().holdMs == 1000);
    assert(r.core->stats().disconnects == 1 && r.core->stats().connectFails == 0);
    assert(g_eventCount == 1 && r.port.connects == 1);
    expectEvent(0, EventType::SENSOR_FAILED, SlotState::IDLE);
  }
}

static void testNameRemoveFromLinkStates() {
  {
    Rig r(Mode::NAME);                                 // SUBSCRIBED → disconnect → EMPTY
    r.nameToSubscribed();
    g_eventCount = 0;
    r.remove();
    assert(r.state() == SlotState::SUBSCRIBED && r.port.slotDisconnects[kHr] == 1);
    r.discEv(r.gen(), 0x16);
    r.update();
    assert(r.state() == SlotState::EMPTY && !r.hr().hasAddr);
    assert(r.core->scanNameMatch("HRM", 3) == -1);
    assert(g_eventCount == 1);
    expectEvent(0, EventType::SENSOR_DISCONNECTED, SlotState::EMPTY, 0x16);
    r.at(r.now + 60000);
    assert(g_eventCount == 1 && r.port.connects == 1 && r.port.discStarts == 1);
  }
  {
    Rig r(Mode::NAME);                                 // CONNECTING → CANCELLING → EMPTY
    assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
    r.update();
    assert(r.report(kMac, "HRM"));
    r.update();
    assert(r.state() == SlotState::CONNECTING);
    g_eventCount = 0;
    r.remove();
    assert(r.state() == SlotState::CANCELLING && r.port.cancelConnects == 1);
    r.failEv(r.gen());
    r.update();
    assert(r.state() == SlotState::EMPTY && r.hr().failCount == 0);
    assert(g_eventCount == 0 && r.core->stats().connectFails == 0);
    r.at(r.now + 60000);
    assert(g_eventCount == 0 && r.port.connects == 1);
  }
}

// §5.3: a slot held in FOUND blocks new windows, so another waiting slot is not
// scanned for until the hold ends. The hold is at most kBackoffMaxMs.
static void testFoundHoldBlocksScanBound() {
  Rig r(Mode::NAME);
  r.port.connectOk = false;
  r.port.connectErr = 6;                               // BLE_HS_ENOMEM: every attempt hard-fails
  assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
  assert(r.core->addByName(SensorType::SPEED_CADENCE, "CSC", r.now));   // never found
  r.update();
  assert(r.report(kMac, "HRM"));
  r.update();
  for (uint8_t fails = 1; fails <= 7; ++fails) {
    const uint32_t backoff = bs::kBackoffMinMs << (fails - 1);
    const uint32_t hold = backoff < bs::kBackoffMaxMs ? backoff : bs::kBackoffMaxMs;
    const uint32_t failedAt = r.now;
    const int starts = r.port.discStarts;
    assert(r.state() == SlotState::IDLE && r.hr().failCount == fails && r.hr().holdMs == hold);
    assert(r.core->scan().owner() == ScanOwner::NAME_SCAN);   // started at failedAt
    assert(r.report(kMac, "HRM"));
    r.update();
    assert(r.state() == SlotState::FOUND);             // held; the running window goes on
    assert(r.core->scan().owner() == ScanOwner::NAME_SCAN);
    if (hold > BBS_NAME_SCAN_WINDOW_MS) {
      r.port.discOn = false;
      assert(bs::pushScanEnd(r.core->queue(), r.core->scan().epoch(), 0));
      r.at(failedAt + BBS_NAME_SCAN_WINDOW_MS);
      assert(r.core->scan().owner() == ScanOwner::OFF);   // CSC IDLE, but no next window
    }
    r.at(failedAt + hold - 1);
    assert(r.state() == SlotState::FOUND && r.cscState() == SlotState::IDLE);
    assert(r.port.discStarts == starts);
    r.at(failedAt + hold);                             // hold over: attempt, fail, next window
    assert(r.port.discStarts == starts + 1);
  }
  assert(r.hr().failCount == 8 && r.hr().holdMs == bs::kBackoffMaxMs);
}

static void testNameSecondDeviceSameName() {
  Rig r(Mode::NAME);
  assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
  assert(r.core->addByName(SensorType::SPEED_CADENCE, "CSC", r.now));
  r.update();
  assert(r.report(kMac, "HRM"));
  assert(r.report(kOtherMac, "HRM"));                  // same window: HR is FOUND by then
  r.update();
  assert(r.state() == SlotState::CONNECTING && memcmp(r.hr().mac, kMac, 6) == 0);
  assert(countEvents(EventType::SENSOR_FOUND) == 1);
  r.connectedEv(r.gen());
  r.update();
  assert(r.state() == SlotState::SUBSCRIBED);
  assert(r.core->scan().owner() == ScanOwner::NAME_SCAN);   // CSC still IDLE
  assert(r.report(kOtherMac, "HRM"));                  // while SUBSCRIBED
  r.update();
  assert(r.state() == SlotState::SUBSCRIBED && memcmp(r.hr().mac, kMac, 6) == 0);
  assert(countEvents(EventType::SENSOR_FOUND) == 1 && r.port.connects == 1);
}

static void testNameMatchBadSlot() {
  Rig r(Mode::NAME);
  assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
  r.update();
  const uint16_t e = r.core->scan().epoch();
  assert(bs::pushScanMatch(r.core->queue(), e, kMac, 0, "HRM", 3, bs::kSlotCount));
  assert(bs::pushScanMatch(r.core->queue(), e, kMac, 0, "HRM", 3, 0xFF));
  r.update();
  assert(r.state() == SlotState::IDLE && !r.hr().hasAddr && g_eventCount == 0);
  assert(bs::pushScanMatch(r.core->queue(), e, kMac, 0, "HRM", 3, kHr));
  r.update();
  assert(r.state() == SlotState::CONNECTING);
}

// ---- §10 step 5: relay, central events, advertising desired state ----

static bool notifyEv(Rig& r, const uint8_t* data, size_t len, uint8_t slot = kHr) {
  return bs::pushNotify(r.core->queue(), slot, r.core->attemptGen(slot), data, len);
}

static void cscToSubscribed(Rig& r) {
  r.addCsc();
  r.update();
  r.match(r.core->scan().epoch(), kCscMac);
  r.update();
  assert(r.cscState() == SlotState::CONNECTING);
  r.connectedEv(r.cscGen(), kCsc);
  r.update();
  assert(r.cscState() == SlotState::SUBSCRIBED);
}

static void testNotifyOnlyWhenSubscribed() {
  const uint8_t hr[] = {0x00, 72};
  Rig r;
  r.port.gattOk = false;                               // keeps the slot in DISCOVERING
  r.add();
  assert(notifyEv(r, hr, sizeof(hr)));                 // WAIT_SCAN
  r.update();
  r.match(r.core->scan().epoch());
  r.update();
  assert(r.state() == SlotState::CONNECTING);
  assert(notifyEv(r, hr, sizeof(hr)));
  r.update();
  r.connectedEv(r.gen());
  r.update();
  assert(r.state() == SlotState::DISCOVERING);
  assert(notifyEv(r, hr, sizeof(hr)));
  r.update();
  assert(r.port.relays[kHr] == 0 && r.core->stats().notifies == 0);

  Rig s;
  s.toSubscribed();
  assert(bs::pushNotify(s.core->queue(), bs::kSlotCount, 0, hr, sizeof(hr)));
  assert(bs::pushNotify(s.core->queue(), 0xFF, 0, hr, sizeof(hr)));
  s.update();
  assert(s.port.relays[kHr] == 0 && s.core->stats().notifies == 0);
  assert(notifyEv(s, hr, sizeof(hr)));
  s.update();
  assert(s.port.relays[kHr] == 1 && s.port.relayLen == 2);
  assert(memcmp(s.port.relayData, hr, sizeof(hr)) == 0);
  const BikeSensor::Stats& st = s.core->stats();
  assert(st.notifies == 1 && st.notifiesTruncated == 0 && st.notifiesMalformed == 0);

  cscToSubscribed(s);                                  // routed by slot
  const uint8_t csc[] = {0x02, 10, 0, 0x00, 0x04};
  assert(notifyEv(s, csc, sizeof(csc), kCsc));
  s.update();
  assert(s.port.relays[kCsc] == 1 && s.port.relays[kHr] == 1);
  assert(s.port.relayLen == sizeof(csc) && memcmp(s.port.relayData, csc, sizeof(csc)) == 0);
}

static void testNotifyTruncated() {
  Rig r;
  r.toSubscribed();
  uint8_t big[BBS_NOTIFY_MAX_LEN + 1] = {0x00, 72};    // valid HR prefix
  assert(notifyEv(r, big, sizeof(big)));
  r.update();
  assert(r.port.relays[kHr] == 0);                     // neither parsed nor relayed
  assert(r.core->stats().notifies == 1 && r.core->stats().notifiesTruncated == 1);
  assert(r.core->stats().notifiesMalformed == 0);
  assert(notifyEv(r, big, BBS_NOTIFY_MAX_LEN));        // exactly the limit: not cut
  r.update();
  assert(r.port.relays[kHr] == 1 && r.port.relayLen == BBS_NOTIFY_MAX_LEN);
  assert(r.core->stats().notifiesTruncated == 1);

  cscToSubscribed(r);                                  // a cut CSC packet leaves the crank alone
  const uint8_t first[] = {0x02, 10, 0, 0x00, 0x04};
  assert(notifyEv(r, first, sizeof(first), kCsc));
  r.update();
  assert(r.csc().crank.crankValid && r.csc().crank.prevCrankRev == 10);
  uint8_t cut[BBS_NOTIFY_MAX_LEN + 1] = {0x02, 20, 0, 0x00, 0x08};
  assert(notifyEv(r, cut, sizeof(cut), kCsc));
  r.update();
  assert(r.csc().crank.prevCrankRev == 10 && r.port.relays[kCsc] == 1);
  assert(r.core->stats().notifiesTruncated == 2);
}

static void testNotifyMalformedStillRelayed() {
  Rig r;
  r.toSubscribed();
  const uint8_t shortHr[] = {0x00};
  const uint8_t wideHr[] = {0x01, 72};                 // uint16 flag, value cut
  assert(notifyEv(r, shortHr, sizeof(shortHr)));
  assert(notifyEv(r, wideHr, sizeof(wideHr)));
  assert(notifyEv(r, nullptr, 0));
  r.update();
  assert(r.core->stats().notifiesMalformed == 3 && r.core->stats().notifies == 3);
  assert(r.port.relays[kHr] == 3 && r.port.relayLen == 0);

  cscToSubscribed(r);
  const uint8_t badCsc[] = {0x02, 10, 0, 0x00};        // crank data needs 4 bytes
  assert(notifyEv(r, badCsc, sizeof(badCsc), kCsc));
  r.update();
  assert(r.core->stats().notifiesMalformed == 4 && !r.csc().crank.crankValid);
  assert(r.port.relays[kCsc] == 1 && r.port.relayLen == sizeof(badCsc));
  assert(memcmp(r.port.relayData, badCsc, sizeof(badCsc)) == 0);
  const uint8_t wheelOnly[] = {0x01, 1, 0, 0, 0, 0x00, 0x04};
  assert(notifyEv(r, wheelOnly, sizeof(wheelOnly), kCsc));
  r.update();
  assert(r.core->stats().notifiesMalformed == 4 && r.port.relays[kCsc] == 2);
}

// §3.1: leaving SUBSCRIBED clears the crank state, so a reconnect does not
// produce a cadence jump.
static void testCrankResetOnLinkEnd() {
  Rig r;
  cscToSubscribed(r);
  const uint8_t crank[] = {0x02, 10, 0, 0x00, 0x04};
  assert(notifyEv(r, crank, sizeof(crank), kCsc));
  r.update();
  assert(r.csc().crank.crankValid);
  r.discEv(r.cscGen(), 0x08, kCsc);
  r.update();
  assert(r.cscState() != SlotState::SUBSCRIBED && !r.csc().crank.crankValid);
  assert(r.csc().crank.prevCrankRev == 0);

  Rig s;                                               // missed DISCONNECTED, found by resync
  cscToSubscribed(s);
  assert(notifyEv(s, crank, sizeof(crank), kCsc));
  s.update();
  assert(s.csc().crank.crankValid);
  s.port.linkUp[kCsc] = false;
  s.core->queue().requestResync();
  s.update();
  assert(s.cscState() != SlotState::SUBSCRIBED && !s.csc().crank.crankValid);
}

static void testCentralEvents() {
  Rig r;
  r.port.centrals = 1;
  assert(bs::pushCentralConnect(r.core->queue()));
  r.update();
  assert(r.core->centralCount() == 1 && g_eventCount == 1);
  expectEvent(0, EventType::CENTRAL_CONNECTED, SlotState::EMPTY, 0, SensorType{});
  assert(g_events[0].count == 1);
  r.port.centrals = 2;                                 // the server's count wins
  assert(bs::pushCentralConnect(r.core->queue()));
  r.update();
  assert(r.core->centralCount() == 2 && g_events[1].count == 2);
  r.port.centrals = 1;
  assert(bs::pushCentralDisconnect(r.core->queue(), 0x13));
  r.update();
  assert(r.core->centralCount() == 1 && g_eventCount == 3);
  expectEvent(2, EventType::CENTRAL_DISCONNECTED, SlotState::EMPTY, 0x13, SensorType{});
  assert(g_events[2].count == 1);

  g_eventCount = 0;
  assert(bs::pushSubscribe(r.core->queue(), 1, kHr));
  assert(bs::pushSubscribe(r.core->queue(), 2, kCsc));         // indicate counts too
  assert(bs::pushSubscribe(r.core->queue(), 0, kCsc));
  assert(bs::pushSubscribe(r.core->queue(), 1, bs::kSlotCount));
  assert(bs::pushSubscribe(r.core->queue(), 1, 0xFF));
  r.update();
  assert(g_eventCount == 3 && r.core->centralCount() == 1);
  expectEvent(0, EventType::CENTRAL_SUBSCRIBED, SlotState::EMPTY, 0, SensorType::HEART_RATE);
  expectEvent(1, EventType::CENTRAL_SUBSCRIBED, SlotState::EMPTY, 0, SensorType::SPEED_CADENCE);
  expectEvent(2, EventType::CENTRAL_UNSUBSCRIBED, SlotState::EMPTY, 0,
              SensorType::SPEED_CADENCE);
  assert(r.port.advStarts == 0);                       // no server: nothing to restart
}

// §3.2 step 3: the central count comes from the server, also with CENTRAL_*
// events still queued behind the resync.
static void testCentralCountResync() {
  Rig r;
  r.port.centrals = 2;
  r.core->queue().requestResync();
  r.update();
  assert(r.core->centralCount() == 2 && g_eventCount == 0);

  r.port.centrals = 3;
  assert(bs::pushCentralConnect(r.core->queue()));     // already in the server's count
  for (uint8_t i = 1; i < BBS_EVENT_QUEUE_DEPTH; ++i) bs::pushScanEnd(r.core->queue(), 0, 0);
  assert(!bs::pushScanEnd(r.core->queue(), 0, 0));
  r.update();
  assert(r.core->centralCount() == 3 && g_eventCount == 1 && g_events[0].count == 3);
}

static void testAdvertisingDesiredState() {
  Rig r;
  r.update();
  assert(r.port.advStarts == 0 && r.port.advStops == 0);
  r.port.advOn = true;                                 // NimBLE restarted it on its own (§0.9)
  r.update();
  assert(!r.port.advOn && r.port.advStops == 1 && r.core->stats().advertisingEnforced == 1);

  r.port.serverOk = false;
  assert(!r.core->startServer());
  r.update();
  assert(r.port.advStarts == 0);
  r.port.serverOk = true;
  assert(r.core->startServer() && r.port.serverStarts == 2);
  assert(r.port.advStarts == 0);                       // follows on the next update()
  r.update();
  assert(r.port.advOn && r.port.advStarts == 1);
  r.update();
  assert(r.port.advStarts == 1);
  r.port.advOn = false;                                // a central connected: advertising ended
  r.port.centrals = 1;
  assert(bs::pushCentralConnect(r.core->queue()));
  r.update();
  assert(r.port.advOn && r.port.advStarts == 2);
  r.port.advOn = false;                                // start refused (e.g. no free connection)
  r.port.advStartOk = false;
  const uint32_t failedAt = r.now;
  r.update();
  assert(!r.port.advOn && r.port.advStarts == 3);
  r.update();
  r.at(failedAt + BBS_SCAN_START_BACKOFF_MS - 1);
  assert(r.port.advStarts == 3);                       // held (rev 28)
  r.at(failedAt + BBS_SCAN_START_BACKOFF_MS);
  assert(r.port.advStarts == 4);                       // retried once the hold expires
  r.at(r.now + 1);
  assert(r.port.advStarts == 4);                       // that failure holds again
  assert(r.core->stats().advertisingEnforced == 1 && r.port.advStops == 1);

  r.port.advStartOk = true;                            // NimBLE restarted it while held:
  r.port.advOn = true;                                 // left alone, and the hold ends
  r.update();
  assert(r.port.advStarts == 4 && r.port.advStops == 1);
  r.port.advOn = false;
  r.update();
  assert(r.port.advOn && r.port.advStarts == 5);
}

// Rev 28: the start hold ends early when a connection may be free again.
static void testAdvertisingHoldReleases() {
  struct Release { const char* what; void (*apply)(Rig&); };
  const Release releases[] = {
      {"CENTRAL_DISCONNECT", [](Rig& r) { assert(bs::pushCentralDisconnect(r.core->queue(), 0x13)); }},
      {"sensor DISCONNECTED", [](Rig& r) { r.discEv(r.gen(), 0x08); }},
      {"link-loss CONNECT_FAIL", [](Rig& r) {
         r.port.connected = false;
         r.failEv(r.gen(), 0x3E);
       }},
      {"HOST_RESET", [](Rig& r) { assert(bs::pushHostReset(r.core->queue())); }},
      {"resync", [](Rig& r) { r.core->queue().requestResync(); }},
  };
  for (const Release& rel : releases) {
    Rig r;
    assert(r.core->startServer());                     // idle: before any link (rev 28 guard)
    r.port.advStartOk = false;
    r.toSubscribed();
    assert(r.port.advStarts == 1);                     // failed once, held through the connect
    r.update();
    assert(r.port.advStarts == 1);
    r.port.advStartOk = true;
    rel.apply(r);
    r.update();
    assert(r.port.advStarts == 2 && r.port.advOn);
  }
  {
    Rig r;                                             // stale DISCONNECTED and a plain
    assert(r.core->startServer());                     // CONNECT_FAIL keep the hold
    r.port.advStartOk = false;
    r.toConnecting();
    assert(r.port.advStarts == 1);
    r.port.advStartOk = true;
    r.discEv(static_cast<uint16_t>(r.gen() - 1), 0x08);
    r.update();
    assert(r.port.advStarts == 1);
    r.failEv(r.gen(), 13);
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.port.advStarts == 1);
    assert(bs::pushCentralConnect(r.core->queue()));
    r.update();
    assert(r.port.advStarts == 1);
  }
  {
    Rig r;                                             // CONNECT_FAIL in CANCELLING keeps it
    assert(r.core->startServer());
    r.port.advStartOk = false;
    r.toConnecting();
    assert(r.port.advStarts == 1);
    r.port.advStartOk = true;
    r.remove();
    assert(r.state() == SlotState::CANCELLING);
    r.failEv(r.gen(), 13);
    r.update();
    assert(r.state() == SlotState::EMPTY && r.port.advStarts == 1);
  }
  {
    Rig r;                                             // gen-current DISCONNECTED in CONNECTING
    assert(r.core->startServer());                     // (link lost before onConnect) ends it
    r.port.advStartOk = false;
    r.toConnecting();
    assert(r.port.advStarts == 1);
    r.port.advStartOk = true;
    r.discEv(r.gen(), 0x08);
    r.update();
    assert(r.state() != SlotState::CONNECTING && r.port.advStarts == 2 && r.port.advOn);
  }
  {
    Rig r;                                             // expiry across the millis() wrap
    r.now = UINT32_MAX - 100;
    assert(r.core->startServer());
    r.port.advStartOk = false;
    const uint32_t failedAt = r.now;
    r.update();
    assert(r.port.advStarts == 1);
    r.port.advStartOk = true;
    r.at(failedAt + BBS_SCAN_START_BACKOFF_MS - 1);    // wrapped past 0
    assert(r.now < failedAt && r.port.advStarts == 1);
    r.at(failedAt + BBS_SCAN_START_BACKOFF_MS);
    assert(r.port.advStarts == 2 && r.port.advOn);
  }
}

// Rev 28 guard: the first server start only while the stack is idle
// (ble_gatts_mutable()); NimBLE asserts otherwise.
static void testStartServerIdleGuard() {
  struct Busy { const char* what; void (*apply)(FakePort&, bool); };
  const Busy busy[] = {
      {"advertising", [](FakePort& p, bool on) { p.advOn = on; }},
      {"discovery", [](FakePort& p, bool on) { p.discOn = on; }},
      {"connect", [](FakePort& p, bool on) { p.connOn = on; }},
      {"HR client", [](FakePort& p, bool on) { p.linkUp[kHr] = on; }},
      {"CSC client", [](FakePort& p, bool on) { p.linkUp[kCsc] = on; }},
      {"last slot client", [](FakePort& p, bool on) { p.linkUp[bs::kSlotCount - 1] = on; }},
  };
  for (const Busy& b : busy) {
    Rig r;
    b.apply(r.port, true);
    assert(!r.core->startServer());
    assert(r.port.serverStarts == 0 && r.port.advDataSets == 0);
    r.update();
    assert(r.port.advStarts == 0);
    b.apply(r.port, false);
    assert(r.core->startServer() && r.port.serverStarts == 1 && r.port.advDataSets == 1);
  }
  {
    Rig r;                                             // started once: no guard any more
    r.port.advDataOk = false;
    assert(!r.core->startServer() && r.port.serverStarts == 1);
    r.port.advDataOk = true;
    r.port.discOn = true;
    r.port.linkUp[kHr] = true;
    assert(r.core->startServer() && r.port.serverStarts == 2);
    assert(r.core->startServer() && r.port.serverStarts == 3);
  }
  {
    Rig r;                                             // a failed first start keeps the guard
    r.port.serverOk = false;
    assert(!r.core->startServer() && r.port.serverStarts == 1);
    r.port.serverOk = true;
    r.port.connOn = true;
    assert(!r.core->startServer() && r.port.serverStarts == 1);
  }
}

static void testAdvertisingAfterHostResetAndResync() {
  {
    Rig r;                                             // re-applied before the HOST_RESET event
    r.core->setEventCallback(onEventHook);
    assert(r.core->startServer());
    r.update();
    r.port.advOn = false;
    bool seen = false;
    g_hook = [&r, &seen](const EventInfo& info) {
      if (info.type != EventType::HOST_RESET) return;
      seen = true;
      assert(r.port.advOn && r.port.advStarts == 2);
    };
    assert(bs::pushHostReset(r.core->queue()));
    r.update();
    g_hook = nullptr;
    assert(seen && r.port.advStarts == 2);
  }
  {
    Rig r;                                             // not wanted: stopped before the event
    r.core->setEventCallback(onEventHook);
    r.port.advOn = true;
    bool seen = false;
    g_hook = [&r, &seen](const EventInfo& info) {
      if (info.type != EventType::HOST_RESET) return;
      seen = true;
      assert(!r.port.advOn && r.core->stats().advertisingEnforced == 1);
    };
    assert(bs::pushHostReset(r.core->queue()));
    r.update();
    g_hook = nullptr;
    assert(seen);
  }
  {
    Rig r;                                             // resync enforces before the drain
    r.core->setEventCallback(onEventHook);
    assert(r.core->startServer());
    r.update();
    r.port.advOn = false;
    r.port.centrals = 1;
    bool seen = false;
    g_hook = [&r, &seen](const EventInfo& info) {
      if (info.type != EventType::CENTRAL_CONNECTED) return;
      seen = true;
      assert(r.port.advOn && r.port.advStarts == 2);
    };
    assert(bs::pushCentralConnect(r.core->queue()));
    fillQueueToResync(*r.core);
    r.update();
    g_hook = nullptr;
    assert(seen && r.port.advStarts == 2);
  }
}

// ---- §10 step 5 (rev 26): data path, server types, name, refreshAdvertising ----

static const uint8_t kCscFirst[] = {0x02, 10, 0, 0x00, 0x04};     // revs 10 @ 1 s
static const uint8_t kCsc60[] = {0x02, 11, 0, 0x00, 0x08};        // +1 rev in 1 s: 60 rpm
static const uint8_t kCscWheel[] = {0x01, 1, 0, 0, 0, 0x00, 0x04};

static uint16_t g_hrValues[8], g_cadValues[8];
static int g_hrCalls = 0, g_cadCalls = 0;
static int g_eventsAtCall = -1, g_relaysAtCall = -1;
static const FakePort* g_port = nullptr;

static void onHr(uint16_t bpm) {
  assert(g_hrCalls < 8);
  g_hrValues[g_hrCalls++] = bpm;
  g_eventsAtCall = g_eventCount;
  g_relaysAtCall = g_port->relays[kHr];
}
static void onCadence(uint16_t rpm) {
  assert(g_cadCalls < 8);
  g_cadValues[g_cadCalls++] = rpm;
}
static void resetValues(const Rig& r) {
  g_hrCalls = g_cadCalls = 0;
  g_eventsAtCall = g_relaysAtCall = -1;
  g_port = &r.port;
}

static void testHeartRateData() {
  Rig r;
  r.core->setHeartRateCallback(onHr);
  resetValues(r);
  r.port.gattOk = false;                               // DISCOVERING: no data
  r.add();
  r.update();
  r.match(r.core->scan().epoch());
  r.update();
  r.connectedEv(r.gen());
  r.update();
  assert(r.state() == SlotState::DISCOVERING);
  const uint8_t hr8[] = {0x00, 72};
  assert(notifyEv(r, hr8, sizeof(hr8)));
  r.update();
  assert(g_hrCalls == 0 && countEvents(EventType::DATA) == 0);

  Rig s;
  s.core->setHeartRateCallback(onHr);
  resetValues(s);
  s.toSubscribed();
  g_eventCount = 0;
  const uint8_t hr16[] = {0x01, 0x2C, 0x01};           // 300 bpm
  const uint8_t bad[] = {0x01, 72};
  uint8_t cut[BBS_NOTIFY_MAX_LEN + 1] = {0x00, 99};
  assert(notifyEv(s, hr8, sizeof(hr8)));
  assert(notifyEv(s, bad, sizeof(bad)));               // relayed, no value, no DATA
  assert(notifyEv(s, cut, sizeof(cut)));               // nothing
  assert(notifyEv(s, hr16, sizeof(hr16)));
  s.update();
  assert(g_hrCalls == 2 && g_hrValues[0] == 72 && g_hrValues[1] == 300);
  assert(g_eventCount == 2);
  expectEvent(0, EventType::DATA, SlotState::SUBSCRIBED);
  expectEvent(1, EventType::DATA, SlotState::SUBSCRIBED);
  assert(g_eventsAtCall == 1 && g_relaysAtCall == 3);  // relay → callback → DATA
  s.core->setHeartRateCallback(nullptr);
  assert(notifyEv(s, hr8, sizeof(hr8)));
  s.update();
  assert(g_hrCalls == 2 && countEvents(EventType::DATA) == 3);
}

static void testCadenceData() {
  Rig r;
  r.core->setCadenceCallback(onCadence);
  r.core->setHeartRateCallback(onHr);
  resetValues(r);
  cscToSubscribed(r);
  g_eventCount = 0;
  assert(notifyEv(r, kCscFirst, sizeof(kCscFirst), kCsc));
  r.update();
  assert(g_cadCalls == 0 && countEvents(EventType::DATA) == 1);   // §6: no cadence yet
  expectEvent(0, EventType::DATA, SlotState::SUBSCRIBED, 0, SensorType::SPEED_CADENCE);
  assert(notifyEv(r, kCsc60, sizeof(kCsc60), kCsc));
  assert(notifyEv(r, kCsc60, sizeof(kCsc60), kCsc));             // no new crank event
  assert(notifyEv(r, kCscWheel, sizeof(kCscWheel), kCsc));       // no crank data
  const uint8_t bad[] = {0x02, 12, 0, 0x00};
  assert(notifyEv(r, bad, sizeof(bad), kCsc));
  r.update();
  assert(g_cadCalls == 2 && g_cadValues[0] == 60 && g_cadValues[1] == 0);
  assert(countEvents(EventType::DATA) == 4 && g_hrCalls == 0);

  r.discEv(r.cscGen(), 0x08, kCsc);                    // link end resets the crank
  r.update();
  assert(r.cscState() == SlotState::CONNECTING);
  r.connectedEv(r.cscGen(), kCsc);
  r.update();
  assert(r.cscState() == SlotState::SUBSCRIBED);
  const uint8_t next[] = {0x02, 12, 0, 0x00, 0x0C};
  assert(notifyEv(r, next, sizeof(next), kCsc));
  r.update();
  assert(g_cadCalls == 2);
}

static uint16_t g_getterValue = 0;
static bool g_getterOk = false;
static Core* g_getterCore = nullptr;
static void onHrReadsGetter(uint16_t) {
  uint32_t age = 0;
  g_getterOk = g_getterCore->lastHeartRate(g_getterValue, age, 0);
}

static void testHeartRateGetter() {
  Rig r;
  uint16_t bpm = 1234;
  uint32_t age = 5678;
  assert(!r.core->lastHeartRate(bpm, age, r.now));     // EMPTY
  r.toSubscribed();
  assert(!r.core->lastHeartRate(bpm, age, r.now));     // SUBSCRIBED, nothing delivered
  const uint8_t bad[] = {0x01, 72};
  uint8_t cut[BBS_NOTIFY_MAX_LEN + 1] = {0x00, 99};
  assert(notifyEv(r, bad, sizeof(bad)));
  assert(notifyEv(r, cut, sizeof(cut)));
  r.update();
  assert(!r.core->lastHeartRate(bpm, age, r.now));
  assert(bpm == 1234 && age == 5678);                  // untouched on false

  const uint8_t hr72[] = {0x00, 72};
  const uint32_t t72 = r.now;
  assert(notifyEv(r, hr72, sizeof(hr72)));
  r.update();
  assert(r.core->lastHeartRate(bpm, age, t72 + 500) && bpm == 72 && age == 500);
  r.now += 200;
  assert(notifyEv(r, bad, sizeof(bad)));               // malformed: value and age kept
  assert(notifyEv(r, cut, sizeof(cut)));               // truncated: same
  r.update();
  assert(r.core->lastHeartRate(bpm, age, t72 + 900) && bpm == 72 && age == 900);
  assert(r.core->lastHeartRate(bpm, age, t72 - 3) && age == 0);   // now read before update()

  g_getterCore = r.core.get();                         // stored before the callback runs
  r.core->setHeartRateCallback(onHrReadsGetter);
  const uint8_t hr300[] = {0x01, 0x2C, 0x01};
  assert(notifyEv(r, hr300, sizeof(hr300)));
  r.update();
  assert(g_getterOk && g_getterValue == 300);
  assert(r.core->lastHeartRate(bpm, age, r.now + 10) && bpm == 300 && age == 10);
  r.core->setHeartRateCallback(nullptr);

  r.discEv(r.gen(), 0x08);                             // link end clears
  r.update();
  assert(r.state() == SlotState::CONNECTING);
  assert(!r.core->lastHeartRate(bpm, age, r.now));
  r.connectedEv(r.gen());
  r.update();
  assert(r.state() == SlotState::SUBSCRIBED);
  assert(!r.core->lastHeartRate(bpm, age, r.now));     // not delivered since re-entering
  assert(notifyEv(r, hr72, sizeof(hr72)));
  r.update();
  assert(r.core->lastHeartRate(bpm, age, r.now) && bpm == 72);

  r.port.connected = false;                            // missed DISCONNECTED, found by resync
  r.core->queue().requestResync();
  r.update();
  assert(r.state() != SlotState::SUBSCRIBED && !r.core->lastHeartRate(bpm, age, r.now));

  Rig s;                                               // remove() from SUBSCRIBED
  s.toSubscribed();
  assert(notifyEv(s, hr72, sizeof(hr72)));
  s.update();
  assert(s.core->lastHeartRate(bpm, age, s.now));
  s.remove();
  s.discEv(s.gen(), 0x16);
  s.update();
  assert(s.state() == SlotState::EMPTY && !s.core->lastHeartRate(bpm, age, s.now));
}

static void testCadenceGetter() {
  Rig r;
  uint16_t rpm = 1234;
  uint32_t age = 5678;
  cscToSubscribed(r);
  assert(!r.core->lastCadence(rpm, age, r.now));
  assert(notifyEv(r, kCscFirst, sizeof(kCscFirst), kCsc));
  assert(notifyEv(r, kCscWheel, sizeof(kCscWheel), kCsc));
  r.update();
  assert(!r.core->lastCadence(rpm, age, r.now) && rpm == 1234 && age == 5678);

  const uint32_t t60 = r.now;
  assert(notifyEv(r, kCsc60, sizeof(kCsc60), kCsc));
  r.update();
  assert(r.core->lastCadence(rpm, age, t60 + 300) && rpm == 60 && age == 300);
  r.now = t60 + 1000;                                  // no new crank event: the codec's 0
  assert(notifyEv(r, kCsc60, sizeof(kCsc60), kCsc));
  r.update();
  assert(r.core->lastCadence(rpm, age, t60 + 1100) && rpm == 0 && age == 100);
  r.now = t60 + 2000;                                  // no cadence: value and age kept
  assert(notifyEv(r, kCscWheel, sizeof(kCscWheel), kCsc));
  const uint8_t bad[] = {0x02, 12, 0, 0x00};
  assert(notifyEv(r, bad, sizeof(bad), kCsc));
  r.update();
  assert(r.core->lastCadence(rpm, age, t60 + 2100) && rpm == 0 && age == 1100);
  uint16_t bpm = 0;
  assert(!r.core->lastHeartRate(bpm, age, r.now));     // per slot

  r.discEv(r.cscGen(), 0x08, kCsc);
  r.update();
  assert(!r.core->lastCadence(rpm, age, r.now));

  Rig s(Mode::MAC, 0x01 | 0x04);                       // CSC not in begin(): source none
  assert(!s.core->addByMac(SensorType::SPEED_CADENCE, kCscMacStr, 0, "CSC", s.now));
  assert(!s.core->lastCadence(rpm, age, s.now));
}

// Rev 28: addBy* rejects a type outside the begin() mask.
static void testAddOutsideMask() {
  const SensorType types[] = {SensorType::HEART_RATE, SensorType::SPEED_CADENCE,
                              SensorType::POWER};
  for (SensorType type : types) {
    const uint8_t others = static_cast<uint8_t>(0x07 & ~BikeSensor::typeBit(type));
    const int slot = bs::slotIndex(type);
    Rig r(Mode::MAC, others);
    assert(!r.core->addByMac(type, kOtherMacStr, 0, "X", r.now));
    if (slot >= 0) assert(r.core->slot(static_cast<uint8_t>(slot)).state == SlotState::EMPTY);
    assert(!r.core->scanFilterMatch(kOtherMac));
    Rig n(Mode::NAME, others);
    assert(!n.core->addByName(type, "X", n.now));
    if (slot >= 0) assert(n.core->slot(static_cast<uint8_t>(slot)).state == SlotState::EMPTY);

    Rig ok(Mode::MAC, BikeSensor::typeBit(type));              // in the mask: accepted if compiled
    assert(ok.core->addByMac(type, kOtherMacStr, 0, "X", ok.now) == (slot >= 0));
    Rig okName(Mode::NAME, BikeSensor::typeBit(type));
    assert(okName.core->addByName(type, "X", okName.now) == (slot >= 0));
  }
  Rig r(Mode::MAC, 0x07);
  assert(!r.core->addByMac(SensorType{}, kOtherMacStr, 0, "X", r.now));
  r.update();
  assert(r.port.discStarts == 0);                      // nothing to scan for
}

static void testServerTypes() {
  struct Case { uint8_t mask, local; };
  const uint8_t power = BBS_COMPILE_POWER ? 0x04 : 0x00;   // local CPS only if compiled
  const Case cases[] = {
      {0x07, static_cast<uint8_t>(0x03 | power)},
      {0x01, 0x01}, {0x02, 0x02}, {0x04, power}, {0x00, 0x00},
      {0xF9, 0x01},                                    // unknown bits ignored
  };
  for (const Case& c : cases) {
    Rig r(Mode::MAC, c.mask);
    assert(r.core->startServer());
    assert(r.port.serverMask == c.local && r.port.advMask == c.local);
    assert(r.port.advDataSets == 1 && r.port.advStarts == 0);
    assert(strcmp(r.port.advName, "homewind-da4eb580") == 0);
  }
  Rig r;
  r.port.serverOk = false;
  assert(!r.core->startServer() && r.port.advDataSets == 0);
  r.port.serverOk = true;
  r.port.advDataOk = false;                            // NimBLE refused a field
  assert(!r.core->startServer() && r.port.advDataSets == 1);
  r.update();
  assert(!r.port.advOn && r.port.advStarts == 0);
  assert(!r.core->refreshAdvertising(r.now));               // not started
  r.port.advDataOk = true;
  assert(r.core->startServer() && r.port.serverStarts == 3);
  r.update();
  assert(r.port.advOn);
}

// §5.4 rev 27 budget: flags 3 + name (2 + prefix + 9) + UUIDs (2 + 2n) ≤ 31.
static void testAdvertisingBudget() {
  struct Case { uint8_t mask; const char* fits; const char* tooLong; };
  const Case cases[] = {
      {0x03, "abcdefghijk", "abcdefghijkl"},                 // HR + CSC: 11 / 12
      {0x01, "abcdefghijklm", "abcdefghijklmn"},             // one UUID: 13 / 14
      {0x00, "abcdefghijklmnopq", "abcdefghijklmnopqr"},     // no UUID: 17 / 18
#if BBS_COMPILE_POWER
      {0x07, "abcdefghi", "abcdefghij"},                     // HR + CSC + CPS: 9 / 10
      {0x04, "abcdefghijklm", "abcdefghijklmn"},             // CPS alone: 13 / 14
#endif
  };
  for (const Case& c : cases) {
    Rig ok(Mode::MAC, c.mask);
    ok.core->setNamePrefix(c.fits);
    assert(ok.core->startServer() && ok.port.advDataSets == 1);

    Rig bad(Mode::MAC, c.mask);                        // nothing created, no port call
    bad.core->setNamePrefix(c.tooLong);
    assert(!bad.core->startServer());
    assert(bad.port.serverStarts == 0 && bad.port.advDataSets == 0);
    bad.update();
    assert(bad.port.advStarts == 0);
  }

  Rig r(Mode::MAC, 0x03);                              // refresh keeps the old data on air
  assert(r.core->startServer());
  r.update();
  assert(r.port.advOn && r.port.advStarts == 1);
  r.core->setNamePrefix("abcdefghijkl");
  assert(!r.core->refreshAdvertising(r.now));
  assert(r.port.advOn && r.port.advStops == 0 && r.port.advStarts == 1);
  assert(r.port.advDataSets == 1 && strcmp(r.port.advName, "homewind-da4eb580") == 0);
  r.port.advOn = false;                                // not advertising: stays off
  assert(!r.core->refreshAdvertising(r.now));
  assert(!r.port.advOn && r.port.advStarts == 1 && r.port.advDataSets == 1);
  r.update();                                          // enforcement restarts the old data
  assert(r.port.advOn && r.port.advStarts == 2 && r.port.advDataSets == 1);
  r.core->setNamePrefix("abcdefghijk");
  assert(r.core->refreshAdvertising(r.now));
  assert(r.port.advDataSets == 2 && strcmp(r.port.advName, "abcdefghijk-da4eb580") == 0);
}

// Data callbacks that remove or replace their own slot.
static Rig* g_dataRig = nullptr;
static uint8_t g_dataAction = 0;                       // 1 remove, 2 same-type addByMac
static void onDataReentrant(uint16_t) {
  Rig& r = *g_dataRig;
  if (g_dataAction == 1) {
    r.remove();
  } else if (g_dataAction == 2) {
    r.replace();
  }
  g_dataAction = 0;
}
static void onCadenceReentrant(uint16_t) {
  Rig& r = *g_dataRig;
  if (g_dataAction == 1) {
    r.core->remove(SensorType::SPEED_CADENCE, r.now);
  } else if (g_dataAction == 2) {
    r.port.expect[kCsc] = kOtherMac;
    assert(r.core->addByMac(SensorType::SPEED_CADENCE, kOtherMacStr, 1, "CSC2", r.now));
  }
  g_dataAction = 0;
}

static void testDataCallbackReentrancy() {
  const uint8_t hr[] = {0x00, 72};
  uint16_t v = 0;
  uint32_t age = 0;
  {
    Rig r;                                             // HR remove()
    g_dataRig = &r;
    r.core->setHeartRateCallback(onDataReentrant);
    r.toSubscribed();
    g_eventCount = 0;
    g_dataAction = 1;
    assert(notifyEv(r, hr, sizeof(hr)));
    assert(notifyEv(r, hr, sizeof(hr)));               // same drain, still SUBSCRIBED
    r.update();
    assert(r.port.disconnects == 1 && r.state() == SlotState::SUBSCRIBED);
    assert(countEvents(EventType::DATA) == 2 && r.port.relays[kHr] == 2);
    r.discEv(r.gen(), 0x16);
    r.update();
    assert(r.state() == SlotState::EMPTY && !r.core->lastHeartRate(v, age, r.now));
    assert(!r.core->scanFilterMatch(kMac));
  }
  {
    Rig r;                                             // HR same-type addByMac()
    g_dataRig = &r;
    r.core->setHeartRateCallback(onDataReentrant);
    r.toSubscribed();
    const uint16_t g = r.gen();
    g_dataAction = 2;
    assert(notifyEv(r, hr, sizeof(hr)));
    r.update();
    assert(r.port.disconnects == 1 && countEvents(EventType::DATA) == 1);
    r.discEv(g, 0x16);
    r.update();
    assert(r.state() == SlotState::CONNECTING && r.gen() == g + 1);
    assert(r.port.lastAddrType == 1 && !r.core->lastHeartRate(v, age, r.now));
  }
  {
    Rig r;                                             // cadence remove()
    g_dataRig = &r;
    r.core->setCadenceCallback(onCadenceReentrant);
    cscToSubscribed(r);
    assert(notifyEv(r, kCscFirst, sizeof(kCscFirst), kCsc));
    r.update();
    g_dataAction = 1;
    assert(notifyEv(r, kCsc60, sizeof(kCsc60), kCsc));
    r.update();
    assert(r.port.slotDisconnects[kCsc] == 1 && r.cscState() == SlotState::SUBSCRIBED);
    r.discEv(r.cscGen(), 0x16, kCsc);
    r.update();
    assert(r.cscState() == SlotState::EMPTY && !r.core->lastCadence(v, age, r.now));
    assert(!r.csc().crank.crankValid);
  }
  {
    Rig r;                                             // cadence same-type addByMac()
    g_dataRig = &r;
    r.core->setCadenceCallback(onCadenceReentrant);
    cscToSubscribed(r);
    assert(notifyEv(r, kCscFirst, sizeof(kCscFirst), kCsc));
    r.update();
    const uint16_t g = r.cscGen();
    g_dataAction = 2;
    assert(notifyEv(r, kCsc60, sizeof(kCsc60), kCsc));
    r.update();
    assert(r.port.slotDisconnects[kCsc] == 1);
    r.discEv(g, 0x16, kCsc);
    r.update();
    assert(r.cscState() == SlotState::CONNECTING && r.cscGen() == g + 1);
    assert(r.port.lastConnectSlot == kCsc && r.port.lastAddrType == 1);
    assert(!r.core->lastCadence(v, age, r.now) && !r.csc().crank.crankValid);
  }
}

// Event callback calling refreshAdvertising() inside CENTRAL_CONNECTED / HOST_RESET.
static void testRefreshFromEventCallback() {
  const EventType types[] = {EventType::CENTRAL_CONNECTED, EventType::HOST_RESET};
  for (EventType type : types) {
    Rig r;
    r.core->setEventCallback(onEventHook);
    assert(r.core->startServer());
    r.update();
    r.port.advOn = false;
    r.core->setNamePrefix("trainer");
    bool refreshed = false;
    g_hook = [&r, &refreshed, type](const EventInfo& info) {
      if (info.type != type) return;
      refreshed = r.core->refreshAdvertising(r.now);
    };
    if (type == EventType::HOST_RESET) {
      assert(bs::pushHostReset(r.core->queue()));
    } else {
      assert(bs::pushCentralConnect(r.core->queue()));
    }
    r.update();
    g_hook = nullptr;
    assert(refreshed && r.port.advOn && strcmp(r.port.advName, "trainer-da4eb580") == 0);
    assert(r.port.advDataSets == 2 && r.core->stats().advertisingEnforced == 0);
    // HOST_RESET re-applies before the event, so the refresh stops it once.
    assert(r.port.advStops == (type == EventType::HOST_RESET ? 1 : 0));
    assert(r.port.advStarts == 3 - (type == EventType::HOST_RESET ? 0 : 1));
    r.update();
    assert(r.port.advOn && r.port.advDataSets == 2);
  }
}

// CONNECT_FAIL with link loss in SUBSCRIBED ends the link like DISCONNECTED.
static void testLinkLossClearsMeasurement() {
  const uint8_t hr[] = {0x00, 72};
  uint16_t v = 0;
  uint32_t age = 0;
  Rig r;
  r.toSubscribed();
  assert(notifyEv(r, hr, sizeof(hr)));
  r.update();
  assert(r.core->lastHeartRate(v, age, r.now));
  r.port.connected = false;
  r.failEv(r.gen(), 0x3E);
  r.update();
  assert(r.state() != SlotState::SUBSCRIBED && !r.core->lastHeartRate(v, age, r.now));

  Rig s;
  cscToSubscribed(s);
  assert(notifyEv(s, kCscFirst, sizeof(kCscFirst), kCsc));
  assert(notifyEv(s, kCsc60, sizeof(kCsc60), kCsc));
  s.update();
  assert(s.csc().crank.crankValid && s.core->lastCadence(v, age, s.now));
  s.port.linkUp[kCsc] = false;
  s.failEv(s.cscGen(), 0x3E, kCsc);
  s.update();
  assert(s.cscState() != SlotState::SUBSCRIBED && !s.csc().crank.crankValid);
  assert(s.csc().crank.prevCrankRev == 0 && !s.core->lastCadence(v, age, s.now));
}

// ---- Power wiring, local Feature, cadence source (§10 step 8) ----

// §5.4: local CSC Feature = sensor & 0x0003 after a successful pass; default
// 0x0002 without a readable 0x2A5C and after remove(CSC).
static void testCscFeatureCopy() {
  Rig r;
  r.port.gattRead.hasFeature = true;
  r.port.gattRead.feature = 0x0007;                    // + multiple locations (bit 2)
  r.toSubscribed();                                    // HR: nothing to copy
  assert(r.port.featureSets == 0);
  cscToSubscribed(r);
  assert(r.port.localFeature[kCsc] == 0x0003 && r.port.featureSets == 1);
  r.port.gattRead = bs::GattResult{};                  // reconnect: no readable 0x2A5C
  r.discEv(r.cscGen(), 0x08, kCsc);
  r.update();
  r.connectedEv(r.cscGen(), kCsc);
  r.update();
  assert(r.cscState() == SlotState::SUBSCRIBED);
  assert(r.port.localFeature[kCsc] == 0x0002 && r.port.featureSets == 2);
  r.port.gattRead.hasFeature = true;                   // failed pass: nothing copied
  r.port.gattRead.feature = 0x0001;
  r.port.gattOk = false;
  r.discEv(r.cscGen(), 0x08, kCsc);
  r.update();
  r.connectedEv(r.cscGen(), kCsc);
  r.update();
  assert(r.cscState() == SlotState::DISCOVERING && r.port.featureSets == 2);

  Rig s;                                               // remove(CSC) restores the default at once
  s.port.gattRead.hasFeature = true;
  s.port.gattRead.feature = 0x0001;
  cscToSubscribed(s);
  assert(s.port.localFeature[kCsc] == 0x0001);
  s.core->remove(SensorType::SPEED_CADENCE, s.now);
  assert(s.port.localFeature[kCsc] == 0x0002 && s.port.featureSets == 2);
  s.discEv(s.cscGen(), 0x16, kCsc);
  s.update();
  s.core->remove(SensorType::SPEED_CADENCE, s.now);    // EMPTY: no-op
  assert(s.cscState() == SlotState::EMPTY && s.port.featureSets == 2);
}

#if BBS_COMPILE_POWER
static const uint8_t kPowerSlot = static_cast<uint8_t>(bs::slotIndex(SensorType::POWER));
static const uint8_t kCpsPlain[] = {0x00, 0x00, 200, 0};   // 200 W, no crank data

static void powerToSubscribed(Rig& r) {
  r.port.expect[kPowerSlot] = kPowerMac;
  assert(r.core->addByMac(SensorType::POWER, kPowerMacStr, 0, "PWR", r.now));
  r.update();
  r.match(r.core->scan().epoch(), kPowerMac);
  r.update();
  assert(r.core->slot(kPowerSlot).state == SlotState::CONNECTING);
  assert(!r.core->providesCadence());                  // not SUBSCRIBED yet
  r.connectedEv(r.core->attemptGen(kPowerSlot), kPowerSlot);
  r.update();
  assert(r.core->slot(kPowerSlot).state == SlotState::SUBSCRIBED);
}

// Flags 0x0020 (crank data), 200 W, crank revs and event time (1/1024 s).
static bool crankEv(Rig& r, uint16_t revs, uint16_t t) {
  const uint8_t p[] = {0x20, 0x00, 200, 0, static_cast<uint8_t>(revs), static_cast<uint8_t>(revs >> 8),
                       static_cast<uint8_t>(t), static_cast<uint8_t>(t >> 8)};
  return notifyEv(r, p, sizeof(p), kPowerSlot);
}

// providesCadence() inside the last recorded event of that type and sensor; -1: none.
static int providesAt(EventType type, SensorType sensorType) {
  for (int i = g_eventCount - 1; i >= 0; --i) {
    if (g_events[i].type == type && g_events[i].sensorType == sensorType) {
      return g_events[i].providesCadence ? 1 : 0;
    }
  }
  return -1;
}

static BikeSensor::PowerSample g_powerSamples[8];
static int g_powerCalls = 0, g_cadAtPower = -1;
static void onPower(const BikeSensor::PowerSample& s) {
  assert(g_powerCalls < 8);
  g_powerSamples[g_powerCalls++] = s;
  g_eventsAtCall = g_eventCount;
  g_relaysAtCall = g_port->relays[kPowerSlot];
  g_cadAtPower = g_cadCalls;
}

// §5.1 NOTIFY for Power: counted, truncated → nothing, malformed → raw relay
// only; otherwise relay → power callback → cadence callback → DATA.
static void testPowerNotify() {
  Rig r;
  resetValues(r);
  g_powerCalls = 0;
  r.core->setPowerCallback(onPower);
  r.core->setCadenceCallback(onCadence);
  powerToSubscribed(r);
  g_eventCount = 0;
  uint8_t cut[BBS_NOTIFY_MAX_LEN + 1] = {0x00, 0x00, 100, 0};
  const uint8_t bad[] = {0x20, 0x00, 100, 0, 10, 0, 0x00};   // crank field cut by one byte
  assert(notifyEv(r, cut, sizeof(cut), kPowerSlot));
  assert(notifyEv(r, bad, sizeof(bad), kPowerSlot));
  r.update();
  BikeSensor::Stats st = r.core->stats();
  assert(st.notifies == 2 && st.notifiesTruncated == 1 && st.notifiesMalformed == 1);
  assert(r.port.relays[kPowerSlot] == 1 && r.port.relayLen == sizeof(bad));
  assert(memcmp(r.port.relayData, bad, sizeof(bad)) == 0);
  assert(g_powerCalls == 0 && g_eventCount == 0 && !r.core->slot(kPowerSlot).crank.crankValid);
  BikeSensor::PowerSample s{};
  uint32_t age = 0;
  assert(!r.core->lastPower(s, age, r.now));

  // Balance (left reference), crank, energy: flags 0x0823, 300 W.
  const uint8_t full[] = {0x23, 0x08, 0x2C, 0x01, 100, 10, 0, 0x00, 0x04, 0x34, 0x12};
  const uint32_t t = r.now;
  assert(notifyEv(r, full, sizeof(full), kPowerSlot));
  r.update();
  assert(g_powerCalls == 1 && g_cadCalls == 0);        // first crank packet: no cadence
  const BikeSensor::PowerSample& p = g_powerSamples[0];
  assert(p.watts == 300 && p.balance == 100 && p.balanceIsLeft && p.flags == 0x0823);
  assert(p.hasEnergy && p.energyKJ == 0x1234 && !p.hasCadence && p.cadence == 0);
  assert(g_relaysAtCall == 2 && g_eventsAtCall == 0 && g_eventCount == 1);
  expectEvent(0, EventType::DATA, SlotState::SUBSCRIBED, 0, SensorType::POWER);
  assert(r.core->lastPower(s, age, t + 40) && s.watts == 300 && s.balance == 100 && age == 40);
  st = r.core->stats();
  assert(st.notifies == 3 && st.notifiesMalformed == 1);
  assert(r.port.relays[kCsc] == 0 && r.port.relays[kHr] == 0);

  const uint8_t neg[] = {0x00, 0x00, 0xF6, 0xFF};      // -10 W, nothing else
  const uint8_t crank60[] = {0x20, 0x00, 0x2C, 0x01, 11, 0, 0x00, 0x08};
  assert(notifyEv(r, neg, sizeof(neg), kPowerSlot));
  assert(notifyEv(r, crank60, sizeof(crank60), kPowerSlot));
  r.update();
  assert(g_powerCalls == 3 && g_cadCalls == 1 && g_cadValues[0] == 60);
  assert(g_powerSamples[1].watts == -10 && g_powerSamples[1].balance == 0xFF);
  assert(!g_powerSamples[1].hasEnergy && !g_powerSamples[1].balanceIsLeft);
  assert(g_powerSamples[2].hasCadence && g_powerSamples[2].cadence == 60);
  assert(g_cadAtPower == 0 && countEvents(EventType::DATA) == 3);   // power before cadence
  r.core->setPowerCallback(nullptr);
  assert(notifyEv(r, neg, sizeof(neg), kPowerSlot));
  r.update();
  assert(g_powerCalls == 3 && countEvents(EventType::DATA) == 4);
}

// §5.4: local CP Feature = meter & 0x000301FF, Location copied; defaults
// 0 / 0x0D without a read and after remove(POWER).
static void testPowerFeatureCopy() {
  {
    Rig r;
    r.port.gattRead.hasFeature = true;
    r.port.gattRead.feature = 0xFFFFFFFF;
    r.port.gattRead.hasLocation = true;
    r.port.gattRead.location = 5;                      // left crank
    powerToSubscribed(r);
    assert(r.port.localFeature[kPowerSlot] == 0x000301FF && r.port.localLocation == 5);
    assert(r.port.featureSets == 1);
    r.core->remove(SensorType::POWER, r.now);          // defaults at once
    assert(r.port.localFeature[kPowerSlot] == 0 && r.port.localLocation == 0x0D);
    r.discEv(r.core->attemptGen(kPowerSlot), 0x16, kPowerSlot);
    r.update();
    r.core->remove(SensorType::POWER, r.now);          // EMPTY: no-op
    assert(r.core->slot(kPowerSlot).state == SlotState::EMPTY && r.port.featureSets == 2);
  }
  {
    Rig r;                                             // Feature only: Location default
    r.port.gattRead.hasFeature = true;
    r.port.gattRead.feature = 0x0000000C;
    r.port.gattRead.location = 5;
    powerToSubscribed(r);
    assert(r.port.localFeature[kPowerSlot] == 0x0C && r.port.localLocation == 0x0D);
  }
  {
    Rig r;                                             // nothing readable: defaults
    powerToSubscribed(r);
    assert(r.port.localFeature[kPowerSlot] == 0 && r.port.localLocation == 0x0D);
  }
  {
    Rig r;                                             // failed pass: nothing copied
    r.port.gattRead.hasFeature = true;
    r.port.gattRead.feature = 0x3;
    r.port.gattOk = false;
    r.port.expect[kPowerSlot] = kPowerMac;
    assert(r.core->addByMac(SensorType::POWER, kPowerMacStr, 0, "PWR", r.now));
    r.update();
    r.match(r.core->scan().epoch(), kPowerMac);
    r.update();
    r.connectedEv(r.core->attemptGen(kPowerSlot), kPowerSlot);
    r.update();
    assert(r.core->slot(kPowerSlot).state == SlotState::DISCOVERING);
    assert(r.port.gattPasses == 1 && r.port.featureSets == 0);
  }
}

// §6b cadence callback with Power as the source: only for a new crank delta;
// one 0 after BBS_POWER_STALE_MS and one when the source leaves Power.
static void testPowerCadenceCallback() {
  Rig r;                                               // CSC EMPTY
  resetValues(r);
  r.core->setCadenceCallback(onCadence);
  powerToSubscribed(r);
  assert(r.core->providesCadence());
  assert(notifyEv(r, kCpsPlain, sizeof(kCpsPlain), kPowerSlot));   // no bit 5
  assert(crankEv(r, 10, 1024));                        // first crank packet: no delta
  r.update();
  assert(g_cadCalls == 0 && r.core->slot(kPowerSlot).crank.crankValid);
  const uint32_t t0 = r.now;
  assert(crankEv(r, 11, 2048));                        // 60 rpm
  assert(crankEv(r, 11, 2048));                        // no new crank event
  assert(notifyEv(r, kCpsPlain, sizeof(kCpsPlain), kPowerSlot));
  r.update();
  assert(g_cadCalls == 1 && g_cadValues[0] == 60);
  r.at(t0 + BBS_POWER_STALE_MS);                       // not stale yet
  assert(g_cadCalls == 1);
  r.at(t0 + BBS_POWER_STALE_MS + 1);                   // one 0, without a notify
  assert(g_cadCalls == 2 && g_cadValues[1] == 0);
  r.at(r.now + 1000);
  assert(crankEv(r, 11, 2048));
  r.update();
  assert(g_cadCalls == 2);
  assert(crankEv(r, 13, 3072));                        // +2 revs in 1 s: 120 rpm
  r.update();
  assert(g_cadCalls == 3 && g_cadValues[2] == 120);
  assert(r.port.relays[kCsc] == 0 && r.port.relays[kPowerSlot] == 7);   // local CSC silent

  r.discEv(r.core->attemptGen(kPowerSlot), 0x08, kPowerSlot);   // source leaves Power: one 0
  r.update();
  assert(g_cadCalls == 4 && g_cadValues[3] == 0 && !r.core->providesCadence());
  r.at(r.now + BBS_POWER_STALE_MS + 1);
  assert(g_cadCalls == 4);
  assert(r.core->slot(kPowerSlot).state == SlotState::CONNECTING);   // direct attempt
  r.connectedEv(r.core->attemptGen(kPowerSlot), kPowerSlot);
  r.update();
  assert(r.core->providesCadence());
  assert(crankEv(r, 20, 5000));                        // crank reset: first packet, no cadence
  r.update();
  assert(g_cadCalls == 4);
  assert(crankEv(r, 21, 6024));
  r.update();
  assert(g_cadCalls == 5 && g_cadValues[4] == 60);
  r.at(r.now + BBS_POWER_STALE_MS + 1);
  assert(g_cadCalls == 6 && g_cadValues[5] == 0);
  assert(crankEv(r, 22, static_cast<uint16_t>(6024 + 62000)));   // 1 rev in > 60 s: 0 rpm
  r.update();
  assert(g_cadCalls == 7 && g_cadValues[6] == 0);
  r.at(r.now + BBS_POWER_STALE_MS + 1);                // a reported 0 does not arm "once"
  assert(g_cadCalls == 7);
}

// §6b source rule and switching.
static void testCadenceSource() {
  uint16_t rpm = 0;
  uint32_t age = 0;
  {
    Rig r;                                             // CSC EMPTY → Power once SUBSCRIBED
    resetValues(r);
    r.core->setCadenceCallback(onCadence);
    assert(!r.core->providesCadence() && !r.core->lastCadence(rpm, age, r.now));
    powerToSubscribed(r);
    assert(r.core->providesCadence() && !r.core->lastCadence(rpm, age, r.now));
    assert(crankEv(r, 10, 1024));
    assert(crankEv(r, 11, 2048));
    r.update();
    assert(g_cadCalls == 1 && r.core->lastCadence(rpm, age, r.now) && rpm == 60);
    r.addCsc();                                        // a configured CSC wins at once
    assert(!r.core->providesCadence() && !r.core->lastCadence(rpm, age, r.now));
    assert(crankEv(r, 12, 3072));                      // Power data, no cadence callback
    r.update();
    assert(g_cadCalls == 2 && g_cadValues[1] == 0);    // the 0 for the source leaving Power
    BikeSensor::PowerSample s{};
    assert(r.core->lastPower(s, age, r.now) && s.hasCadence && s.cadence == 60);
    assert(r.cscState() == SlotState::WAIT_SCAN && r.port.relays[kCsc] == 0);
    r.core->remove(SensorType::SPEED_CADENCE, r.now);  // back to Power at once
    assert(r.core->providesCadence() && r.core->lastCadence(rpm, age, r.now) && rpm == 60);
    r.update();
    assert(g_cadCalls == 2);                           // no 0: nothing armed

    r.now += BBS_SLEEP_SCAN_DURATION_MS + BBS_SLEEP_SCAN_INTERVAL_MS;   // next sleep window
    cscToSubscribed(r);                                // CSC and Power SUBSCRIBED: CSC
    assert(!r.core->providesCadence());
    assert(crankEv(r, 13, 4096));
    assert(notifyEv(r, kCscFirst, sizeof(kCscFirst), kCsc));
    assert(notifyEv(r, kCsc60, sizeof(kCsc60), kCsc));
    r.update();
    assert(g_cadCalls == 3 && g_cadValues[2] == 60 && r.port.relays[kCsc] == 2);
    assert(r.core->lastCadence(rpm, age, r.now) && rpm == 60);
    r.discEv(r.cscGen(), 0x08, kCsc);                  // reconnecting CSC still wins
    r.update();
    assert(r.cscState() != SlotState::SUBSCRIBED && !r.core->providesCadence());
    assert(!r.core->lastCadence(rpm, age, r.now));
  }
  {
    // CSC not in the begin() mask; also CSC not compiled (enabled_ is mask ∩
    // compiled, so both lack the CSC bit): Power is the source.
    Rig r(Mode::MAC, 0x05);
    resetValues(r);
    r.core->setCadenceCallback(onCadence);
    assert(!r.core->addByMac(SensorType::SPEED_CADENCE, kCscMacStr, 0, "CSC", r.now));
    powerToSubscribed(r);
    assert(r.core->providesCadence());
    assert(crankEv(r, 10, 1024));
    assert(crankEv(r, 11, 2048));
    r.update();
    assert(g_cadCalls == 1 && g_cadValues[0] == 60);
    assert(r.core->lastCadence(rpm, age, r.now) && rpm == 60);
    r.port.linkUp[kPowerSlot] = false;                 // missed DISCONNECTED, found by resync
    r.core->queue().requestResync();
    r.update();
    assert(!r.core->providesCadence());
    assert(providesAt(EventType::SENSOR_DISCONNECTED, SensorType::POWER) == 0);
  }
  {
    Rig r;                                             // the source is current inside the events
    powerToSubscribed(r);
    assert(providesAt(EventType::SENSOR_CONNECTED, SensorType::POWER) == 1);
    r.now += BBS_SLEEP_SCAN_DURATION_MS + BBS_SLEEP_SCAN_INTERVAL_MS;
    cscToSubscribed(r);
    r.core->remove(SensorType::SPEED_CADENCE, r.now);  // from SUBSCRIBED: CSC until EMPTY
    assert(!r.core->providesCadence());
    r.discEv(r.cscGen(), 0x16, kCsc);
    r.update();
    assert(r.cscState() == SlotState::EMPTY);
    assert(providesAt(EventType::SENSOR_DISCONNECTED, SensorType::SPEED_CADENCE) == 1);
    r.discEv(r.core->attemptGen(kPowerSlot), 0x08, kPowerSlot);
    r.update();
    assert(providesAt(EventType::SENSOR_DISCONNECTED, SensorType::POWER) == 0);
  }
}

// Getters: stale rule at the caller's now, now read before update(), ages.
static void testPowerGetters() {
  Rig r;
  powerToSubscribed(r);
  assert(crankEv(r, 10, 1024));
  r.update();
  r.now += 100;
  const uint32_t t = r.now;
  assert(crankEv(r, 11, 2048));
  r.update();
  BikeSensor::PowerSample s{};
  uint32_t age = 0;
  uint16_t rpm = 0;
  assert(r.core->lastPower(s, age, t - 5) && age == 0 && s.cadence == 60);   // no wrap
  assert(r.core->lastCadence(rpm, age, t - 5) && rpm == 60 && age == 0);
  assert(r.core->lastPower(s, age, t + BBS_POWER_STALE_MS) && s.cadence == 60);
  assert(r.core->lastPower(s, age, t + BBS_POWER_STALE_MS + 1) && s.cadence == 0 && s.hasCadence);
  assert(r.core->lastCadence(rpm, age, t + BBS_POWER_STALE_MS + 1) && rpm == 0);
  assert(age == BBS_POWER_STALE_MS + 1);
  r.now = t + 1000;                                    // packet age vs crank-event age
  assert(notifyEv(r, kCpsPlain, sizeof(kCpsPlain), kPowerSlot));
  r.update();
  assert(r.core->lastPower(s, age, t + 1500) && age == 500 && s.cadence == 60);
  assert(r.core->lastCadence(rpm, age, t + 1500) && age == 1500 && rpm == 60);
  r.discEv(r.core->attemptGen(kPowerSlot), 0x08, kPowerSlot);   // link end clears
  r.update();
  assert(!r.core->lastPower(s, age, r.now) && !r.core->lastCadence(rpm, age, r.now));
  assert(!r.core->slot(kPowerSlot).crank.crankValid);
  r.connectedEv(r.core->attemptGen(kPowerSlot), kPowerSlot);   // no cadence carried over
  r.update();
  assert(notifyEv(r, kCpsPlain, sizeof(kCpsPlain), kPowerSlot));
  r.update();
  assert(r.core->lastPower(s, age, r.now) && !s.hasCadence && s.cadence == 0);
  assert(!r.core->lastCadence(rpm, age, r.now));
}

// §4 / §7.5: the meter's MAC or name cannot be added for another type; a
// SCAN_MATCH for its address is ignored by another slot.
static void testPowerDuplicates() {
  {
    Rig r;
    assert(r.core->addByMac(SensorType::POWER, kPowerMacStr, 0, "PWR", r.now));
    assert(!r.core->addByMac(SensorType::SPEED_CADENCE, kPowerMacStr, 0, "CSC", r.now));
    assert(!r.core->addByMac(SensorType::HEART_RATE, "AA:BB:CC:DD:EE:04", 1, nullptr, r.now));
    assert(r.cscState() == SlotState::EMPTY && r.state() == SlotState::EMPTY);
    r.addCsc();
    assert(!r.core->addByMac(SensorType::POWER, kCscMacStr, 0, "PWR", r.now));
    assert(memcmp(r.core->slot(kPowerSlot).mac, kPowerMac, 6) == 0);
    r.core->remove(SensorType::POWER, r.now);
    r.core->remove(SensorType::SPEED_CADENCE, r.now);
    assert(r.core->addByMac(SensorType::SPEED_CADENCE, kPowerMacStr, 0, "CSC", r.now));
  }
  {
    Rig r(Mode::NAME);
    r.port.expect[kPowerSlot] = kPowerMac;
    assert(r.core->addByName(SensorType::POWER, "Assioma", r.now));
    assert(!r.core->addByName(SensorType::SPEED_CADENCE, "Assioma", r.now));
    r.update();
    assert(r.report(kPowerMac, "Assioma"));
    r.update();
    r.connectedEv(r.core->attemptGen(kPowerSlot), kPowerSlot);
    r.update();
    assert(r.core->slot(kPowerSlot).state == SlotState::SUBSCRIBED);
    assert(r.core->addByName(SensorType::SPEED_CADENCE, "Assio", r.now));   // its shortened name
    r.update();
    assert(r.core->scan().owner() == ScanOwner::NAME_SCAN);
    assert(r.report(kPowerMac, "Assio"));
    r.update();
    assert(r.cscState() == SlotState::IDLE && countEvents(EventType::SENSOR_FOUND) == 1);
  }
}

#if BBS_SENSOR_SECURITY
// As powerToSubscribed(), with a pass that fails: the slot stays DISCOVERING.
static void powerToSubscribedFails(Rig& r) {
  r.port.expect[kPowerSlot] = kPowerMac;
  assert(r.core->addByMac(SensorType::POWER, kPowerMacStr, 0, "PWR", r.now));
  r.update();
  r.match(r.core->scan().epoch(), kPowerMac);
  r.update();
  r.connectedEv(r.core->attemptGen(kPowerSlot), kPowerSlot);
  r.update();
  assert(r.core->slot(kPowerSlot).state == SlotState::DISCOVERING);
}

// Power slot after a hard fail: next sleep window, match, connect, one pass.
static void powerRetry(Rig& r) {
  r.at(r.now + 40000);
  assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW);
  r.match(r.core->scan().epoch(), kPowerMac);
  r.update();
  assert(r.core->slot(kPowerSlot).state == SlotState::CONNECTING);
  r.connectedEv(r.core->attemptGen(kPowerSlot), kPowerSlot);
  r.update();
}
#endif

// §5.2 Power steps 1 and 6, §6a: proactive security only with
// BBS_SENSOR_SECURITY; encryption/bond state in info() while SUBSCRIBED;
// securityOk/Fails per successful pass.
static void testPowerSecurity() {
  Rig r;
  r.port.gattRead.encrypted = true;
  r.port.gattRead.bonded = true;
  powerToSubscribed(r);
  assert(r.port.lastSecure == (BBS_SENSOR_SECURITY != 0));
  BikeSensor::SensorInfo info{};
  assert(r.core->info(SensorType::POWER, info) && info.encrypted && info.bonded);
  assert(info.state == SlotState::SUBSCRIBED && info.providesCadence);
  assert(r.core->stats().securityOk == (BBS_SENSOR_SECURITY ? 1u : 0u));
  assert(r.core->stats().securityFails == 0);
  r.discEv(r.core->attemptGen(kPowerSlot), 0x08, kPowerSlot);
  r.update();
  assert(r.core->info(SensorType::POWER, info) && !info.encrypted && !info.bonded);
  assert(!info.providesCadence && !info.hasBattery);
  r.port.gattRead.encrypted = r.port.gattRead.bonded = false;   // pairing refused (Tacx)
  assert(r.core->slot(kPowerSlot).state == SlotState::CONNECTING);   // direct attempt
  r.connectedEv(r.core->attemptGen(kPowerSlot), kPowerSlot);
  r.update();
  assert(r.core->slot(kPowerSlot).state == SlotState::SUBSCRIBED);  // unencrypted is not fatal
  assert(r.core->stats().securityFails == (BBS_SENSOR_SECURITY ? 1u : 0u));
  assert(r.core->stats().securityOk == (BBS_SENSOR_SECURITY ? 1u : 0u));
  // HR and CSC passes never secure (the fake asserts it), and count nothing.
  r.now += BBS_SLEEP_SCAN_DURATION_MS + BBS_SLEEP_SCAN_INTERVAL_MS;
  cscToSubscribed(r);
  assert(!r.port.lastSecure && r.port.securePasses == (BBS_SENSOR_SECURITY ? 2 : 0));
  assert(r.core->stats().securityOk + r.core->stats().securityFails ==
         (BBS_SENSOR_SECURITY ? 2u : 0u));
  r.port.gattRead.encrypted = true;                    // HR: the Core does not count it
  r.now += BBS_SLEEP_SCAN_DURATION_MS + BBS_SLEEP_SCAN_INTERVAL_MS;
  r.toSubscribed();
  assert(r.core->stats().securityOk == (BBS_SENSOR_SECURITY ? 1u : 0u));
}

#if BBS_SENSOR_SECURITY
// §5.2: a watchdog abort while step 1 runs sets skipProactiveSecure; later
// passes skip step 1 until remove()/addBy* clear it.
static void testSkipProactiveSecure() {
  const auto abortInSecure = [](Rig& r) {
    r.port.gattOk = false;
    r.port.gattRead.secureCut = true;
    r.port.duringGatt = [&r] { assert(r.core->watchdog().onTimer()); };
  };
  const auto healthy = [](Rig& r) {
    r.port.gattOk = true;
    r.port.gattRead.secureCut = false;
    r.port.duringGatt = nullptr;
  };
  {
    Rig r;
    abortInSecure(r);
    powerToSubscribedFails(r);
    assert(r.port.lastSecure && r.core->slot(kPowerSlot).skipProactiveSecure);
    assert(r.core->stats().gattBudgetAborts == 1);
    r.discEv(r.core->attemptGen(kPowerSlot), 0x16, kPowerSlot);
    r.update();
    assert(countEvents(EventType::SENSOR_FAILED) == 1);
    healthy(r);
    powerRetry(r);
    assert(!r.port.lastSecure && r.port.securePasses == 1);   // on-demand pairing only
    assert(r.core->slot(kPowerSlot).state == SlotState::SUBSCRIBED);
    r.discEv(r.core->attemptGen(kPowerSlot), 0x08, kPowerSlot);   // link loss keeps it
    r.update();
    assert(r.core->slot(kPowerSlot).skipProactiveSecure);
    r.connectedEv(r.core->attemptGen(kPowerSlot), kPowerSlot);
    r.update();
    assert(!r.port.lastSecure && r.port.securePasses == 1);
    r.core->remove(SensorType::POWER, r.now);           // from SUBSCRIBED: cleared at EMPTY
    r.discEv(r.core->attemptGen(kPowerSlot), 0x16, kPowerSlot);
    r.update();
    assert(r.core->slot(kPowerSlot).state == SlotState::EMPTY);
    assert(!r.core->slot(kPowerSlot).skipProactiveSecure);
    r.now += 40000;
    powerToSubscribed(r);
    assert(r.port.lastSecure && r.port.securePasses == 2);
  }
  for (int viaRemove = 0; viaRemove < 2; ++viaRemove) {
    Rig r;                                             // same-type addByMac replace, remove()
    abortInSecure(r);
    powerToSubscribedFails(r);
    r.discEv(r.core->attemptGen(kPowerSlot), 0x16, kPowerSlot);
    r.update();
    assert(r.core->slot(kPowerSlot).state == SlotState::WAIT_SCAN);
    assert(r.core->slot(kPowerSlot).skipProactiveSecure);
    if (viaRemove) {
      r.core->remove(SensorType::POWER, r.now);
      assert(r.core->slot(kPowerSlot).state == SlotState::EMPTY);
    } else {
      assert(r.core->addByMac(SensorType::POWER, kPowerMacStr, 0, "PWR", r.now));
      assert(r.core->slot(kPowerSlot).state == SlotState::FOUND);
    }
    assert(!r.core->slot(kPowerSlot).skipProactiveSecure);
  }
  {
    Rig r;                                             // not set: abort after step 1, or a
    r.port.gattOk = false;                             // link lost in step 1 without the watchdog
    r.port.duringGatt = [&r] { assert(r.core->watchdog().onTimer()); };
    powerToSubscribedFails(r);
    assert(r.core->stats().gattBudgetAborts == 1 && !r.core->slot(kPowerSlot).skipProactiveSecure);
    r.discEv(r.core->attemptGen(kPowerSlot), 0x16, kPowerSlot);
    r.update();
    r.port.duringGatt = nullptr;
    r.port.gattRead.secureCut = true;
    powerRetry(r);
    assert(r.port.lastSecure && !r.core->slot(kPowerSlot).skipProactiveSecure);
  }
  {
    Rig r(Mode::NAME);                                 // addByName (replace) clears it
    r.port.expect[kPowerSlot] = kPowerMac;
    abortInSecure(r);
    assert(r.core->addByName(SensorType::POWER, "Assioma", r.now));
    r.update();
    assert(r.report(kPowerMac, "Assioma"));
    r.update();
    r.connectedEv(r.core->attemptGen(kPowerSlot), kPowerSlot);
    r.update();
    assert(r.core->slot(kPowerSlot).skipProactiveSecure);
    assert(r.core->addByName(SensorType::POWER, "Assioma", r.now));
    assert(!r.core->slot(kPowerSlot).skipProactiveSecure);
  }
}
#endif
#else
static void onPowerNever(const BikeSensor::PowerSample&) { assert(false); }

// §4: lastPower/setPowerCallback exist without Power; false / never called.
static void testPowerAbsent() {
  Rig r;
  r.core->setPowerCallback(onPowerNever);
  r.toSubscribed();
  const uint8_t hr[] = {0x00, 72};
  assert(notifyEv(r, hr, sizeof(hr)));
  r.update();
  BikeSensor::PowerSample s{};
  uint32_t age = 0;
  assert(!r.core->lastPower(s, age, r.now) && !r.core->providesCadence());
}
#endif

// 2.x BBSIdentity::SetDeviceNamePrefix + GetBleDeviceNameLower, String ops
// replaced one to one.
static std::string legacyName(const char* prefix, uint32_t id32) {
  std::string p;
  if (prefix == nullptr || strlen(prefix) == 0) {
    p = "homewind";
  } else {
    p = prefix;
    for (char& c : p) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (char& c : p) {
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) c = '-';
    }
    size_t pos;
    while ((pos = p.find("--")) != std::string::npos) p.erase(pos, 1);
    while (!p.empty() && p.front() == '-') p.erase(0, 1);
    while (!p.empty() && p.back() == '-') p.pop_back();
    if (p.empty()) p = "homewind";
  }
  char name[48];
  snprintf(name, sizeof(name), "%s-%08lx", p.c_str(), static_cast<unsigned long>(id32));
  return name;
}

static void testServerName() {
  Rig r;
  char buf[bs::kServerNameSize];
  r.core->serverName(buf, sizeof(buf));
  assert(strcmp(buf, "homewind-da4eb580") == 0);
  r.port.chip = 0x1;
  r.core->serverName(buf, sizeof(buf));
  assert(strcmp(buf, "homewind-00000001") == 0);
  char small[6];
  memset(small, 'x', sizeof(small));
  r.core->serverName(small, sizeof(small));
  assert(strcmp(small, "homew") == 0);
  char one = 'x';
  r.core->serverName(&one, 0);
  assert(one == 'x');
  r.core->serverName(nullptr, 10);

  const char* fixed[] = {nullptr, "", "-", "HomeWind", "My Bike!", "--a--b--", "a_b c",
                         "x\x80y\xffz", "Zwift-Hub-01",
                         "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnop",
                         "abcdefghijklmnopqrstuvwxyz0123456789abcdefghij----x"};
  for (const char* p : fixed) {
    r.core->setNamePrefix(p);
    r.core->serverName(buf, sizeof(buf));
    assert(legacyName(p, r.port.chip) == buf);
  }
  const char alphabet[] = "aZ09-_ .\x80\xff";
  uint32_t seed = 12345;
  for (int i = 0; i < 20000; ++i) {
    char p[64];
    seed = seed * 1103515245u + 12345u;
    const size_t len = (seed >> 16) % sizeof(p);
    for (size_t k = 0; k < len; ++k) {
      seed = seed * 1103515245u + 12345u;
      p[k] = alphabet[(seed >> 16) % (sizeof(alphabet) - 1)];
    }
    p[len] = '\0';
    r.port.chip = seed;
    r.core->setNamePrefix(p);
    r.core->serverName(buf, sizeof(buf));
    assert(legacyName(p, r.port.chip) == buf);
  }
}

static void testRefreshAdvertising() {
  Rig r;
  r.core->setNamePrefix("Bike");
  assert(!r.core->refreshAdvertising(r.now));               // no server yet
  assert(r.port.advDataSets == 0 && r.port.advStarts == 0);
  assert(r.core->startServer());
  assert(strcmp(r.port.advName, "bike-da4eb580") == 0);
  r.update();
  assert(r.port.advOn && r.port.advStarts == 1);

  r.core->setNamePrefix("Trainer");                    // only through refreshAdvertising()
  r.update();
  assert(r.port.advDataSets == 1 && strcmp(r.port.advName, "bike-da4eb580") == 0);
  assert(r.core->refreshAdvertising(r.now));
  assert(r.port.advDataSets == 2 && strcmp(r.port.advName, "trainer-da4eb580") == 0);
  assert(!r.port.advOnAtDataSet && r.port.advStops == 1);   // stopped before the rebuild
  assert(r.port.advOn && r.port.advStarts == 2 && r.port.advMask == bs::kCompiledMask);
  assert(r.core->stats().advertisingEnforced == 0);

  r.port.advOn = false;                                // not advertising: no stop
  assert(r.core->refreshAdvertising(r.now));
  assert(r.port.advStops == 1 && r.port.advOn && r.port.advStarts == 3);

  r.port.advStartOk = false;                           // restart refused: still applied (rev 28)
  r.port.advOn = false;
  assert(r.core->refreshAdvertising(r.now) && r.port.advDataSets == 4);
  assert(r.port.advStarts == 4 && !r.port.advOn);
  assert(r.core->refreshAdvertising(r.now) && r.port.advDataSets == 5);
  assert(r.port.advStarts == 4);                       // the restart follows the start hold
  r.port.advStartOk = true;
  r.now += BBS_SCAN_START_BACKOFF_MS;
  r.port.advDataOk = false;                            // NimBLE refused a field
  assert(!r.core->refreshAdvertising(r.now));
  assert(r.port.advOn && r.port.advStarts == 5);

  r.port.advDataOk = true;                             // held, then NimBLE restarted it:
  r.port.advOn = false;                                // the refresh restarts it at once
  r.port.advStartOk = false;
  r.update();
  assert(r.port.advStarts == 6 && !r.port.advOn);
  r.port.advStartOk = true;
  r.port.advOn = true;
  assert(r.core->refreshAdvertising(r.now));
  assert(r.port.advOn && r.port.advStarts == 7 && r.port.advStops == 2);
}

// ---- Discovery (§3.3, §5.3) ----

static_assert(BBS_MAX_DISCOVERED >= 5, "the discovery tests store up to 5 sensors");

struct Ad {                                            // one ADV or SCAN_RSP payload
  uint8_t data[64] = {};
  size_t len = 0;
  Ad& uuids(std::initializer_list<uint16_t> list) {
    data[len++] = static_cast<uint8_t>(1 + 2 * list.size());
    data[len++] = 0x03;
    for (uint16_t u : list) {
      data[len++] = static_cast<uint8_t>(u & 0xFF);
      data[len++] = static_cast<uint8_t>(u >> 8);
    }
    return *this;
  }
  Ad& name(const char* n, bool complete = true) {
    const size_t l = strlen(n);
    data[len++] = static_cast<uint8_t>(1 + l);
    data[len++] = complete ? 0x09 : 0x08;
    memcpy(data + len, n, l);
    len += l;
    return *this;
  }
  Ad& bytes(std::initializer_list<uint8_t> list) {
    for (uint8_t b : list) data[len++] = b;
    return *this;
  }
};

// What gapScanCb does with one report; epoch 0 = the running discovery's.
static void discReport(Rig& r, const uint8_t* mac, const Ad& ad, uint8_t addrType = 0,
                       uint16_t epoch = 0) {
  bs::AdFields fields;
  bs::parseAd(ad.data, ad.len, fields);
  r.core->discoveryReport(epoch != 0 ? epoch : r.core->scan().activeDiscEpoch(), mac, addrType,
                          fields);
}

static void discover(Rig& r, uint8_t mask = 0x07, uint32_t ms = 15000) {
  assert(r.core->startDiscovery(mask, ms, r.now));
  r.update();
  assert(r.core->scan().owner() == ScanOwner::DISCOVERY);
}

static void discEnd(Rig& r) {
  r.port.discOn = false;
  assert(bs::pushScanEnd(r.core->queue(), r.core->scan().epoch(), 0));
}

// DISCOVERY_DONE runs after the state change: closed, table already replaced.
static void expectDone(int i, FinishReason reason, uint8_t count, uint32_t generation) {
  assert(i < g_eventCount);
  const Recorded& e = g_events[i];
  assert(e.type == EventType::DISCOVERY_DONE && e.sensorType == SensorType{});
  assert(e.reason == static_cast<int>(reason) && e.count == count);
  assert(e.generation == generation && !e.discovering && e.tableCount == count);
}

static void testDiscoveryNatural() {
  Rig r;
  DiscoveredSensor d;
  assert(!r.core->discovering() && r.core->discoveredCount() == 0 && !r.core->discovered(0, d));
  r.core->stopDiscovery();                             // nothing open: no event
  assert(r.core->startDiscovery(0x03, 15000, r.now));
  assert(r.core->discovering() && r.core->scan().owner() == ScanOwner::DISCOVERY_PENDING);
  assert(r.core->scan().activeDiscEpoch() == 0 && r.port.discStarts == 0);
  assert(!r.core->startDiscovery(0x03, 15000, r.now));   // pending
  r.update();
  assert(r.core->scan().owner() == ScanOwner::DISCOVERY && r.port.discStarts == 1);
  assert(r.port.lastDurationMs == 15000 && r.core->stats().scansStarted == 1);
  assert(!r.port.lastFilterDuplicates);                // names from SCAN_RSP (§3.3)
  const uint16_t e = r.core->scan().epoch();
  assert(e != 0 && r.core->scan().activeDiscEpoch() == e);
  assert(r.port.activeAtStart == e);                   // published before ble_gap_disc()
  assert(!r.core->startDiscovery(0x03, 15000, r.now));   // open
  assert(g_eventCount == 0);

  discReport(r, kMac, Ad().uuids({0x180D}), 1);        // ADV: type only
  discReport(r, kMac, Ad().name("Polar H10"), 0);      // SCAN_RSP: name merged, addrType kept
  discReport(r, kOtherMac, Ad().uuids({0x1816}).name("CSC", false));
  discReport(r, kOtherMac, Ad().name("Cadence"));      // complete replaces shortened
  discReport(r, kOtherMac, Ad().name("Cad", false));   // never the reverse
  discReport(r, kOtherMac, Ad().name("Other"));        // the first complete name stays
  discReport(r, kCscMac, Ad().name("Phone"));          // no type: not stored
  discReport(r, kCscMac, Ad().uuids({0x180F}));        // battery only: not stored
  discReport(r, kCscMac, Ad().uuids({0x180D}), 0, static_cast<uint16_t>(e + 1));   // other epoch
  assert(r.core->discoveredCount() == 0);              // the table changes on DISCOVERY_DONE
  r.update();
  assert(r.core->discovering() && g_eventCount == 0);

  discEnd(r);
  r.update();
  assert(r.core->scan().owner() == ScanOwner::OFF && r.core->scan().activeDiscEpoch() == 0);
  assert(g_eventCount == 1);
  expectDone(0, FinishReason::NATURAL, 2, 1);
  assert(r.core->discovered(0, d));                    // sorted by name
  assert(strcmp(d.name, "Cadence") == 0 && memcmp(d.mac, kOtherMac, 6) == 0);
  assert(d.typeMask == 0x02 && d.addrType == 0);
  assert(r.core->discovered(1, d));
  assert(strcmp(d.name, "Polar H10") == 0 && memcmp(d.mac, kMac, 6) == 0);
  assert(d.typeMask == 0x01 && d.addrType == 1);
  assert(!r.core->discovered(2, d) && !r.core->discovered(255, d));

  assert(bs::pushScanEnd(r.core->queue(), e, 0));      // late SCAN_END: finished once
  r.update();
  r.core->stopDiscovery();
  assert(g_eventCount == 1 && r.core->discoveredCount() == 2);
  assert(r.port.discStarts == 1 && r.port.discCancels == 0);
}

static void testDiscoveryFields() {
  const uint8_t a[6] = {0x10, 0, 0, 0, 0, 1}, b[6] = {0x10, 0, 0, 0, 0, 2},
                c[6] = {0x10, 0, 0, 0, 0, 3}, d4[6] = {0x10, 0, 0, 0, 0, 4},
                e5[6] = {0x10, 0, 0, 0, 0, 5}, f[6] = {0x10, 0, 0, 0, 0, 6},
                g[6] = {0x10, 0, 0, 0, 0, 7};
  const char long40[] = "0123456789012345678901234567890123456789";
  {
    Rig r;
    discover(r, 0x03);
    discReport(r, a, Ad().uuids({0x1816, 0x1818}).name("Meter"));   // CSC + CPS: only CSC
    discReport(r, b, Ad().uuids({0x1818}).name("PowerOnly"));       // not requested
    discReport(r, c, Ad().name("Sh", false).uuids({0x180D}).name("Complete HR"));
    discReport(r, d4, Ad().uuids({0x180D}).name("Partial").bytes({0x05, 0xFF, 0x01}));  // cut TLV
    discReport(r, e5, Ad().bytes({0x02, 0x03, 0x0D}).name("Odd"));   // odd UUID list first
    discReport(r, f, Ad().uuids({0x180D}).bytes({0x00}).name("AfterPad"));
    discReport(r, g, Ad().uuids({0x1816}).bytes({0x01, 0x09}));      // empty name: none
    discReport(r, g, Ad().name(long40));
    r.core->stopDiscovery();
    expectDone(0, FinishReason::MANUAL, 5, 1);
    DiscoveredSensor s;
    assert(r.core->discovered(0, s) && memcmp(s.mac, g, 6) == 0);
    assert(strlen(s.name) == 31 && strncmp(s.name, long40, 31) == 0 && s.typeMask == 0x02);
    assert(r.core->discovered(1, s) && memcmp(s.mac, c, 6) == 0);
    assert(strcmp(s.name, "Complete HR") == 0 && s.typeMask == 0x01);
    assert(r.core->discovered(2, s) && memcmp(s.mac, a, 6) == 0);
    assert(strcmp(s.name, "Meter") == 0 && s.typeMask == 0x02);
    assert(r.core->discovered(3, s) && memcmp(s.mac, d4, 6) == 0);
    assert(strcmp(s.name, "Partial") == 0 && s.typeMask == 0x01);
    assert(r.core->discovered(4, s) && memcmp(s.mac, f, 6) == 0);
    assert(s.name[0] == '\0' && s.typeMask == 0x01);   // unnamed last
  }
  {
    Rig r;                                             // Power requested: only if compiled
    discover(r, 0x07);
    discReport(r, b, Ad().uuids({0x1818}).name("PowerOnly"));
    discReport(r, a, Ad().uuids({0x1816, 0x1818}).name("Meter"));
    r.core->stopDiscovery();
    DiscoveredSensor s;
#if BBS_COMPILE_POWER
    expectDone(0, FinishReason::MANUAL, 2, 1);
    assert(r.core->discovered(0, s) && strcmp(s.name, "Meter") == 0 && s.typeMask == 0x06);
    assert(r.core->discovered(1, s) && strcmp(s.name, "PowerOnly") == 0 && s.typeMask == 0x04);
#else
    expectDone(0, FinishReason::MANUAL, 1, 1);
    assert(r.core->discovered(0, s) && strcmp(s.name, "Meter") == 0 && s.typeMask == 0x02);
#endif
  }
  {
    Rig r;                                             // HR requested: CSC types dropped
    discover(r, 0x01);
    discReport(r, a, Ad().uuids({0x1816}).name("CscOnly"));
    discReport(r, b, Ad().uuids({0x180D, 0x1816}).name("Sh", false));
    discReport(r, b, Ad().bytes({0x01, 0x09}));        // an empty complete name keeps "Sh"
    r.core->stopDiscovery();
    expectDone(0, FinishReason::MANUAL, 1, 1);
    DiscoveredSensor s;
    assert(r.core->discovered(0, s) && memcmp(s.mac, b, 6) == 0);
    assert(strcmp(s.name, "Sh") == 0 && s.typeMask == 0x01);
  }
  {
    Rig r;                                             // full buffer: new addresses dropped
    discover(r, 0x01);
    uint8_t mac[6] = {0x20, 0, 0, 0, 0, 0};
    for (uint16_t i = 0; i <= BBS_MAX_DISCOVERED; ++i) {
      mac[4] = static_cast<uint8_t>(i >> 8);
      mac[5] = static_cast<uint8_t>(i);
      discReport(r, mac, Ad().uuids({0x180D}));
    }
    mac[4] = 0;
    mac[5] = 0;
    discReport(r, mac, Ad().name("First"));            // a stored address still merges
    discReport(r, a, Ad().uuids({0x1816}).name("Csc"));   // not requested
    r.core->stopDiscovery();
    expectDone(0, FinishReason::MANUAL, BBS_MAX_DISCOVERED, 1);
    DiscoveredSensor s;
    assert(r.core->discovered(0, s) && strcmp(s.name, "First") == 0);
    assert(s.mac[4] == 0 && s.mac[5] == 0);
    for (uint8_t i = 1; i < BBS_MAX_DISCOVERED; ++i) {
      assert(r.core->discovered(i, s) && s.name[0] == '\0');
      assert(s.mac[5] == i && s.mac[4] == 0);          // unnamed keep arrival order
    }
  }
  {
    Rig r;                                             // named by strcmp, stable; unnamed last
    discover(r, 0x01);
    discReport(r, a, Ad().uuids({0x180D}));
    discReport(r, b, Ad().uuids({0x180D}).name("b"));
    discReport(r, c, Ad().uuids({0x180D}));
    discReport(r, d4, Ad().uuids({0x180D}).name("A"));
    discReport(r, e5, Ad().uuids({0x180D}).name("b"));
    discReport(r, f, Ad().uuids({0x180D}));
    r.core->stopDiscovery();
    expectDone(0, FinishReason::MANUAL, 6, 1);
    const uint8_t* order[6] = {d4, b, e5, a, c, f};
    const char* names[6] = {"A", "b", "b", "", "", ""};
    DiscoveredSensor s;
    for (uint8_t i = 0; i < 6; ++i) {
      assert(r.core->discovered(i, s) && memcmp(s.mac, order[i], 6) == 0);
      assert(strcmp(s.name, names[i]) == 0);
    }
  }
}

static void testDiscoveryDuration() {
  Rig r;
  const uint32_t limit = bs::kDiscoveryDurationLimitMs;
  static_assert(bs::kDiscoveryDurationLimitMs == INT32_MAX - BBS_SCAN_END_GRACE_MS, "");
  assert(!r.core->startDiscovery(0x01, 0, r.now));     // NimBLE would scan 10.24 s
  assert(!r.core->startDiscovery(0x01, limit, r.now));
  assert(!r.core->startDiscovery(0x01, UINT32_MAX, r.now));
  r.update();
  assert(!r.core->discovering() && r.core->scan().owner() == ScanOwner::OFF);
  assert(r.port.discStarts == 0 && r.port.discCancels == 0 && g_eventCount == 0);
  assert(r.core->startDiscovery(0x01, limit - 1, r.now));
  r.update();
  assert(r.core->scan().owner() == ScanOwner::DISCOVERY && r.port.lastDurationMs == limit - 1);
  const uint32_t start = r.now;
  r.at(start + (limit - 1) + BBS_SCAN_END_GRACE_MS - 1);
  assert(r.core->scan().owner() == ScanOwner::DISCOVERY);
  r.at(start + (limit - 1) + BBS_SCAN_END_GRACE_MS);
  expectDone(0, FinishReason::TIMEOUT, 0, 1);          // rejected calls did not bump discGen
}

static void testDiscoveryCancelsWindows() {
  {
    Rig r;                                             // MAC: the SLEEP window is cancelled
    r.add();
    r.update();
    assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW);
    const uint16_t e1 = r.core->scan().epoch();
    const uint32_t windowStart = r.now;
    assert(bs::pushScanEnd(r.core->queue(), e1, 0));  // queued before the cancel
    r.match(e1);
    r.now += 100;
    assert(r.core->startDiscovery(0x01, 15000, r.now));
    assert(r.port.discCancels == 1 && !r.port.discOn);
    r.update();                                        // the stale SCAN_END/SCAN_MATCH change nothing
    assert(r.core->scan().owner() == ScanOwner::DISCOVERY && r.core->scan().epoch() == e1 + 1);
    assert(!r.port.lastFilterDuplicates);
    assert(r.state() == SlotState::WAIT_SCAN && countEvents(EventType::SENSOR_FOUND) == 0);
    r.match(r.core->scan().epoch());                   // discovery epoch: no SCAN_MATCH use
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && countEvents(EventType::SENSOR_FOUND) == 0);
    discEnd(r);
    r.update();
    assert(countEvents(EventType::DISCOVERY_DONE) == 1);
    assert(r.core->scan().owner() == ScanOwner::OFF);  // the sleep schedule still applies
    r.at(windowStart + BBS_SLEEP_SCAN_DURATION_MS + BBS_SLEEP_SCAN_INTERVAL_MS - 1);
    assert(r.port.discStarts == 2);
    r.at(windowStart + BBS_SLEEP_SCAN_DURATION_MS + BBS_SLEEP_SCAN_INTERVAL_MS);
    assert(r.port.discStarts == 3 && r.core->scan().owner() == ScanOwner::SLEEP_WINDOW);
    assert(r.port.lastFilterDuplicates);
  }
  {
    Rig r(Mode::NAME);                                 // NAME: windows resume after the discovery
    assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
    r.update();
    assert(r.core->scan().owner() == ScanOwner::NAME_SCAN);
    assert(r.core->startDiscovery(0x01, 15000, r.now));
    assert(r.port.discCancels == 1);
    r.update();
    assert(r.core->scan().owner() == ScanOwner::DISCOVERY && r.port.discStarts == 2);
    assert(!r.port.lastFilterDuplicates);
    assert(r.report(kMac, "HRM"));                     // SCAN_MATCH with the discovery epoch
    r.update();
    assert(r.state() == SlotState::IDLE && countEvents(EventType::SENSOR_FOUND) == 0);
    discEnd(r);
    r.update();
    assert(countEvents(EventType::DISCOVERY_DONE) == 1);
    assert(r.core->scan().owner() == ScanOwner::NAME_SCAN && r.port.discStarts == 3);
  }
}

static void testDiscoveryStop() {
  {
    Rig r;                                             // pending: no scan to cancel
    r.toConnecting();
    assert(r.core->startDiscovery(0x07, 15000, r.now));
    r.update();
    assert(r.core->scan().owner() == ScanOwner::DISCOVERY_PENDING && r.port.discStarts == 1);
    const int cancels = r.port.discCancels;
    r.core->stopDiscovery();
    assert(r.core->scan().owner() == ScanOwner::OFF && r.port.discCancels == cancels);
    assert(countEvents(EventType::DISCOVERY_DONE) == 1);
    expectDone(g_eventCount - 1, FinishReason::MANUAL, 0, 1);
    r.core->stopDiscovery();                           // once
    r.connectedEv(r.gen());
    r.update();
    assert(r.state() == SlotState::SUBSCRIBED && r.port.discStarts == 1);
    assert(countEvents(EventType::DISCOVERY_DONE) == 1);
  }
  {
    Rig r;                                             // running: cancelled, matches kept
    discover(r);
    const uint16_t e = r.core->scan().epoch();
    discReport(r, kMac, Ad().uuids({0x180D}).name("HRM"));
    r.core->stopDiscovery();
    assert(r.port.discCancels == 1 && !r.port.discOn);
    assert(r.core->scan().owner() == ScanOwner::OFF && r.core->scan().activeDiscEpoch() == 0);
    expectDone(0, FinishReason::MANUAL, 1, 1);
    assert(bs::pushScanEnd(r.core->queue(), e, 0));
    r.update();
    assert(g_eventCount == 1);
  }
}

static void testDiscoveryTimeout() {
  Rig r;
  discover(r, 0x01, 5000);
  const uint32_t start = r.now;
  const uint16_t e = r.core->scan().epoch();
  discReport(r, kMac, Ad().uuids({0x180D}).name("HRM"));
  r.at(start + 5000 + BBS_SCAN_END_GRACE_MS - 1);
  assert(r.core->scan().owner() == ScanOwner::DISCOVERY && g_eventCount == 0);
  r.at(start + 5000 + BBS_SCAN_END_GRACE_MS);          // scan still active: cancelled
  assert(r.port.discCancels == 1 && !r.port.discOn && r.core->stats().discoveryTimeouts == 1);
  assert(r.core->scan().owner() == ScanOwner::OFF);
  expectDone(0, FinishReason::TIMEOUT, 1, 1);
  assert(bs::pushScanEnd(r.core->queue(), e, 0));
  r.update();
  assert(g_eventCount == 1 && r.core->stats().discoveryTimeouts == 1);
}

static void testDiscoveryHostReset() {
  {
    Rig r;                                             // the reset delivers SCAN_END first
    discover(r);
    discReport(r, kMac, Ad().uuids({0x180D}).name("HRM"));
    assert(bs::pushScanEnd(r.core->queue(), r.core->scan().epoch(), 0));
    assert(bs::pushHostReset(r.core->queue()));
    r.port.discOn = false;
    r.update();
    assert(g_eventCount == 2);
    expectDone(0, FinishReason::NATURAL, 1, 1);
    expectEvent(1, EventType::HOST_RESET, SlotState::EMPTY, 0, SensorType{});
  }
  {
    Rig r;                                             // HOST_RESET alone: matches kept
    discover(r);
    discReport(r, kMac, Ad().uuids({0x180D}).name("HRM"));
    assert(bs::pushHostReset(r.core->queue()));
    r.port.discOn = false;
    r.update();
    assert(g_eventCount == 2 && r.core->stats().hostResets == 1);
    expectDone(0, FinishReason::HOST_RESET, 1, 1);
    expectEvent(1, EventType::HOST_RESET, SlotState::EMPTY, 0, SensorType{});
    assert(r.core->scan().owner() == ScanOwner::OFF && r.core->scan().activeDiscEpoch() == 0);
  }
  {
    Rig r;                                             // pending: DONE after the slot reset
    r.toConnecting();
    assert(r.core->startDiscovery(0x07, 15000, r.now));
    r.update();
    g_eventCount = 0;
    r.port.connOn = false;
    assert(bs::pushHostReset(r.core->queue()));
    r.update();
    assert(g_eventCount == 2);
    expectDone(0, FinishReason::HOST_RESET, 0, 1);
    assert(g_events[0].state == SlotState::WAIT_SCAN);
    expectEvent(1, EventType::HOST_RESET, SlotState::WAIT_SCAN, 0, SensorType{});
  }
}

static void testDiscoveryResync() {
  {
    Rig r;                                             // scan still active: keeps running
    discover(r);
    discReport(r, kMac, Ad().uuids({0x180D}).name("HRM"));
    fillQueueToResync(*r.core);
    r.update();
    assert(r.core->scan().owner() == ScanOwner::DISCOVERY && g_eventCount == 0);
    r.port.discOn = false;                             // scan gone without SCAN_END
    fillQueueToResync(*r.core);
    r.update();
    assert(r.core->scan().owner() == ScanOwner::OFF);
    expectDone(0, FinishReason::RESYNC, 1, 1);
  }
  {
    Rig r;                                             // pending ends too (no scan is active)
    r.toConnecting();
    assert(r.core->startDiscovery(0x07, 15000, r.now));
    r.update();
    g_eventCount = 0;
    fillQueueToResync(*r.core);
    r.update();
    assert(g_eventCount == 1 && r.state() == SlotState::CONNECTING);
    expectDone(0, FinishReason::RESYNC, 0, 1);
  }
  {
    Rig r;                                             // DONE precedes the slot repair's events
    r.toSubscribed();
    discover(r);
    g_eventCount = 0;
    r.port.connected = false;
    r.port.discOn = false;
    fillQueueToResync(*r.core);
    r.update();
    assert(g_eventCount == 2);
    expectDone(0, FinishReason::RESYNC, 0, 1);
    expectEvent(1, EventType::SENSOR_DISCONNECTED, SlotState::WAIT_SCAN, 0);
  }
}

static void testDiscoveryStartFailed() {
  {
    Rig r;                                             // ble_gap_disc fails
    discover(r);
    discReport(r, kMac, Ad().uuids({0x180D}).name("HRM"));
    r.core->stopDiscovery();
    assert(r.core->discoveredCount() == 1);
    r.port.discStartRc = 2;
    assert(r.core->startDiscovery(0x07, 15000, r.now));
    r.update();
    assert(r.port.discStarts == 2 && r.core->stats().scanStartFails == 1);
    assert(r.core->scan().owner() == ScanOwner::OFF && r.core->scan().activeDiscEpoch() == 0);
    expectDone(1, FinishReason::START_FAILED, 0, 2);   // the table is replaced (empty)
    r.port.discStartRc = 0;                            // the backoff holds the next window
    r.add();
    r.at(r.now + BBS_SCAN_START_BACKOFF_MS - 1);
    assert(r.port.discStarts == 2);
    r.at(r.now + 1);
    assert(r.port.discStarts == 3 && r.core->scan().owner() == ScanOwner::SLEEP_WINDOW);
  }
  {
    Rig r;                                             // not synced: retried after the backoff
    r.port.synced = false;
    assert(r.core->startDiscovery(0x07, 15000, r.now));
    r.update();
    assert(r.core->scan().owner() == ScanOwner::DISCOVERY_PENDING && r.port.discStarts == 0);
    r.port.synced = true;
    r.at(r.now + BBS_SCAN_START_BACKOFF_MS - 1);
    assert(r.port.discStarts == 0);
    r.at(r.now + 1);
    assert(r.core->scan().owner() == ScanOwner::DISCOVERY && r.port.discStarts == 1);
    assert(r.core->stats().scanStartFails == 0 && g_eventCount == 0);
  }
  {
    Rig r;                                             // pending too long behind a connect
    r.toConnecting();
    const uint32_t t0 = r.now;
    assert(r.core->startDiscovery(0x07, 15000, t0));
    r.at(t0 + bs::kDiscoveryPendingMaxMs - 1);
    assert(r.state() == SlotState::CANCELLING);       // connect watchdog
    assert(r.core->scan().owner() == ScanOwner::DISCOVERY_PENDING);
    r.at(t0 + bs::kDiscoveryPendingMaxMs);
    assert(r.core->scan().owner() == ScanOwner::OFF && r.port.discStarts == 1);
    assert(r.core->stats().scanStartFails == 0);
    expectDone(g_eventCount - 1, FinishReason::START_FAILED, 0, 1);
  }
  {
    Rig r;                                             // waits for NimBLE to stop scanning/connecting
    r.add();
    r.update();
    assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW && r.port.discStarts == 1);
    r.port.cancelLeavesDiscOn = true;                  // the cancel failed: the window runs on
    assert(r.core->startDiscovery(0x07, 15000, r.now));
    r.at(r.now + 1000);
    assert(r.core->scan().owner() == ScanOwner::DISCOVERY_PENDING && r.port.discStarts == 1);
    r.port.discOn = false;
    r.port.connOn = true;
    r.at(r.now + 1000);
    assert(r.core->scan().owner() == ScanOwner::DISCOVERY_PENDING && r.port.discStarts == 1);
    r.port.connOn = false;
    r.update();
    assert(r.core->scan().owner() == ScanOwner::DISCOVERY && r.port.discStarts == 2);
    assert(r.core->stats().scanStartFails == 0 && g_eventCount == 0);
  }
  {
    Rig r;                                             // ...but not beyond the pending limit
    r.port.discOn = true;
    const uint32_t t0 = r.now;
    assert(r.core->startDiscovery(0x07, 15000, t0));
    r.at(t0 + bs::kDiscoveryPendingMaxMs - 1);
    assert(r.core->scan().owner() == ScanOwner::DISCOVERY_PENDING && g_eventCount == 0);
    r.at(t0 + bs::kDiscoveryPendingMaxMs);
    assert(r.core->scan().owner() == ScanOwner::OFF && r.port.discStarts == 0);
    assert(r.core->stats().scanStartFails == 0);
    expectDone(0, FinishReason::START_FAILED, 0, 1);
  }
}

static void testDiscoveryBlocksConnects() {
  {
    Rig r;                                             // a held FOUND slot goes back to WAIT_SCAN
    r.port.connectOk = false;
    r.port.connectErr = bs::kHsEBusy;
    r.add();
    r.update();
    r.match(r.core->scan().epoch());
    r.update();
    assert(r.state() == SlotState::FOUND && r.port.connects == 1);
    r.port.connectOk = true;
    assert(r.core->startDiscovery(0x07, 15000, r.now));
    assert(r.state() == SlotState::WAIT_SCAN);
    r.at(r.now + 500);
    assert(r.core->scan().owner() == ScanOwner::DISCOVERY && r.port.connects == 1);
  }
  {
    Rig r;                                             // STOPPING_FOR_CONNECT goes back too
    r.port.cancelLeavesDiscOn = true;
    r.add();
    r.update();
    r.match(r.core->scan().epoch());
    r.update();
    assert(r.state() == SlotState::STOPPING_FOR_CONNECT);
    assert(r.core->startDiscovery(0x07, 15000, r.now));
    assert(r.state() == SlotState::WAIT_SCAN);
    r.port.cancelLeavesDiscOn = false;
    r.port.discOn = false;
    r.update();
    assert(r.core->scan().owner() == ScanOwner::DISCOVERY && r.port.connects == 0);
  }
  {
    Rig r;                                             // FOUND during the discovery waits for DONE
    r.add();
    discover(r);
    r.replace();
    assert(r.state() == SlotState::FOUND);
    r.at(r.now + 1000);
    assert(r.state() == SlotState::FOUND && r.port.connects == 0);
    discEnd(r);
    r.update();
    assert(countEvents(EventType::DISCOVERY_DONE) == 1);
    assert(r.state() == SlotState::CONNECTING && r.port.connects == 1);
  }
  {
    Rig r;                                             // a drop's direct attempt waits too
    r.toSubscribed();
    discover(r);
    r.discEv(r.gen());
    r.update();
    assert(r.state() == SlotState::FOUND && r.port.connects == 1);
    discEnd(r);
    r.update();
    assert(r.state() == SlotState::CONNECTING && r.port.connects == 2);
  }
  {
    Rig r;                                             // pending waits for a connect + GATT pass
    r.toConnecting();
    assert(r.core->startDiscovery(0x07, 15000, r.now));
    r.update();
    assert(r.core->scan().owner() == ScanOwner::DISCOVERY_PENDING && r.port.discStarts == 1);
    r.connectedEv(r.gen());
    r.update();                                        // SUBSCRIBED, then the start in one pass
    assert(r.state() == SlotState::SUBSCRIBED && r.port.gattPasses == 1);
    assert(r.core->scan().owner() == ScanOwner::DISCOVERY && r.port.discStarts == 2);
  }
  {
    Rig r;                                             // ...and for a CANCELLING outcome
    r.toConnecting();
    assert(r.core->startDiscovery(0x07, 15000, r.now));
    r.remove();
    r.update();
    assert(r.state() == SlotState::CANCELLING);
    assert(r.core->scan().owner() == ScanOwner::DISCOVERY_PENDING);
    r.failEv(r.gen());
    r.update();
    assert(r.state() == SlotState::EMPTY && r.core->scan().owner() == ScanOwner::DISCOVERY);
  }
}

static void testDiscoveryStaleEpochWhilePending() {
  Rig r;                                               // a late report of the finished scan
  discover(r, 0x01);
  const uint16_t e = r.core->scan().epoch();
  r.core->stopDiscovery();
  expectDone(0, FinishReason::MANUAL, 0, 1);
  r.port.discOn = true;                                // keeps the next one pending
  assert(r.core->startDiscovery(0x01, 15000, r.now));
  r.update();
  assert(r.core->scan().owner() == ScanOwner::DISCOVERY_PENDING && r.core->scan().epoch() == e);
  discReport(r, kMac, Ad().uuids({0x180D}).name("HRM"), 0, e);
  assert(r.core->discoveredCount() == 0);
  r.port.discOn = false;
  r.update();
  assert(r.core->scan().owner() == ScanOwner::DISCOVERY && r.core->scan().epoch() == e + 1);
  discEnd(r);
  r.update();
  expectDone(1, FinishReason::NATURAL, 0, 2);         // never entered discBuf
}

static void testDiscoveryFromCallback() {
  Rig r;                                               // DONE callback starts the next one
  r.core->setEventCallback(onEventHook);
  int dones = 0;
  g_hook = [&r, &dones](const EventInfo& info) {
    if (info.type != EventType::DISCOVERY_DONE) return;
    r.core->stopDiscovery();                           // already closed: no second DONE
    if (dones++ == 0) assert(r.core->startDiscovery(0x01, 15000, r.now));
  };
  discover(r, 0x01);
  const uint16_t e = r.core->scan().epoch();
  discReport(r, kMac, Ad().uuids({0x180D}).name("HRM"));
  discEnd(r);
  r.update();                                          // DONE, restart, started in the same pass
  assert(dones == 1 && g_eventCount == 1);
  expectDone(0, FinishReason::NATURAL, 1, 1);
  assert(r.core->discovering() && r.core->scan().owner() == ScanOwner::DISCOVERY);
  assert(r.core->scan().epoch() == e + 1 && r.port.discStarts == 2);
  assert(r.core->discoveredCount() == 1);              // previous table until the next DONE
  discReport(r, kOtherMac, Ad().uuids({0x180D}).name("Other"));
  r.core->stopDiscovery();
  g_hook = nullptr;
  assert(dones == 2 && g_eventCount == 2);
  expectDone(1, FinishReason::MANUAL, 1, 2);           // the buffer was cleared at the start
  DiscoveredSensor d;
  assert(r.core->discovered(0, d) && strcmp(d.name, "Other") == 0);
}

// gapScanCb (host task) upserts while update() starts, finishes and reads.
static void testDiscoveryConcurrentReports() {
  Rig r;
  r.core->setEventCallback(nullptr);                   // 300 DONEs; g_events holds 32
  std::atomic<bool> stop{false};
  std::thread host([&r, &stop] {
    uint8_t mac[6] = {0x30, 0, 0, 0, 0, 0};
    for (uint32_t n = 0; !stop.load(); ++n) {
      mac[5] = static_cast<uint8_t>(n % 40);
      discReport(r, mac, Ad().uuids({0x180D}).name(n % 2 ? "A" : "B", n % 3 == 0));
    }
  });
  DiscoveredSensor d;
  for (int i = 0; i < 300; ++i) {
    discover(r, 0x01);
    for (int k = 0; k < 20; ++k) {
      for (uint8_t j = 0; j < r.core->discoveredCount(); ++j) {
        assert(r.core->discovered(j, d));
        assert(d.typeMask == 0x01 && strlen(d.name) == 1 && d.mac[0] == 0x30);
      }
    }
    if (i % 2) {
      r.core->stopDiscovery();
    } else {
      discEnd(r);
      r.update();
    }
    assert(!r.core->discovering() && r.core->discoveredCount() <= BBS_MAX_DISCOVERED);
  }
  stop.store(true);
  host.join();
}

// ---- §10 step 7: pause/quiesce, end()/begin(), Stats ----

// §5.5: advertising only, stopped at once; NimBLE's own restarts are enforced off.
static void testPauseResume() {
  Rig r;
  assert(r.core->startServer());
  r.update();
  assert(r.port.advOn && r.port.advStarts == 1);
  r.core->pause();
  assert(!r.port.advOn && r.port.advStops == 1);
  r.core->pause();
  r.update();
  assert(r.port.advStops == 1 && r.port.advStarts == 1);
  assert(r.core->stats().advertisingEnforced == 0);
  r.port.advOn = true;                                 // NimBLE restarted it (§0.9)
  r.update();
  assert(!r.port.advOn && r.core->stats().advertisingEnforced == 1);
  r.toSubscribed();                                    // scans and connects go on
  assert(r.core->startDiscovery(0x01, 15000, r.now));  // so does discovery
  r.update();
  assert(r.core->scan().owner() == ScanOwner::DISCOVERY);
  r.core->stopDiscovery();
  assert(r.core->refreshAdvertising(r.now) && !r.port.advOn);
  r.core->resume();
  assert(!r.port.advOn);                               // follows on the next update()
  r.update();
  assert(r.port.advOn && r.port.advStarts == 2);
}

// Rev 28: the start hold never delays a stop (pause, quiesce, enforcement).
static void testStopNeverHeld() {
  for (int quiesce = 0; quiesce < 2; ++quiesce) {
    Rig r;
    assert(r.core->startServer());
    r.port.advStartOk = false;
    r.update();
    assert(r.port.advStarts == 1 && !r.port.advOn);    // failed: the next start is held
    r.port.advStartOk = true;
    r.port.advOn = true;                               // NimBLE restarted it during the hold
    if (quiesce) {
      r.core->quiesce(r.now);
    } else {
      r.core->pause();
    }
    assert(!r.port.advOn && r.port.advStops == 1);
    r.now += 1;                                        // still inside the hold
    r.port.advOn = true;
    r.update();
    assert(!r.port.advOn && r.port.advStops == 2 && r.core->stats().advertisingEnforced == 1);
    assert(r.port.advStarts == 1);
  }
}

// §4 quiesce() step 3: a pending or running discovery ends with QUIESCED,
// DISCOVERY_DONE after all five steps.
static void testQuiesceFinishesDiscovery() {
  {
    Rig r;                                             // running
    r.core->setEventCallback(onEventHook);
    assert(r.core->startServer());
    r.update();
    discover(r);
    const uint16_t e = r.core->scan().epoch();
    discReport(r, kMac, Ad().uuids({0x180D}).name("HRM"));
    const int cancels = r.port.discCancels;
    bool seen = false;
    g_hook = [&r, &seen](const EventInfo& info) {
      if (info.type != EventType::DISCOVERY_DONE) return;
      seen = true;
      assert(!r.port.advOn && !r.port.discOn);
      assert(r.core->scan().owner() == ScanOwner::OFF && r.core->scan().activeDiscEpoch() == 0);
      assert(!r.core->startDiscovery(0x01, 15000, r.now));   // the latch is already set
    };
    r.core->quiesce(r.now);
    g_hook = nullptr;
    assert(seen && r.port.discCancels == cancels + 1 && g_eventCount == 1);
    expectDone(0, FinishReason::QUIESCED, 1, 1);
    r.core->quiesce(r.now);                            // idempotent
    assert(bs::pushScanEnd(r.core->queue(), e, 0));
    r.update();
    assert(g_eventCount == 1 && r.port.discCancels == cancels + 1 && r.core->quiesced());
  }
  {
    Rig r;                                             // pending behind a connect
    r.toConnecting();
    assert(r.core->startDiscovery(0x07, 15000, r.now));
    r.update();
    assert(r.core->scan().owner() == ScanOwner::DISCOVERY_PENDING);
    g_eventCount = 0;
    const int cancels = r.port.discCancels;
    r.core->quiesce(r.now);
    assert(g_eventCount == 1);
    expectDone(0, FinishReason::QUIESCED, 0, 1);
    assert(g_events[0].state == SlotState::CANCELLING);   // step 5 ran before the event
    assert(r.port.discCancels == cancels && r.port.cancelConnects == 1);
    assert(!r.core->quiesced());
    r.failEv(r.gen());
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.core->quiesced());
    assert(r.port.discStarts == 1 && g_eventCount == 1);
  }
}

// §4 quiesce() steps 4 and 5, the latch rejections and update() under quiesce.
static void testQuiesceSlotsAndScans() {
  {
    Rig r;                                             // CONNECTING → CANCELLING, FOUND → WAIT_SCAN
    assert(r.core->startServer());
    r.toConnecting();
    r.addCsc();
    assert(r.core->addByMac(SensorType::SPEED_CADENCE, kCscMacStr, 0, "CSC", r.now));
    assert(r.cscState() == SlotState::FOUND);
    g_eventCount = 0;
    r.core->quiesce(r.now);
    assert(r.state() == SlotState::CANCELLING && r.port.cancelConnects == 1);
    assert(r.cscState() == SlotState::WAIT_SCAN && !r.core->quiesced());
    assert(!r.core->addByMac(SensorType::HEART_RATE, kOtherMacStr, 0, nullptr, r.now));
    assert(!r.core->startDiscovery(0x01, 15000, r.now));
    assert(!r.core->refreshAdvertising(r.now));
    const int scans = r.port.discStarts, connects = r.port.connects;
    r.at(r.now + 60000);                               // no scan, no connect
    assert(r.port.discStarts == scans && r.port.connects == connects);
    r.failEv(r.gen());                                 // neutral cancel outcome
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.hr().failCount == 0);
    assert(r.core->stats().connectFails == 0 && g_eventCount == 0 && r.core->quiesced());
    r.port.advOn = true;                               // NimBLE restart: not quiesced until stopped
    assert(!r.core->quiesced());
    r.update();
    assert(!r.port.advOn && r.core->quiesced());
    r.core->remove(SensorType::SPEED_CADENCE, r.now);  // allowed (Homewind REMOVING)
    assert(r.cscState() == SlotState::EMPTY);
  }
  {
    Rig r;                                             // a drop under quiesce: no direct attempt
    r.toSubscribed();
    r.core->quiesce(r.now);
    assert(r.state() == SlotState::SUBSCRIBED && r.core->quiesced());   // links may remain
    g_eventCount = 0;
    r.discEv(r.gen(), 0x13);
    r.at(r.now + 60000);
    assert(r.state() == SlotState::FOUND && r.port.connects == 1 && r.port.discStarts == 1);
    expectEvent(0, EventType::SENSOR_DISCONNECTED, SlotState::FOUND, 0x13);
    assert(g_eventCount == 1 && r.core->quiesced());
  }
  {
    Rig r;                                             // SLEEP window cancelled; a failed cancel
    r.add();                                           // keeps quiesced() false
    r.update();
    assert(r.core->scan().owner() == ScanOwner::SLEEP_WINDOW);
    const uint16_t e = r.core->scan().epoch();
    r.port.cancelLeavesDiscOn = true;
    r.core->quiesce(r.now);
    assert(r.core->scan().owner() == ScanOwner::OFF && r.port.discCancels == 1);
    r.match(e);                                        // late report of the window
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && !r.core->quiesced());
    r.port.discOn = false;
    assert(r.core->quiesced() && r.port.discStarts == 1);
  }
  {
    Rig r;                                             // STOPPING_FOR_CONNECT → WAIT_SCAN
    r.port.cancelLeavesDiscOn = true;
    r.add();
    r.update();
    r.match(r.core->scan().epoch());
    r.update();
    assert(r.state() == SlotState::STOPPING_FOR_CONNECT);
    r.core->quiesce(r.now);
    assert(r.state() == SlotState::WAIT_SCAN);
    r.port.discOn = false;
    r.update();
    assert(r.port.connects == 0 && r.core->quiesced());
  }
  {
    Rig r(Mode::NAME);                                 // NAME: window cancelled, addByName rejected
    assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
    r.update();
    assert(r.core->scan().owner() == ScanOwner::NAME_SCAN);
    r.core->quiesce(r.now);
    assert(r.core->scan().owner() == ScanOwner::OFF && r.core->quiesced());
    assert(!r.core->addByName(SensorType::SPEED_CADENCE, "CSC", r.now));
    assert(!r.core->addByName(SensorType::HEART_RATE, "HRM2", r.now));
    assert(strcmp(r.hr().name, "HRM") == 0);
  }
}

// A DISCOVERING slot whose pass has not run when quiesce() arrives (from a
// callback in the same drain): no pass, resync repairs it neutrally.
static void testQuiesceResyncDiscovering() {
  for (int lost = 0; lost < 2; ++lost) {
    Rig r;
    r.core->setEventCallback(onEventHook);
    r.toConnecting();
    g_eventCount = 0;
    g_hook = [&r](const EventInfo& info) {
      if (info.type == EventType::CENTRAL_CONNECTED) r.core->quiesce(r.now);
    };
    r.connectedEv(r.gen());
    assert(bs::pushCentralConnect(r.core->queue()));
    r.update();
    g_hook = nullptr;
    assert(r.state() == SlotState::DISCOVERING && r.port.slotDisconnects[kHr] == 1);
    assert(r.port.gattPasses == 0 && !r.core->quiesced());
    r.core->quiesce(r.now);                            // idempotent: no second disconnect
    assert(r.port.slotDisconnects[kHr] == 1);
    r.update();
    assert(r.state() == SlotState::DISCOVERING && r.port.gattPasses == 0);
    r.port.connected = lost == 0;
    fillQueueToResync(*r.core);
    r.update();
    if (lost) {
      assert(r.state() == SlotState::WAIT_SCAN && r.core->stats().disconnects == 1);
    } else {
      assert(r.state() == SlotState::DISCOVERING && r.port.slotDisconnects[kHr] == 1);
      r.discEv(r.gen());
      r.update();
      assert(r.state() == SlotState::WAIT_SCAN);       // no direct attempt under quiesce
    }
    assert(r.hr().failCount == 0 && r.core->stats().connectFails == 0 && r.core->quiesced());
    assert(r.port.gattPasses == 0 && r.port.timerStarts == 0);
    assert(g_eventCount == 1 && countEvents(EventType::CENTRAL_CONNECTED) == 1);
    r.at(r.now + 60000);
    assert(r.port.connects == 1 && r.port.discStarts == 1);
  }
}

static void testScanning() {
  {
    Rig r;
    assert(!r.core->scanning());
    r.add();
    r.update();
    assert(r.core->scanning());
    r.port.cancelLeavesDiscOn = true;
    r.core->quiesce(r.now);
    assert(r.core->scan().owner() == ScanOwner::OFF && r.core->scanning());   // NimBLE still scans
    r.port.discOn = false;
    assert(!r.core->scanning());
  }
  {
    Rig r;                                             // a pending discovery is scanning too
    r.toConnecting();
    assert(r.core->startDiscovery(0x01, 15000, r.now));
    assert(!r.port.discOn && r.core->scanning());
  }
}

// §3.2 drop counters live in the queue; stats() merges them.
static void testStatsDropCounters() {
  Rig r;
  assert(r.core->stats().notifiesDropped == 0 && r.core->stats().eventsDropped == 0);
  const uint8_t data[2] = {0x00, 70};
  for (uint8_t i = 0; i < BBS_EVENT_QUEUE_DEPTH; ++i) {
    assert(bs::pushNotify(r.core->queue(), kHr, r.gen(), data, sizeof(data)));
  }
  assert(!bs::pushNotify(r.core->queue(), kHr, r.gen(), data, sizeof(data)));
  assert(!bs::pushScanMatch(r.core->queue(), 1, kMac, 0, "HRM", 3));
  fillQueueToResync(*r.core);                          // evicts every NOTIFY, then drops one
  const bs::QueueCounters q = r.core->queue().counters();
  BikeSensor::Stats s = r.core->stats();
  assert(s.notifiesDropped == 1u + BBS_EVENT_QUEUE_DEPTH && s.matchesDropped == 1);
  assert(s.eventsDropped == 1 && q.eventsDropped == 1);
  r.update();
  s = r.core->stats();
  assert(s.notifiesDropped == 1u + BBS_EVENT_QUEUE_DEPTH && s.matchesDropped == 1);
  assert(s.eventsDropped == 1);
}

// §4 end() steps 1–6 and begin() after end().
static void testBeginEnd() {
  {
    Rig r;                                             // begin(): stack, clients, server
    assert(r.core->begin("Bike X", 0x07, Mode::MAC));
    assert(strcmp(r.port.gapName, "bike-x-da4eb580") == 0);
    assert(r.port.stackInits == 1 && r.port.opens == 1 && r.port.serverStarts == 1);
    assert(!r.core->begin("Bike X", 0x07, Mode::MAC) && r.port.opens == 1);
    r.update();
    assert(r.port.advOn);
  }
  {
    Rig r;                                             // initStack() first: begin() keeps its name
    assert(r.core->initStack("first") && r.core->initStack("second"));
    assert(r.port.stackInits == 1 && strcmp(r.port.gapName, "first-da4eb580") == 0);
    r.port.openOk = false;
    assert(!r.core->begin("other", 0x07, Mode::MAC));
    r.port.openOk = true;
    assert(r.core->begin("other", 0x07, Mode::MAC));   // a retry
    assert(r.port.stackInits == 1 && r.port.opens == 2 && strcmp(r.port.advName, "first-da4eb580") == 0);
  }
  {
    Rig r;                                             // happy path, then begin() again
    assert(r.core->begin("homewind", 0x07, Mode::MAC));
    r.update();
    r.toSubscribed();
    r.port.clock = r.now;
    r.port.onTick = [&r] {                             // the link's DISCONNECTED arrives
      if (r.port.slotDisconnects[kHr] > 0 && r.port.connected) r.discEv(r.gen());
    };
    g_eventCount = 0;
    r.core->pause();
    r.core->quiesce(r.now);
    r.core->resume();                                  // rejected: the pause latch stays
    assert(r.core->end());
    assert(r.port.closes == 1 && r.port.slotDisconnects[kHr] == 1 && !r.port.advOn);
    expectEvent(0, EventType::SENSOR_DISCONNECTED, SlotState::FOUND, 0x208);
    assert(r.core->end() && r.port.closes == 1);       // already down
    r.port.advOn = true;
    r.update();                                        // nothing touches the stack
    assert(r.port.advOn && r.port.advStops == 1 && r.port.connects == 1);
    r.port.advOn = false;
    assert(!r.core->addByMac(SensorType::HEART_RATE, kMacStr, 0, nullptr, r.now));
    assert(!r.core->startServer() && r.port.serverStarts == 1);
    assert(r.core->begin("homewind", 0x01, Mode::NAME));   // latch cleared, fresh session
    assert(r.port.stackInits == 2 && r.port.opens == 2 && r.port.serverStarts == 2);
    assert(r.state() == SlotState::EMPTY && r.core->mode() == Mode::NAME && !r.core->quiesced());
    assert(!r.core->addByName(SensorType::SPEED_CADENCE, "CSC", r.now));   // new mask
    assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
    r.update();
    assert(!r.port.advOn && r.core->scan().owner() == ScanOwner::NAME_SCAN);   // still paused
    r.core->resume();
    r.update();
    assert(r.port.advOn);
  }
  {
    Rig r;                                             // step 2 timeout: no teardown, latch stays
    assert(r.core->begin("homewind", 0x07, Mode::MAC));
    r.toConnecting();
    r.port.clock = 5000;
    assert(!r.core->end());
    assert(r.port.clock - 5000 >= bs::kConnectWatchdogMs && r.port.closes == 0);
    assert(r.state() == SlotState::CANCELLING);
    assert(!r.core->begin("homewind", 0x07, Mode::MAC) && r.port.opens == 1);
    assert(!r.core->addByMac(SensorType::SPEED_CADENCE, kCscMacStr, 0, nullptr, r.now));
    r.failEv(r.gen());
    r.update();
    assert(r.core->quiesced());
  }
  {
    Rig r;                                             // step 4 timeout: DISCONNECTED never comes
    assert(r.core->begin("homewind", 0x07, Mode::MAC));
    r.toSubscribed();
    r.port.clock = 5000;
    assert(!r.core->end());
    assert(r.port.clock - 5000 >= bs::kEndDisconnectMs && r.port.closes == 0);
    assert(r.state() == SlotState::SUBSCRIBED && r.port.slotDisconnects[kHr] == 1);
    assert(!r.core->begin("homewind", 0x07, Mode::MAC));
  }
  {
    Rig r;                                             // pending remove: isConnected() already
    assert(r.core->begin("homewind", 0x07, Mode::MAC));   // false, still waited for
    r.toSubscribed();
    r.remove();
    r.port.connected = false;                          // DISCONNECTING
    r.port.disconnecting[kHr] = true;
    bool delivered = false;
    r.port.onTick = [&r, &delivered] {
      if (r.port.ticks == 5) {
        delivered = true;
        r.discEv(r.gen());
      }
    };
    assert(r.core->end() && delivered && r.port.closes == 1 && r.state() == SlotState::EMPTY);
  }
  {
    Rig r;                                             // failed deinit: not down
    assert(r.core->begin("homewind", 0x07, Mode::MAC));
    r.port.closeOk = false;
    assert(!r.core->end() && r.port.closes == 1);
    assert(!r.core->begin("homewind", 0x07, Mode::MAC));
    r.port.closeOk = true;
    assert(r.core->end() && r.port.closes == 2);
  }
  {
    Rig r;                                             // quiesce() without end(): one-way
    r.core->quiesce(r.now);
    assert(!r.core->begin("homewind", 0x07, Mode::MAC) && r.port.stackInits == 0);
  }
}

// CONNECTED and a callback's quiesce() in one drain: DISCOVERING, no pass run.
static void quiesceInDiscovering(Rig& r) {
  r.core->setEventCallback(onEventHook);
  g_hook = [&r](const EventInfo& info) {
    if (info.type == EventType::CENTRAL_CONNECTED) r.core->quiesce(r.now);
  };
  r.connectedEv(r.gen());
  assert(bs::pushCentralConnect(r.core->queue()));
  r.update();
  g_hook = nullptr;
  assert(r.state() == SlotState::DISCOVERING && r.port.gattPasses == 0);
}

// §4 end() steps 3–4: a client still DISCONNECTING while its slot already
// waits (resync repaired it) keeps its handle; end() waits for it, or fails.
static void testEndOrphanDisconnecting() {
  for (int arrives = 0; arrives < 2; ++arrives) {
    Rig r;
    assert(r.core->begin("homewind", 0x07, Mode::MAC));
    r.toConnecting();
    quiesceInDiscovering(r);
    r.port.connected = false;                          // DISCONNECTING, its DISCONNECTED missed
    r.port.disconnecting[kHr] = true;
    fillQueueToResync(*r.core);
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && r.core->quiesced());
    r.port.clock = 5000;
    r.port.onTick = [&r, arrives] {
      if (arrives && r.port.ticks == 5) r.discEv(r.gen());
    };
    if (arrives) {
      assert(r.core->end() && r.port.ticks == 5 && r.port.closes == 1);
    } else {
      assert(!r.core->end() && r.port.closes == 0);
      assert(r.port.clock - 5000 >= bs::kEndDisconnectMs);
      assert(!r.core->begin("homewind", 0x07, Mode::MAC));
    }
  }
  {
    Rig r;                                             // DISCONNECTED queued just before the
    assert(r.core->begin("homewind", 0x07, Mode::MAC));   // handle clears: handled before close
    r.toSubscribed();
    r.port.onConnHandle = [&r](uint8_t slot) {
      if (slot == kHr && r.port.slotDisconnects[kHr] == 1 && r.port.connected) r.discEv(r.gen());
    };
    g_eventCount = 0;
    assert(r.core->end() && r.port.ticks == 0 && r.port.closes == 1);
    assert(g_eventCount == 1);
    expectEvent(0, EventType::SENSOR_DISCONNECTED, SlotState::FOUND, 0x208);
  }
}

// §4 begin(): a name and UUID list that do not fit, or a failed stack init,
// leave the session as it was.
static void testBeginFailsUnchanged() {
  {
    Rig r;                                             // before the first begin()
    r.add();
    assert(!r.core->begin("abcdefghijkl", 0x03, Mode::NAME));   // 21-byte name + 2 UUIDs
    assert(r.port.stackInits == 0 && r.port.opens == 0 && r.port.serverStarts == 0);
    assert(r.state() == SlotState::WAIT_SCAN && r.core->mode() == Mode::MAC);
    r.port.stackInitOk = false;
    assert(!r.core->begin("homewind", 0x03, Mode::NAME));
    assert(r.port.stackInits == 1 && r.port.opens == 0 && r.state() == SlotState::WAIT_SCAN);
    r.port.stackInitOk = true;
    assert(r.core->begin("abcdefghijkl", 0x01, Mode::MAC));     // one UUID fits
    assert(strcmp(r.port.gapName, "abcdefghijkl-da4eb580") == 0);
    assert(r.state() == SlotState::EMPTY && r.port.serverMask == 0x01);
  }
  {
    Rig r;                                             // after end(): the latch stays
    assert(r.core->begin("homewind", 0x07, Mode::MAC));
    r.add();
    assert(r.core->end());
    assert(!r.core->begin("abcdefghijkl", 0x03, Mode::MAC));
    assert(r.port.stackInits == 1 && r.port.opens == 1 && r.state() == SlotState::WAIT_SCAN);
    assert(!r.core->addByMac(SensorType::SPEED_CADENCE, kCscMacStr, 0, nullptr, r.now));
    assert(r.core->begin("homewind", 0x07, Mode::MAC) && r.state() == SlotState::EMPTY);
  }
}

// begin() after a startDiscovery() before it: no open discovery, no epoch.
static void testBeginClearsDiscovery() {
  Rig r;
  assert(r.core->startDiscovery(0x01, 15000, r.now));
  r.update();
  assert(r.core->discovering() && r.core->scan().activeDiscEpoch() != 0);
  r.port.discOn = false;
  assert(r.core->begin("homewind", 0x07, Mode::MAC));
  assert(!r.core->discovering() && r.core->scan().activeDiscEpoch() == 0);
  assert(r.core->scan().owner() == ScanOwner::OFF && g_eventCount == 0);
  assert(r.core->startDiscovery(0x01, 15000, r.now));
}

// A DISCOVERING slot whose pass already failed: quiesce()'s disconnect is a
// second one (NimBLE: BLE_HS_EALREADY, still true); its one DISCONNECTED is
// the hard fail.
static void testQuiesceAfterFailedPass() {
  Rig r;
  r.port.gattOk = false;
  r.toConnecting();
  r.connectedEv(r.gen());
  r.update();
  assert(r.state() == SlotState::DISCOVERING && r.hr().failPending);
  assert(r.port.slotDisconnects[kHr] == 1 && r.port.gattPasses == 1);
  r.port.connected = false;
  r.port.disconnecting[kHr] = true;
  g_eventCount = 0;
  r.core->quiesce(r.now);
  assert(r.state() == SlotState::DISCOVERING && !r.core->quiesced());
  r.update();
  assert(r.port.gattPasses == 1 && g_eventCount == 0);
  r.discEv(r.gen(), 0x16);
  r.update();
  assert(r.state() == SlotState::WAIT_SCAN && r.hr().failCount == 1 && r.core->quiesced());
  assert(r.core->stats().connectFails == 0 && r.core->stats().disconnects == 1);
  assert(g_eventCount == 1);
  expectEvent(0, EventType::SENSOR_FAILED, SlotState::WAIT_SCAN);
  r.at(r.now + 60000);
  assert(r.port.connects == 1 && r.port.gattPasses == 1);
}

// HOST_RESET and resync under quiesce: link slots go to their wait state,
// advertising NimBLE restarted on sync is stopped again, quiesced() follows.
static void testQuiesceHostResetAndResync() {
  {
    Rig r;
    assert(!r.core->quiesced());
    assert(r.core->startServer());
    r.update();
    r.toConnecting();
    r.core->quiesce(r.now);
    assert(r.state() == SlotState::CANCELLING && !r.port.advOn);
    g_eventCount = 0;
    r.port.connOn = false;
    r.port.advOn = true;                               // NimBLEAdvertising::onHostSync
    assert(bs::pushHostReset(r.core->queue()));
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && !r.port.advOn && r.core->quiesced());
    assert(r.core->stats().advertisingEnforced == 1 && r.hr().failCount == 0);
    assert(g_eventCount == 1);
    expectEvent(0, EventType::HOST_RESET, SlotState::WAIT_SCAN, 0, SensorType{});
  }
  {
    Rig r;
    assert(r.core->startServer());
    r.update();
    r.toConnecting();
    quiesceInDiscovering(r);
    assert(!r.port.advOn && !r.core->quiesced());
    r.port.connected = false;                          // DISCONNECTED missed
    r.port.advOn = true;
    fillQueueToResync(*r.core);
    r.update();
    assert(r.state() == SlotState::WAIT_SCAN && !r.port.advOn && r.core->quiesced());
    assert(r.core->stats().advertisingEnforced == 1 && r.hr().failCount == 0);
    assert(r.port.gattPasses == 0);
  }
}

// NAME mode: DISCOVERING under quiesce ends in IDLE, without a pass or a window.
static void testNameQuiesceDiscovering() {
  Rig r(Mode::NAME);
  assert(r.core->addByName(SensorType::HEART_RATE, "HRM", r.now));
  r.update();
  assert(r.report(kMac, "HRM"));
  r.update();
  assert(r.state() == SlotState::CONNECTING);
  quiesceInDiscovering(r);
  assert(r.port.slotDisconnects[kHr] == 1 && !r.core->quiesced());
  r.discEv(r.gen());
  r.update();
  assert(r.state() == SlotState::IDLE && r.hr().failCount == 0 && r.core->quiesced());
  r.at(r.now + 60000);
  assert(r.port.discStarts == 1 && r.port.connects == 1 && r.port.gattPasses == 0);
}

// §4 info()/connected(): identity, state, battery only while SUBSCRIBED
// (2.x reset it on disconnect), last disconnect reason.
static void testInfoAndConnected() {
  Rig r;
  BikeSensor::SensorInfo info{};
  assert(!r.core->info(static_cast<SensorType>(0x08), info));
  assert(!r.core->connected(static_cast<SensorType>(0x08)));
#if !BBS_COMPILE_POWER
  assert(!r.core->info(SensorType::POWER, info) && !r.core->connected(SensorType::POWER));
#endif
  assert(r.core->info(SensorType::HEART_RATE, info));
  assert(info.state == SlotState::EMPTY && info.name[0] == '\0' && !info.hasBattery);
  assert(info.battery == 255 && info.lastDisconnectReason == 0 && !info.providesCadence);
  r.add();
  assert(r.core->info(SensorType::HEART_RATE, info) && info.state == SlotState::WAIT_SCAN);
  assert(strcmp(info.name, "HRM") == 0 && memcmp(info.mac, kMac, 6) == 0 && info.addrType == 0);
  assert(!r.core->connected(SensorType::HEART_RATE));
  r.update();
  r.match(r.core->scan().epoch(), kMac, 1);
  r.update();
  assert(r.core->info(SensorType::HEART_RATE, info) && info.state == SlotState::CONNECTING);
  assert(info.addrType == 1 && !r.core->connected(SensorType::HEART_RATE));
  bool inPass = false;                                 // during the GATT pass: DISCOVERING
  r.port.duringGatt = [&] {
    assert(r.core->info(SensorType::HEART_RATE, info) && info.state == SlotState::DISCOVERING);
    assert(!r.core->connected(SensorType::HEART_RATE));
    inPass = true;
  };
  r.connectedEv(r.gen());
  r.update();
  r.port.duringGatt = nullptr;
  assert(inPass && r.core->connected(SensorType::HEART_RATE));
  assert(r.core->info(SensorType::HEART_RATE, info) && info.state == SlotState::SUBSCRIBED);
  assert(info.hasBattery && info.battery == 80 && !info.encrypted && !info.bonded);
  assert(!info.providesCadence);
  r.discEv(r.gen(), 0x13);
  r.update();
  assert(!r.core->connected(SensorType::HEART_RATE));
  assert(r.core->info(SensorType::HEART_RATE, info) && info.state == SlotState::CONNECTING);
  assert(!info.hasBattery && info.battery == 255 && info.lastDisconnectReason == 0x13);
  r.port.gattBattery = 255;                            // no battery service
  r.connectedEv(r.gen());
  r.update();
  assert(r.core->info(SensorType::HEART_RATE, info) && info.state == SlotState::SUBSCRIBED);
  assert(!info.hasBattery && info.lastDisconnectReason == 0x13);
  r.remove();
  r.discEv(r.gen(), 0x16);
  r.update();
  assert(r.core->info(SensorType::HEART_RATE, info) && info.state == SlotState::EMPTY);
  assert(info.name[0] == '\0' && info.lastDisconnectReason == 0 && !info.hasBattery);
  const uint8_t zeros[6] = {};
  assert(memcmp(info.mac, zeros, 6) == 0);
}

// info() from another task while update() changes the slot (no deadlock, a
// consistent copy: battery known only in SUBSCRIBED).
static void testInfoConcurrent() {
  Rig r;
  std::atomic<bool> stop{false};
  std::thread reader([&] {
    BikeSensor::SensorInfo info{};
    while (!stop.load()) {
      assert(r.core->info(SensorType::HEART_RATE, info));
      assert(!info.hasBattery || info.state == SlotState::SUBSCRIBED);
    }
  });
  r.toSubscribed();
  r.core->setEventCallback(nullptr);                   // more events than g_events holds
  for (int i = 0; i < 200; ++i) {
    r.discEv(r.gen(), 0x08);
    r.update();
    r.connectedEv(r.gen());
    r.update();
    assert(r.state() == SlotState::SUBSCRIBED);
  }
  stop = true;
  reader.join();
}

// §4 deleteBonds(): false while a slot is CONNECTING/CANCELLING/DISCOVERING/
// SUBSCRIBED or without a stack; allowed in the other states and under quiesce.
static void testDeleteBonds() {
  Rig r;
  assert(!r.core->deleteBonds() && r.port.bondDeletes == 0);   // no stack
  r.port.stackUp = true;
  assert(r.core->deleteBonds() && r.port.bondDeletes == 1);    // all EMPTY
  r.add();
  assert(r.core->deleteBonds() && r.port.bondDeletes == 2);    // WAIT_SCAN
  r.port.cancelLeavesDiscOn = true;
  r.update();
  r.match(r.core->scan().epoch());
  r.update();
  assert(r.state() == SlotState::STOPPING_FOR_CONNECT);
  assert(r.core->deleteBonds() && r.port.bondDeletes == 3);
  r.port.cancelLeavesDiscOn = false;
  r.port.discOn = false;
  r.update();
  assert(r.state() == SlotState::CONNECTING && !r.core->deleteBonds());
  r.core->quiesce(r.now);
  assert(r.state() == SlotState::CANCELLING && !r.core->deleteBonds());
  r.failEv(r.gen());
  r.update();
  assert(r.state() == SlotState::WAIT_SCAN && r.core->quiesced());
  assert(r.core->deleteBonds() && r.port.bondDeletes == 4);    // under quiesce
  r.port.bondsOk = false;
  assert(!r.core->deleteBonds() && r.port.bondDeletes == 5);
  {
    Rig s;                                             // DISCOVERING (failed pass), SUBSCRIBED
    s.port.stackUp = true;
    s.port.gattOk = false;
    s.toConnecting();
    s.connectedEv(s.gen());
    s.update();
    assert(s.state() == SlotState::DISCOVERING && !s.core->deleteBonds());
    s.port.gattOk = true;
    s.discEv(s.gen());
    s.update();
    s.at(s.now + 40000);
    s.match(s.core->scan().epoch());
    s.update();
    s.connectedEv(s.gen());
    s.update();
    assert(s.state() == SlotState::SUBSCRIBED && !s.core->deleteBonds());
    s.addCsc();                                        // another slot idle: still false
    assert(!s.core->deleteBonds() && s.port.bondDeletes == 0);
  }
}

// §4 Stats: the store handler's atomic counters are copied into stats().
static void testBondStats() {
  Rig r;
  assert(r.core->stats().bondEvictions == 0 && r.core->stats().bondStoreRejects == 0);
  r.core->bondCounters().evictions += 2;
  r.core->bondCounters().rejects += 1;
  const BikeSensor::Stats st = r.core->stats();
  assert(st.bondEvictions == 2 && st.bondStoreRejects == 1);
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
  testSlotIndex();
  testDuplicateMac();
  testTwoMatchesOneWindow();
  testMatchAfterStopAndAcrossWindows();
  testInFlightBlocksOtherSlots();
  testHeldFoundDoesNotBlockOtherSlot();
  testClientEventRouting();
  testNeedScanAcrossSlots();
  testHostResetAndResyncAcrossSlots();
  testCallbackTouchesOtherSlot();
  testCallbackReplacesOtherSlot();
  testBothSubscribedDropOnePass();
#if BBS_COMPILE_POWER
  testThreeSlotsOrdering();
#endif
  testCrossModeAdd();
  testAddByNameAndDuplicates();
  testNameWindows();
  testNameHappyPath();
  testNameDropRefoundNewAddress();
  testNameHardFailIdleBackoff();
  testNameRuntimeGuard();
  testNameReplace();
  testNameTwoMatchesOneWindow();
  testNameSafetyHostResetOrphan();
  testNameCallbackAddsOtherSlot();
  testNameResync();
  testNameRemoveFromLinkStates();
  testFoundHoldBlocksScanBound();
  testNameSecondDeviceSameName();
  testNameMatchBadSlot();
  testNotifyOnlyWhenSubscribed();
  testNotifyTruncated();
  testNotifyMalformedStillRelayed();
  testCrankResetOnLinkEnd();
  testCentralEvents();
  testCentralCountResync();
  testAdvertisingDesiredState();
  testAdvertisingAfterHostResetAndResync();
  testAdvertisingHoldReleases();
  testStartServerIdleGuard();
  testAddOutsideMask();
  testHeartRateData();
  testCadenceData();
  testHeartRateGetter();
  testCadenceGetter();
  testServerTypes();
  testAdvertisingBudget();
  testServerName();
  testRefreshAdvertising();
  testDataCallbackReentrancy();
  testRefreshFromEventCallback();
  testLinkLossClearsMeasurement();
  testCscFeatureCopy();
#if BBS_COMPILE_POWER
  testPowerNotify();
  testPowerFeatureCopy();
  testPowerCadenceCallback();
  testCadenceSource();
  testPowerGetters();
  testPowerDuplicates();
  testPowerSecurity();
#if BBS_SENSOR_SECURITY
  testSkipProactiveSecure();
#endif
#else
  testPowerAbsent();
#endif
  testDiscoveryNatural();
  testDiscoveryFields();
  testDiscoveryDuration();
  testDiscoveryCancelsWindows();
  testDiscoveryStop();
  testDiscoveryTimeout();
  testDiscoveryHostReset();
  testDiscoveryResync();
  testDiscoveryStartFailed();
  testDiscoveryBlocksConnects();
  testDiscoveryStaleEpochWhilePending();
  testDiscoveryFromCallback();
  testDiscoveryConcurrentReports();
  testPauseResume();
  testStopNeverHeld();
  testQuiesceFinishesDiscovery();
  testQuiesceSlotsAndScans();
  testQuiesceResyncDiscovering();
  testScanning();
  testStatsDropCounters();
  testBeginEnd();
  testEndOrphanDisconnecting();
  testBeginFailsUnchanged();
  testBeginClearsDiscovery();
  testQuiesceAfterFailedPass();
  testQuiesceHostResetAndResync();
  testNameQuiesceDiscovering();
  testInfoAndConnected();
  testInfoConcurrent();
  testDeleteBonds();
  testBondStats();
  return 0;
}
