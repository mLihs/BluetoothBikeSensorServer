#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_timer.h>

#include "BBSCore.h"
#include "BBSEventPush.h"

// Device side of BlePort (§2): raw-GAP scan with a static callback, one
// reused client per slot with static callbacks, the reset detector, the
// GATT esp_timer and the local relay server. NimBLE callbacks only push
// events (§2 callback rule).
// Header-only until the public BikeSensor.cpp exists, so the 2.x build does
// not compile or link it.

static_assert(BLE_HS_EALREADY == BikeSensor::internal::kHsEAlready, "");
static_assert(BLE_HS_EBUSY == BikeSensor::internal::kHsEBusy, "");
static_assert(BLE_HS_ENOTSYNCED == BikeSensor::internal::kHsENotSynced, "");
static_assert(BLE_HS_CONN_HANDLE_NONE == BikeSensor::internal::kConnHandleNone, "");
static_assert(BLE_STORE_EVENT_OVERFLOW == BikeSensor::internal::kStoreEventOverflow, "");
static_assert(BLE_STORE_EVENT_FULL == BikeSensor::internal::kStoreEventFull, "");
static_assert(BLE_STORE_OBJ_TYPE_OUR_SEC == BikeSensor::internal::kStoreObjOurSec, "");
static_assert(BLE_STORE_OBJ_TYPE_PEER_SEC == BikeSensor::internal::kStoreObjPeerSec, "");
static_assert(BLE_STORE_OBJ_TYPE_CCCD == BikeSensor::internal::kStoreObjCccd, "");
static_assert(BLE_STORE_OBJ_TYPE_PEER_ADDR == BikeSensor::internal::kStoreObjPeerAddr, "");
static_assert(BLE_STORE_OBJ_TYPE_CSFC == BikeSensor::internal::kStoreObjCsfc, "");

namespace BikeSensor {
namespace internal {

class NimBLEPort final : public BlePort {
 public:
  static constexpr uint8_t kSlots = kSlotCount;

  // Before the first Core call. One instance per program; gapScanCb finds it
  // through self_.
  void attach(Core& core) {
    core_ = &core;
    self_ = this;
  }

  bool stackReady() override { return NimBLEDevice::isInitialized(); }
  // §6a: init() resets the sm_* settings (NimBLEDevice.cpp:996–1004), so they
  // follow every init: bonding, no MITM, Secure Connections, Just Works, and
  // ENC-only key distribution (sm_our/sm_their_key_dist). init() adds ID with
  // LL privacy; an IRK bond makes ble_gap_unpair return BLE_HS_EBUSY while
  // advertising or discovery runs (ble_gap.c:8313–8323).
  bool stackInit(const char* gapName) override {
    if (!NimBLEDevice::init(gapName)) return false;
#if BBS_SENSOR_SECURITY
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC);
    NimBLEDevice::setDeviceCallbacks(&storeCb_);
#endif
    return true;
  }

  // ble_gap_unpair per bond: also terminates links to bonded peers.
  bool deleteAllBonds() override { return NimBLEDevice::deleteAllBonds(); }

  // After NimBLEDevice::init(). False if a client, the reset detector or the
  // GATT timer cannot be created; a retry keeps what exists.
  bool open() override {
    for (uint8_t i = 0; i < kSlots; ++i) {
      if (clients_[i] == nullptr) clients_[i] = NimBLEDevice::createClient();
      if (clients_[i] == nullptr) return false;
      clientCb_[i].slot = i;
      clients_[i]->setClientCallbacks(&clientCb_[i], false);
    }
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan == nullptr) return false;
    scan->setScanCallbacks(&resetDetector_, false);
    if (gattTimer_ != nullptr) return true;
    esp_timer_create_args_t args{};
    args.callback = gattTimerCb;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "bbs_gatt";
    return esp_timer_create(&args, &gattTimer_) == ESP_OK;
  }

  // deinit(true) deletes the server, advertising, scan and clients; forget
  // them. A client with a handle would survive it: ble_hs_stop does not wait
  // for a terminating link, then deleteClient's disconnect() fails
  // (NimBLEDevice.cpp:365–369).
  bool close() override {
    for (NimBLEClient* client : clients_) {
      if (client != nullptr && client->getConnHandle() != BLE_HS_CONN_HANDLE_NONE) return false;
    }
    if (gattTimer_ != nullptr) {
      if (esp_timer_delete(gattTimer_) != ESP_OK) return false;
      gattTimer_ = nullptr;
    }
    if (!NimBLEDevice::deinit(true)) return false;
    for (uint8_t i = 0; i < kSlots; ++i) {
      clients_[i] = nullptr;
      localChr_[i] = nullptr;
      featureChr_[i] = nullptr;
    }
    locationChr_ = nullptr;
    server_ = nullptr;
    return true;
  }

