#include "BBSTrainer.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

using BikeSensor::EventInfo;
using BikeSensor::EventType;
using BikeSensor::TrainerCheck;
using BikeSensor::TrainerInfo;
using BikeSensor::TrainerState;
namespace bs = BikeSensor::internal;
using bs::TrnEv;
using bs::TrnEvent;
using bs::TrnTable;
using bs::TrnUuid;

constexpr uint8_t R = bs::kPropRead, W = bs::kPropWrite, WNR = bs::kPropWriteNr;
constexpr uint8_t N = bs::kPropNotify, I = bs::kPropIndicate;

static TrnUuid u16(uint16_t v) {
  return TrnUuid{2, {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)}};
}

// "6e40fec1-b5a3-..." → little endian.
static TrnUuid u128(const char* s) {
  auto hex = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
  TrnUuid u{};
  u.len = 16;
  int k = 15;
  for (const char* p = s; *p != 0 && k >= 0;) {
    if (*p == '-') {
      ++p;
      continue;
    }
    u.b[k--] = static_cast<uint8_t>(hex(p[0]) << 4 | hex(p[1]));
    p += 2;
  }
  return u;
}

static void addSvc(TrnTable& t, TrnUuid u) { t.svc[t.svcCount++] = u; }
static void addChr(TrnTable& t, TrnUuid u, uint8_t props, std::vector<uint8_t> value = {}) {
  bs::TrnChr& c = t.chr[t.chrCount++];
  c.uuid = u;
  c.svc = static_cast<uint8_t>(t.svcCount - 1);
  c.props = props;
  c.cccd = (props & (N | I)) != 0;
  c.len = static_cast<uint8_t>(value.size());
  memcpy(c.value, value.data(), value.size());
}
static std::vector<uint8_t> str(const char* s) { return std::vector<uint8_t>(s, s + strlen(s)); }

const uint8_t kNeoMac[6] = {0xE4, 0x6D, 0x1A, 0x2B, 0x3C, 0x4D};
constexpr uint64_t kChip = 0x1234;

// Tacx NEO 2T as discovered in the G5 log: 8 services, 25 characteristics.
static void neo(TrnTable& t) {
  memset(&t, 0, sizeof(t));
  memcpy(t.mac, kNeoMac, 6);
  addSvc(t, u16(0x1816));
  addChr(t, u16(0x2A5B), N);
  addChr(t, u16(0x2A5C), R, {0x07, 0x00});
  addChr(t, u16(0x2A5D), R, {0x0C});
  addChr(t, u16(0x2A55), W | I);
  addSvc(t, u16(0x1818));
  addChr(t, u16(0x2A63), N);
  addChr(t, u16(0x2A65), R, {0x2C, 0x00, 0x01, 0x00});
  addChr(t, u16(0x2A5D), R, {0x0C});
  addChr(t, u16(0x2A64), N);
  addChr(t, u16(0x2A66), W | I);
  addSvc(t, u128("6e40fec1-b5a3-f393-e0a9-e50e24dcca9e"));
  addChr(t, u128("6e40fec2-b5a3-f393-e0a9-e50e24dcca9e"), N);
  addChr(t, u128("6e40fec3-b5a3-f393-e0a9-e50e24dcca9e"), W | WNR);
  addSvc(t, u16(0x180A));
  addChr(t, u16(0x2A29), R, str("Tacx"));
  addChr(t, u16(0x2A24), R, str("T2875"));
  addChr(t, u16(0x2A25), R, str("TA1019525"));
  addChr(t, u16(0x2A27), R, str("1"));
  addChr(t, u16(0x2A26), R, str("2.7.0"));
  addSvc(t, u128("cf285001-00db-15a1-0e3f-ffa5794c3719"));
  addChr(t, u128("cf285002-00db-15a1-0e3f-ffa5794c3719"), W | WNR);
  addChr(t, u128("cf285003-00db-15a1-0e3f-ffa5794c3719"), N);
  addSvc(t, u128("fe03a000-17d0-470a-8798-4ad3e1c1f35b"));
  addChr(t, u128("fe03a001-17d0-470a-8798-4ad3e1c1f35b"), R, {0x00, 0x00, 0x40, 0x00});
  addChr(t, u128("fe03a00f-17d0-470a-8798-4ad3e1c1f35b"), W | I);
  addSvc(t, u16(0xFC82));
  addChr(t, u128("00000002-19ca-4651-86e5-fa29dcdd09d1"), N);
  addChr(t, u128("00000003-19ca-4651-86e5-fa29dcdd09d1"), WNR);
  addChr(t, u128("00000004-19ca-4651-86e5-fa29dcdd09d1"), I);
  addSvc(t, u128("fa1c030e-52d1-2062-0777-bc61f0b8e27b"));
  addChr(t, u128("fa1c0302-52d1-2062-0777-bc61f0b8e27b"), W | WNR);
  addChr(t, u128("fa1c0301-52d1-2062-0777-bc61f0b8e27b"), N);
}

// FC82's characteristics moved to the previous service: FC82 is empty (G5).
static void emptyFc82(TrnTable& t) {
  const int fc82 = bs::findService(t, u16(0xFC82));
  for (uint8_t i = 0; i < t.chrCount; ++i) {
    if (t.chr[i].svc == fc82) t.chr[i].svc = static_cast<uint8_t>(fc82 - 1);
  }
}

// The NEO's advertising (G5): ADV and SCAN_RSP.
const uint8_t kNeoAdv[] = {0x02, 0x01, 0x06, 0x04, 0x16, 0x82, 0xFC, 0x01, 0x11, 0x07,
                           0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3,
                           0xA3, 0xB5, 0xC1, 0xFE, 0x40, 0x6E};
const uint8_t kNeoRsp[] = {0x12, 0x09, 'T', 'a', 'c', 'x', ' ', 'N', 'e', 'o', ' ', '2', 'T', ' ',
                           '1', '4', '1', '8', '9', 0x03, 0x19, 0x80, 0x04, 0x07, 0x03, 0x18,
                           0x18, 0x82, 0xFC, 0x0A, 0x18};

// pad: the ADV zero-padded to 31 bytes, as some controllers report it.
static bs::TrnAdv neoAd(bool rsp = true, bool pad = false) {
  bs::TrnAdv a{};
  memcpy(a.adv, kNeoAdv, sizeof(kNeoAdv));
  a.advLen = pad ? 31 : sizeof(kNeoAdv);
  if (rsp) memcpy(a.rsp, kNeoRsp, sizeof(kNeoRsp));
  a.rspLen = rsp ? sizeof(kNeoRsp) : 0;
  return a;
}

static int idxOf(const TrnTable& t, uint16_t uuid, int nth = 0) {
  for (uint8_t i = 0; i < t.chrCount; ++i) {
    if (bs::uuid16(t.chr[i].uuid) == uuid && nth-- == 0) return i;
  }
  return -1;
}

// ---- Fake trainer GATT server and app side

struct Write {
  uint16_t handle;
  std::vector<uint8_t> data;
  bool response;
};
struct Sent {
  uint16_t conn;
  uint8_t idx;
  uint8_t len;
  bool indicate;
  std::vector<uint8_t> data;
};

constexpr uint16_t kTrainerConn = 1;
constexpr uint16_t kAppConn = 5;

class FakePort : public bs::TrainerPort {
 public:
  explicit FakePort(bs::Trainer& t) : q_(t.queue()) {
    t.attach(*this);
    neo(db);
  }

