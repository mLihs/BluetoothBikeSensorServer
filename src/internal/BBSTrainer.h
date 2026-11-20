#pragma once

#include <cstdint>
#include <cstring>

#include "BBSBlePort.h"
#include "BBSMux.h"
#include "BBSTrainerTable.h"
#include "BBSTypes.h"

// Trainer mirror runtime (TRAINER_PROXY_DESIGN § 2.2), Arduino-free: one
// trainer link driven by non-blocking GATT client procedures (F9), the setup
// read, the ordered app → trainer FIFO, trainer → app relay with indication
// pacing, the CCCD reference count and the link coupling. The port only
// pushes TrnEvents; everything else runs on the update() task.

namespace BikeSensor {
namespace internal {

constexpr uint8_t kTrnQueueDepth = 48;
static_assert(kTrnQueueDepth > kTrnChrMax, "one service's characteristics arrive in a burst");
constexpr uint8_t kTrnCtlReserve = 8;          // queue slots a NOTIFY cannot take
constexpr uint8_t kTrnFifoDepth = 16;
constexpr uint8_t kTrnIndDepth = 8;            // § 2.2.3, per app link
constexpr uint32_t kTrnConnectWatchdogMs = BBS_CONNECT_TIMEOUT_MS + 2000;
constexpr uint32_t kTrnLinkBudgetMs = 20000;   // connected → READY: MTU, discovery, reads
constexpr uint32_t kTrnRetryMs = 2000;         // ENOMEM/EBUSY retries; CCCD and capacity retries
constexpr uint32_t kTrnSeenHoldMs = 1000;      // after a failed attempt, once the trainer advertises
constexpr uint32_t kTrnHoldMaxMs = 30000;      // as the slots' kBackoffMaxMs; also a changed trainer
constexpr uint32_t kTrnSetupScanMs = 5000;     // setup: scan for the trainer's advertising
constexpr uint16_t kTrnMtu = 247;              // § 2.2.4, both sides
constexpr int kHsETimeout = 13;                // BLE_HS_ETIMEOUT
constexpr int kHsEDone = 14;                   // BLE_HS_EDONE
constexpr uint8_t kCccdUnknown = 0xFF;         // the trainer's CCCD after a failed write

// Trainer side (gen = link generation): CONNECTED .. NOTIFY. App side (h1 =
// app connection): APP_*.
enum class TrnEv : uint8_t {
  CONNECTED, CONNECT_FAIL, DISCONNECTED, MTU, SVC, CHR, DSC, READ, PROC_DONE, WRITE_DONE, NOTIFY,
  APP_CONNECT, APP_DISCONNECT, APP_MTU, APP_WRITE, APP_SUB, APP_IND_DONE
};

struct TrnEvent {
  TrnEv kind;
  uint8_t idx;          // CHR: properties; APP_WRITE/APP_SUB: characteristic index; NOTIFY: 1 indication
  uint16_t gen;
  int16_t status;       // rc, ATT status, reason; NOTIFY: original length
  uint16_t h1;          // conn handle; SVC: start; CHR: definition; DSC/NOTIFY: handle; MTU
  uint16_t h2;          // SVC: end; CHR: value handle; APP_SUB: CCCD value; APP_*: MTU
  uint8_t len;
  union {
    TrnUuid uuid;
    uint8_t data[kTrnValueMax];
  };
};

template <typename T, uint8_t N>
struct TrnRing {
  T buf[N];
  uint8_t head = 0, count = 0;
  T* push() { return count == N ? nullptr : &buf[(head + count++) % N]; }
  T& front() { return buf[head]; }
  void pop() {
    head = static_cast<uint8_t>((head + 1) % N);
    count--;
  }
};

// push() from any task (NimBLE callbacks, the port's exchangeMtu()), pop()
// from update(). A notification is dropped (counted) once only
// kTrnCtlReserve slots are left, so a stalled
// update() loses notifications first; an indication (NimBLE has confirmed it
// already) may use the reserve and is dropped only by a full ring. Losing
// any other event marks the queue, so update() rechecks the links instead
// of going on with a gap.
class TrnQueue {
 public:
  bool push(const TrnEvent& ev) {
    MuxGuard guard(mux_);
    const bool full = ev.kind == TrnEv::NOTIFY && ev.idx == 0 && ring_.count >= kTrnQueueDepth - kTrnCtlReserve;
    TrnEvent* slot = full ? nullptr : ring_.push();
    if (slot != nullptr) {
      *slot = ev;
    } else if (ev.kind == TrnEv::NOTIFY) {
      dropped_++;
    } else {
      lost_ = true;
    }
    return slot != nullptr;
  }
  bool pop(TrnEvent& out) {
    MuxGuard guard(mux_);
    if (ring_.count == 0) return false;
    out = ring_.front();
    ring_.pop();
    return true;
  }
  bool takeLost() {
    MuxGuard guard(mux_);
    const bool lost = lost_;
    lost_ = false;
    return lost;
  }
  uint32_t dropped() {
    MuxGuard guard(mux_);
    return dropped_;
  }
  void clear() {
    MuxGuard guard(mux_);
    ring_.count = 0;
    lost_ = false;
    dropped_ = 0;
  }

