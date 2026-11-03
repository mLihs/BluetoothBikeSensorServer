#include "BBSStoreStatus.h"

#include <cassert>
#include <cstdio>
#include <initializer_list>

namespace bs = BikeSensor::internal;
using bs::StoreVerdict;

// A bond list as ble_store_util_bonded_peers returns it, oldest first.
struct FakeBonds {
  int n = 0;
  bool liveAt[4] = {};
  bool cccdAt[4] = {};
  bool csfcAt[4] = {};
  bool eraseOk = true;
  int erased = -1, erases = 0, counts = 0, ownsCalls = 0;

  int count() {
    counts++;
    return n;
  }
  bool live(int i) { return liveAt[i]; }
  bool owns(int i, int objType) {
    ownsCalls++;
    assert(objType == bs::kStoreObjCccd || objType == bs::kStoreObjCsfc);
    return objType == bs::kStoreObjCccd ? cccdAt[i] : csfcAt[i];
  }
  bool erase(int i) {
    erases++;
    erased = i;
    return eraseOk;
  }
};

static StoreVerdict run(int eventCode, int objType, FakeBonds& b, bs::BondCounters& c) {
  return bs::storeStatus(eventCode, objType, b, c);
}

// FULL precedes every pairing at the maximum: always proceed, touch nothing.
static void testFull() {
  FakeBonds b;
  b.n = 3;
  bs::BondCounters c;
  assert(run(bs::kStoreEventFull, 0, b, c) == StoreVerdict::PROCEED);
  assert(b.counts == 0 && b.erases == 0 && c.evictions == 0 && c.rejects == 0);
}

// OUR_SEC / PEER_SEC / PEER_ADDR: the oldest peer without a link goes, no
// record check; a live peer (the meter mid-ride, a central) is never deleted.
static void testBondOverflow() {
  for (int type : {bs::kStoreObjOurSec, bs::kStoreObjPeerSec, bs::kStoreObjPeerAddr}) {
    FakeBonds b;
    b.n = 3;
    b.liveAt[0] = true;
    bs::BondCounters c;
    assert(run(bs::kStoreEventOverflow, type, b, c) == StoreVerdict::PROCEED);
    assert(b.erased == 1 && b.erases == 1 && b.ownsCalls == 0);
    assert(c.evictions == 1 && c.rejects == 0);
    FakeBonds all;                                     // every peer connected: reject
    all.n = 2;
    all.liveAt[0] = all.liveAt[1] = true;
    assert(run(bs::kStoreEventOverflow, type, all, c) == StoreVerdict::REJECT);
    assert(all.erases == 0 && c.evictions == 1 && c.rejects == 1);
    FakeBonds none;                                    // empty list: reject
    assert(run(bs::kStoreEventOverflow, type, none, c) == StoreVerdict::REJECT);
    assert(c.rejects == 2);
  }
}

// CCCD / CSFC: only an unconnected peer that owns a record of that type.
static void testRecordOverflow() {
  {
    FakeBonds b;                                       // 0 live, 1 no CCCD, 2 has one
    b.n = 3;
    b.liveAt[0] = true;
    b.cccdAt[0] = true;
    b.csfcAt[1] = true;
    b.cccdAt[2] = true;
    bs::BondCounters c;
    assert(run(bs::kStoreEventOverflow, bs::kStoreObjCccd, b, c) == StoreVerdict::PROCEED);
    assert(b.erased == 2 && b.erases == 1 && c.evictions == 1);
  }
  {
    FakeBonds b;                                       // CSFC checks CSFC records
    b.n = 3;
    b.cccdAt[0] = true;
    b.csfcAt[1] = true;
    bs::BondCounters c;
    assert(run(bs::kStoreEventOverflow, bs::kStoreObjCsfc, b, c) == StoreVerdict::PROCEED);
    assert(b.erased == 1 && c.evictions == 1);
  }
  {
    FakeBonds b;                                       // nobody frees a record: reject
    b.n = 2;                                           // (the sensor bonds stay)
    b.liveAt[0] = true;
    b.cccdAt[0] = true;
    bs::BondCounters c;
    assert(run(bs::kStoreEventOverflow, bs::kStoreObjCccd, b, c) == StoreVerdict::REJECT);
    assert(b.erases == 0 && c.evictions == 0 && c.rejects == 1);
  }
}

// A failed delete ends with a reject, so ble_store_write cannot loop on it.
static void testEraseFails() {
  FakeBonds b;
  b.n = 2;
  b.eraseOk = false;
  bs::BondCounters c;
  assert(run(bs::kStoreEventOverflow, bs::kStoreObjOurSec, b, c) == StoreVerdict::REJECT);
  assert(b.erases == 1 && c.evictions == 0 && c.rejects == 1);
}

// Other overflow types (ENC_ADV_DATA 5, PEER_DEV_REC 4, LOCAL_IRK 7) and
// unknown events go to NimBLE's default handler untouched.
static void testDelegate() {
  FakeBonds b;
  b.n = 3;
  bs::BondCounters c;
  for (int type : {4, 5, 7, 0, 99}) {
    assert(run(bs::kStoreEventOverflow, type, b, c) == StoreVerdict::DELEGATE);
  }
  assert(run(3, bs::kStoreObjOurSec, b, c) == StoreVerdict::DELEGATE);
  assert(run(0, 0, b, c) == StoreVerdict::DELEGATE);
  assert(b.counts == 0 && b.erases == 0 && c.evictions == 0 && c.rejects == 0);
}

int main() {
  testFull();
  testBondOverflow();
  testRecordOverflow();
  testEraseFails();
  testDelegate();
  std::puts("store_status_test OK");
  return 0;
}