  uint32_t nowMs() override { return millis(); }
  void waitTick() override { vTaskDelay(1); }

  bool hsSynced() override { return ble_hs_synced() != 0; }
  bool discActive() override { return ble_gap_disc_active() != 0; }
  bool connActive() override { return ble_gap_conn_active() != 0; }

  int discStart(uint16_t epoch, uint32_t durationMs) override {
    uint8_t ownAddrType = 0;
    const int rc = ble_hs_id_infer_auto(0, &ownAddrType);
    if (rc != 0) return rc;
    ble_gap_disc_params params{};
    params.itvl = 97;                   // 2.x server/sleep-scan parameters
    params.window = 67;
    params.passive = 0;
    params.filter_duplicates = 1;
    return ble_gap_disc(ownAddrType, static_cast<int32_t>(durationMs), &params, gapScanCb,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(epoch)));
  }

  int discCancel() override { return ble_gap_disc_cancel(); }

  // Before the first open(), and after close(), there are no clients:
  // nothing connects and nothing is connected.
  bool connect(uint8_t slot, const uint8_t mac[6], uint8_t addrType) override {
    NimBLEClient* client = clients_[slot];
    if (client == nullptr) return false;
    client->setConnectTimeout(BBS_CONNECT_TIMEOUT_MS);
    client->setConnectRetries(0);
    return client->connect(NimBLEAddress(mac, addrType), true, true);
  }

  int lastError(uint8_t slot) override {
    return clients_[slot] != nullptr ? clients_[slot]->getLastError() : BLE_HS_ENOTSYNCED;
  }
  bool cancelConnect(uint8_t slot) override {
    return clients_[slot] != nullptr && clients_[slot]->cancelConnect();
  }
  bool disconnect(uint8_t slot) override {
    return clients_[slot] != nullptr && clients_[slot]->disconnect();
  }
  bool isConnected(uint8_t slot) override {
    return clients_[slot] != nullptr && clients_[slot]->isConnected();
  }
  uint16_t connHandle(uint8_t slot) override {
    return clients_[slot] != nullptr ? clients_[slot]->getConnHandle() : BLE_HS_CONN_HANDLE_NONE;
  }

  bool gattPass(uint8_t slot, SensorType type, bool secure, GattResult& out) override {
    NimBLEClient* client = clients_[slot];
    out = GattResult{};
    // Power step 1 (§5.2): blocks until ENC_CHANGE or DISCONNECT; the return
    // value is not reliable (§0.12). Unencrypted is not fatal. The DISCONNECT
    // handler clears the link state before it releases the wait.
    if (secure) {
      client->secureConnection(false);
      if (!client->isConnected()) {
        out.secureCut = true;
        return false;
      }
    }
    NimBLERemoteService* service = client->getService(NimBLEUUID(serviceUuid(type)));
    if (service == nullptr) return false;
    NimBLERemoteCharacteristic* chr =
        service->getCharacteristic(NimBLEUUID(measurementUuid(type)));
    if (chr == nullptr || !chr->canNotify()) return false;
    uint32_t value = 0;
    if (type != SensorType::HEART_RATE) {          // optional, non-fatal
      out.hasFeature = readLe(service, featureUuid(type), type == SensorType::POWER ? 4 : 2,
                              out.feature);
    }
    if (type == SensorType::POWER && readLe(service, 0x2A5D, 1, value)) {
      out.hasLocation = true;
      out.location = static_cast<uint8_t>(value);
    }
    // Captures one byte: std::function keeps it inline, no heap.
    if (!chr->subscribe(true,
                        [slot](NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
                          pushNotify(self_->core_->queue(), slot,
                                     self_->core_->attemptGen(slot), data, len);
                        },
                        true)) {
      return false;
    }
    NimBLERemoteService* batt = client->getService(NimBLEUUID(static_cast<uint16_t>(0x180F)));
    if (readLe(batt, 0x2A19, 1, value)) out.battery = static_cast<uint8_t>(value);
    if (type == SensorType::POWER) {              // step 6
      const NimBLEConnInfo info = client->getConnInfo();
      out.encrypted = info.isEncrypted();
      out.bonded = info.isBonded();
    }
    return true;
  }

  void gattTimerStart(uint32_t ms) override {
    esp_timer_start_once(gattTimer_, static_cast<uint64_t>(ms) * 1000u);
  }
  void gattTimerStop() override { esp_timer_stop(gattTimer_); }

  // One local service per type in typeMask (the Core passes begin() mask ∩
  // compiled; rev 31: no runtime service changes), §5.4: measurement
  // (NOTIFY); CSC also Feature 0x2A5C, CP also Feature 0x2A65 and Sensor
  // Location 0x2A5D (READ, defaults until a GATT pass). Each measurement value
  // is pre-sized to BBS_NOTIFY_MAX_LEN, so setValue() in relay() never
  // reallocates (NimBLEAttValue grows only past its capacity).
  bool serverStart(uint8_t typeMask) override {
    if (server_ == nullptr) {
      NimBLEServer* server = NimBLEDevice::createServer();
      if (server == nullptr) return false;
      server->setCallbacks(&serverCb_, false);
      server->advertiseOnDisconnect(false);
      static const uint8_t kZeros[BBS_NOTIFY_MAX_LEN] = {};
      for (SensorType type : kTypes) {
        const int slot = slotIndex(type);
        if (slot < 0 || (typeMask & typeBit(type)) == 0) continue;
        NimBLEService* service = server->createService(NimBLEUUID(serviceUuid(type)));
        NimBLECharacteristic* chr = service->createCharacteristic(
            NimBLEUUID(measurementUuid(type)), NIMBLE_PROPERTY::NOTIFY);
        chr->setValue(kZeros, sizeof(kZeros));
        chrCb_[slot].slot = static_cast<uint8_t>(slot);
        chr->setCallbacks(&chrCb_[slot]);
        if (type != SensorType::HEART_RATE) {
          featureChr_[slot] =
              service->createCharacteristic(NimBLEUUID(featureUuid(type)), NIMBLE_PROPERTY::READ);
        }
        if (type == SensorType::POWER) {
          locationChr_ = service->createCharacteristic(NimBLEUUID(static_cast<uint16_t>(0x2A5D)),
                                                       NIMBLE_PROPERTY::READ);
        }
        setLocalFeature(type, type == SensorType::POWER ? kCpFeatureDefault : kCscFeatureDefault,
                        kCpLocationDefault);
        service->start();
        localChr_[slot] = chr;
      }
      server_ = server;
    }
    // The first start must happen while the stack is idle (no advertising,
    // scan, connect or connection); otherwise NimBLE asserts (ble_svc_gap_init).
    // Retry only after an unexpected error, never in a loop: each failed first
    // start re-runs ble_gatts_count_cfg and inflates NimBLE's pool sizes.
    return server_->start();
  }

  // Little endian, same sizes as created: setValue() does not reallocate.
  void setLocalFeature(SensorType type, uint32_t feature, uint8_t location) override {
    const int slot = slotIndex(type);
    if (slot < 0 || featureChr_[slot] == nullptr) return;
    const uint8_t bytes[4] = {static_cast<uint8_t>(feature), static_cast<uint8_t>(feature >> 8),
                              static_cast<uint8_t>(feature >> 16),
                              static_cast<uint8_t>(feature >> 24)};
    featureChr_[slot]->setValue(bytes, type == SensorType::POWER ? 4 : 2);
    if (type == SensorType::POWER && locationChr_ != nullptr) locationChr_->setValue(&location, 1);
  }

  // Edits NimBLE's own advertising data (no HCI); start() applies it, also
  // the restart after a host reset (NimBLEAdvertising::onHostSync).
  bool advSetData(const char* name, uint8_t typeMask) override {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->clearData();
    bool ok = adv->setName(name);
    for (SensorType type : kTypes) {
      if ((typeMask & typeBit(type)) != 0) {
        ok = adv->addServiceUUID(NimBLEUUID(serviceUuid(type))) && ok;
      }
    }
    return ok;
  }

  uint32_t chipId() override { return static_cast<uint32_t>(ESP.getEfuseMac()); }

  uint8_t centralCount() override {
    return server_ != nullptr ? server_->getConnectedCount() : 0;
  }
  bool advActive() override { return ble_gap_adv_active() != 0; }
  bool advStart() override { return NimBLEDevice::getAdvertising()->start(); }
  bool advStop() override { return NimBLEDevice::getAdvertising()->stop(); }

  // notify() without a value: ble_gatts_chr_updated() sends the stored value
  // to subscribed centrals only; no std::vector, no heap.
  void relay(uint8_t slot, const uint8_t* data, uint8_t len) override {
    NimBLECharacteristic* chr = localChr_[slot];
    if (chr == nullptr) return;
    chr->setValue(data, len);
    chr->notify();
  }

 private:
  class ClientCallbacks : public NimBLEClientCallbacks {
   public:
    uint8_t slot = 0;

    void onConnect(NimBLEClient*) override {
      pushConnected(self_->core_->queue(), slot, self_->core_->attemptGen(slot));
    }
    void onConnectFail(NimBLEClient*, int reason) override {
      pushConnectFail(self_->core_->queue(), slot, self_->core_->attemptGen(slot),
                      clampReason(reason));
    }
    void onDisconnect(NimBLEClient*, int reason) override {
      pushDisconnected(self_->core_->queue(), slot, self_->core_->attemptGen(slot),
                       clampReason(reason));
    }
  };

  class ServerCallbacks : public NimBLEServerCallbacks {
   public:
    void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
      pushCentralConnect(self_->core_->queue());
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
      pushCentralDisconnect(self_->core_->queue(), clampReason(reason));
    }
  };

  class CharacteristicCallbacks : public NimBLECharacteristicCallbacks {
   public:
    uint8_t slot = 0;

    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t subValue) override {
      pushSubscribe(self_->core_->queue(), subValue, slot);
    }
  };

  // Never scans. NimBLE calls onScanEnd(…, BLE_HS_ENOTSYNCED) after every
  // host resync (NimBLEScan::onHostSync).
  class ResetDetector : public NimBLEScanCallbacks {
   public:
    void onScanEnd(const NimBLEScanResults&, int reason) override {
      if (reason == BLE_HS_ENOTSYNCED) pushHostReset(self_->core_->queue());
    }
  };