 private:
  Mux mux_;
  TrnRing<TrnEvent, kTrnQueueDepth> ring_;
  bool lost_ = false;
  uint32_t dropped_ = 0;
};

// Device: NimBLETrainer (BBSTrainerNimBLE.h). Host tests: a fake. int returns
// are NimBLE host codes (0 = started; results follow as TrnEvents).
class TrainerPort {
 public:
  // An orphaned link to the peer (BLE_HS_EDONE) is terminated by the port.
  virtual int connect(const uint8_t mac[6], uint8_t addrType, uint16_t gen) = 0;
  virtual void cancelConnect() = 0;
  virtual void terminate(uint16_t conn) = 0;
  // The link exists and still is ours: peer = the trainer's address (central
  // role); nullptr = an app (peripheral role).
  virtual bool connAlive(uint16_t conn, const uint8_t* peer) = 0;
  virtual int exchangeMtu(uint16_t conn, uint16_t gen) = 0;
  virtual int discServices(uint16_t conn, uint16_t gen) = 0;
  virtual int discChrs(uint16_t conn, uint16_t start, uint16_t end, uint16_t gen) = 0;
  virtual int discDscs(uint16_t conn, uint16_t valHandle, uint16_t end, uint16_t gen) = 0;
  virtual int read(uint16_t conn, uint16_t handle, uint16_t gen) = 0;
  // response: Write Request (WRITE_DONE follows), else Write Command.
  virtual int write(uint16_t conn, uint16_t handle, const uint8_t* data, uint8_t len,
                    bool response, uint16_t gen) = 0;
  // Mirrored characteristic idx to the app; an indication reports APP_IND_DONE.
  virtual int notifyApp(uint16_t conn, uint8_t idx, const uint8_t* data, uint8_t len,
                        bool indicate) = 0;
  virtual void disconnectApp(uint16_t conn) = 0;
  virtual void disconnectApps() = 0;            // every peripheral-role link
  virtual void setLocalValue(uint8_t idx, const uint8_t* data, uint8_t len) = 0;
  virtual size_t loadTable(TrnBlob& b) = 0;     // bytes read, 0 if none
  virtual bool storeTable(const TrnBlob& b) = 0;
  virtual bool eraseTable() = 0;
  // Static random (nullptr: public); stops advertising.
  virtual bool setOwnAddress(const uint8_t addr[6]) = 0;
  virtual uint64_t chipMac() = 0;

 protected:
  ~TrainerPort() = default;
};

class Trainer {
 public:
  void attach(TrainerPort& port) { port_ = &port; }
  TrnQueue& queue() { return queue_; }
  const TrnTable& table() const { return table_; }
  bool mirror() const { return mirror_; }
  // § 2.2.6, § 2.3: only with the trainer READY, no app and the own address set.
  bool advertise() const { return link_ == Link::READY && appConn_ == kConnHandleNone && addrSet_; }
  bool idle() const { return link_ == Link::DOWN; }
  bool connecting() const { return link_ == Link::CONNECTING; }
  bool ready() const { return link_ == Link::READY; }
  // The setup link connects with advertising off (S3 controller resources).
  bool quietAdv() const { return mode_ == Mode::SETUP && !capturing_; }
  uint16_t connHandle() const { return conn_; }
  uint16_t linkEnds() const { return linkEnds_; }   // ended links (mirror or setup)
  void setDataCallback(void (*cb)(uint16_t, const uint8_t*, uint8_t)) { dataCb_ = cb; }
  // Core's decoder for the read-along 0x2A63, called before the data callback.
  void setCpsHook(void* ctx, void (*hook)(void*, const uint8_t*, uint8_t, uint32_t)) {
    cpsCtx_ = ctx;
    cpsHook_ = hook;
  }
  bool forget() { return port_ != nullptr && port_->eraseTable(); }

