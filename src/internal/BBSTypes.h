#pragma once

#include <cstdint>

// Public value types from design §4, Arduino-free so the codec host tests can
// use them. BikeSensor.h (step 3) includes this header.

namespace BikeSensor {

enum class SensorType : uint8_t { HEART_RATE = 0x01, SPEED_CADENCE = 0x02, POWER = 0x04 };
constexpr uint8_t typeBit(SensorType t) { return static_cast<uint8_t>(t); }

struct PowerSample {
  int16_t  watts;
  uint16_t cadence;        // rpm; 0 = none (!hasCadence) or stale (hasCadence)
  bool     hasCadence;     // true after first valid crank delta; stays true when stale
  uint8_t  balance;        // 1/2 %; 0xFF = not present
  bool     balanceIsLeft;  // flags bit 1
  uint16_t energyKJ;
  bool     hasEnergy;
  uint16_t flags;          // raw CPS flags
};

}  // namespace BikeSensor
