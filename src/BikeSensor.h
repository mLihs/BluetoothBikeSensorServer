#pragma once

#include <cstddef>
#include <cstdint>

#include "internal/BBSConfig.h"
#include "internal/BBSTypes.h"

// BikeSensor 3.0 public API. Call update() and every mutating function from
// one task (normally the Arduino loop()). Getters marked "any task" copy
// under a lock. Never call end() from an event or data callback.
// Before a successful begin() (and after a successful end()), update(),
// addByName(), addByMac(), remove(), startDiscovery(), stopDiscovery() and
// startServer() are ignored and return false where they return a value;
// end() returns true.

namespace BikeSensor {

// Lifecycle.
bool initStack(const char* deviceName);   // optional: NimBLE only, before begin()
bool startServer();                       // begin() starts the server already
bool begin(const char* deviceName, uint8_t typeMask, Mode mode);   // check the result
void update();
bool end();                               // false: not torn down, reboot

// Sensors. MAC "xx:xx:xx:xx:xx:xx"; addrType 0 public, 1 random, -1 auto.
bool addByName(SensorType type, const char* name);
bool addByMac(SensorType type, const char* mac, int8_t addrType, const char* name = nullptr);
bool remove(SensorType type);
bool connected(SensorType type);          // any task
bool info(SensorType type, SensorInfo& out);   // any task

// Discovery. Results are valid from DISCOVERY_DONE on; getters any task.
bool startDiscovery(uint8_t typeMask, uint32_t durationMs = 15000);
void stopDiscovery();
bool discovering();
uint8_t discoveredCount();
bool discovered(uint8_t i, DiscoveredSensor& out);

// Last measurements, any task. ageMs: time since the packet that produced it.
bool lastHeartRate(uint16_t& bpm, uint32_t& ageMs);
bool lastCadence(uint16_t& rpm, uint32_t& ageMs);
bool lastPower(PowerSample& out, uint32_t& ageMs);   // false without BBS_COMPILE_POWER

// pause()/resume(): advertising only. quiesce(): one-way until end() + begin().
void pause();
void resume();
void quiesce();
bool quiesced();

// Relay server.
bool advertising();                       // any task
bool refreshAdvertising();                // applies setNamePrefix(); false = data not applied
uint8_t centralCount();                   // any task
void setNamePrefix(const char* prefix);
void serverName(char* out, size_t len);   // "<prefix>-<chip id>", any task

bool scanning();                          // any task
const char* disconnectReasonString(int reason);
Stats stats();
void setEventCallback(EventCallback cb);
void setHeartRateCallback(void (*cb)(uint16_t bpm));
void setCadenceCallback(void (*cb)(uint16_t rpm));
void setPowerCallback(void (*cb)(const PowerSample& sample));

}  // namespace BikeSensor