  // Core::begin(), before the server starts: a fresh session. With `on`,
  // the stored table, its hash and the mirror's address (§ 2.3); false:
  // hub mode. The table stays as loaded until the next begin().
  bool begin(bool on, uint32_t now) {
    mirror_ = changed_ = false;
    mode_ = Mode::OFF;
    link_ = Link::DOWN;
    conn_ = appConn_ = kConnHandleNone;
    clearRelay();
    memset(internal_, 0, sizeof(internal_));
    queue_.clear();
    info_ = TrainerInfo{};
    info_.mtu = info_.appMtu = 23;
    fails_ = 0;
    hold(now, 0);
    if (on && port_ != nullptr && openTable(stored_, port_->loadTable(stored_))) {
      info_.hash = structureHash(table_);
      mirrorAddress(info_.hash, port_->chipMac(), info_.address);
      mirror_ = addrSet_ = port_->setOwnAddress(info_.address);
    }
    if (!mirror_) {
      memset(&stored_, 0, sizeof(stored_));
      info_.hash = 0;
      memset(info_.address, 0, sizeof(info_.address));
      if (port_ != nullptr) port_->setOwnAddress(nullptr);   // after a mirror session
    }
    if (mirror_) mode_ = Mode::MIRROR;
    target(table_.mac, false);
    return mirror_;
  }

  // After Core started the setup discovery: ends the mirror link and
  // captures the target's advertising and address type. The mirror stays
  // off until the next begin(); the GATT cannot change at runtime (F12).
  void setup(const uint8_t mac[6]) {
    dropLink();
    target(mac, true);
    mode_ = Mode::SETUP;
    capRetry_ = setupLink_ = false;
  }

  // Any task (gapScanCb), every scan report: marks the trainer as seen; in
  // setup also its latest ADV and SCAN_RSP.
  void capture(const uint8_t mac[6], uint8_t addrType, const uint8_t* ad, uint8_t len,
               bool scanRsp) {
    MuxGuard guard(adMux_);
    if (memcmp(mac, target_, sizeof(target_)) != 0) return;
    seen_ = true;
    if (!capturing_) return;
    targetType_ = addrType;
    if (len > 31) len = 31;
    memcpy(scanRsp ? cap_.rsp : cap_.adv, ad, len);
    (scanRsp ? cap_.rspLen : cap_.advLen) = len;
  }

  // Every DISCOVERY_DONE: the setup scan ends; connect if the target was seen.
  void discoveryDone() {
    if (mode_ != Mode::SETUP || !capturing_) return;
    bool seen;
    {
      MuxGuard guard(adMux_);
      capturing_ = false;
      seen = seen_;
    }
    if (!seen) return finishSetup(TrainerCheck::NOT_FOUND);
    fails_ = 0;
    hold(lastNow_, 0);
  }

  // The trainer link wants the initiator; Core grants it (mayConnect) when
  // no slot is in flight and nothing scans or connects.
  bool wantsLink(uint32_t now) const {
    return (mode_ == Mode::MIRROR || quietAdv()) && link_ == Link::DOWN &&
           now - holdFromMs_ >= holdMs_;
  }
  bool blocksScan() const { return wantsLink(lastNow_) || link_ == Link::CONNECTING; }

  void update(uint32_t now, bool mayConnect, EventCallback cb) {
    lastNow_ = now;
    cb_ = cb;
    if (port_ == nullptr) return;
    TrnEvent ev;
    for (uint8_t i = 0; i < kTrnQueueDepth && queue_.pop(ev); ++i) handle(ev, now);
    if (queue_.takeLost()) {
      resync(now);                            // first: a lost disconnect may have freed a handle
      if (link_ == Link::DISCOVERING || link_ == Link::READY) dropLink();
      if (mirror_) port_->disconnectApps();   // also an app whose APP_CONNECT was lost
    }
    bool seen;
    {
      MuxGuard guard(adMux_);
      seen = seen_;
      if (!capturing_) seen_ = false;   // the setup scan reads it at DISCOVERY_DONE
    }
    if (seen && mode_ == Mode::MIRROR && seenRetry_ && !changed_ && holdMs_ > kTrnSeenHoldMs) {
      holdMs_ = kTrnSeenHoldMs;
    }
    if (mirror_ && !addrSet_) addrSet_ = port_->setOwnAddress(info_.address);
    if (mayConnect && wantsLink(now)) connect(now);
    const uint32_t age = now - sinceMs_;
    if (link_ == Link::CONNECTING && age >= kTrnConnectWatchdogMs) {
      port_->cancelConnect();
      lose(now);
    } else if (link_ == Link::CLOSING && age >= kTrnConnectWatchdogMs) {
      if (!port_->connAlive(conn_, peer_)) return lose(now);
      port_->terminate(conn_);
      sinceMs_ = now;
    } else if (link_ == Link::DISCOVERING && age >= kTrnLinkBudgetMs) {
      dropLink();
    }
    if (link_ != Link::READY || appKicked_) kickApp();
    if (link_ == Link::DISCOVERING) pumpDiscovery();
    if (link_ != Link::READY) return;
    if (now - cccdRetryMs_ >= kTrnRetryMs) {
      cccdRetryMs_ = now;
      for (uint8_t i = 0; i < table_.chrCount; ++i) {
        if (upWanted_[i] == kCccdUnknown) refresh(i);
      }
    }
    pumpOps(now);
    pumpInd();
  }