  TrnTable db{};
  uint16_t base = 0x10;              // first handle of the layout
  uint16_t mtu = 107;
  bool ackWrites = true;
  bool answerReads = true;
  bool trainerOn = true;             // off: every connect ends with CONNECT_FAIL
  bool silentConnect = false;        // CONNECTED lost
  bool deliverTerminate = true;
  bool appAlive = false;
  bool addrOk = true;
  bool handleReused = false;         // the trainer's handle now belongs to another peer
  uint16_t orphanConn = 0;           // a stale attempt's link to the trainer
  std::vector<int> writeRc;          // consumed front first; empty = 0
  std::vector<int> connectRc;
  int notifyRc = 0;
  bool stored = false;
  TrnTable store{};
  uint8_t ownAddr[6] = {};
  uint8_t lastType = 0xFF;
  int connects = 0, terminates = 0, localValues = 0, orphanKills = 0, appKills = 0;
  std::vector<Write> writes;
  std::vector<Sent> sent;
  std::vector<uint16_t> kicked;

  uint16_t gen() const { return gen_; }
  uint16_t valHandle(uint8_t i) const { return layout().val[i]; }
  uint16_t cccdHandle(uint8_t i) const { return layout().cccd[i]; }

  void push(TrnEv kind, int16_t status = 0, uint16_t h1 = 0, uint16_t h2 = 0, uint8_t idx = 0,
            const uint8_t* data = nullptr, uint8_t len = 0, uint16_t gen = 0xFFFF) {
    TrnEvent ev{};
    ev.kind = kind;
    ev.gen = gen == 0xFFFF ? gen_ : gen;
    ev.status = status;
    ev.h1 = h1;
    ev.h2 = h2;
    ev.idx = idx;
    ev.len = len;
    if (data != nullptr) memcpy(ev.data, data, len);
    assert(q_.push(ev));
  }
  void pushUuid(TrnEv kind, const TrnUuid& u, uint16_t h1, uint16_t h2 = 0, uint8_t idx = 0) {
    TrnEvent ev{};
    ev.kind = kind;
    ev.gen = gen_;
    ev.h1 = h1;
    ev.h2 = h2;
    ev.idx = idx;
    ev.uuid = u;
    assert(q_.push(ev));
  }
  // Fills the queue with ignored events; the next push is lost.
  void flood() {
    TrnEvent ev{};
    ev.kind = TrnEv::APP_MTU;
    ev.h1 = 99;
    while (q_.push(ev)) {
    }
  }
  // Notifications of characteristic i until one is refused; returns how many fit.
  int floodNotify(uint8_t i) {
    TrnEvent ev{};
    ev.kind = TrnEv::NOTIFY;
    ev.gen = gen_;
    ev.h1 = valHandle(i);
    ev.status = ev.len = 4;
    int n = 0;
    while (q_.push(ev)) n++;
    return n;
  }
  // Trainer → relay notification or indication of characteristic i.
  void notify(uint8_t i, uint8_t len, int16_t origLen = -1, bool indication = false,
              const uint8_t* data = nullptr) {
    static const uint8_t kZeros[bs::kTrnValueMax] = {};
    push(TrnEv::NOTIFY, origLen < 0 ? len : origLen, valHandle(i), 0, indication,
         data != nullptr ? data : kZeros, len);
  }
  void drop(int16_t reason = 0x08) {
    linked_ = false;
    push(TrnEv::DISCONNECTED, reason);
  }
  void appConnect() {
    appAlive = true;
    push(TrnEv::APP_CONNECT, 0, kAppConn, 247);
  }
  void appDisconnect() {
    appAlive = false;
    push(TrnEv::APP_DISCONNECT, 0, kAppConn);
  }

  // The port terminates an orphaned link and returns BLE_HS_EDONE.
  int connect(const uint8_t mac[6], uint8_t addrType, uint16_t gen) override {
    assert(memcmp(mac, db.mac, 6) == 0);
    connects++;
    lastType = addrType;
    if (linked_) {
      linked_ = false;
      orphanKills++;
      return bs::kHsEDone;
    }
    if (!connectRc.empty()) {
      const int rc = connectRc.front();
      connectRc.erase(connectRc.begin());
      if (rc != 0) return rc;
    }
    gen_ = gen;
    if (!trainerOn) {
      push(TrnEv::CONNECT_FAIL, bs::kHsETimeout);
      return 0;
    }
    linked_ = true;
    if (!silentConnect) push(TrnEv::CONNECTED, 0, kTrainerConn);
    return 0;
  }
  void cancelConnect() override {}
  void terminate(uint16_t conn) override {
    terminates++;
    if (conn == kTrainerConn && linked_ && deliverTerminate) drop(0x16);
  }
  // Trainer links are checked with the trainer's address, apps with nullptr.
  bool connAlive(uint16_t conn, const uint8_t* peer) override {
    if (peer == nullptr) {
      assert(conn != kTrainerConn);
      return appAlive;
    }
    assert(memcmp(peer, db.mac, 6) == 0);
    return conn == kTrainerConn ? linked_ && !handleReused : conn == orphanConn;
  }
  int exchangeMtu(uint16_t, uint16_t) override {
    push(TrnEv::MTU, 0, mtu);
    return 0;
  }
  int discServices(uint16_t, uint16_t) override {
    pushUuid(TrnEv::SVC, u16(0x1800), 1, 3);   // GAP and GATT first, as on the NEO
    pushUuid(TrnEv::SVC, u16(0x1801), 4, 4);
    const Layout l = layout();
    for (uint8_t s = 0; s < db.svcCount; ++s) pushUuid(TrnEv::SVC, db.svc[s], l.svcStart[s], l.svcEnd[s]);
    pushUuid(TrnEv::SVC, u16(0xFE59), 0xFF00, 0xFF10);
    push(TrnEv::PROC_DONE);
    return 0;
  }
  int discChrs(uint16_t, uint16_t start, uint16_t end, uint16_t) override {
    const Layout l = layout();
    for (uint8_t i = 0; i < db.chrCount; ++i) {
      if (l.def[i] >= start && l.def[i] <= end) {
        pushUuid(TrnEv::CHR, db.chr[i].uuid, l.def[i], l.val[i], db.chr[i].props);
      }
    }
    push(TrnEv::PROC_DONE);
    return 0;
  }
  int discDscs(uint16_t, uint16_t val, uint16_t end, uint16_t) override {
    const Layout l = layout();
    for (uint8_t i = 0; i < db.chrCount; ++i) {
      if (l.val[i] == val && l.cccd[i] != 0 && l.cccd[i] <= end) {
        pushUuid(TrnEv::DSC, u16(0x2902), l.cccd[i]);
      }
    }
    push(TrnEv::PROC_DONE);
    return 0;
  }
  int read(uint16_t, uint16_t handle, uint16_t) override {
    const Layout l = layout();
    for (uint8_t i = 0; i < db.chrCount && answerReads; ++i) {
      if (l.val[i] == handle) push(TrnEv::READ, 0, handle, 0, 0, db.chr[i].value, db.chr[i].len);
    }
    return 0;
  }
  int write(uint16_t, uint16_t handle, const uint8_t* data, uint8_t len, bool response,
            uint16_t) override {
    int rc = 0;
    if (!writeRc.empty()) {
      rc = writeRc.front();
      writeRc.erase(writeRc.begin());
    }
    if (rc != 0) return rc;
    writes.push_back({handle, std::vector<uint8_t>(data, data + len), response});
    if (response && ackWrites) push(TrnEv::WRITE_DONE);
    return 0;
  }
  int notifyApp(uint16_t conn, uint8_t idx, const uint8_t* data, uint8_t len, bool indicate) override {
    if (notifyRc == 0) sent.push_back({conn, idx, len, indicate, std::vector<uint8_t>(data, data + len)});
    return notifyRc;
  }
  void disconnectApp(uint16_t conn) override { kicked.push_back(conn); }
  void disconnectApps() override { appKills++; }
  void setLocalValue(uint8_t, const uint8_t*, uint8_t) override { localValues++; }
  size_t loadTable(bs::TrnBlob& b) override {
    if (!stored) return 0;
    b.table = store;
    bs::sealTable(b);
    return sizeof(b);
  }
  bool storeTable(const bs::TrnBlob& b) override {
    assert(bs::openTable(b, sizeof(b)));
    store = b.table;
    stored = true;
    return true;
  }
  bool eraseTable() override {
    stored = false;
    return true;
  }
  bool setOwnAddress(const uint8_t addr[6]) override {
    if (!addrOk) return false;
    if (addr == nullptr) {
      memset(ownAddr, 0, 6);
    } else {
      memcpy(ownAddr, addr, 6);
    }
    return true;
  }
  uint64_t chipMac() override { return kChip; }

