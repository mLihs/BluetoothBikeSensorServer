#pragma once

#include <atomic>
#include <cstdint>

#include "BBSMux.h"

#ifndef BBS_EVENT_QUEUE_DEPTH
#define BBS_EVENT_QUEUE_DEPTH 24
#endif
#ifndef BBS_NOTIFY_MAX_LEN
#define BBS_NOTIFY_MAX_LEN 36               // full CPS measurement is 34 B (§6)
#endif
static_assert(BBS_EVENT_QUEUE_DEPTH >= 1 && BBS_EVENT_QUEUE_DEPTH <= 255,
              "ring indices are uint8_t");
static_assert(BBS_NOTIFY_MAX_LEN >= 20 && BBS_NOTIFY_MAX_LEN <= 38, "Event must stay <= 48 B");

namespace BikeSensor {
namespace internal {

enum class Ev : uint8_t {
  SCAN_MATCH, SCAN_END, HOST_RESET, CONNECTED, CONNECT_FAIL, DISCONNECTED,
  NOTIFY, CENTRAL_CONNECT, CENTRAL_DISCONNECT, SUBSCRIBE
};

struct Event {                          // 46 B
  Ev kind; uint8_t slot; int16_t reason; uint16_t gen;   // gen = attemptGen or scan epoch
  union {
    struct { uint8_t addr[6]; uint8_t addrType; char name[32]; } match;
    struct { uint8_t len; uint8_t data[BBS_NOTIFY_MAX_LEN]; } notify;
    struct { uint16_t subValue; } sub;
  };
};
static_assert(sizeof(Event) <= 48, "Event must stay <= 48 B");

constexpr bool isDroppable(Ev kind) {
  return kind == Ev::SCAN_MATCH || kind == Ev::NOTIFY;
}

struct QueueCounters {
  uint32_t eventsDropped;
  uint32_t notifiesDropped;
  uint32_t matchesDropped;
};

// Fixed ring. push() from any task (NimBLE host task), pop() only from update().
class EventQueue {
 public:
  // Full ring: a droppable event is dropped; a protected event evicts the
  // oldest droppable one. With none left it is dropped and resync is requested.
  bool push(const Event& ev) {
    bool overflow = false;
    {
      MuxGuard guard(mux_);
      if (count_ == kDepth) {
        if (isDroppable(ev.kind)) {
          countDropped(ev.kind);
          return false;
        }
        if (!evictOldestDroppable()) {
          counters_.eventsDropped++;
          overflow = true;
        }
      }
      if (!overflow) {
        ring_[index(count_)] = ev;
        count_++;
      }
    }
    if (overflow) {
      requestResync();
      return false;
    }
    return true;
  }

  bool pop(Event& out) {
    MuxGuard guard(mux_);
    if (count_ == 0) return false;
    out = ring_[head_];
    head_ = index(1);
    count_--;
    return true;
  }

  uint8_t size() {
    MuxGuard guard(mux_);
    return count_;
  }

  QueueCounters counters() {
    MuxGuard guard(mux_);
    return counters_;
  }

  // Queue side of the §3.2 resync only: update() takes the flag before its
  // drain and reconciles slots, scan and advertising (not built until §10 step 3).
  void requestResync() { resyncNeeded_.store(true, std::memory_order_release); }

  bool takeResync() {
    return resyncNeeded_.exchange(false, std::memory_order_acq_rel);
  }

 private:
  static constexpr uint8_t kDepth = BBS_EVENT_QUEUE_DEPTH;

  uint8_t index(uint8_t offset) const {
    return static_cast<uint8_t>((head_ + offset) % kDepth);
  }

  void countDropped(Ev kind) {
    if (kind == Ev::SCAN_MATCH) {
      counters_.matchesDropped++;
    } else {
      counters_.notifiesDropped++;
    }
  }

  // Removes the oldest droppable event and keeps the order of the rest:
  // the older (protected) events move one place toward the tail.
  bool evictOldestDroppable() {
    for (uint8_t k = 0; k < count_; ++k) {
      if (!isDroppable(ring_[index(k)].kind)) continue;
      countDropped(ring_[index(k)].kind);
      for (uint8_t j = k; j > 0; --j) {
        ring_[index(j)] = ring_[index(static_cast<uint8_t>(j - 1))];
      }
      head_ = index(1);
      count_--;
      return true;
    }
    return false;
  }

  Mux mux_;                              // queueMux (§3.4)
  Event ring_[kDepth];
  uint8_t head_ = 0;
  uint8_t count_ = 0;
  QueueCounters counters_{};
  std::atomic<bool> resyncNeeded_{false};
};

}  // namespace internal
}  // namespace BikeSensor