  // § 2.2.3 internal holder (read-along) for every notify/indicate
  // characteristic with this 16-bit UUID. After begin().
  void readAlong(uint16_t uuid, bool on) {
    for (uint8_t i = 0; i < table_.chrCount; ++i) {
      if (uuid16(table_.chr[i].uuid) != uuid || allowed(i) == 0) continue;
      internal_[i] = on;
      refresh(i);
    }
  }

  // quiesce(): no link from now on; a running setup ends with LINK_FAILED.
  void stop() {
    if (mode_ == Mode::SETUP) return finishSetup(TrainerCheck::LINK_FAILED);
    mode_ = Mode::OFF;
    dropLink();
  }

  // NimBLE's host reset drops every link and the random address
  // (ble_hs_id_reset); update() sets it again until it succeeds.
  void hostReset(uint32_t now) {
    if (link_ != Link::DOWN) lose(now);
    if (appConn_ != kConnHandleNone) appGone();
    if (mirror_) addrSet_ = port_->setOwnAddress(info_.address);
  }

  void resync(uint32_t now) {
    checkApp();                         // first: lose() kicks a still known app
    if (conn_ != kConnHandleNone && !port_->connAlive(conn_, peer_)) lose(now);
  }

  // update() task, like stats().
  void info(TrainerInfo& out) {
    out = info_;
    out.state = state();
    out.mirror = mirror_;
    out.appConnected = appConn_ != kConnHandleNone;
    memcpy(out.mac, table_.mac, sizeof(out.mac));
    out.addrType = table_.addrType;
    out.services = table_.svcCount;
    out.characteristics = table_.chrCount;
    out.dropped += queue_.dropped();
  }

 private:
  enum class Mode : uint8_t { OFF, MIRROR, SETUP };
  enum class Link : uint8_t { DOWN, CONNECTING, DISCOVERING, READY, CLOSING };
  enum class Phase : uint8_t { MTU, SVCS, CHRS, DSCS, READS };
  struct Msg {                          // FIFO entry (app → trainer) or indication
    uint8_t idx;
    bool cccd;
    bool tried;
    uint8_t len;
    uint32_t sinceMs;
    uint8_t data[kTrnValueMax];
  };

  static bool busy(int rc) { return rc == kHsENoMem || rc == kHsEBusy; }

  // CCCD bits the characteristic allows: 1 notify, 2 indicate.
  uint8_t allowed(uint8_t i) const {
    return static_cast<uint8_t>((table_.chr[i].props & (kPropNotify | kPropIndicate)) >> 4);
  }

  void hold(uint32_t now, uint32_t ms) {
    holdFromMs_ = now;
    holdMs_ = ms;
  }

  void emit(EventType type, int reason) {
    if (cb_ == nullptr) return;
    EventInfo info{};
    info.type = type;
    info.reason = reason;
    cb_(info);
  }

  void target(const uint8_t mac[6], bool capture) {
    MuxGuard guard(adMux_);
    memcpy(target_, mac, sizeof(target_));
    capturing_ = capture;
    seen_ = false;
    cap_ = TrnAdv{};
  }

  void clearRelay() {
    ops_.count = inds_.count = 0;
    opBusy_ = indBusy_ = awaiting_ = false;
  }

  // § 2.2.6: an app link exists only while the trainer link is READY. An app
  // still there kConnectWatchdogMs after the kick is kicked again.
  void kickApp() {
    if (appConn_ == kConnHandleNone) return;
    if (appKicked_) {
      if (lastNow_ - kickMs_ < kTrnConnectWatchdogMs) return;
      if (!port_->connAlive(appConn_, nullptr)) return appGone();
    }
    port_->disconnectApp(appConn_);
    appKicked_ = true;
    kickMs_ = lastNow_;
  }

  // A lost APP_DISCONNECT would keep the mirror off the air for good.
  void checkApp() {
    if (appConn_ != kConnHandleNone && !port_->connAlive(appConn_, nullptr)) appGone();
  }

  void appGone() {
    appConn_ = kConnHandleNone;
    inds_.count = 0;
    indBusy_ = false;
    for (uint8_t i = 0; i < table_.chrCount; ++i) {
      if (appSub_[i] == 0) continue;
      appSub_[i] = 0;
      refresh(i);
    }
  }

  void dropLink() {
    if (link_ == Link::READY) emit(EventType::TRAINER_DISCONNECTED, 0);
    kickApp();
    clearRelay();
    if (link_ == Link::CONNECTING) {
      port_->cancelConnect();
      linkGen_++;
      link_ = Link::DOWN;
    } else if (link_ == Link::DISCOVERING || link_ == Link::READY) {
      port_->terminate(conn_);
      link_ = Link::CLOSING;
      sinceMs_ = lastNow_;
    }
  }