 private:
  struct Layout {
    uint16_t svcStart[bs::kTrnSvcMax], svcEnd[bs::kTrnSvcMax];
    uint16_t def[bs::kTrnChrMax], val[bs::kTrnChrMax], cccd[bs::kTrnChrMax];
  };
  Layout layout() const {
    Layout l{};
    uint16_t h = base;
    for (uint8_t s = 0; s < db.svcCount; ++s) {
      l.svcStart[s] = h++;
      for (uint8_t i = 0; i < db.chrCount; ++i) {
        if (db.chr[i].svc != s) continue;
        l.def[i] = h++;
        l.val[i] = h++;
        if (db.chr[i].cccd) l.cccd[i] = h++;
      }
      l.svcEnd[s] = static_cast<uint16_t>(h - 1);
    }
    return l;
  }

  bs::TrnQueue& q_;
  uint16_t gen_ = 0;
  bool linked_ = false;
};

static std::vector<EventInfo> gEvents;
static void onEvent(const EventInfo& e) { gEvents.push_back(e); }

static int countEvents(EventType type, int reason = -1) {
  int n = 0;
  for (const EventInfo& e : gEvents) n += e.type == type && (reason < 0 || e.reason == reason);
  return n;
}

static void run(bs::Trainer& t, uint32_t& now, int loops = 200) {
  for (int k = 0; k < loops; ++k) {
    t.update(now, true, onEvent);
    now += 10;
  }
}

static TrainerInfo info(bs::Trainer& t) {
  TrainerInfo i{};
  t.info(i);
  return i;
}

// A READY mirror of the NEO with the app connected (MTU 247).
static void ready(bs::Trainer& t, FakePort& p, uint32_t& now, bool app = true) {
  gEvents.clear();
  neo(p.store);
  p.stored = true;
  assert(t.begin(true, now));
  run(t, now);
  assert(info(t).state == TrainerState::READY);
  if (app) {
    p.appConnect();
    run(t, now, 1);
    assert(info(t).appConnected && !t.advertise());
  }
}

// ---- Table

static void testUuid() {
  TrnUuid u;
  const uint8_t base2a63[16] = {0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
                                0x00, 0x10, 0x00, 0x00, 0x63, 0x2A, 0x00, 0x00};
  assert(bs::makeUuid(base2a63, 16, u) && u.len == 2 && bs::uuid16(u) == 0x2A63);
  const uint8_t s32[4] = {0x16, 0x18, 0x00, 0x00};
  assert(bs::makeUuid(s32, 4, u) && u.len == 2 && bs::uuid16(u) == 0x1816);
  const uint8_t l32[4] = {0x78, 0x56, 0x34, 0x12};
  assert(bs::makeUuid(l32, 4, u) && u.len == 16 && u.b[12] == 0x78 && u.b[15] == 0x12);
  const TrnUuid fec1 = u128("6e40fec1-b5a3-f393-e0a9-e50e24dcca9e");
  assert(bs::makeUuid(fec1.b, 16, u) && bs::sameUuid(u, fec1) && bs::uuid16(u) == 0);
  assert(u.b[0] == 0x9E && u.b[15] == 0x6E);
  assert(!bs::makeUuid(s32, 3, u));
  assert(bs::skippedService(u16(0x1800)) && bs::skippedService(u16(0x1801)));
  assert(bs::skippedService(u16(0xFE59)) && bs::skippedService(u16(0x180D)));
  assert(bs::skippedService(u128("8ec90001-f315-4f60-9fb8-838830daea50")));
  assert(bs::skippedService(u128("00001530-1212-efde-1523-785feabcd123")));
  assert(!bs::skippedService(u16(0x1816)) && !bs::skippedService(fec1));
}

static void testCrc() {
  const char* s = "123456789";
  const uint8_t* p = reinterpret_cast<const uint8_t*>(s);
  assert(bs::crc32(p, 9) == 0xCBF43926u);
  assert(bs::crc32(p + 4, 5, bs::crc32(p, 4)) == 0xCBF43926u);
  assert(bs::crc32(p, 0) == 0);
}

// Raw blob: length, version, CRC; then a valid CRC over bad content.
static void testCodec() {
  static bs::TrnBlob b, x;
  neo(b.table);
  b.table.adLen = 5;
  memcpy(b.table.ad, kNeoAdv + 3, 5);
  bs::sealTable(b);
  assert(bs::openTable(b, sizeof(b)));
  assert(!bs::openTable(b, sizeof(b) - 1) && !bs::openTable(b, sizeof(b) + 1));
  x = b;
  x.table.chr[15].value[0] ^= 1;
  assert(!bs::openTable(x, sizeof(x)));
  x = b;
  x.version = 1;
  assert(!bs::openTable(x, sizeof(x)));
  auto resealed = [](void (*bad)(TrnTable&)) {
    static bs::TrnBlob y;
    neo(y.table);
    bad(y.table);
    bs::sealTable(y);
    return bs::openTable(y, sizeof(y));
  };
  assert(resealed([](TrnTable&) {}));
  assert(!resealed([](TrnTable& t) { t.svc[2].len = 3; }));
  assert(!resealed([](TrnTable& t) { t.chr[4].uuid.len = 0; }));
  assert(!resealed([](TrnTable& t) { t.chr[24].svc = 8; }));          // index
  assert(!resealed([](TrnTable& t) { t.chr[5].svc = 0; }));           // ungrouped
  assert(!resealed([](TrnTable& t) { t.chr[0].len = bs::kTrnValueMax + 1; }));
  assert(!resealed([](TrnTable& t) { t.chrCount = bs::kTrnChrMax + 1; }));
  assert(!resealed([](TrnTable& t) { t.svcCount = bs::kTrnSvcMax + 1; }));
  assert(!resealed([](TrnTable& t) { t.adLen = bs::kTrnAdCopyMax + 1; }));
}

static void testHash() {
  static TrnTable a, b;
  neo(a);
  const uint32_t h = bs::structureHash(a);
  b = a;
  b.chr[1].value[0] = 0x55;
  b.chr[0].cccd = false;
  memset(b.mac, 0, 6);
  assert(bs::structureHash(b) == h);   // values, CCCD flags, address: not structure
  b = a;
  b.chr[3].props = W;
  assert(bs::structureHash(b) != h);
  b = a;
  b.chr[4].uuid = u16(0x2A64);
  assert(bs::structureHash(b) != h);
  b = a;
  std::swap(b.svc[0], b.svc[1]);
  assert(bs::structureHash(b) != h);
  b = a;
  b.chrCount--;
  assert(bs::structureHash(b) != h);
}

static void testAddress() {
  uint8_t a[6], b[6], c[6];
  bs::mirrorAddress(0x11223344, 0xABCD, a);
  bs::mirrorAddress(0x11223344, 0xABCD, b);
  assert(memcmp(a, b, 6) == 0 && (a[0] & 0xC0) == 0xC0);
  bs::mirrorAddress(0x11223345, 0xABCD, c);
  assert(memcmp(a, c, 6) != 0 && (c[0] & 0xC0) == 0xC0);
  bs::mirrorAddress(0x11223344, 0xABCE, c);
  assert(memcmp(a, c, 6) != 0);
  // ESP32 eFuse MAC: OUI in the low bytes; two chips may differ only above bit 31.
  bs::mirrorAddress(0x11223344, 0x00000000ABCDull | (1ull << 40), c);
  assert(memcmp(a, c, 6) != 0);
  bs::mirrorAddress(0x11223344, 0x00000000ABCDull | (1ull << 32), c);
  assert(memcmp(a, c, 6) != 0);
}

