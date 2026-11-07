# BluetoothBikeSensorServer Library

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**Version**: 3.1.0
**Platform**: ESP32 (Arduino Core 3.x)
**Author**: Martin Lihs
**Last Updated**: 2026-11-06

> **3.0 is a new major version.** The 2.x functions (`BluetoothBikeSensorServer*`, `BikeSensorServer*`), `BLETelemetry` and `HomewindBleIdentity` are gone; there is no compatibility layer. See [CHANGELOG.md](CHANGELOG.md) for the full list.

---

## Overview

BluetoothBikeSensorServer connects an ESP32 to BLE fitness sensors (Heart Rate `0x180D`, Speed/Cadence `0x1816`, Cycling Power `0x1818`) and relays their measurements to centrals (Zwift, Garmin, phone apps) through a local GATT server.

- One application task: NimBLE callbacks only queue events; `BikeSensor::update()` runs the state machines and calls your callbacks.
- No `delay()` in the library, non-blocking connects, no heap churn while scanning.
- MAC mode (a 5 s sleep scan every 30 s until the MAC is seen; after a drop one direct attempt first) or NAME mode (10 s scan windows until the name is seen).
- Discovery with name, MAC, address type and advertised types.
- HR, CSC cadence and Power (watts, cadence, pedal balance, energy) as callbacks and getters; cadence from the power meter when no CSC sensor is configured.
- One-way `quiesce()` for OTA and maintenance.

## Requirements

- ESP32 with BLE (built for ESP32 and ESP32-S3, hardware-tested on the ESP32-S3), arduino-esp32 core 3.3.x.
- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) **2.5.0 or newer** (2.5.2 tested).
  Known issue: NimBLE-Arduino 2.3.0–2.5.2 index a `BLE_MAX_CONNECTIONS + 1` array with the controller's connection handle; on the ESP32-S3 a central (relay client) can get a larger handle, which corrupts host memory and crashes on disconnect. Use a NimBLE-Arduino with the fix (per-connection flag, as in upstream esp-nimble) or a patched copy like Homewind's `NimBLE-Arduino` workspace copy (`PATCHES.md`).
- Extended advertising (`CONFIG_BT_NIMBLE_EXT_ADV`) must be off (`#error` otherwise).

Install into your Arduino `libraries` folder and include one header:

```cpp
#include <BikeSensor.h>
```

## Quick start

```cpp
#include <BikeSensor.h>

namespace bs = BikeSensor;

void onHeartRate(uint16_t bpm) { Serial.printf("HR %u\n", bpm); }

void setup() {
  Serial.begin(115200);
  if (!bs::begin("homewind", bs::typeBit(bs::SensorType::HEART_RATE), bs::Mode::MAC)) {
    Serial.println("begin() failed");
    return;
  }
  bs::setHeartRateCallback(onHeartRate);
  bs::addByMac(bs::SensorType::HEART_RATE, "f0:13:c3:ee:56:97", 0, "HR");
}

void loop() {
  bs::update();   // every loop pass
}
```

`begin()` initializes NimBLE (unless it is already up), creates one client per compiled sensor type and the relay server. Advertising starts on the next `update()` with the name `"<prefix>-<chip id>"`, e.g. `homewind-da4eb580`.

## Examples

| Example | Shows |
|---|---|
| `Basic_MAC` | one HR sensor by MAC, lifecycle events, status |
| `Basic_Name` | one HR sensor by name |
| `Discovery` | periodic discovery, prints name / MAC / address type |
| `Telemetry` | HR + CSC by MAC, `stats()`, heap and `update()` timing |
| `Relay_WebServer` | HR + CSC relay plus a WebSocket page (ESPAsyncWebServer, AsyncTCP) |
| `Power` | power meter by name; ships `build_opt.h` with `-DBBS_COMPILE_POWER=1` |
| `NimBLEHeapTest` | heap cost of NimBLE, the library and one connection |

## API

All functions live in `namespace BikeSensor` (`src/BikeSensor.h`).

### Threading