  void finishSetup(TrainerCheck result) {
    mode_ = Mode::OFF;
    {
      MuxGuard guard(adMux_);
      capturing_ = false;
    }
    dropLink();
    emit(EventType::TRAINER_SETUP_DONE, static_cast<int>(result));
  }

  void connect(uint32_t now) {
    linkGen_++;
    setupLink_ = mode_ == Mode::SETUP;
    memcpy(peer_, setupLink_ ? target_ : table_.mac, sizeof(peer_));
    const int rc = port_->connect(peer_, setupLink_ ? targetType_ : table_.addrType, linkGen_);
    if (rc != 0) return connectFailed(now, rc);
    link_ = Link::CONNECTING;
    sinceMs_ = now;
  }

  // Setup retries a capacity refusal once (advertising is off by then).
  void connectFailed(uint32_t now, int code) {
    if (mode_ != Mode::SETUP) return backoff(now, true);
    if ((code == kHsENoMem || code == kHciMemCapacity || code == kHciConnLimit) && !capRetry_) {
      capRetry_ = true;
      return hold(now, kTrnRetryMs);
    }
    finishSetup(TrainerCheck::LINK_FAILED);
  }

  // Only a failed connect may be shortened by a scan report: a trainer that
  // advertises but fails discovery or the check keeps the full backoff.
  void backoff(uint32_t now, bool connectFailed) {
    if (fails_ < 6) fails_++;
    const uint32_t ms = 1000u << (fails_ - 1);
    hold(now, changed_ || ms > kTrnHoldMaxMs ? kTrnHoldMaxMs : ms);
    seenRetry_ = connectFailed;
  }

  // Missed or forced end of the link: later events of this generation are stale.
  void lose(uint32_t now) {
    linkGen_++;
    linkLost(now, 0);
  }

  // Trainer lost → app disconnected (§ 2.2.6). A READY link reconnects at
  // once; otherwise the backoff. In setup, only the setup's own link ends
  // it, not the mirror link setup() closed.
  void linkLost(uint32_t now, int reason) {
    const Link was = link_;
    if (conn_ != kConnHandleNone) linkEnds_++;
    link_ = Link::DOWN;
    conn_ = kConnHandleNone;
    clearRelay();
    kickApp();
    if (was == Link::READY) emit(EventType::TRAINER_DISCONNECTED, reason);
    if (mode_ == Mode::SETUP) {
      if (setupLink_) finishSetup(TrainerCheck::LINK_FAILED);
    } else if (was == Link::READY) {
      fails_ = 0;
      hold(now, 0);
    } else {
      backoff(now, was == Link::CONNECTING);
    }
  }

  void handle(const TrnEvent& ev, uint32_t now) {
    if (ev.kind <= TrnEv::NOTIFY && ev.gen != linkGen_) {
      // A cancelled attempt's link, if the handle is still that peer's.
      if (ev.kind == TrnEv::CONNECTED && port_->connAlive(ev.h1, peer_)) port_->terminate(ev.h1);
      return;
    }
    // APP_IND_DONE carries no connection (NimBLE's NOTIFY_TX status).
    const bool app = appConn_ != kConnHandleNone &&
                     (ev.h1 == appConn_ || ev.kind == TrnEv::APP_IND_DONE);
    const bool chr = ev.idx < table_.chrCount;
    switch (ev.kind) {
      case TrnEv::CONNECTED:
        if (link_ != Link::CONNECTING) return port_->terminate(ev.h1);
        conn_ = ev.h1;
        link_ = Link::DISCOVERING;
        sinceMs_ = now;
        disc_.table.svcCount = disc_.table.chrCount = 0;
        overflow_ = false;
        info_.mtu = 23;
        return next(Phase::MTU);
      case TrnEv::CONNECT_FAIL:
        if (link_ != Link::CONNECTING) return;
        link_ = Link::DOWN;
        return connectFailed(now, ev.status);
      case TrnEv::DISCONNECTED:
        if (link_ != Link::DOWN) linkLost(now, ev.status);
        return;
      case TrnEv::NOTIFY:
        if (link_ == Link::READY) relayUp(ev);
        return;
      case TrnEv::WRITE_DONE:
        if (link_ != Link::READY || !opBusy_) return;
        opBusy_ = false;
        if (ev.status != 0) opFailed(ops_.front());   // ATT errors cannot reach the app (F5)
        return ops_.pop();
      case TrnEv::APP_CONNECT:
        if (!mirror_) return;
        if (appConn_ != kConnHandleNone) return port_->disconnectApp(ev.h1);   // one app
        appConn_ = ev.h1;
        info_.appMtu = ev.h2;
        appKicked_ = false;
        memset(appSub_, 0, sizeof(appSub_));
        return;
      case TrnEv::APP_DISCONNECT:
        if (app) appGone();
        return;
      case TrnEv::APP_MTU:
        if (app) info_.appMtu = ev.h2;
        return;
      case TrnEv::APP_WRITE:
        if (!app || !chr) return;
        if (link_ == Link::READY && pushOp(ev.idx, false, ev.data, ev.len)) return;
        info_.dropped++;
        return kickApp();
      case TrnEv::APP_SUB:
        if (!app || !chr) return;
        appSub_[ev.idx] = static_cast<uint8_t>(ev.h2 & 3);
        return refresh(ev.idx);
      case TrnEv::APP_IND_DONE:
        if (!app) return;
        indBusy_ = false;
        if (ev.status == kHsETimeout) info_.dropped++;
        return;
      default:
        if (link_ == Link::DISCOVERING) onDiscovery(ev);
        return;
    }
  }