static void testCheck() {
  static TrnTable t, x;
  const bs::TrnAdv ad = neoAd();
  neo(t);
  assert(bs::checkTable(t, false, &ad) == TrainerCheck::OK);
  assert(bs::checkTable(t, false, nullptr) == TrainerCheck::OK);
  assert(bs::checkTable(t, true, &ad) == TrainerCheck::TOO_LARGE);
  x = t;
  x.svcCount = x.chrCount = 0;
  assert(bs::checkTable(x, false, nullptr) == TrainerCheck::EMPTY);
  x = t;
  emptyFc82(x);
  assert(bs::checkTable(x, false, &ad) == TrainerCheck::EMPTY_SERVICE);
  x = t;
  x.chr[0].cccd = false;
  assert(bs::checkTable(x, false, &ad) == TrainerCheck::NO_CCCD);

  // Advertised 0x1818 / FE-C 128-bit missing; FE59 (DFU) advertised but skipped.
  x = t;
  x.svc[1] = u16(0x1826);
  assert(bs::checkTable(x, false, &ad) == TrainerCheck::MISSING_ADVERTISED);
  x = t;
  x.svc[2] = u16(0x1826);
  assert(bs::checkTable(x, false, &ad) == TrainerCheck::MISSING_ADVERTISED);
  bs::TrnAdv dfu{};
  const uint8_t dfuList[] = {0x03, 0x03, 0x59, 0xFE};
  memcpy(dfu.adv, dfuList, sizeof(dfuList));
  dfu.advLen = sizeof(dfuList);
  assert(bs::checkTable(t, false, &dfu) == TrainerCheck::OK);
  // A zero-padded ADV does not hide the SCAN_RSP (0x1818 listed there).
  const bs::TrnAdv padded = neoAd(true, true);
  x = t;
  x.svc[1] = u16(0x1826);
  assert(bs::checkTable(x, false, &padded) == TrainerCheck::MISSING_ADVERTISED);

  // Empty services: dropped unless advertised.
  x = t;
  emptyFc82(x);
  bs::dropEmptyServices(x, nullptr);
  assert(x.svcCount == 7 && bs::findService(x, u16(0xFC82)) < 0);
  assert(x.chr[24].svc == 6 && bs::checkTable(x, false, nullptr) == TrainerCheck::OK);
  x = t;
  emptyFc82(x);
  bs::dropEmptyServices(x, &ad);
  assert(x.svcCount == 8 && bs::checkTable(x, false, &ad) == TrainerCheck::EMPTY_SERVICE);
  // Service data (16 82 FC in the ADV) names FC82 although the SCAN_RSP does not.
  bs::TrnAdv named{};
  const uint8_t name[] = {0x03, 0x09, 'N', 'E'};
  memcpy(named.rsp, name, sizeof(name));
  named.rspLen = sizeof(name);
  x = t;
  emptyFc82(x);
  bs::dropEmptyServices(x, &named);
  assert(x.svcCount == 7);
  memcpy(named.adv, kNeoAdv, sizeof(kNeoAdv));
  named.advLen = sizeof(kNeoAdv);
  x = t;
  emptyFc82(x);
  bs::dropEmptyServices(x, &named);
  assert(x.svcCount == 8);
  // No SCAN_RSP caught: the list may be incomplete, nothing is dropped.
  const bs::TrnAdv none{};
  x = t;
  emptyFc82(x);
  bs::dropEmptyServices(x, &none);
  assert(x.svcCount == 8 && bs::checkTable(x, false, &none) == TrainerCheck::EMPTY_SERVICE);
  // Service data of a UUID without a service is no missing advertised service.
  bs::TrnAdv data{};
  const uint8_t vendor[] = {0x05, 0x16, 0x34, 0x12, 0x01, 0x02};
  memcpy(data.adv, vendor, sizeof(vendor));
  data.advLen = sizeof(vendor);
  assert(bs::checkTable(t, false, &data) == TrainerCheck::OK);
}

static void testAdv() {
  static TrnTable t;
  neo(t);
  bs::copyAdFields(neoAd(true, true), t);   // service data (ADV), appearance (SCAN_RSP)
  assert(t.adLen == 9 && t.ad[1] == 0x16 && t.ad[2] == 0x82 && t.ad[3] == 0xFC && t.ad[6] == 0x19);
  bs::TrnAdv a;
  assert(bs::buildMirrorAdv(t, "Tacx Neo 2T 14189", true, a));
  assert(a.advLen == 3 + 9 + 18 && a.adv[12] == 17 && a.adv[13] == 0x07 && a.adv[14] == 0x9E);
  // 1816, 1818, FC82 (no DIS), HR, then the name.
  const uint8_t rsp[] = {0x09, 0x03, 0x16, 0x18, 0x18, 0x18, 0x82, 0xFC, 0x0D, 0x18, 0x12, 0x09};
  assert(a.rspLen == sizeof(rsp) + 17 && memcmp(a.rsp, rsp, sizeof(rsp)) == 0);
  assert(memcmp(a.rsp + sizeof(rsp), "Tacx Neo 2T 14189", 17) == 0);
  assert(bs::buildMirrorAdv(t, "Homewind Relay Train1", false, a) && a.rspLen == 31);
  assert(!bs::buildMirrorAdv(t, "Homewind Relay Train1", true, a));
}

// ---- Runtime

