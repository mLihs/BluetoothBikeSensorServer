#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <nvs.h>

#include "BBSTrainer.h"

// Device side of TrainerPort: the raw GAP link to the trainer with
// non-blocking ble_gattc_* procedures, the mirrored services on the relay
// server, the mirror's advertising and the table in NVS. Callbacks (and
// exchangeMtu()) only push TrnEvents. Included only by BBSNimBLEPort.h.

namespace BikeSensor {
namespace internal {

static_assert(BLE_HS_EDONE == kHsEDone && BLE_HS_ETIMEOUT == kHsETimeout, "");
// Discovered properties (BLE_GATT_CHR_PROP_*) go to createCharacteristic()
// (NIMBLE_PROPERTY) unchanged.
static_assert(BLE_GATT_CHR_PROP_READ == kPropRead && NIMBLE_PROPERTY::READ == kPropRead &&
                  BLE_GATT_CHR_PROP_WRITE_NO_RSP == kPropWriteNr && NIMBLE_PROPERTY::WRITE_NR == kPropWriteNr &&
                  BLE_GATT_CHR_PROP_WRITE == kPropWrite && NIMBLE_PROPERTY::WRITE == kPropWrite &&
                  BLE_GATT_CHR_PROP_NOTIFY == kPropNotify && NIMBLE_PROPERTY::NOTIFY == kPropNotify &&
                  BLE_GATT_CHR_PROP_INDICATE == kPropIndicate && NIMBLE_PROPERTY::INDICATE == kPropIndicate,
              "");

class NimBLETrainer final : public TrainerPort {
 public:
  void attach(TrnQueue& queue) {
    queue_ = &queue;
    self_ = this;
  }

  // close(): deinit(true) deleted the server.
  void forget() { memset(chr_, 0, sizeof(chr_)); }

  // serverStart(), mirror only, before server->start(). Every value is
  // pre-sized to kTrnValueMax: setValue() never reallocates and onWrite can
  // copy it without heap.
  void build(NimBLEServer* server, const TrnTable& t) {
    static const uint8_t kZeros[kTrnValueMax] = {};
    NimBLEDevice::setMTU(kTrnMtu);
    for (uint8_t s = 0; s < t.svcCount; ++s) {
      NimBLEService* service = server->createService(NimBLEUUID(t.svc[s].b, t.svc[s].len));
      for (uint8_t i = 0; i < t.chrCount; ++i) {
        const TrnChr& c = t.chr[i];
        if (c.svc != s) continue;
        NimBLECharacteristic* chr = service->createCharacteristic(
            NimBLEUUID(c.uuid.b, c.uuid.len), c.props, kTrnValueMax);
        chr->setValue(kZeros, sizeof(kZeros));
        chr->setValue(c.value, c.len);
        chrCb_[i].idx = i;
        chr->setCallbacks(&chrCb_[i]);
        chr_[i] = chr;
      }
    }
  }

  // § 2.4 mirror advertising. Unlike the hub's advSetData() this goes to the
  // controller at once; NimBLE keeps both copies for the restart after a host
  // reset (NimBLEAdvertising::onHostSync).
  bool advSet(const TrnTable& t, const char* name, bool hr) {
    TrnAdv a;
    buildMirrorAdv(t, name, hr, a);
    NimBLEAdvertisementData data, rsp;
    data.addData(a.adv, a.advLen);
    rsp.addData(a.rsp, a.rspLen);
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->enableScanResponse(true);
    return adv->setAdvertisementData(data) && adv->setScanResponseData(rsp);
  }

  // Any task, also the server callbacks. a: the link generation (arg());
  // nullptr for app events.
  static void push(TrnEv kind, void* a, int status, uint16_t h1 = 0, uint16_t h2 = 0,
                   uint8_t idx = 0, const uint8_t* data = nullptr, uint8_t len = 0,
                   const TrnUuid* uuid = nullptr) {
    const int16_t s = static_cast<int16_t>(status > INT16_MAX ? INT16_MAX : status);
    TrnEvent ev{kind, idx, gen(a), s, h1, h2, len, {}};
    if (uuid != nullptr) ev.uuid = *uuid;
    if (len > 0) memcpy(ev.data, data, len);
    self_->queue_->push(ev);
  }