  // ---- Discovery (§ 2.2.1) into disc_: MTU, services, characteristics,
  // CCCDs of notify/indicate characteristics, reads. One procedure at a time.

  void next(Phase phase) {
    phase_ = phase;
    cursor_ = 0;
    awaiting_ = false;
  }

  void onDiscovery(const TrnEvent& ev) {
    TrnTable& d = disc_.table;
    switch (ev.kind) {
      case TrnEv::MTU:
        if (ev.status == 0) info_.mtu = ev.h1;
        return next(Phase::SVCS);
      case TrnEv::SVC:
        if (skippedService(ev.uuid)) return;
        overflow_ = overflow_ || d.svcCount == kTrnSvcMax;
        if (overflow_) return;
        svcStart_[d.svcCount] = ev.h1;
        svcEnd_[d.svcCount] = ev.h2;
        d.svc[d.svcCount++] = ev.uuid;
        return;
      case TrnEv::CHR: {
        if (d.chrCount == kTrnChrMax) {
          overflow_ = true;
          return;
        }
        const uint8_t i = d.chrCount++;
        d.chr[i] = TrnChr{ev.uuid, cursor_, static_cast<uint8_t>(ev.idx & kTrnPropMask), false, 0, {}};
        chrVal_[i] = ev.h2;
        chrEnd_[i] = svcEnd_[cursor_];
        if (i > 0 && d.chr[i - 1].svc == cursor_) chrEnd_[i - 1] = static_cast<uint16_t>(ev.h1 - 1);
        cccd_[i] = 0;
        return;
      }
      case TrnEv::DSC:
        if (uuid16(ev.uuid) != 0x2902) return;
        cccd_[cursor_] = ev.h1;
        d.chr[cursor_].cccd = true;
        return;
      case TrnEv::READ: {
        TrnChr& c = d.chr[cursor_];
        c.len = ev.status == 0 ? ev.len : 0;
        memcpy(c.value, ev.data, c.len);
        break;
      }
      case TrnEv::PROC_DONE:
        if (ev.status != 0) return dropLink();
        if (phase_ == Phase::SVCS) return next(Phase::CHRS);
        break;
      default:
        return;
    }
    ++cursor_;
    awaiting_ = false;
  }

  // The next procedure of the current phase; skips what needs none.
  void pumpDiscovery() {
    TrnTable& d = disc_.table;
    while (link_ == Link::DISCOVERING && !awaiting_) {
      int rc = 0;
      const uint8_t i = cursor_;
      if (phase_ == Phase::MTU) {
        rc = port_->exchangeMtu(conn_, linkGen_);
        if (rc != 0 && !busy(rc)) {
          next(Phase::SVCS);
          continue;
        }
      } else if (phase_ == Phase::SVCS) {
        rc = port_->discServices(conn_, linkGen_);
      } else if (phase_ == Phase::CHRS) {
        if (i >= d.svcCount) {
          // § 2.3: an equal structure maps mirrored index i to this link's handles.
          dropEmptyServices(d, mode_ == Mode::SETUP ? &cap_ : nullptr);
          if (mode_ == Mode::MIRROR && (overflow_ || structureHash(d) != info_.hash)) {
            return changedTrainer();
          }
          next(Phase::DSCS);
          continue;
        }
        rc = port_->discChrs(conn_, svcStart_[i], svcEnd_[i], linkGen_);
      } else if (i >= d.chrCount) {
        if (phase_ == Phase::READS) return finishDiscovery();
        next(Phase::READS);
        continue;
      } else if (phase_ == Phase::DSCS) {
        const uint8_t notifying = d.chr[i].props & (kPropNotify | kPropIndicate);
        if (notifying == 0 || chrVal_[i] >= chrEnd_[i]) {
          ++cursor_;
          continue;
        }
        rc = port_->discDscs(conn_, chrVal_[i], chrEnd_[i], linkGen_);
      } else {
        if ((d.chr[i].props & kPropRead) == 0) {
          ++cursor_;
          continue;
        }
        rc = port_->read(conn_, chrVal_[i], linkGen_);
      }
      if (rc == 0) {
        awaiting_ = true;
      } else if (!busy(rc)) {
        dropLink();
      }
      return;
    }
  }

