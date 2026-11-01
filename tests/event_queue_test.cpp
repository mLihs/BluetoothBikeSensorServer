#include "BBSEventPush.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>

using BikeSensor::internal::Ev;
using BikeSensor::internal::Event;
using BikeSensor::internal::EventQueue;
using BikeSensor::internal::QueueCounters;
namespace bs = BikeSensor::internal;

static_assert(sizeof(Event) <= 48, "");
static_assert(sizeof(Event) == 46, "default layout is 46 B (§3.2)");

constexpr uint8_t kDepth = BBS_EVENT_QUEUE_DEPTH;
static_assert(kDepth >= 3, "tests assume depth >= 3");
static const uint8_t kAddr[6] = {1, 2, 3, 4, 5, 6};
static const uint8_t kData[4] = {0xAA, 0xBB, 0xCC, 0xDD};

static void pushMatch(EventQueue& q, uint16_t epoch) {
  bs::pushScanMatch(q, epoch, kAddr, 0, "HRM", 3);
}

static Event popOne(EventQueue& q) {
  Event ev{};
  assert(q.pop(ev));
  return ev;
}

static void testPushPopFifoAndWrap() {
  auto q = std::make_unique<EventQueue>();
  Event ev{};
  assert(!q->pop(ev));

  for (uint16_t round = 0; round < 3 * kDepth; ++round) {
    assert(bs::pushConnected(*q, 1, round));
    assert(bs::pushDisconnected(*q, 1, round, -7));
    Event a = popOne(*q);
    Event b = popOne(*q);
    assert(a.kind == Ev::CONNECTED && a.slot == 1 && a.gen == round);
    assert(b.kind == Ev::DISCONNECTED && b.gen == round && b.reason == -7);
  }
  assert(q->size() == 0);

  for (uint16_t i = 0; i < kDepth; ++i) assert(bs::pushScanEnd(*q, i, 0));
  assert(q->size() == kDepth);
  for (uint16_t i = 0; i < kDepth; ++i) assert(popOne(*q).gen == i);
  assert(!q->pop(ev));

  QueueCounters c = q->counters();
  assert(c.eventsDropped == 0 && c.notifiesDropped == 0 && c.matchesDropped == 0);
  assert(!q->takeResync());
}

static void testDroppableOverflow() {
  auto q = std::make_unique<EventQueue>();
  for (uint16_t i = 0; i < kDepth; ++i) pushMatch(*q, i);
  assert(!bs::pushNotify(*q, 0, 9, kData, sizeof(kData)));
  pushMatch(*q, 999);
  pushMatch(*q, 999);

  QueueCounters c = q->counters();
  assert(c.notifiesDropped == 1 && c.matchesDropped == 2 && c.eventsDropped == 0);
  assert(!q->takeResync());
  assert(q->size() == kDepth);
  for (uint16_t i = 0; i < kDepth; ++i) assert(popOne(*q).gen == i);

  // A full ring of protected events also drops a new droppable, without resync.
  for (uint16_t i = 0; i < kDepth; ++i) bs::pushHostReset(*q);
  assert(!bs::pushNotify(*q, 0, 9, kData, sizeof(kData)));
  c = q->counters();
  assert(c.notifiesDropped == 2 && c.eventsDropped == 0);
  assert(!q->takeResync());
}

static void testProtectedEvictsOldestDroppable() {
  auto q = std::make_unique<EventQueue>();
  // [CONNECTED g0, MATCH g1, NOTIFY g2, CONNECTED g3, ..., CONNECTED g(D-1)]
  assert(bs::pushConnected(*q, 0, 0));
  pushMatch(*q, 1);
  assert(bs::pushNotify(*q, 0, 2, kData, sizeof(kData)));
  for (uint16_t i = 3; i < kDepth; ++i) assert(bs::pushConnected(*q, 0, i));
  assert(q->size() == kDepth);

  assert(bs::pushDisconnected(*q, 0, 100, 0x13));
  QueueCounters c = q->counters();
  assert(c.matchesDropped == 1 && c.notifiesDropped == 0 && c.eventsDropped == 0);

  assert(bs::pushSubscribe(*q, 1));
  c = q->counters();
  assert(c.matchesDropped == 1 && c.notifiesDropped == 1 && c.eventsDropped == 0);
  assert(!q->takeResync());
  assert(q->size() == kDepth);

  Event first = popOne(*q);
  assert(first.kind == Ev::CONNECTED && first.gen == 0);
  for (uint16_t i = 3; i < kDepth; ++i) {
    Event ev = popOne(*q);
    assert(ev.kind == Ev::CONNECTED && ev.gen == i);
  }
  Event disc = popOne(*q);
  assert(disc.kind == Ev::DISCONNECTED && disc.gen == 100 && disc.reason == 0x13);
  Event sub = popOne(*q);
  assert(sub.kind == Ev::SUBSCRIBE && sub.sub.subValue == 1);
  Event none{};
  assert(!q->pop(none));
}