Call `update()` and every mutating function from one task, normally `loop()`. Getters marked *any task* in the header (`info`, `connected`, `discovering`, `discovered*`, `last*`, `scanning`, `advertising`, `centralCount`, `serverName`) copy under a lock and may run from other tasks. `quiesced()` and `stats()` belong to the `update()` task. Never call the library from AsyncTCP or other web callbacks; set a flag and act in `loop()` (see `Relay_WebServer`). Callbacks run inside `update()`; never call `end()` from one.

### Lifecycle

| Function | Notes |
|---|---|
| `bool initStack(const char* deviceName)` | Optional two-phase start: NimBLE only. |
| `bool begin(const char* deviceName, uint8_t typeMask, Mode mode)` | Check the result. Starts the relay server. False while already begun, after an `end()` that returned false, or if the name and service UUIDs do not fit the advertising data. |
| `void update()` | Call on every loop pass. |
| `bool end()` | Quiesces, disconnects every link, deinitializes NimBLE. False means not torn down: reboot. |

Before a successful `begin()` (and after a successful `end()`), `update()`, `addByName()`, `addByMac()`, `remove()`, `startDiscovery()`, `stopDiscovery()` are ignored and return false; `end()` returns true. `pause()`, `resume()` and `quiesce()` are not blocked: `pause()` before `begin()` also stops advertising the application started itself, and `quiesce()` before `begin()` makes `begin()` fail until reboot. `typeMask` is built with `typeBit()`: `HEART_RATE` 0x01, `SPEED_CADENCE` 0x02, `POWER` 0x04. Types that are not compiled are ignored. Changing types or mode needs `end()` + `begin()`.

`deviceName` is the name prefix only when `begin()` (or `initStack()`) initializes NimBLE; if NimBLE is already up, `begin()` ignores it and keeps the current prefix (call `setNamePrefix()` before `begin()` in that case). When `begin()` does initialize NimBLE, its `deviceName` replaces a prefix set earlier with `setNamePrefix()`.

### Sensors

| Function | Notes |
|---|---|
| `bool addByMac(SensorType, const char* mac, int8_t addrType, const char* name = nullptr, bool nameFallback = false)` | MAC mode. `mac` = `xx:xx:xx:xx:xx:xx`; `addrType` 0 public, 1 random, -1 auto. `nameFallback` (3.1): see below. |
| `bool addByName(SensorType, const char* name)` | NAME mode. Exact, case-sensitive match. |
| `bool remove(SensorType)` | From every state; a live link is disconnected first. True once the slot is empty or on its way there. |
| `bool connected(SensorType)` | Subscribed: between `SENSOR_CONNECTED` and `SENSOR_DISCONNECTED`. |
| `bool info(SensorType, SensorInfo&)` | Name, MAC, address type, state, battery, last disconnect reason, `encrypted`, `providesCadence`. False only for a type that is not compiled. |

`addBy*` returns false for a type outside the `begin()` mask, in the other mode, under quiesce, for an empty name, or when **another** type already holds the same MAC (or, in NAME mode, the same name). Identical names for different types are rejected on purpose. Calling `addBy*` again for the same type replaces the identity and reconnects. In NAME mode, avoid a name that equals another sensor's shortened advertised name: the slot that resolves first keeps that address until `remove()` or a replace.

Name fallback (3.1): some sensors with a random address (for example the Garmin Cadence Sensor 2) pick a new one after deep sleep. With `nameFallback = true`, a random address (`addrType` 1, or -1 that detects random) and a non-empty `name`, the waiting slot also accepts a scan report (advertising or scan response) with exactly that name from any address, takes over its address and connects; `info().mac` shows the new address, so the application can store it. A report that lists 16-bit service UUIDs must list the slot's service. A report from the configured MAC still wins, and an address held by another slot is ignored. While such a slot waits, its sleep scans run without the controller's duplicate filter (like NAME windows), so a name sent only in the scan response is seen; every advertising report then reaches the scan callback. Otherwise the flag is ignored and `addByMac()` behaves as in 3.0.

> The fallback trusts the exact name. Use it only for names that identify one device, such as names with a serial number (`CAD-BLE0446949`); with a generic name a nearby sensor of the same model can be taken over.

Battery and `encrypted` are only known while the sensor is connected. One device cannot fill two slots: a power meter that also advertises CSC delivers cadence through the Power slot (below).

### Discovery