  // Not advertised (§ 2.3); reported once, retried after kTrnHoldMaxMs.
  void changedTrainer() {
    if (!changed_) emit(EventType::TRAINER_CONNECTED, 1);
    changed_ = true;
    dropLink();
  }

  // Setup: check, store, done. Mirror: an incomplete discovery retries; else
  // READY with refreshed read values and the internal subscriptions.
  void finishDiscovery() {
    TrnTable& d = disc_.table;
    if (mode_ == Mode::SETUP) {
      memcpy(d.mac, target_, sizeof(d.mac));
      d.addrType = targetType_;
      copyAdFields(cap_, d);
      TrainerCheck result = checkTable(d, overflow_, &cap_);
      if (result == TrainerCheck::OK) {
        sealTable(disc_);
        if (!port_->storeTable(disc_)) result = TrainerCheck::STORE_FAILED;
      }
      return finishSetup(result);
    }
    if (checkTable(d, false, nullptr) != TrainerCheck::OK) return dropLink();
    changed_ = false;
    fails_ = 0;
    link_ = Link::READY;
    for (uint8_t i = 0; i < d.chrCount; ++i) {
      if ((d.chr[i].props & kPropRead) != 0) port_->setLocalValue(i, d.chr[i].value, d.chr[i].len);
      upWanted_[i] = 0;
      refresh(i);
    }
    emit(EventType::TRAINER_CONNECTED, 0);
  }

  // ---- Relay (§ 2.2.3)

  // Rev 8 CCCD reference count: the trainer's CCCD carries the union of the
  // app's bits and the internal read-along; only changes are written, in
  // FIFO order with the app's writes. A failed write is retried.
  void refresh(uint8_t i) {
    const uint8_t allow = allowed(i);
    uint8_t want = appSub_[i] & allow;
    if (internal_[i]) want |= (allow & 1) ? 1 : allow;
    if (link_ != Link::READY || want == upWanted_[i]) return;
    const uint8_t value[2] = {want, 0};
    upWanted_[i] = pushOp(i, true, value, sizeof(value)) ? want : kCccdUnknown;
  }

  bool pushOp(uint8_t idx, bool cccd, const uint8_t* data, uint8_t len) {
    Msg* op = ops_.push();
    if (op == nullptr) return false;
    *op = Msg{idx, cccd, false, len, 0, {}};
    memcpy(op->data, data, len);
    return true;
  }

  void opFailed(const Msg& op) {
    info_.writeErrors++;
    if (op.cccd) upWanted_[op.idx] = kCccdUnknown;
  }

  // Head first; a Write Request or CCCD write holds the FIFO until
  // WRITE_DONE, so a subscription reaches the trainer before the next write.
  // Write type from the trainer's properties (F6). ENOMEM/EBUSY retry for
  // kTrnRetryMs; any other failure is counted.
  void pumpOps(uint32_t now) {
    while (!opBusy_ && ops_.count > 0) {
      Msg& op = ops_.front();
      const uint16_t handle = op.cccd ? cccd_[op.idx] : chrVal_[op.idx];
      const bool response = op.cccd || (table_.chr[op.idx].props & kPropWrite) != 0;
      // § 2.2.4: no long writes.
      const int rc = handle == 0 || op.len + 3u > info_.mtu
                         ? -1
                         : port_->write(conn_, handle, op.data, op.len, response, linkGen_);
      if (rc == 0 && !op.cccd) info_.down++;
      if (rc == 0 && response) {
        opBusy_ = true;
        return;
      }
      if (busy(rc)) {
        if (!op.tried) op.sinceMs = now;
        op.tried = true;
        if (now - op.sinceMs < kTrnRetryMs) return;
      }
      if (rc != 0) opFailed(op);
      ops_.pop();
    }
  }

  // Trainer → internal read-along and app. Notifications go out at once;
  // indications queue (kTrnIndDepth), one outstanding (F8); overflow drops
  // the app. Oversize: truncated by the cap or longer than the app's MTU − 3
  // (F11: NimBLE truncates).
  void relayUp(const TrnEvent& ev) {
    uint8_t i = 0;
    while (i < table_.chrCount && chrVal_[i] != ev.h1) ++i;
    if (i == table_.chrCount) return;
    info_.up++;
    const bool toApp = appConn_ != kConnHandleNone && appSub_[i] != 0;
    if (ev.status > ev.len || (toApp && ev.len + 3u > info_.appMtu)) info_.oversize++;
    if (internal_[i]) {
      const uint16_t uuid = uuid16(table_.chr[i].uuid);
      if (uuid == 0x2A63 && cpsHook_ != nullptr) cpsHook_(cpsCtx_, ev.data, ev.len, lastNow_);
      if (dataCb_ != nullptr) dataCb_(uuid, ev.data, ev.len);
    }
    if (!toApp) return;
    if ((appSub_[i] & 1) != 0) {
      if (port_->notifyApp(appConn_, i, ev.data, ev.len, false) != 0) info_.dropped++;
      return;
    }
    Msg* in = inds_.push();
    if (in == nullptr) {
      info_.dropped++;
      return kickApp();
    }
    *in = Msg{i, false, false, ev.len, 0, {}};
    memcpy(in->data, ev.data, ev.len);
  }