#if BBS_SENSOR_SECURITY
  // §6a store status, host task without the host lock (ble_store.c:102;
  // overflow runs after the unlock, :53–71), so the store and GAP calls below
  // may take it. No disconnect, no heap.
  class StoreCallbacks : public NimBLEDeviceCallbacks {
   public:
    int onStoreStatus(ble_store_status_event* event, void* arg) override {
      Bonds bonds;
      const int type = event->event_code == BLE_STORE_EVENT_OVERFLOW ? event->overflow.obj_type : 0;
      switch (storeStatus(event->event_code, type, bonds, self_->core_->bondCounters())) {
        case StoreVerdict::PROCEED: return 0;
        case StoreVerdict::REJECT: return BLE_HS_ESTORE_CAP;
        default: return ble_store_util_status_rr(event, arg);
      }
    }
  };

  // storeStatus() view of the bond list, read on first use.
  struct Bonds {
    ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
    int n = -1;
    int count() {
      if (n < 0 && ble_store_util_bonded_peers(peers, &n, MYNEWT_VAL(BLE_STORE_MAX_BONDS)) != 0) {
        n = 0;
      }
      return n;
    }
    bool live(int i) { return ble_gap_conn_find_by_addr(&peers[i], nullptr) == 0; }
    bool erase(int i) { return ble_store_util_delete_peer(&peers[i]) == 0; }
    bool owns(int i, int objType) {
      Owner owner{&peers[i], false};
      ble_store_iterate(objType, ownerCb, &owner);
      return owner.found;
    }
    struct Owner {
      const ble_addr_t* peer;
      bool found;
    };
    static int ownerCb(int objType, ble_store_value* value, void* arg) {
      Owner& owner = *static_cast<Owner*>(arg);
      const ble_addr_t& addr =
          objType == BLE_STORE_OBJ_TYPE_CCCD ? value->cccd.peer_addr : value->csfc.peer_addr;
      owner.found = ble_addr_cmp(&addr, owner.peer) == 0;
      return owner.found ? 1 : 0;       // 1 stops the iteration
    }
  };