static void testSetup() {
  static bs::Trainer t;
  static FakePort p(t);
  uint32_t now = 1000;
  gEvents.clear();
  assert(!t.begin(false, now) && !t.mirror());
  run(t, now, 1);
  t.setup(kNeoMac);
  assert(info(t).state == TrainerState::SETUP && !t.quietAdv());
  const uint8_t other[6] = {1, 2, 3, 4, 5, 6};
  t.capture(other, 0, kNeoAdv, sizeof(kNeoAdv), false);
  t.discoveryDone();
  assert(countEvents(EventType::TRAINER_SETUP_DONE, int(TrainerCheck::NOT_FOUND)) == 1);
  assert(info(t).state == TrainerState::OFF);

  // The address type comes from the scan report; advertising off while connecting.
  t.setup(kNeoMac);
  t.capture(kNeoMac, 1, kNeoAdv, sizeof(kNeoAdv), false);
  t.capture(kNeoMac, 1, kNeoRsp, sizeof(kNeoRsp), true);
  run(t, now, 1);   // the loop runs between the report and DISCOVERY_DONE
  t.discoveryDone();
  assert(t.blocksScan() && t.quietAdv());
  run(t, now);
  assert(countEvents(EventType::TRAINER_SETUP_DONE, int(TrainerCheck::OK)) == 1);
  assert(p.stored && p.terminates == 1 && t.idle() && !t.quietAdv());
  assert(p.lastType == 1 && p.store.addrType == 1);
  assert(p.store.svcCount == 8 && p.store.chrCount == 25);   // GAP, GATT, FE59 skipped
  assert(bs::structureHash(p.store) == bs::structureHash(p.db));
  assert(memcmp(p.store.mac, kNeoMac, 6) == 0);
  for (uint8_t i = 0; i < 25; ++i) {
    assert(p.store.chr[i].len == p.db.chr[i].len && p.store.chr[i].cccd == p.db.chr[i].cccd);
  }
  // Service data FC82 and appearance copied for the mirror's advertising.
  assert(p.store.adLen == 9 && p.store.ad[1] == 0x16 && p.store.ad[6] == 0x19);
  assert(info(t).state == TrainerState::OFF && !t.blocksScan());

  // FC82 without characteristics, listed in the SCAN_RSP: rejected, nothing stored.
  p.stored = false;
  emptyFc82(p.db);
  t.setup(kNeoMac);
  t.capture(kNeoMac, 0, kNeoAdv, sizeof(kNeoAdv), false);
  t.capture(kNeoMac, 0, kNeoRsp, sizeof(kNeoRsp), true);
  t.discoveryDone();
  run(t, now);
  assert(countEvents(EventType::TRAINER_SETUP_DONE, int(TrainerCheck::EMPTY_SERVICE)) == 1);
  assert(!p.stored);
  // No SCAN_RSP caught (FC82 named only in the ADV's service data): still rejected (G5).
  t.setup(kNeoMac);
  t.capture(kNeoMac, 0, kNeoAdv, sizeof(kNeoAdv), false);
  t.discoveryDone();
  run(t, now);
  assert(countEvents(EventType::TRAINER_SETUP_DONE, int(TrainerCheck::EMPTY_SERVICE)) == 2);
  assert(!p.stored);
  // An empty service nothing names, with a SCAN_RSP: dropped, stored.
  t.setup(kNeoMac);
  const uint8_t name[] = {0x03, 0x09, 'N', 'E'};
  t.capture(kNeoMac, 0, name, sizeof(name), false);
  t.capture(kNeoMac, 0, name, sizeof(name), true);
  t.discoveryDone();
  run(t, now);
  assert(countEvents(EventType::TRAINER_SETUP_DONE, int(TrainerCheck::OK)) == 2);
  assert(p.stored && p.store.svcCount == 7 && p.store.chrCount == 25);

  // Capacity refusal: one retry, then LINK_FAILED.
  neo(p.db);
  p.connectRc = {bs::kHciMemCapacity};
  t.setup(kNeoMac);
  t.capture(kNeoMac, 0, kNeoAdv, sizeof(kNeoAdv), false);
  t.discoveryDone();
  run(t, now, 1);
  assert(p.connectRc.empty() && t.quietAdv() && t.idle());
  run(t, now, 300);
  assert(countEvents(EventType::TRAINER_SETUP_DONE, int(TrainerCheck::OK)) == 3);
  p.connectRc = {bs::kHsENoMem, bs::kHciMemCapacity};
  t.setup(kNeoMac);
  t.capture(kNeoMac, 0, kNeoAdv, sizeof(kNeoAdv), false);
  t.discoveryDone();
  run(t, now, 300);
  assert(countEvents(EventType::TRAINER_SETUP_DONE, int(TrainerCheck::LINK_FAILED)) == 1);
}

static void testMirrorReady() {
  static bs::Trainer t;
  static FakePort p(t);
  uint32_t now = 1000;
  ready(t, p, now, false);
  uint8_t addr[6];
  bs::mirrorAddress(bs::structureHash(p.store), kChip, addr);
  const TrainerInfo i = info(t);
  assert(memcmp(p.ownAddr, addr, 6) == 0 && memcmp(i.address, addr, 6) == 0);
  assert(i.mirror && i.mtu == 107 && i.services == 8 && i.characteristics == 25);
  assert(countEvents(EventType::TRAINER_CONNECTED, 0) == 1 && t.advertise());
  assert(p.localValues == 10 && p.connects == 1 && p.writes.empty());

  // begin() drains the queue and resets the counters.
  p.notify(0, 3);
  run(t, now, 1);
  assert(info(t).up == 1);
  p.notify(0, 3);
  assert(t.begin(true, now) && info(t).up == 0 && t.idle());
  run(t, now, 1);
  assert(info(t).up == 0);
}

static void testFifoOrder() {
  static bs::Trainer t;
  static FakePort p(t);
  uint32_t now = 1000;
  ready(t, p, now);
  p.ackWrites = false;
  const uint8_t cp = static_cast<uint8_t>(idxOf(p.db, 0x2A66));
  const uint8_t cmd[3] = {0x00, 0x11, 0x22};
  p.push(TrnEv::APP_SUB, 0, kAppConn, 2, cp);
  p.push(TrnEv::APP_WRITE, 0, kAppConn, 0, cp, cmd, 1);
  p.push(TrnEv::APP_WRITE, 0, kAppConn, 0, cp, cmd, 3);
  run(t, now, 3);
  // CCCD first; the control point write waits for its response.
  assert(p.writes.size() == 1 && p.writes[0].handle == p.cccdHandle(cp) && p.writes[0].response);
  assert(p.writes[0].data == std::vector<uint8_t>({0x02, 0x00}));
  p.push(TrnEv::WRITE_DONE);
  run(t, now, 1);
  assert(p.writes.size() == 2 && p.writes[1].handle == p.valHandle(cp) && p.writes[1].response);
  assert(p.writes[1].data.size() == 1);
  p.push(TrnEv::WRITE_DONE);
  run(t, now, 1);
  assert(p.writes.size() == 3 && p.writes[2].data.size() == 3);
  p.push(TrnEv::WRITE_DONE, 3);   // ATT error: counted, the FIFO goes on
  run(t, now, 1);
  assert(info(t).writeErrors == 1 && info(t).down == 2);

  // Write type from the trainer's properties: FC82 0003 (index 21) is
  // write-without-response only → commands, no wait.
  assert(p.db.chr[21].props == WNR);
  p.push(TrnEv::APP_WRITE, 0, kAppConn, 0, 21, cmd, 2);
  p.push(TrnEv::APP_WRITE, 0, kAppConn, 0, 21, cmd, 2);
  run(t, now, 1);
  assert(p.writes.size() == 5 && !p.writes[3].response && !p.writes[4].response);
}

static void testIndicationPacing() {
  static bs::Trainer t;
  static FakePort p(t);
  uint32_t now = 1000;
  ready(t, p, now);
  const uint8_t cp = static_cast<uint8_t>(idxOf(p.db, 0x2A66));
  const uint8_t bike = static_cast<uint8_t>(idxOf(p.db, 0x2A63));
  p.push(TrnEv::APP_SUB, 0, kAppConn, 2, cp);
  p.push(TrnEv::APP_SUB, 0, kAppConn, 1, bike);
  run(t, now, 2);
  for (int k = 0; k < 3; ++k) p.notify(cp, 3);
  p.notify(bike, 8);
  p.notify(bike, 8);
  run(t, now, 2);
  // Notifications at once, one indication outstanding.
  assert(p.sent.size() == 3 && p.sent[0].indicate == false && p.sent[2].indicate);
  p.push(TrnEv::APP_IND_DONE, bs::kHsEDone, kAppConn);
  run(t, now, 1);
  assert(p.sent.size() == 4 && p.sent[3].indicate && p.sent[3].idx == cp);
  p.push(TrnEv::APP_IND_DONE, bs::kHsEDone, kAppConn);
  run(t, now, 1);
  assert(p.sent.size() == 5);
  p.push(TrnEv::APP_IND_DONE, bs::kHsETimeout, kAppConn);
  run(t, now, 1);
  assert(p.sent.size() == 5 && p.kicked.empty() && info(t).dropped == 1);

  // Queued indications are not sent after the app unsubscribed.
  p.notify(cp, 3);
  p.notify(cp, 3);
  run(t, now, 1);
  assert(p.sent.size() == 6);
  p.push(TrnEv::APP_SUB, 0, kAppConn, 0, cp);
  p.push(TrnEv::APP_IND_DONE, bs::kHsEDone, kAppConn);
  run(t, now, 1);
  assert(p.sent.size() == 6 && info(t).dropped == 1);
  p.push(TrnEv::APP_SUB, 0, kAppConn, 2, cp);
  run(t, now, 1);

  // Depth 8 queued while one is outstanding; the ninth drops the app.
  p.notify(cp, 3);
  run(t, now, 1);
  for (int k = 0; k < 8; ++k) p.notify(cp, 3);
  run(t, now, 1);
  assert(p.sent.size() == 7 && p.kicked.empty());
  p.notify(cp, 3);
  run(t, now, 1);
  assert(p.kicked.size() == 1 && p.kicked[0] == kAppConn && info(t).dropped == 2);
}

