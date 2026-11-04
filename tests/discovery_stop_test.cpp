#include "BBSDiscoveryStop.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <thread>

using BBSInternal::DiscoveryPub;
using BBSInternal::DiscoveryStopState;

static void makeRunning(DiscoveryStopState& state) {
  assert(state.tryBeginStart());
  assert(state.markRunning());
}

static uint32_t requestStop(DiscoveryStopState& state) {
  uint32_t generation = 0;
  assert(state.tryRequestStop(generation));
  return generation;
}

static bool claimScanEnd(DiscoveryStopState& state) {
  bool hostEventPending = false;
  assert(state.tryClaimScanEnd(hostEventPending));
  return hostEventPending;
}

static void testNaturalEnd() {
  {
    DiscoveryStopState state;
    makeRunning(state);
    assert(!claimScanEnd(state));
    assert(state.state() == DiscoveryPub::PUBLISHING);
    assert(state.markReady());
    assert(state.finishPublish());
    assert(state.state() == DiscoveryPub::IDLE);
  }
  {
    DiscoveryStopState state;
    makeRunning(state);
    requestStop(state);
    assert(!claimScanEnd(state));
    assert(state.state() == DiscoveryPub::PUBLISHING);
  }
}

static void testStopQueueAndClaims() {
  DiscoveryStopState state;
  makeRunning(state);
  const uint32_t generation = requestStop(state);
  uint32_t ignoredGeneration = 0;
  assert(!state.tryRequestStop(ignoredGeneration));
  assert(state.tryQueueStopped(generation));
  assert(!state.tryQueueStopped(generation));
  assert(state.tryClaimHost());
  assert(!state.tryClaimHost());
  bool hostEventPending = false;
  assert(!state.tryClaimScanEnd(hostEventPending));
  assert(state.markReady());
  assert(state.isReady());
  assert(state.finishPublish());
}

static void testNaturalEndWinsQueuedFallback() {
  DiscoveryStopState state;
  makeRunning(state);
  const uint32_t generation = requestStop(state);
  assert(state.tryQueueStopped(generation));
  assert(claimScanEnd(state));
  assert(!state.tryClaimHost());
  assert(state.markWaitingForHostDrain());
  assert(state.state() == DiscoveryPub::WAIT_HOST_DRAIN);
  assert(!state.tryBeginStart());
  assert(state.finishHostDrain());
  assert(state.isReady());
  bool hostEventPending = false;
  assert(!state.tryClaimScanEnd(hostEventPending));
}

static void testFailedStopWaitsForNaturalEnd() {
  DiscoveryStopState state;
  makeRunning(state);
  requestStop(state);
  assert(state.state() == DiscoveryPub::STOP_REQUESTED);
  assert(!claimScanEnd(state));
}

static void testOldStopCannotQueueNewGeneration() {
  DiscoveryStopState state;
  makeRunning(state);
  const uint32_t oldGeneration = requestStop(state);
  assert(!claimScanEnd(state));
  assert(state.markReady());
  assert(state.finishPublish());

  assert(state.tryBeginStart());
  assert(state.markRunning());
  const uint32_t newGeneration = requestStop(state);
  assert(oldGeneration != newGeneration);
  assert(!state.tryQueueStopped(oldGeneration));
  assert(state.tryQueueStopped(newGeneration));
}

static void testExpiryQueue() {
  DiscoveryStopState state;
  makeRunning(state);
  assert(state.tryQueueExpired());
  assert(state.tryClaimHost());

  DiscoveryStopState stopped;
  makeRunning(stopped);
  requestStop(stopped);
  assert(!stopped.tryQueueExpired());

  assert(!BBSInternal::runExpired(1000, 1000, 0, 3000));
  assert(!BBSInternal::runExpired(24000, 1000, 20000, 3000));
  assert(BBSInternal::runExpired(24001, 1000, 20000, 3000));
  assert(BBSInternal::runExpired(0x00000020u, 0xfffffff0u, 16u, 16u));
}

