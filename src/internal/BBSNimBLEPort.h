#pragma once

#include <NimBLEDevice.h>
#include <esp_timer.h>

#include "BBSCore.h"
#include "BBSEventPush.h"

// Device side of BlePort (§2): raw-GAP scan with a static callback, one
// reused client per slot with static callbacks, the reset detector and the
// GATT esp_timer. NimBLE callbacks only push events (§2 callback rule).
// Header-only until the public BikeSensor.cpp exists, so the 2.x build does
// not compile or link it.

static_assert(BLE_HS_EALREADY == BikeSensor::internal::kHsEAlready, "");
static_assert(BLE_HS_EBUSY == BikeSensor::internal::kHsEBusy, "");
static_assert(BLE_HS_ENOTSYNCED == BikeSensor::internal::kHsENotSynced, "");

namespace BikeSensor {
namespace internal {

class NimBLEPort final : public BlePort {
 public:
  static constexpr uint8_t kSlots = kSlotCount;

  // After NimBLEDevice::init(). False if a client or the GATT timer cannot be
  // created. One instance per program; gapScanCb finds it through self_.
  bool begin(Core& core) {
    core_ = &core;
    self_ = this;
    for (uint8_t i = 0; i < kSlots; ++i) {
      clients_[i] = NimBLEDevice::createClient();
      if (clients_[i] == nullptr) return false;
      clientCb_[i].slot = i;
      clients_[i]->setClientCallbacks(&clientCb_[i], false);
    }
    NimBLEDevice::getScan()->setScanCallbacks(&resetDetector_, false);
    esp_timer_create_args_t args{};
    args.callback = gattTimerCb;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "bbs_gatt";
    return esp_timer_create(&args, &gattTimer_) == ESP_OK;
  }

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

  bool connect(uint8_t slot, const uint8_t mac[6], uint8_t addrType) override {
    NimBLEClient* client = clients_[slot];
    client->setConnectTimeout(BBS_CONNECT_TIMEOUT_MS);
    client->setConnectRetries(0);
    return client->connect(NimBLEAddress(mac, addrType), true, true);
  }

  int lastError(uint8_t slot) override { return clients_[slot]->getLastError(); }
  bool cancelConnect(uint8_t slot) override { return clients_[slot]->cancelConnect(); }
  bool disconnect(uint8_t slot) override { return clients_[slot]->disconnect(); }
  bool isConnected(uint8_t slot) override { return clients_[slot]->isConnected(); }
  uint16_t connHandle(uint8_t slot) override { return clients_[slot]->getConnHandle(); }

  bool gattPass(uint8_t slot, SensorType type, uint8_t& battery) override {
    NimBLEClient* client = clients_[slot];
    battery = 255;
    NimBLERemoteService* service = client->getService(NimBLEUUID(serviceUuid(type)));
    if (service == nullptr) return false;
    NimBLERemoteCharacteristic* chr =
        service->getCharacteristic(NimBLEUUID(measurementUuid(type)));
    if (chr == nullptr || !chr->canNotify()) return false;
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
    NimBLERemoteCharacteristic* level =
        batt != nullptr ? batt->getCharacteristic(NimBLEUUID(static_cast<uint16_t>(0x2A19)))
                        : nullptr;
    if (level != nullptr && level->canRead()) {
      const NimBLEAttValue value = level->readValue();
      if (value.size() >= 1) battery = value[0];
    }
    return true;
  }

  void gattTimerStart(uint32_t ms) override {
    esp_timer_start_once(gattTimer_, static_cast<uint64_t>(ms) * 1000u);
  }
  void gattTimerStop() override { esp_timer_stop(gattTimer_); }

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

  // Never scans. NimBLE calls onScanEnd(…, BLE_HS_ENOTSYNCED) after every
  // host resync (NimBLEScan::onHostSync).
  class ResetDetector : public NimBLEScanCallbacks {
   public:
    void onScanEnd(const NimBLEScanResults&, int reason) override {
      if (reason == BLE_HS_ENOTSYNCED) pushHostReset(self_->core_->queue());
    }
  };

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

  // NimBLE host task. cb_arg carries the scan epoch.
  static int gapScanCb(ble_gap_event* event, void* arg) {
    const uint16_t epoch = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(arg));
    Core& core = *self_->core_;
    if (event->type == BLE_GAP_EVENT_DISC) {
      const ble_gap_disc_desc& desc = event->disc;
      uint8_t mac[6];
      for (uint8_t i = 0; i < 6; ++i) mac[i] = desc.addr.val[5 - i];
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

  static inline NimBLEPort* self_ = nullptr;
  Core* core_ = nullptr;
  NimBLEClient* clients_[kSlots] = {};
  ClientCallbacks clientCb_[kSlots];
  ResetDetector resetDetector_;
  esp_timer_handle_t gattTimer_ = nullptr;
};

}  // namespace internal
}  // namespace BikeSensor