  int connect(const uint8_t mac[6], uint8_t addrType, uint16_t gen) override {
    ble_addr_t peer{};
    peer.type = addrType;
    for (uint8_t i = 0; i < 6; ++i) peer.val[i] = mac[5 - i];
    const uint8_t own = ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, nullptr, nullptr) == 0
                            ? BLE_OWN_ADDR_PUBLIC
                            : BLE_OWN_ADDR_RANDOM;
    const int rc = ble_gap_connect(own, &peer, BBS_CONNECT_TIMEOUT_MS, nullptr, gapCb, arg(gen));
    ble_gap_conn_desc desc;
    if (rc == BLE_HS_EDONE && ble_gap_conn_find_by_addr(&peer, &desc) == 0) {
      ble_gap_terminate(desc.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return rc;
  }
  void cancelConnect() override { ble_gap_conn_cancel(); }
  void terminate(uint16_t conn) override { ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM); }
  // A handle may have been reused by a sensor or another central.
  bool connAlive(uint16_t conn, const uint8_t* peer) override {
    ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn, &desc) != 0) return false;
    if (peer == nullptr) return desc.role == BLE_GAP_ROLE_SLAVE;
    bool ours = desc.role == BLE_GAP_ROLE_MASTER;
    for (uint8_t i = 0; i < 6; ++i) ours = ours && desc.peer_ota_addr.val[5 - i] == peer[i];
    return ours;
  }
  int exchangeMtu(uint16_t conn, uint16_t gen) override {
    const int rc = ble_gattc_exchange_mtu(conn, mtuCb, arg(gen));
    if (rc != BLE_HS_EALREADY) return rc;
    // The trainer (NEO 2T) exchanged first; NimBLE then never calls mtuCb.
    const uint16_t mtu = ble_att_mtu(conn);
    if (mtu == 0) return BLE_HS_ENOTCONN;
    push(TrnEv::MTU, arg(gen), 0, mtu);
    return 0;
  }
  int discServices(uint16_t conn, uint16_t gen) override {
    return ble_gattc_disc_all_svcs(conn, svcCb, arg(gen));
  }
  int discChrs(uint16_t conn, uint16_t start, uint16_t end, uint16_t gen) override {
    return ble_gattc_disc_all_chrs(conn, start, end, chrCb, arg(gen));
  }
  int discDscs(uint16_t conn, uint16_t valHandle, uint16_t end, uint16_t gen) override {
    return ble_gattc_disc_all_dscs(conn, valHandle, end, dscCb, arg(gen));
  }
  int read(uint16_t conn, uint16_t handle, uint16_t gen) override {
    return ble_gattc_read(conn, handle, readCb, arg(gen));
  }
  int write(uint16_t conn, uint16_t handle, const uint8_t* data, uint8_t len, bool response,
            uint16_t gen) override {
    if (!response) return ble_gattc_write_no_rsp_flat(conn, handle, data, len);
    return ble_gattc_write_flat(conn, handle, data, len, writeCb, arg(gen));
  }