| Function | Notes |
|---|---|
| `bool startDiscovery(uint8_t typeMask, uint32_t durationMs = 15000)` | False under quiesce, while a discovery is open, or for a duration of 0 or ≥ `INT32_MAX − BBS_SCAN_END_GRACE_MS`. Waits for a running connect; no connect starts until it ends. |
| `void stopDiscovery()` | Finishes with `FinishReason::MANUAL`; results are kept. |
| `bool discovering()` | True until `DISCOVERY_DONE`. |
| `uint8_t discoveredCount()`, `bool discovered(uint8_t i, DiscoveredSensor&)` | Table of the last finished discovery: named sensors first (sorted by name), unnamed last with an empty name. At most `BBS_MAX_DISCOVERED` (16). |

Discovery also works while the server is paused.

### Measurements

| Function | Notes |
|---|---|
| `setHeartRateCallback(void (*)(uint16_t bpm))` | Every HR packet. |
| `setCadenceCallback(void (*)(uint16_t rpm))` | Only packets that report a cadence. |
| `setPowerCallback(void (*)(const PowerSample&))` | Every valid CPS packet (needs `BBS_COMPILE_POWER=1`). |
| `bool lastHeartRate(uint16_t&, uint32_t& ageMs)`, `bool lastCadence(...)`, `bool lastPower(PowerSample&, uint32_t& ageMs)` | False while the sensor is not connected or has not delivered yet. `ageMs` = age of the packet that produced the value. |

CSC speed is not reported (raw CSC packets are still relayed). `PowerSample`: `watts`, `cadence` (0 when stale: no crank event for `BBS_POWER_STALE_MS`, 3 s), `hasCadence`, `balance` (½ %, 0xFF = none), `balanceIsLeft`, `energyKJ`/`hasEnergy`, raw `flags`.

**Cadence source.** A configured CSC sensor (connected or not) is the cadence source. Without one, a connected power meter is; `info(POWER).providesCadence` then is true and the cadence callback reports 0 once after the meter stops sending crank data. A CSC sensor that is configured but left at home means no cadence until it is removed. Nothing is synthesized onto the local CSC service: pair the app's cadence to the relayed power service, which carries the meter's crank data.

### Events

`setEventCallback(void (*)(const EventInfo&))` reports:

| Event | Fields |
|---|---|
| `SENSOR_FOUND` | scan found the configured sensor |
| `SENSOR_CONNECTED` / `SENSOR_DISCONNECTED` | subscribed / link ended; `reason` = NimBLE disconnect reason (0 if the library found the link gone) |
| `SENSOR_FAILED` | attempt failed, backoff 1 s → 30 s; `reason` 0, or the NimBLE host return code `6`, `0x207` or `0x209` of a capacity wait (below) — not a disconnect reason |
| `DATA` | a parsed packet from a sensor |
| `DISCOVERY_DONE` | `count`, `generation`, `reason` = `FinishReason` |
| `CENTRAL_CONNECTED` / `CENTRAL_DISCONNECTED` | `count` = centrals now; `reason` on disconnect |
| `CENTRAL_SUBSCRIBED` / `CENTRAL_UNSUBSCRIBED` | `sensorType` of the local characteristic |
| `HOST_RESET` | NimBLE host reset; slots recover on their own |

`disconnectReasonString(int)` names NimBLE's HCI reasons (`0x200` + HCI code, e.g. `0x213` "Remote User Terminated"); other non-zero codes give "Unknown Reason". Use it only for `SENSOR_DISCONNECTED`/`CENTRAL_DISCONNECTED` reasons and `info().lastDisconnectReason`.

### Relay server and advertising

| Function | Notes |
|---|---|
| `bool advertising()` | NimBLE advertising is active. |
| `void pause()` / `void resume()` | Advertising only; scans, connects and discovery go on. |
| `void setNamePrefix(const char*)` + `bool refreshAdvertising()` | New prefix (lowercase, `[a-z0-9-]`), applied by the refresh. |
| `void serverName(char* out, size_t len)` | `"<prefix>-<chip id>"`. |
| `uint8_t centralCount()` | Connected centrals. |

