#pragma once

#include <cstdint>

// Public value types from design §3.1 and §4, Arduino-free so the host tests
// can use them. The public BikeSensor.h will include this header.

namespace BikeSensor {

enum class SensorType : uint8_t { HEART_RATE = 0x01, SPEED_CADENCE = 0x02, POWER = 0x04 };
constexpr uint8_t typeBit(SensorType t) { return static_cast<uint8_t>(t); }

enum class SlotState : uint8_t {
  EMPTY = 0, IDLE = 1, WAIT_SCAN = 2, FOUND = 3, STOPPING_FOR_CONNECT = 4,
  CONNECTING = 5, CANCELLING = 6, DISCOVERING = 7, SUBSCRIBED = 8
};
enum class Mode : uint8_t { NAME = 0, MAC = 1 };

enum class EventType : uint8_t {
  SENSOR_FOUND = 0, SENSOR_CONNECTED = 1, SENSOR_DISCONNECTED = 2, SENSOR_FAILED = 3,
  DATA = 4, DISCOVERY_DONE = 5, CENTRAL_CONNECTED = 6, CENTRAL_DISCONNECTED = 7,
  CENTRAL_SUBSCRIBED = 8, CENTRAL_UNSUBSCRIBED = 9, HOST_RESET = 10
};
enum class FinishReason : uint8_t {
  NATURAL = 0, MANUAL = 1, TIMEOUT = 2, HOST_RESET = 3, RESYNC = 4, START_FAILED = 5, QUIESCED = 6
};
struct EventInfo {
  EventType type;
  SensorType sensorType;
  int reason;                 // disconnect reason, or FinishReason for DISCOVERY_DONE
  uint8_t count;
  uint32_t generation;        // DISCOVERY_DONE
};
typedef void (*EventCallback)(const EventInfo&);

// §3.3: one discovered advertiser. name "" = none seen; typeMask = advertised
// types ∩ the discovery's requested mask ∩ the compiled types.
struct DiscoveredSensor { char name[32]; uint8_t mac[6]; uint8_t addrType; uint8_t typeMask; };
static_assert(sizeof(DiscoveredSensor) == 40, "");

struct SensorInfo {
  char name[32]; uint8_t mac[6]; int8_t addrType;
  SlotState state; uint8_t battery; bool hasBattery; int lastDisconnectReason;
  bool encrypted, bonded;     // Power only (HR/CSC false), end of the GATT pass; false when not SUBSCRIBED
  bool providesCadence;       // POWER only: this slot is the cadence source (§6b)
};

struct Stats {
  uint32_t connects, connectFails, disconnects, notifies, notifiesTruncated, notifiesMalformed;
  uint32_t eventsDropped, notifiesDropped, matchesDropped, scansStarted, scanStartFails;
  uint32_t discoveryTimeouts, gattBudgetAborts, hostResets, cancelSafetyExits, advertisingEnforced;
  uint32_t securityOk, securityFails, bondEvictions, bondStoreRejects;
};

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