static void testEvictionAtHead() {
  auto q = std::make_unique<EventQueue>();
  // Oldest droppable is at head (k == 0): zero shifts in evictOldestDroppable.
  pushMatch(*q, 0);
  for (uint16_t i = 1; i < kDepth; ++i) assert(bs::pushConnected(*q, 0, i));
  assert(bs::pushDisconnected(*q, 0, 99, 1));
  assert(q->counters().matchesDropped == 1);
  assert(!q->takeResync());
  for (uint16_t i = 1; i < kDepth; ++i) {
    Event ev = popOne(*q);
    assert(ev.kind == Ev::CONNECTED && ev.gen == i);
  }
  Event disc = popOne(*q);
  assert(disc.kind == Ev::DISCONNECTED && disc.gen == 99);
}

static void testMultiEvictionInterleave() {
  auto q = std::make_unique<EventQueue>();
  // [P, D, P, D, ...] then several protected pushes; each evicts the oldest D.
  for (uint16_t i = 0; i < kDepth; ++i) {
    if ((i % 2) == 0) {
      assert(bs::pushConnected(*q, 0, i));
    } else {
      pushMatch(*q, i);
    }
  }
  const uint16_t droppable = (kDepth / 2);
  for (uint16_t n = 0; n < droppable; ++n) {
    assert(bs::pushDisconnected(*q, 0, static_cast<uint16_t>(1000 + n), 0));
  }
  QueueCounters c = q->counters();
  assert(c.matchesDropped == droppable && c.eventsDropped == 0);
  assert(!q->takeResync());
  assert(q->size() == kDepth);
  uint16_t protectedLeft = 0;
  for (uint16_t i = 0; i < kDepth; ++i) {
    Event ev = popOne(*q);
    assert(ev.kind != Ev::SCAN_MATCH && ev.kind != Ev::NOTIFY);
    if (ev.kind == Ev::CONNECTED) protectedLeft++;
  }
  assert(protectedLeft == (kDepth - droppable));
}

static void testEvictionAcrossWrap() {
  auto q = std::make_unique<EventQueue>();
  for (uint16_t i = 0; i < kDepth / 2; ++i) bs::pushHostReset(*q);
  for (uint16_t i = 0; i < kDepth / 2; ++i) popOne(*q);
  // head is mid-ring, so the eviction shift crosses the end of the array.
  for (uint16_t i = 0; i < kDepth - 1; ++i) assert(bs::pushCentralConnect(*q));
  pushMatch(*q, 500);
  assert(bs::pushCentralDisconnect(*q, 0x08));
  assert(q->counters().matchesDropped == 1);
  for (uint16_t i = 0; i < kDepth - 1; ++i) assert(popOne(*q).kind == Ev::CENTRAL_CONNECT);
  Event last = popOne(*q);
  assert(last.kind == Ev::CENTRAL_DISCONNECT && last.reason == 0x08);
}

static void testProtectedOnlyFullRequestsResync() {
  auto q = std::make_unique<EventQueue>();
  for (uint16_t i = 0; i < kDepth; ++i) assert(bs::pushConnectFail(*q, 2, i, 13));
  assert(!bs::pushScanEnd(*q, 77, 0));
  assert(!bs::pushHostReset(*q));

  QueueCounters c = q->counters();
  assert(c.eventsDropped == 2 && c.notifiesDropped == 0 && c.matchesDropped == 0);
  assert(q->size() == kDepth);
  assert(q->takeResync());
  assert(!q->takeResync());
  for (uint16_t i = 0; i < kDepth; ++i) {
    Event ev = popOne(*q);
    assert(ev.kind == Ev::CONNECT_FAIL && ev.gen == i);
  }

  q->requestResync();
  assert(q->takeResync());
  assert(!q->takeResync());
}