The server has one service per type in the `begin()` mask: HR `0x2A37`; CSC `0x2A5B` + Feature `0x2A5C`; CPS `0x2A63` + Feature `0x2A65` + Sensor Location `0x2A5D`. Advertising carries the flags, the name and the service UUIDs in 31 bytes, so the prefix may be at most 13 bytes with one type, 11 with two, 9 with three (`"homewind"` fits). If it does not fit, `refreshAdvertising()` returns false and the old name stays on air, while `serverName()` already returns the new one.

- CSC Feature and CP Feature/Location take the sensor's values after it connects, without the bits that need a control point; CSC keeps bit 0 (wheel data) because raw packets are relayed although there is no SC Control Point. A central that read Feature before the sensor connected keeps the old value until it reads again.
- The library does not pair, bond or manage bonds. NimBLE pairs on demand (Just Works, no bonding) when a sensor requires encryption; such a sensor pairs again on every connect.

### Pause, quiesce, maintenance

`quiesce()` stops advertising, discovery, scans and connects and is one-way until `end()` + `begin()` (or reboot). `update()` must keep running; `quiesced()` turns true once nothing is in flight. Connected sensors may remain; remove them afterwards if needed.

### Stats

`stats()` returns counters: connects, connect fails, disconnects, notifies (truncated, malformed), dropped events/notifies/matches, scans started/failed, discovery timeouts, GATT budget aborts, host resets, cancel safety exits, advertising enforced.

## Connection limits

NimBLE's default `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` is 3, shared by sensors and centrals:

- Two sensors leave one central. A further connect fails.
- Three sensors (HR + CSC + Power) leave no central. On a classic ESP32 `-DCONFIG_BT_NIMBLE_MAX_CONNECTIONS=4` helps (more RAM). On the ESP32-S3 the core's prebuilt `sdkconfig.h` sets it to 3 after the `-D` flags, so the flag has no effect there (see below).

### ESP32-S3: more than three links

The prebuilt ESP32-S3 core gives 3 host links (controller `ble_max_act` 5). HR + CSC + one central works. For more links, put this `sdkconfig.h` into the sketch folder:

```cpp
#pragma once
#include_next <sdkconfig.h>
#undef CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 8
```

- Hardware-tested on the ESP32-S3 with 8 (controller `ble_max_act` 10) and with 6 (`ble_max_act` 8). With 4 links connected the free heap was about 233 KB.
- arduino-cli puts the sketch folder first in the include path (core 3.3.12 verbose build log: `-I<sketch folder>` before the core's `include` folders, for sketch, library and core sources). The value applies to the whole build of the sketch, not only to this library.
- Every configured link costs NimBLE host and controller memory. Use the smallest value you need.

A sensor connect refused for lack of controller or host resources (`6`, `0x207`, `0x209`) is not counted as the sensor's fault: the slot waits without backoff or scanning until a sensor link or central goes away, at the latest after `BBS_CAPACITY_RETRY_MS` (60 s). `SENSOR_FAILED` then carries the code in `reason`, once per code. A `0x207` while advertising runs is retried once with advertising stopped (at most about 7 s).

## Compile-time options

Set with `-D` (e.g. a sketch `build_opt.h` on core 3.x, which also reaches library sources):

| Macro | Default |
|---|---|
| `BBS_COMPILE_HEART_RATE`, `BBS_COMPILE_CSC`, `BBS_COMPILE_POWER` | 1, 1, 0 |
| `BBS_MAX_DISCOVERED` | 16 |
| `BBS_CONNECT_TIMEOUT_MS` | 5000 |
| `BBS_GATT_BUDGET_MS` | 8000 (worst-case `update()` stall after a connect) |
| `BBS_SLEEP_SCAN_DURATION_MS` / `BBS_SLEEP_SCAN_INTERVAL_MS` | 5000 / 30000 |
| `BBS_NAME_SCAN_WINDOW_MS` | 10000 |
| `BBS_POWER_STALE_MS` | 3000 |
| `BBS_SCAN_END_GRACE_MS` | 3000 |
| `BBS_CAPACITY_RETRY_MS` | 60000 |
| `BBS_EVENT_QUEUE_DEPTH`, `BBS_NOTIFY_MAX_LEN` | 24, 36 |

## License

MIT License – see [LICENSE](LICENSE).