static void testStartGuardsAndFailure() {
  DiscoveryStopState idle;
  assert(!idle.markReady());
  assert(!idle.finishPublish());
  assert(idle.tryBeginStart());
  assert(!idle.tryBeginStart());
  uint32_t ignoredGeneration = 0;
  assert(!idle.tryRequestStop(ignoredGeneration));
  assert(idle.markStartFailed());
  assert(idle.isReady());
  assert(!idle.tryBeginStart());
  assert(idle.finishPublish());
  assert(idle.tryBeginStart());

  DiscoveryStopState running;
  makeRunning(running);
  assert(!running.tryBeginStart());
  const uint32_t generation = requestStop(running);
  assert(!running.tryBeginStart());
  assert(running.tryQueueStopped(generation));
  assert(!running.tryBeginStart());
  assert(running.tryClaimHost());
  assert(!running.tryBeginStart());
  assert(running.markReady());
  assert(!running.tryBeginStart());
}

static void testClaimRace() {
  constexpr size_t kIterations = 100000;
  auto states = std::make_unique<DiscoveryStopState[]>(kIterations);
  for (size_t i = 0; i < kIterations; ++i) {
    assert(states[i].tryBeginStart());
    assert(states[i].markRunning());
    const uint32_t generation = requestStop(states[i]);
    assert(states[i].tryQueueStopped(generation));
  }

  std::atomic<bool> start{false};
  std::atomic<size_t> scanWins{0};
  std::atomic<size_t> hostWins{0};
  std::thread scanThread([&] {
    while (!start.load(std::memory_order_acquire)) {
    }
    for (size_t i = 0; i < kIterations; ++i) {
      bool hostEventPending = false;
      if (states[i].tryClaimScanEnd(hostEventPending)) {
        scanWins.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });
  std::thread hostThread([&] {
    while (!start.load(std::memory_order_acquire)) {
    }
    for (size_t i = 0; i < kIterations; ++i) {
      if (states[i].tryClaimHost()) {
        hostWins.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });
  start.store(true, std::memory_order_release);
  scanThread.join();
  hostThread.join();

  assert(scanWins.load() + hostWins.load() == kIterations);
  for (size_t i = 0; i < kIterations; ++i) {
    assert(states[i].state() == DiscoveryPub::PUBLISHING);
  }
}

static void testStopVsNaturalEndRace() {
  constexpr size_t kIterations = 100000;
  auto states = std::make_unique<DiscoveryStopState[]>(kIterations);
  for (size_t i = 0; i < kIterations; ++i) {
    makeRunning(states[i]);
  }

  std::atomic<bool> start{false};
  std::thread stopThread([&] {
    while (!start.load(std::memory_order_acquire)) {
    }
    for (size_t i = 0; i < kIterations; ++i) {
      uint32_t generation = 0;
      if (states[i].tryRequestStop(generation)) {
        states[i].tryQueueStopped(generation);
      }
    }
  });
  std::thread endThread([&] {
    while (!start.load(std::memory_order_acquire)) {
    }
    for (size_t i = 0; i < kIterations; ++i) {
      bool hostEventPending = false;
      assert(states[i].tryClaimScanEnd(hostEventPending));
    }
  });
  start.store(true, std::memory_order_release);
  stopThread.join();
  endThread.join();

  for (size_t i = 0; i < kIterations; ++i) {
    assert(states[i].state() == DiscoveryPub::PUBLISHING);
  }
}

int main() {
  testNaturalEnd();
  testStopQueueAndClaims();
  testNaturalEndWinsQueuedFallback();
  testFailedStopWaitsForNaturalEnd();
  testOldStopCannotQueueNewGeneration();
  testExpiryQueue();
  testStartGuardsAndFailure();
  testClaimRace();
  testStopVsNaturalEndRace();
  return 0;
}