static void testNotifyCopy() {
  auto q = std::make_unique<EventQueue>();
  uint8_t big[BBS_NOTIFY_MAX_LEN + 5];
  for (size_t i = 0; i < sizeof(big); ++i) big[i] = static_cast<uint8_t>(i);

  assert(bs::pushNotify(*q, 1, 4, big, sizeof(big)));
  assert(bs::pushNotify(*q, 1, 4, big, BBS_NOTIFY_MAX_LEN));
  assert(bs::pushNotify(*q, 1, 4, nullptr, 0));

  Event cut = popOne(*q);
  assert(cut.kind == Ev::NOTIFY && cut.slot == 1 && cut.gen == 4);
  assert(cut.notify.len == BBS_NOTIFY_MAX_LEN);
  assert(cut.reason == static_cast<int16_t>(sizeof(big)));
  assert(cut.reason > cut.notify.len);
  assert(memcmp(cut.notify.data, big, BBS_NOTIFY_MAX_LEN) == 0);

  Event full = popOne(*q);
  assert(full.notify.len == BBS_NOTIFY_MAX_LEN && full.reason == BBS_NOTIFY_MAX_LEN);

  Event empty = popOne(*q);
  assert(empty.notify.len == 0 && empty.reason == 0);
}

static void testMatchCopy() {
  auto q = std::make_unique<EventQueue>();
  const char longName[] = "0123456789012345678901234567890123456789";
  assert(bs::pushScanMatch(*q, 3, kAddr, 1, longName, sizeof(longName) - 1));
  assert(bs::pushScanMatch(*q, 3, kAddr, 0, "Tickr", 3));
  assert(bs::pushScanMatch(*q, 3, kAddr, 0, nullptr, 10));

  Event a = popOne(*q);
  assert(a.kind == Ev::SCAN_MATCH && a.gen == 3 && a.match.addrType == 1);
  assert(memcmp(a.match.addr, kAddr, 6) == 0);
  assert(strlen(a.match.name) == 31);
  assert(strncmp(a.match.name, longName, 31) == 0);

  Event b = popOne(*q);
  assert(strcmp(b.match.name, "Tic") == 0);

  Event c = popOne(*q);
  assert(c.match.name[0] == '\0');
}

// Host task pushes while update() pops. Every pushed event is either popped
// in order or counted as dropped.
static void testConcurrentPushPop() {
  constexpr uint32_t kEvents = 60000;  // gen is uint16_t; stay below the wrap
  auto q = std::make_unique<EventQueue>();
  std::atomic<bool> done{false};

  std::thread producer([&] {
    for (uint32_t i = 0; i < kEvents; ++i) {
      const uint16_t seq = static_cast<uint16_t>(i);
      if (i % 3 == 0) {
        bs::pushConnected(*q, 0, seq);
      } else if (i % 3 == 1) {
        bs::pushNotify(*q, 0, seq, kData, sizeof(kData));
      } else {
        pushMatch(*q, seq);
      }
    }
    done.store(true, std::memory_order_release);
  });

  uint32_t popped = 0;
  int32_t lastGen = -1;
  Event ev{};
  for (;;) {
    const bool finished = done.load(std::memory_order_acquire);
    while (q->pop(ev)) {
      assert(static_cast<int32_t>(ev.gen) > lastGen);
      lastGen = ev.gen;
      popped++;
    }
    if (finished) break;
  }
  producer.join();

  QueueCounters c = q->counters();
  assert(popped + c.eventsDropped + c.notifiesDropped + c.matchesDropped == kEvents);
  assert(q->takeResync() == (c.eventsDropped > 0));
}

int main() {
  testPushPopFifoAndWrap();
  testDroppableOverflow();
  testProtectedEvictsOldestDroppable();
  testEvictionAtHead();
  testMultiEvictionInterleave();
  testEvictionAcrossWrap();
  // nullptr notify payload must not crash (caller hygiene).
  {
    auto q = std::make_unique<EventQueue>();
    assert(bs::pushNotify(*q, 0, 1, nullptr, 8));
    Event ev = popOne(*q);
    assert(ev.notify.len == 0 && ev.reason == 0);
  }
  testProtectedOnlyFullRequestsResync();
  testNotifyCopy();
  testMatchCopy();
  testConcurrentPushPop();
  return 0;
}
