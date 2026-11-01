#pragma once

#include <atomic>
#include <cstdint>

// Arm generation for the GATT watchdog (§5.2). update() arms before the
// blocking pass and disarms after it; the esp_timer callback terminates the
// link only if the generation it was started with is still the armed one, so
// a callback that races the stop cannot cut a fresh link.

namespace BikeSensor {
namespace internal {

class GattWatchdog {
 public:
  // update() task, before the pass. Returns the arm generation.
  uint32_t arm(uint16_t connHandle) {
    connHandle_.store(connHandle, std::memory_order_relaxed);
    const uint32_t gen = armGen_.fetch_add(1, std::memory_order_acq_rel) + 1;
    timerGen_.store(gen, std::memory_order_release);
    return gen;
  }

  // update() task, after the pass and before stopping the timer.
  void disarm() { armGen_.fetch_add(1, std::memory_order_acq_rel); }

  // esp_timer task. True: call ble_gap_terminate(connHandle()) now.
  bool onTimer() {
    const uint32_t gen = timerGen_.load(std::memory_order_acquire);
    if (gen == 0 || gen != armGen_.load(std::memory_order_acquire)) return false;
    firedGen_.store(gen, std::memory_order_release);
    return true;
  }

  uint16_t connHandle() const { return connHandle_.load(std::memory_order_relaxed); }

  // update() task, after disarm(): did the timer terminate the pass `gen`?
  bool fired(uint32_t gen) const { return firedGen_.load(std::memory_order_acquire) == gen; }

 private:
  std::atomic<uint32_t> armGen_{0};
  std::atomic<uint32_t> timerGen_{0};
  std::atomic<uint32_t> firedGen_{0};
  std::atomic<uint16_t> connHandle_{0};
};

}  // namespace internal
}  // namespace BikeSensor