  // Skips indications the app unsubscribed from meanwhile.
  void pumpInd() {
    while (!indBusy_ && inds_.count > 0 && appConn_ != kConnHandleNone) {
      const Msg& in = inds_.front();
      if ((appSub_[in.idx] & 2) != 0) {
        const int rc = port_->notifyApp(appConn_, in.idx, in.data, in.len, true);
        if (busy(rc)) return;
        indBusy_ = rc == 0;
        if (rc != 0) info_.dropped++;
      }
      inds_.pop();
    }
  }

  TrainerState state() const {
    if (mode_ == Mode::SETUP) return TrainerState::SETUP;
    if (mode_ == Mode::OFF) return TrainerState::OFF;
    switch (link_) {
      case Link::CONNECTING: return TrainerState::CONNECTING;
      case Link::DISCOVERING: return TrainerState::DISCOVERING;
      case Link::READY: return TrainerState::READY;
      default: return changed_ ? TrainerState::CHANGED : TrainerState::WAITING;
    }
  }

  TrainerPort* port_ = nullptr;
  EventCallback cb_ = nullptr;
  void (*dataCb_)(uint16_t, const uint8_t*, uint8_t) = nullptr;
  void* cpsCtx_ = nullptr;
  void (*cpsHook_)(void*, const uint8_t*, uint8_t, uint32_t) = nullptr;
  TrnQueue queue_;
  TrnBlob stored_{};                    // the mirror's table, read-only after begin()
  const TrnTable& table_ = stored_.table;
  TrnBlob disc_{};                      // this link's discovery; setup stores it
  Mode mode_ = Mode::OFF;
  Link link_ = Link::DOWN;
  bool mirror_ = false;
  bool changed_ = false;                // structure differs from the table; reported once
  bool addrSet_ = false;                // the mirror's address is the controller's
  bool capRetry_ = false;
  bool setupLink_ = false;              // the link was opened by the setup
  bool seenRetry_ = false;              // the last failure was a connect
  uint8_t peer_[6] = {};                // address of the current link
  TrainerInfo info_{};                  // counters, MTUs, hash, address; info() adds the rest
  uint16_t linkGen_ = 0;
  uint16_t linkEnds_ = 0;
  uint16_t conn_ = kConnHandleNone;
  uint32_t lastNow_ = 0;
  uint32_t sinceMs_ = 0;                // entry into CONNECTING, DISCOVERING or CLOSING
  uint32_t holdFromMs_ = 0;             // next connect not before holdFromMs_ + holdMs_
  uint32_t holdMs_ = 0;
  uint8_t fails_ = 0;
  // Scan reports of target_ (any task, under adMux_): seen_, and in setup
  // (capturing_) the ADV, SCAN_RSP and address type.
  Mux adMux_;
  bool capturing_ = false;
  bool seen_ = false;
  uint8_t target_[6] = {};
  uint8_t targetType_ = 0;
  TrnAdv cap_{};
  // Discovery.
  Phase phase_ = Phase::MTU;
  uint8_t cursor_ = 0;
  bool awaiting_ = false;
  bool overflow_ = false;
  uint16_t svcStart_[kTrnSvcMax] = {}, svcEnd_[kTrnSvcMax] = {};
  uint16_t chrVal_[kTrnChrMax] = {}, chrEnd_[kTrnChrMax] = {}, cccd_[kTrnChrMax] = {};
  // Relay.
  uint16_t appConn_ = kConnHandleNone;
  bool appKicked_ = false;
  uint32_t kickMs_ = 0;
  uint8_t appSub_[kTrnChrMax] = {};     // app's CCCD bits (1 notify, 2 indicate)
  bool internal_[kTrnChrMax] = {};      // read-along holder
  uint8_t upWanted_[kTrnChrMax] = {};   // last CCCD value queued to the trainer
  uint32_t cccdRetryMs_ = 0;
  TrnRing<Msg, kTrnFifoDepth> ops_;
  bool opBusy_ = false;
  TrnRing<Msg, kTrnIndDepth> inds_;
  bool indBusy_ = false;
};

}  // namespace internal
}  // namespace BikeSensor
