#pragma once

#include <atomic>
#include <cstdint>

namespace BBSInternal {

enum class DiscoveryPub : uint32_t {
  IDLE,
  STARTING,
  RUNNING,
  STOP_REQUESTED,
  HOST_QUEUED,
  PUBLISHING,
  READY,
  WAIT_HOST_DRAIN
};

class DiscoveryStopState {
 public:
  DiscoveryPub state() const {
    return decodeState(state_.load(std::memory_order_acquire));
  }

  bool tryBeginStart() {
    uint32_t current = state_.load(std::memory_order_acquire);
    while (decodeState(current) == DiscoveryPub::IDLE) {
      const uint32_t nextGeneration =
          (decodeGeneration(current) + 1u) & kGenerationMask;
      if (state_.compare_exchange_strong(
              current,
              encode(nextGeneration, DiscoveryPub::STARTING),
              std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        return true;
      }
    }
    return false;
  }

  bool markRunning() {
    return transition(DiscoveryPub::STARTING, DiscoveryPub::RUNNING);
  }

  bool markStartFailed() {
    return transition(DiscoveryPub::STARTING, DiscoveryPub::READY);
  }

  bool tryRequestStop(uint32_t& generation) {
    uint32_t current = state_.load(std::memory_order_acquire);
    while (decodeState(current) == DiscoveryPub::RUNNING) {
      const uint32_t currentGeneration = decodeGeneration(current);
      if (state_.compare_exchange_strong(
              current,
              encode(currentGeneration, DiscoveryPub::STOP_REQUESTED),
              std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        generation = currentGeneration;
        return true;
      }
    }
    return false;
  }

  bool tryQueueStopped(uint32_t generation) {
    uint32_t expected = encode(generation, DiscoveryPub::STOP_REQUESTED);
    return state_.compare_exchange_strong(
        expected,
        encode(generation, DiscoveryPub::HOST_QUEUED),
        std::memory_order_acq_rel,
        std::memory_order_acquire);
  }

  bool tryQueueExpired() {
    return transition(DiscoveryPub::RUNNING, DiscoveryPub::HOST_QUEUED);
  }

  bool tryClaimScanEnd(bool& hostEventPending) {
    uint32_t current = state_.load(std::memory_order_acquire);
    while (isScanEndClaimable(decodeState(current))) {
      const DiscoveryPub claimedState = decodeState(current);
      if (state_.compare_exchange_strong(
              current,
              encode(decodeGeneration(current), DiscoveryPub::PUBLISHING),
              std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        hostEventPending = claimedState == DiscoveryPub::HOST_QUEUED;
        return true;
      }
    }
    return false;
  }

  bool tryClaimHost() {
    return transition(DiscoveryPub::HOST_QUEUED, DiscoveryPub::PUBLISHING);
  }

  bool markReady() {
    return transition(DiscoveryPub::PUBLISHING, DiscoveryPub::READY);
  }

  bool markWaitingForHostDrain() {
    return transition(DiscoveryPub::PUBLISHING, DiscoveryPub::WAIT_HOST_DRAIN);
  }

  bool finishHostDrain() {
    return transition(DiscoveryPub::WAIT_HOST_DRAIN, DiscoveryPub::READY);
  }

  bool isReady() const {
    return state() == DiscoveryPub::READY;
  }

  bool finishPublish() {
    return transition(DiscoveryPub::READY, DiscoveryPub::IDLE);
  }

 private:
  static constexpr uint32_t kStateBits = 3;
  static constexpr uint32_t kStateMask = (1u << kStateBits) - 1u;
  static constexpr uint32_t kGenerationMask =
      (1u << (32u - kStateBits)) - 1u;

  static constexpr uint32_t encode(uint32_t generation, DiscoveryPub state) {
    return ((generation & kGenerationMask) << kStateBits) |
           static_cast<uint32_t>(state);
  }

  static constexpr DiscoveryPub decodeState(uint32_t value) {
    return static_cast<DiscoveryPub>(value & kStateMask);
  }

  static constexpr uint32_t decodeGeneration(uint32_t value) {
    return value >> kStateBits;
  }

  static constexpr bool isScanEndClaimable(DiscoveryPub state) {
    return state == DiscoveryPub::RUNNING ||
           state == DiscoveryPub::STOP_REQUESTED ||
           state == DiscoveryPub::HOST_QUEUED;
  }

  bool transition(DiscoveryPub from, DiscoveryPub to) {
    uint32_t current = state_.load(std::memory_order_acquire);
    while (decodeState(current) == from) {
      if (state_.compare_exchange_strong(
              current,
              encode(decodeGeneration(current), to),
              std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        return true;
      }
    }
    return false;
  }

  std::atomic<uint32_t> state_{encode(0, DiscoveryPub::IDLE)};
};

inline bool runExpired(uint32_t now,
                       uint32_t startedMs,
                       uint32_t durationMs,
                       uint32_t graceMs) {
  if (durationMs == 0) return false;
  const uint32_t elapsed = static_cast<uint32_t>(now - startedMs);
  return elapsed > durationMs &&
         static_cast<uint32_t>(elapsed - durationMs) > graceMs;
}

}  // namespace BBSInternal