static void testLengthCap() {
  static bs::Trainer t;
  static FakePort p(t);
  p.mtu = 23;
  uint32_t now = 1000;
  ready(t, p, now);
  const uint8_t bike = static_cast<uint8_t>(idxOf(p.db, 0x2A63));
  p.push(TrnEv::APP_SUB, 0, kAppConn, 1, bike);
  p.push(TrnEv::APP_MTU, 0, kAppConn, 23);
  run(t, now, 1);
  p.notify(bike, 20);
  p.notify(bike, 21);                       // > app MTU − 3
  p.notify(bike, bs::kTrnValueMax, 200);    // truncated by the cap (and > MTU − 3): once
  run(t, now, 1);
  assert(info(t).oversize == 2 && info(t).up == 3 && p.sent.size() == 3);
  // > trainer MTU − 3: no long write.
  const uint8_t big[bs::kTrnValueMax] = {};
  const size_t before = p.writes.size();
  p.push(TrnEv::APP_WRITE, 0, kAppConn, 0, static_cast<uint8_t>(idxOf(p.db, 0x2A66)), big, 21);
  p.push(TrnEv::APP_WRITE, 0, kAppConn, 0, static_cast<uint8_t>(idxOf(p.db, 0x2A66)), big, 20);
  run(t, now, 1);
  assert(info(t).writeErrors == 1 && p.writes.size() == before + 1 && p.writes.back().data.size() == 20);
}

static void testRefcount() {
  static bs::Trainer t;
  static FakePort p(t);
  uint32_t now = 1000;
  ready(t, p, now);
  const uint8_t bike = static_cast<uint8_t>(idxOf(p.db, 0x2A63));
  const uint16_t cccd = p.cccdHandle(bike);
  auto last = [&]() { return p.writes.back().data[0]; };
  t.readAlong(0x2A63, true);
  run(t, now, 1);
  assert(p.writes.size() == 1 && p.writes[0].handle == cccd && last() == 1);
  p.push(TrnEv::APP_SUB, 0, kAppConn, 1, bike);
  p.push(TrnEv::APP_SUB, 0, kAppConn, 0, bike);
  run(t, now, 1);
  assert(p.writes.size() == 1);             // internal holder keeps it on
  t.readAlong(0x2A63, false);
  run(t, now, 1);
  assert(p.writes.size() == 2 && last() == 0);
  p.push(TrnEv::APP_SUB, 0, kAppConn, 1, bike);
  run(t, now, 1);
  assert(p.writes.size() == 3 && last() == 1);
  t.readAlong(0x2A63, true);
  t.readAlong(0x2A63, false);
  run(t, now, 1);
  assert(p.writes.size() == 3);             // app holder keeps it on
  // Indicate-only characteristic: read-along subscribes with indications.
  t.readAlong(0x2A66, true);
  run(t, now, 1);
  assert(p.writes.size() == 4 && last() == 2);
  // App disconnect releases its holders only.
  p.appDisconnect();
  run(t, now, 1);
  assert(p.writes.size() == 5 && p.writes.back().handle == cccd && last() == 0);
  assert(t.advertise());

  // Read-along data reaches the callback without an app.
  static int calls;
  calls = 0;
  t.setDataCallback([](uint16_t uuid, const uint8_t*, uint8_t len) {
    calls += uuid == 0x2A66 && len == 3;
  });
  p.notify(static_cast<uint8_t>(idxOf(p.db, 0x2A66)), 3);
  run(t, now, 1);
  assert(calls == 1);
}

// A failed CCCD write (host error, ATT error) is written again.
static void testCccdRetry() {
  static bs::Trainer t;
  static FakePort p(t);
  uint32_t now = 1000;
  ready(t, p, now, false);
  const uint16_t cccd = p.cccdHandle(static_cast<uint8_t>(idxOf(p.db, 0x2A63)));
  p.writeRc = {7};
  t.readAlong(0x2A63, true);
  run(t, now, 1);
  assert(p.writes.empty() && info(t).writeErrors == 1);
  run(t, now, 210);
  assert(p.writes.size() == 1 && p.writes[0].handle == cccd && p.writes[0].data[0] == 1);
  p.ackWrites = false;
  t.readAlong(0x2A63, false);
  run(t, now, 1);
  p.push(TrnEv::WRITE_DONE, 3);
  run(t, now, 1);
  assert(p.writes.size() == 2 && info(t).writeErrors == 2);
  p.ackWrites = true;
  run(t, now, 210);
  assert(p.writes.size() == 3 && p.writes[2].data[0] == 0);
  run(t, now, 210);
  assert(p.writes.size() == 3);
}

static void testRetry() {
  static bs::Trainer t;
  static FakePort p(t);
  uint32_t now = 1000;
  ready(t, p, now);
  const uint8_t cp = static_cast<uint8_t>(idxOf(p.db, 0x2A66));
  const uint8_t cmd[1] = {0x05};
  p.writeRc = {bs::kHsENoMem, bs::kHsEBusy, bs::kHsENoMem};
  p.push(TrnEv::APP_WRITE, 0, kAppConn, 0, cp, cmd, 1);
  run(t, now, 5);
  assert(p.writes.size() == 1 && info(t).writeErrors == 0);   // retried, not dropped
  p.writeRc.assign(1000, bs::kHsENoMem);
  p.push(TrnEv::APP_WRITE, 0, kAppConn, 0, cp, cmd, 1);
  p.push(TrnEv::APP_WRITE, 0, kAppConn, 0, cp, cmd, 1);
  run(t, now, 100);
  assert(p.writes.size() == 1 && info(t).writeErrors == 0);
  run(t, now, 320);                         // > kTrnRetryMs per entry
  assert(info(t).writeErrors == 2);
  p.writeRc.clear();
  p.writeRc.push_back(7);                   // any other error: counted at once
  p.push(TrnEv::APP_WRITE, 0, kAppConn, 0, cp, cmd, 1);
  run(t, now, 1);
  assert(info(t).writeErrors == 3);
}

