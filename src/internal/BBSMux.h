#pragma once

// Short critical section shared by the NimBLE host task and the update() task.
// Device: portMUX spinlock. Host tests: std::mutex.
// Hold only for bounded copies; never across a NimBLE call (§3.4).

#if defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#else
#include <mutex>
#endif

namespace BikeSensor {
namespace internal {

class Mux {
 public:
#if defined(ESP_PLATFORM)
  Mux() { portMUX_INITIALIZE(&mux_); }
  void lock() { portENTER_CRITICAL(&mux_); }
  void unlock() { portEXIT_CRITICAL(&mux_); }
#else
  Mux() = default;
  void lock() { mux_.lock(); }
  void unlock() { mux_.unlock(); }
#endif

  Mux(const Mux&) = delete;
  Mux& operator=(const Mux&) = delete;

 private:
#if defined(ESP_PLATFORM)
  portMUX_TYPE mux_;
#else
  std::mutex mux_;
#endif
};

class MuxGuard {
 public:
  explicit MuxGuard(Mux& mux) : mux_(mux) { mux_.lock(); }
  ~MuxGuard() { mux_.unlock(); }

  MuxGuard(const MuxGuard&) = delete;
  MuxGuard& operator=(const MuxGuard&) = delete;

 private:
  Mux& mux_;
};

}  // namespace internal
}  // namespace BikeSensor