  // The custom mbuf also carries a 0-length value; notify(data, 0) would send
  // the stored one. ble_gatts_*_custom frees it on failure.
  int notifyApp(uint16_t conn, uint8_t idx, const uint8_t* data, uint8_t len,
                bool indicate) override {
    if (chr_[idx] == nullptr) return BLE_HS_ENOENT;
    os_mbuf* om = ble_hs_mbuf_from_flat(data, len);
    if (om == nullptr) return BLE_HS_ENOMEM;
    if (!indicate) return ble_gatts_notify_custom(conn, chr_[idx]->getHandle(), om);
    return ble_gatts_indicate_custom(conn, chr_[idx]->getHandle(), om);
  }
  void disconnectApp(uint16_t conn) override {
    if (connAlive(conn, nullptr)) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
  }
  // The server's own peer list also holds links whose APP_CONNECT was lost.
  // getPeerDevices() allocates a small vector (<= CONFIG_BT_NIMBLE_MAX_CONNECTIONS
  // handles); acceptable on this rare recovery path. Its getPeerInfo(index)
  // would not allocate but indexes the wrong slot (NimBLE 2.x).
  void disconnectApps() override {
    NimBLEServer* server = NimBLEDevice::getServer();
    if (server == nullptr) return;
    for (uint16_t conn : server->getPeerDevices()) disconnectApp(conn);
  }
  void setLocalValue(uint8_t idx, const uint8_t* data, uint8_t len) override {
    if (chr_[idx] != nullptr) chr_[idx]->setValue(data, len);
  }