#endif

  static int16_t clampReason(int reason) {
    if (reason > INT16_MAX) return INT16_MAX;
    if (reason < INT16_MIN) return INT16_MIN;
    return static_cast<int16_t>(reason);
  }

  static uint16_t serviceUuid(SensorType type) {
    switch (type) {
      case SensorType::HEART_RATE: return 0x180D;
      case SensorType::SPEED_CADENCE: return 0x1816;
      default: return 0x1818;
    }
  }

  static uint16_t measurementUuid(SensorType type) {
    switch (type) {
      case SensorType::HEART_RATE: return 0x2A37;
      case SensorType::SPEED_CADENCE: return 0x2A5B;
      default: return 0x2A63;
    }
  }

  // CSC Feature, CP Feature.
  static uint16_t featureUuid(SensorType type) {
    return type == SensorType::SPEED_CADENCE ? 0x2A5C : 0x2A65;
  }

  // GATT pass: the first len bytes of a readable characteristic, little endian.
  // False if the service or characteristic is missing, or the read is short.
  static bool readLe(NimBLERemoteService* service, uint16_t uuid, size_t len, uint32_t& out) {
    NimBLERemoteCharacteristic* chr =
        service != nullptr ? service->getCharacteristic(NimBLEUUID(uuid)) : nullptr;
    if (chr == nullptr || !chr->canRead()) return false;
    const NimBLEAttValue value = chr->readValue();
    if (value.size() < len) return false;
    out = 0;
    for (size_t i = 0; i < len; ++i) out |= static_cast<uint32_t>(value[i]) << (8 * i);
    return true;
  }

  // NimBLE host task. cb_arg carries the scan epoch.
  static int gapScanCb(ble_gap_event* event, void* arg) {
    const uint16_t epoch = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(arg));
    Core& core = *self_->core_;
    if (event->type == BLE_GAP_EVENT_DISC) {
      const ble_gap_disc_desc& desc = event->disc;
      uint8_t mac[6];
      for (uint8_t i = 0; i < 6; ++i) mac[i] = desc.addr.val[5 - i];
      if (epoch == core.scan().activeDiscEpoch()) {
        // Discovery: upsert under tableMux, no event (§2 copy exception).
        // Fields before a malformed element are whole elements; they count.
        AdFields fields;
        parseAd(desc.data, desc.length_data, fields);
        core.discoveryReport(epoch, mac, desc.addr.type, fields);
        return 0;
      }
      if (core.mode() == Mode::MAC) {
        if (!core.scanFilterMatch(mac)) return 0;   // any slot's MAC; update() picks the slot
        AdFields fields;
        parseAd(desc.data, desc.length_data, fields);  // partial fields are fine for a MAC match
        pushScanMatch(core.queue(), epoch, mac, desc.addr.type, fields.name, fields.nameLen);
        return 0;
      }
      // NAME: this report alone (ADV or SCAN_RSP, no merge); elements before a
      // malformed one are kept.
      AdFields fields;
      parseAd(desc.data, desc.length_data, fields);
      const int slot = core.scanNameMatch(fields.name, fields.nameLen);
      if (slot < 0) return 0;
      pushScanMatch(core.queue(), epoch, mac, desc.addr.type, fields.name, fields.nameLen,
                    static_cast<uint8_t>(slot));
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
      pushScanEnd(core.queue(), epoch, clampReason(event->disc_complete.reason));
    }
    return 0;
  }

  // esp_timer task: only the guarded terminate (§9).
  static void gattTimerCb(void* arg) {
    GattWatchdog& watchdog = static_cast<NimBLEPort*>(arg)->core_->watchdog();
    if (watchdog.onTimer()) ble_gap_terminate(watchdog.connHandle(), BLE_ERR_REM_USER_CONN_TERM);
  }

  static constexpr SensorType kTypes[] = {SensorType::HEART_RATE, SensorType::SPEED_CADENCE,
                                          SensorType::POWER};
  static inline NimBLEPort* self_ = nullptr;
  Core* core_ = nullptr;
  NimBLEClient* clients_[kSlots] = {};
  ClientCallbacks clientCb_[kSlots];
  ResetDetector resetDetector_;
  esp_timer_handle_t gattTimer_ = nullptr;
  NimBLEServer* server_ = nullptr;
  ServerCallbacks serverCb_;
  CharacteristicCallbacks chrCb_[kSlots];
  NimBLECharacteristic* localChr_[kSlots] = {};
  NimBLECharacteristic* featureChr_[kSlots] = {};   // CSC 0x2A5C, CP 0x2A65
  NimBLECharacteristic* locationChr_ = nullptr;      // CP 0x2A5D
#if BBS_SENSOR_SECURITY
  StoreCallbacks storeCb_;
#endif
};

}  // namespace internal
}  // namespace BikeSensor