static void testLinkCoupling() {
  static bs::Trainer t;
  static FakePort p(t);
  uint32_t now = 1000;
  ready(t, p, now);
  p.push(TrnEv::APP_CONNECT, 0, kAppConn + 1, 247);   // second app
  run(t, now, 1);
  assert(p.kicked.size() == 1 && p.kicked[0] == kAppConn + 1);
  p.drop();
  run(t, now, 1);
  assert(p.kicked.size() == 2 && p.kicked[1] == kAppConn);
  assert(countEvents(EventType::TRAINER_DISCONNECTED, 8) == 1 && !t.advertise());
  p.appDisconnect();
  run(t, now);
  assert(info(t).state == TrainerState::READY && p.connects == 2 && t.advertise());

  // App connected while the trainer is away: dropped.
  p.drop();
  run(t, now, 1);
  p.appConnect();
  run(t, now, 1);
  assert(p.kicked.size() == 3 && p.kicked[2] == kAppConn);
  p.appDisconnect();
  run(t, now);
  assert(info(t).state == TrainerState::READY && t.advertise());

  // An app that stays after the kick is kicked again; gone without an event: released.
  p.appConnect();
  run(t, now, 1);
  p.drop();
  run(t, now, 1);
  assert(p.kicked.size() == 4);
  run(t, now, 710);
  assert(p.kicked.size() == 5 && info(t).appConnected);
  p.appAlive = false;
  run(t, now, 710);
  assert(p.kicked.size() == 5 && !info(t).appConnected && t.advertise());

  // Stale CONNECTED of a cancelled attempt is terminated, unless its handle
  // already belongs to another link.
  const int terminates = p.terminates;
  p.orphanConn = 9;
  p.push(TrnEv::CONNECTED, 0, 9, 0, 0, nullptr, 0, 0);
  run(t, now, 1);
  assert(p.terminates == terminates + 1);
  p.push(TrnEv::CONNECTED, 0, 10, 0, 0, nullptr, 0, 0);
  run(t, now, 1);
  assert(p.terminates == terminates + 1);

  // Host reset: link lost, own address set again (retried until it works);
  // no mirror advertising before.
  memset(p.ownAddr, 0, 6);
  p.addrOk = false;
  t.hostReset(now);
  run(t, now);
  assert(info(t).state == TrainerState::READY && !t.advertise() && p.ownAddr[0] == 0);
  p.addrOk = true;
  run(t, now, 1);
  assert(t.advertise() && info(t).address[0] == p.ownAddr[0] && p.ownAddr[0] != 0);

  // quiesce
  t.stop();
  run(t, now);
  assert(t.idle() && info(t).state == TrainerState::OFF && !t.blocksScan());
}

// Lost events (queue overflow) and stale ones of an earlier link.
static void testLostEvents() {
  static bs::Trainer t;
  static FakePort p(t);
  uint32_t now = 1000;
  ready(t, p, now);
  // The app went away and its APP_DISCONNECT did not fit: the link is dropped
  // (a gap), the app released; the mirror comes back.
  p.appAlive = false;
  p.flood();
  run(t, now, 1);
  run(t, now);
  assert(p.terminates == 1 && p.connects == 2 && info(t).state == TrainerState::READY);
  assert(!info(t).appConnected && t.advertise());
  // Resync finds a vanished app as well.
  p.appConnect();
  run(t, now, 1);
  p.appAlive = false;
  t.resync(now);
  assert(!info(t).appConnected);

  // Stale WRITE_DONE / READ of the previous link are ignored.
  const uint16_t old = p.gen();
  p.drop();
  run(t, now);
  assert(info(t).state == TrainerState::READY && p.gen() != old);
  p.ackWrites = false;
  t.readAlong(0x2A63, true);
  t.readAlong(0x2A66, true);
  run(t, now, 1);
  assert(p.writes.size() == 1);
  p.push(TrnEv::WRITE_DONE, 0, 0, 0, 0, nullptr, 0, old);
  p.push(TrnEv::READ, 0, p.valHandle(1), 0, 0, nullptr, 0, old);
  run(t, now, 1);
  assert(p.writes.size() == 1);
  p.push(TrnEv::WRITE_DONE);
  run(t, now, 1);
  assert(p.writes.size() == 2);

  // Disconnect during discovery: no TRAINER_DISCONNECTED, backoff, then READY.
  gEvents.clear();
  p.answerReads = false;
  p.drop();
  run(t, now, 10);
  assert(info(t).state == TrainerState::DISCOVERING);
  p.answerReads = true;
  p.drop();
  run(t, now, 10);
  assert(info(t).state == TrainerState::WAITING && countEvents(EventType::TRAINER_DISCONNECTED) == 1);
  run(t, now);
  assert(info(t).state == TrainerState::READY);
}

// Rediscovery with shifted handles: same structure, mapped by index.
static void testRediscovery() {
  static bs::Trainer t;
  static FakePort p(t);
  uint32_t now = 1000;
  ready(t, p, now, false);
  const uint8_t bike = static_cast<uint8_t>(idxOf(p.db, 0x2A63));
  const uint16_t oldCccd = p.cccdHandle(bike);
  p.base = 0x40;
  p.drop();
  run(t, now);
  assert(info(t).state == TrainerState::READY && p.cccdHandle(bike) != oldCccd);
  p.appConnect();
  p.push(TrnEv::APP_SUB, 0, kAppConn, 1, bike);
  run(t, now, 2);
  assert(p.writes.back().handle == p.cccdHandle(bike));
  p.notify(bike, 8);
  run(t, now, 1);
  assert(p.sent.size() == 1 && p.sent[0].idx == bike);
  assert(bs::structureHash(t.table()) == info(t).hash);
}

static void testWatchdog() {
  static bs::Trainer t;
  static FakePort p(t);
  uint32_t now = 1000;
  ready(t, p, now, false);
  // CLOSING: a link that does not go away is terminated again.
  p.deliverTerminate = false;
  t.stop();
  run(t, now, 1);
  assert(p.terminates == 1 && !t.idle());
  run(t, now, 710);
  assert(p.terminates == 2 && !t.idle());
  p.deliverTerminate = true;
  run(t, now, 710);
  assert(p.terminates == 3 && t.idle());

  // m-3: the handle now belongs to another peer: not terminated again.
  ready(t, p, now, false);
  p.deliverTerminate = false;
  t.stop();
  run(t, now, 1);
  const int terminates = p.terminates;
  p.handleReused = true;
  run(t, now, 710);
  assert(p.terminates == terminates && t.idle());
  p.handleReused = false;
  p.deliverTerminate = true;
  p.drop();
  run(t, now, 1);

  // CONNECTED lost: the watchdog gives up, the orphaned link is terminated
  // by the next connect (BLE_HS_EDONE).
  static bs::Trainer t2;
  static FakePort p2(t2);
  p2.silentConnect = true;
  neo(p2.store);
  p2.stored = true;
  assert(t2.begin(true, now));
  run(t2, now, 750);
  assert(p2.connects == 1 && info(t2).state == TrainerState::WAITING);
  p2.silentConnect = false;
  run(t2, now, 300);
  assert(p2.orphanKills == 1 && p2.connects == 3 && info(t2).state == TrainerState::READY);
}