  // NVS "bbs"/"trn_gatt"; a larger blob does not load.
  size_t loadTable(TrnBlob& b) override {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return 0;
    size_t len = sizeof(b);
    if (nvs_get_blob(h, kNvsKey, &b, &len) != ESP_OK) len = 0;
    nvs_close(h);
    return len;
  }
  bool storeTable(const TrnBlob& b) override {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_set_blob(h, kNvsKey, &b, sizeof(b)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
  }
  bool eraseTable() override {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    const esp_err_t rc = nvs_erase_key(h, kNvsKey);
    const bool ok = (rc == ESP_OK || rc == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
  }

  // The controller refuses a new random address while advertising. nullptr:
  // back to NimBLE's default (public).
  bool setOwnAddress(const uint8_t addr[6]) override {
    NimBLEDevice::getAdvertising()->stop();
    if (addr == nullptr) return NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);
    uint8_t le[6];
    for (uint8_t i = 0; i < 6; ++i) le[i] = addr[5 - i];
    return NimBLEDevice::setOwnAddr(le) && NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
  }
  uint64_t chipMac() override { return ESP.getEfuseMac(); }

 private:
  class ChrCallbacks : public NimBLECharacteristicCallbacks {
   public:
    uint8_t idx = 0;

    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
      struct Raw {
        uint8_t b[kTrnValueMax];
      };
      const Raw raw = chr->getValue<Raw>(nullptr, true);
      push(TrnEv::APP_WRITE, nullptr, 0, info.getConnHandle(), 0, idx, raw.b,
           static_cast<uint8_t>(chr->getLength()));
    }
    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& info, uint16_t subValue) override {
      push(TrnEv::APP_SUB, nullptr, 0, info.getConnHandle(), subValue, idx);
    }
    // Indications report only their end (EDONE, ETIMEOUT). NimBLE does not
    // fill connInfo for NOTIFY_TX; the trainer has one app link.
    void onStatus(NimBLECharacteristic*, NimBLEConnInfo&, int code) override {
      if (code == BLE_HS_EDONE || code == BLE_HS_ETIMEOUT) {
        push(TrnEv::APP_IND_DONE, nullptr, code, kConnHandleNone, 0, idx);
      }
    }
  };

  static void* arg(uint16_t gen) { return reinterpret_cast<void*>(static_cast<uintptr_t>(gen)); }
  static uint16_t gen(void* arg) { return static_cast<uint16_t>(reinterpret_cast<uintptr_t>(arg)); }

  static void pushUuid(TrnEv kind, void* a, const ble_uuid_any_t& uuid, uint16_t h1, uint16_t h2,
                       uint8_t idx) {
    TrnUuid u;
    if (uuid.u.type == BLE_UUID_TYPE_128) {
      makeUuid(uuid.u128.value, 16, u);
    } else {
      const bool u16 = uuid.u.type == BLE_UUID_TYPE_16;
      const uint32_t v = u16 ? uuid.u16.value : uuid.u32.value;
      const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                            static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
      makeUuid(b, u16 ? 2 : 4, u);
    }
    push(kind, a, 0, h1, h2, idx, nullptr, 0, &u);
  }

  static void pushData(TrnEv kind, void* a, int status, uint16_t h1, const os_mbuf* om, uint8_t ind = 0) {
    const uint16_t total = om != nullptr ? OS_MBUF_PKTLEN(om) : 0;
    uint8_t data[kTrnValueMax];
    const uint8_t len = static_cast<uint8_t>(total < kTrnValueMax ? total : kTrnValueMax);
    if (len > 0) os_mbuf_copydata(om, 0, len, data);
    push(kind, a, kind == TrnEv::NOTIFY ? total : status, h1, 0, ind, data, len);
  }

  // Item callbacks: status 0; the end: BLE_HS_EDONE (→ PROC_DONE 0) or an error.
  static int procEnd(const ble_gatt_error* error, void* a) {
    push(TrnEv::PROC_DONE, a, error->status == BLE_HS_EDONE ? 0 : error->status);
    return 0;
  }

  static int gapCb(ble_gap_event* event, void* a) {
    switch (event->type) {
      case BLE_GAP_EVENT_CONNECT:
        // As NimBLEClient: the link exists after a failed feature exchange.
        if (event->connect.status == 0 ||
            event->connect.status == BLE_HS_HCI_ERR(BLE_ERR_UNSUPP_REM_FEATURE)) {
          push(TrnEv::CONNECTED, a, 0, event->connect.conn_handle);
        } else {
          push(TrnEv::CONNECT_FAIL, a, event->connect.status);
        }
        break;
      case BLE_GAP_EVENT_DISCONNECT:
        push(TrnEv::DISCONNECTED, a, event->disconnect.reason);
        break;
      case BLE_GAP_EVENT_NOTIFY_RX:   // NimBLE confirms indications itself
        pushData(TrnEv::NOTIFY, a, 0, event->notify_rx.attr_handle, event->notify_rx.om,
                 event->notify_rx.indication);
        break;
      default:
        break;
    }
    return 0;
  }

  static int mtuCb(uint16_t, const ble_gatt_error* error, uint16_t mtu, void* a) {
    push(TrnEv::MTU, a, error->status, mtu);
    return 0;
  }
  static int svcCb(uint16_t, const ble_gatt_error* error, const ble_gatt_svc* svc, void* a) {
    if (error->status != 0) return procEnd(error, a);
    pushUuid(TrnEv::SVC, a, svc->uuid, svc->start_handle, svc->end_handle, 0);
    return 0;
  }
  static int chrCb(uint16_t, const ble_gatt_error* error, const ble_gatt_chr* chr, void* a) {
    if (error->status != 0) return procEnd(error, a);
    pushUuid(TrnEv::CHR, a, chr->uuid, chr->def_handle, chr->val_handle, chr->properties);
    return 0;
  }
  static int dscCb(uint16_t, const ble_gatt_error* error, uint16_t, const ble_gatt_dsc* dsc,
                   void* a) {
    if (error->status != 0) return procEnd(error, a);
    pushUuid(TrnEv::DSC, a, dsc->uuid, dsc->handle, 0, 0);
    return 0;
  }
  static int readCb(uint16_t, const ble_gatt_error* error, ble_gatt_attr* attr, void* a) {
    pushData(TrnEv::READ, a, error->status, error->att_handle,
             error->status == 0 ? attr->om : nullptr);
    return 0;
  }
  static int writeCb(uint16_t, const ble_gatt_error* error, ble_gatt_attr*, void* a) {
    push(TrnEv::WRITE_DONE, a, error->status);
    return 0;
  }

  static constexpr const char* kNvsNamespace = "bbs";
  static constexpr const char* kNvsKey = "trn_gatt";
  static inline NimBLETrainer* self_ = nullptr;
  TrnQueue* queue_ = nullptr;
  NimBLECharacteristic* chr_[kTrnChrMax] = {};
  ChrCallbacks chrCb_[kTrnChrMax];
};

}  // namespace internal
}  // namespace BikeSensor