// Trainer off: backoff up to 30 s; a scan report of the trainer shortens it.
static void testBackoff() {
  static bs::Trainer t;
  static FakePort p(t);
  uint32_t now = 1000;
  neo(p.store);
  p.stored = true;
  p.trainerOn = false;
  assert(t.begin(true, now));
  std::vector<uint32_t> at;
  for (int k = 0; k < 12000; ++k) {   // 120 s
    const int before = p.connects;
    t.update(now, true, onEvent);
    if (p.connects != before) at.push_back(now);
    now += 10;
  }
  const uint32_t gaps[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
  assert(at.size() == 8);
  for (size_t k = 0; k < 7; ++k) assert(at[k + 1] - at[k] >= gaps[k] && at[k + 1] - at[k] <= gaps[k] + 20);
  assert(info(t).state == TrainerState::WAITING);
  run(t, now, 50);                    // the next attempt is due at 121 s
  assert(p.connects == 8);
  p.trainerOn = true;
  t.capture(kNeoMac, 0, nullptr, 0, false);
  run(t, now, 2);
  assert(p.connects == 9);
  run(t, now);
  assert(info(t).state == TrainerState::READY);
}

static void testChanged() {
  static bs::Trainer t;
  static FakePort p(t);
  uint32_t now = 1000;
  gEvents.clear();
  neo(p.store);
  p.stored = true;
  p.db.chr[3].props = W;   // firmware update changed the structure
  assert(t.begin(true, now));
  run(t, now);
  assert(countEvents(EventType::TRAINER_CONNECTED, 1) == 1);
  assert(countEvents(EventType::TRAINER_DISCONNECTED) == 0);
  assert(info(t).state == TrainerState::CHANGED && !t.advertise() && p.connects == 1);
  // The mirror's table is untouched by the discovery.
  assert(t.table().chr[3].props == (W | I) && bs::structureHash(t.table()) == info(t).hash);
  t.capture(kNeoMac, 0, nullptr, 0, false);   // advertising does not shorten the wait
  run(t, now, 2000);   // 20 s
  assert(p.connects == 1);
  run(t, now, 1100);
  assert(p.connects == 2 && countEvents(EventType::TRAINER_CONNECTED, 1) == 1);
  neo(p.db);
  run(t, now, 3100);
  assert(p.connects == 3 && info(t).state == TrainerState::READY);
  assert(countEvents(EventType::TRAINER_CONNECTED, 0) == 1);
}

static void testDiscoveryFailure() {
  static bs::Trainer t;
  static FakePort p(t);
  uint32_t now = 1000;
  gEvents.clear();
  neo(p.store);
  p.stored = true;
  p.db.chr[0].cccd = false;   // notify without CCCD: incomplete
  assert(t.begin(true, now));
  run(t, now, 50);
  assert(p.connects == 1 && p.terminates == 1 && info(t).state == TrainerState::WAITING);
  assert(countEvents(EventType::TRAINER_CONNECTED) == 0);
  run(t, now, 100);    // 1 s backoff
  assert(p.connects == 2);
  // The trainer keeps advertising: a failed discovery keeps the full backoff
  // (2, 4, 8, 16, 30 s), the 1 s shortcut is for failed connects only.
  for (int k = 0; k < 6000; ++k) {   // 60 s
    t.capture(kNeoMac, 0, nullptr, 0, false);
    t.update(now, true, onEvent);
    now += 10;
  }
  assert(p.connects >= 6 && p.connects <= 7);
}

// M-1: a second setup while the mirror is READY with an app: the mirror link
// that setup() closes does not end the setup, early or late.
static void testSetupWhileMirror() {
  static bs::Trainer t;
  static FakePort p(t);
  uint32_t now = 1000;
  for (int late = 0; late < 2; ++late) {
    ready(t, p, now);
    p.deliverTerminate = late == 0;
    t.setup(kNeoMac);
    run(t, now, 1);
    assert(p.kicked.back() == kAppConn && info(t).state == TrainerState::SETUP);
    t.capture(kNeoMac, 0, kNeoAdv, sizeof(kNeoAdv), false);
    t.capture(kNeoMac, 0, kNeoRsp, sizeof(kNeoRsp), true);
    t.discoveryDone();
    run(t, now, 1);
    if (late == 1) {
      assert(countEvents(EventType::TRAINER_SETUP_DONE) == 0);
      p.deliverTerminate = true;
      p.drop();
    }
    run(t, now);
    assert(countEvents(EventType::TRAINER_SETUP_DONE) == 1);
    assert(countEvents(EventType::TRAINER_SETUP_DONE, int(TrainerCheck::OK)) == 1 && p.stored);
    p.appDisconnect();
    run(t, now, 1);
  }
}

// M-2: a stalled update() fills the queue with notifications; the reserve
// still takes writes, subscriptions and a disconnect; only notifications drop.
static void testNotifyReserve() {
  static bs::Trainer t;
  static FakePort p(t);
  uint32_t now = 1000;
  ready(t, p, now);
  const uint8_t bike = static_cast<uint8_t>(idxOf(p.db, 0x2A63));
  const uint8_t cp = static_cast<uint8_t>(idxOf(p.db, 0x2A66));
  p.push(TrnEv::APP_SUB, 0, kAppConn, 1, bike);
  run(t, now, 2);                           // CCCD write and its WRITE_DONE
  const size_t writes = p.writes.size();
  assert(p.floodNotify(bike) == bs::kTrnQueueDepth - bs::kTrnCtlReserve);
  const uint8_t cmd[1] = {0x05};
  p.push(TrnEv::APP_WRITE, 0, kAppConn, 0, cp, cmd, 1);
  p.push(TrnEv::APP_SUB, 0, kAppConn, 2, cp);
  run(t, now, 3);
  assert(info(t).state == TrainerState::READY && p.terminates == 0 && p.kicked.empty());
  assert(p.writes.size() == writes + 2 && info(t).dropped == 1);
  assert(p.sent.size() == size_t(bs::kTrnQueueDepth - bs::kTrnCtlReserve));
  // A trainer indication (already confirmed by NimBLE) may use the reserve.
  p.floodNotify(bike);
  p.notify(cp, 3, -1, true);
  run(t, now, 1);
  assert(p.sent.back().idx == cp && p.sent.back().indicate && info(t).dropped == 2);
  p.floodNotify(bike);
  p.drop();
  run(t, now, 1);
  assert(countEvents(EventType::TRAINER_DISCONNECTED, 8) == 1 && info(t).dropped == 3);
}

// G5: the NEO's FC82 0004 device-info indication (~70 bytes) reaches the app
// whole (it was cut to 64 bytes before).
static void testLongIndication() {
  static bs::Trainer t;
  static FakePort p(t);
  uint32_t now = 1000;
  ready(t, p, now);
  const uint8_t info4 = 22;                 // FC82 ...0004, indicate
  assert(p.db.chr[info4].props == I && p.db.svc[p.db.chr[info4].svc].b[1] == 0xFC);
  p.push(TrnEv::APP_SUB, 0, kAppConn, 2, info4);
  run(t, now, 2);
  uint8_t value[70] = {0x3C, 0x08, 0x00, 0x12, 0x41};
  for (uint8_t k = 5; k < sizeof(value); ++k) value[k] = k;
  p.notify(info4, sizeof(value), -1, true, value);
  run(t, now, 1);
  assert(p.sent.size() == 1 && p.sent[0].indicate && p.sent[0].idx == info4);
  assert(p.sent[0].data == std::vector<uint8_t>(value, value + sizeof(value)));
  assert(info(t).oversize == 0 && info(t).dropped == 0);
}

// Lost events: the trainer's DISCONNECTED and the app's APP_DISCONNECT, and
// sensors got both handles. Neither handle is terminated.
static void testLostHandleReuse() {
  static bs::Trainer t;
  static FakePort p(t);
  uint32_t now = 1000;
  ready(t, p, now);
  const int terminates = p.terminates;
  p.handleReused = true;
  p.appAlive = false;
  p.flood();
  run(t, now, 1);
  assert(p.terminates == terminates && p.kicked.empty());
  assert(countEvents(EventType::TRAINER_DISCONNECTED) == 1 && !info(t).appConnected);
  p.handleReused = false;
  run(t, now, 300);
  assert(info(t).state == TrainerState::READY && t.advertise());
}

// m-4: an app whose APP_CONNECT was lost is disconnected; the mirror does not
// advertise beside it.
static void testLostAppConnect() {
  static bs::Trainer t;
  static FakePort p(t);
  uint32_t now = 1000;
  ready(t, p, now, false);
  p.flood();
  TrnEvent ev{};
  ev.kind = TrnEv::APP_CONNECT;
  ev.h1 = kAppConn;
  ev.h2 = 247;
  assert(!t.queue().push(ev));
  run(t, now, 1);
  assert(p.appKills == 1 && !info(t).appConnected && !t.advertise());
  run(t, now);
  assert(info(t).state == TrainerState::READY && t.advertise());
}

int main() {
  testUuid();
  testCrc();
  testCodec();
  testHash();
  testAddress();
  testCheck();
  testAdv();
  testSetup();
  testMirrorReady();
  testFifoOrder();
  testIndicationPacing();
  testLengthCap();
  testRefcount();
  testCccdRetry();
  testRetry();
  testLinkCoupling();
  testLostEvents();
  testRediscovery();
  testWatchdog();
  testBackoff();
  testChanged();
  testDiscoveryFailure();
  testSetupWhileMirror();
  testNotifyReserve();
  testLostAppConnect();
  testLongIndication();
  testLostHandleReuse();
  return 0;
}
