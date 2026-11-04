# BluetoothBikeSensorServer Library

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**Version**: 2.2.21.0
**Platform**: ESP32 (Arduino Core)  
**Author**: Martin Lihs  
**Last Updated**: 2026-10-31

> **Note**: This library uses a modern per-sensor-type API. All sensor operations require specifying the sensor type (`SENSOR_TYPE_HEART_RATE`, `SENSOR_TYPE_SPEED_CADENCE`, or `SENSOR_TYPE_POWER_METER`). Legacy APIs have been removed for a cleaner, more consistent interface.

---

## Overview

**BluetoothBikeSensorServer** is an ESP32 Arduino library that acts as a Bluetooth Low Energy (BLE) relay for fitness sensors. It connects to BLE sensors (Heart Rate, Speed/Cadence, Power Meter) and relays their data to connected central devices (fitness apps, trainers, etc.) via a local GATT server.

The library provides a simple, name-based connection workflow using **NimBLE-Arduino 2.x**, making it easy to connect to sensors without managing MAC addresses manually.

---

## What is BluetoothBikeSensorServer?

BluetoothBikeSensorServer serves as a **BLE sensor relay** that:

1. **Connects to BLE sensors** (Heart Rate monitors, Speed/Cadence sensors, Power meters)
2. **Relays sensor data** to connected central devices (Zwift, TrainerRoad, Garmin, etc.)
3. **Provides discovery functionality** to find and list available sensors
4. **Manages sensor connections** automatically with name-based matching
5. **Supports multiple sensor types** simultaneously (HR + CSC + Power)

### Use Cases

- **Fitness App Integration**: Relay sensor data to Zwift, TrainerRoad, or other fitness apps
- **Multi-Device Support**: Connect multiple sensors and relay to multiple central devices
- **Sensor Discovery**: Find and list available BLE sensors in range
- **Smart Trainer Integration**: Provide sensor data to smart trainers and training platforms

---

## Key Features

### ✅ Implemented Features

1. **Multi-Sensor Support**
   - Heart Rate (HR) sensors (BLE Service 0x180D)
   - Speed/Cadence (CSC) sensors (BLE Service 0x1816)
   - Power Meter sensors (BLE Service 0x1818)
   - Support for all three sensor types simultaneously

2. **Connection Modes**
   - **Scan-Based Mode** (default): Connect to sensors by device name (no MAC address required)
   - **MAC Sleep-Scan Mode** (v2.2.13.0+): Connect by MAC address using periodic short scans
   - Automatic scanning and connection when sensor is in range (scan mode)
   - Robust reconnection handling with permanent MAC preservation in MAC mode (v2.2.8.0+)

3. **Sensor Discovery**
   - Async discovery mode to find available sensors
   - Filter by sensor type (HR, CSC, Power)
   - List discovered sensors with name, MAC, and type
   - Configurable discovery duration and max sensors

4. **BLE GATT Server**
   - Creates virtual sensor server that central devices can connect to
   - Mirrors sensor data to connected centrals in real-time
   - Supports multiple simultaneous central connections
   - Dynamic service advertising based on connected sensors

5. **Device Identity**
   - MAC-based device naming: `{prefix}-xxxxxxxx` (e.g., "homewind-da4eb580")
   - Configurable device name prefix
   - Homewind BLE Identity integration for consistent naming

6. **Connection Management**
   - Automatic sensor reconnection on disconnect
   - Sensor MAC preservation option for faster reconnection (scan mode)
   - Permanent MAC preservation in MAC Sleep-Scan mode (v2.2.8.0+) - sensors reconnect even after long offline periods
   - Per-sensor-type connection status tracking
   - Battery level monitoring (when available)

7. **Runtime Configuration**
   - Enable/disable sensor types at runtime
   - Add/remove sensors dynamically
   - Pause/resume advertising
   - Refresh advertising identity (for Zwift cache bypass)

8. **Callbacks & Events**
   - Data update callbacks (HR, Speed/Cadence, Power)
   - Sensor connection/disconnection callbacks (per type)
   - Central device connection/disconnection callbacks
   - Discovery complete callback

---

## Hardware Requirements

### Minimum Requirements
- **Microcontroller**: ESP32 (any variant: ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6, ESP32-H2)
- **BLE**: Built-in Bluetooth Low Energy support
- **RAM**: ~50KB free heap (typical)
- **Flash**: Minimal (library code only)

### Supported Sensors
- **Heart Rate Monitors**: Any BLE HR sensor (e.g., Wahoo TICKR, Polar H10)
- **Speed/Cadence Sensors**: Any BLE CSC sensor (e.g., Wahoo RPM, Garmin Speed/Cadence)
- **Power Meters**: Any BLE Power meter (e.g., Stages, 4iiii, Garmin)

---

## Software Dependencies

### Required
- **ESP32 Arduino Core** - Platform support
- **NimBLE-Arduino** - Version **2.x** (REQUIRED, NOT compatible with 1.x)

### Optional
- **DebugLog** - For debug logging (optional, can use Serial directly)

---

## Installation

### Arduino IDE

1. Download or clone this library into your Arduino `libraries` folder:
   ```bash
   cd ~/Documents/Arduino/libraries
   git clone <repository-url> BluetoothBikeSensorServer
   ```

2. Install the required dependency:
   - Open Arduino IDE
   - Go to **Tools → Manage Libraries**
   - Search for **NimBLE-Arduino** and install version **2.x** (NOT 1.x)

3. Restart Arduino IDE

### PlatformIO

Add to your `platformio.ini`:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
lib_deps = 
    h2zero/NimBLE-Arduino@^2.0.0
    file://../libraries/BluetoothBikeSensorServer
```

### Manual Installation

1. Copy the `BluetoothBikeSensorServer` folder to your project's `libraries` directory
2. Ensure `NimBLE-Arduino` version 2.x is installed (required dependency)
3. Include the header in your sketch:
   ```cpp
   #include <BluetoothBikeSensorServer.h>
   ```

---

## Quick Start

### Minimal Example (Heart Rate Only)

```cpp
#include <BluetoothBikeSensorServer.h>

const char* HR_SENSOR_NAME = "TICKR FIT";  // Replace with your sensor name

void onHeartRateChanged(uint16_t hr) {
  Serial.print("Heart rate: ");
  Serial.println(hr);
}

void setup() {
  Serial.begin(115200);
  
  // Initialize library (default device name prefix: "homewind")
  BluetoothBikeSensorServerBegin();
  
  // Set data callback
  BluetoothBikeSensorServerSetHeartRateCallback(onHeartRateChanged);
  
  // Add sensor by name
  BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, HR_SENSOR_NAME);
}

void loop() {
  // Must call this regularly
  BluetoothBikeSensorServerLoop();
  
  delay(100);
}
```

### Multi-Sensor Example

```cpp
#include <BluetoothBikeSensorServer.h>

void onHeartRateChanged(uint16_t hr) {
  Serial.print("HR: ");
  Serial.println(hr);
}

void onSpeedCadenceChanged(uint16_t speed, uint16_t cadence) {
  Serial.print("Speed: ");
  Serial.print(speed / 100.0 * 3.6);  // Convert to km/h
  Serial.print(" km/h, Cadence: ");
  Serial.print(cadence);
  Serial.println(" rpm");
}

void onPowerChanged(int16_t power) {
  Serial.print("Power: ");
  Serial.println(power);
}

void setup() {
  Serial.begin(115200);
  BluetoothBikeSensorServerBegin();
  
  // Set data callbacks
  BluetoothBikeSensorServerSetHeartRateCallback(onHeartRateChanged);
  BluetoothBikeSensorServerSetSpeedCadenceCallback(onSpeedCadenceChanged);
  BluetoothBikeSensorServerSetPowerCallback(onPowerChanged);
  
  // Add sensors by name
  BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, "TICKR FIT");
  BikeSensorServerAddSensor(SENSOR_TYPE_SPEED_CADENCE, "Ridesense");
  BikeSensorServerAddSensor(SENSOR_TYPE_POWER_METER, "Power Meter");
}

void loop() {
  BluetoothBikeSensorServerLoop();
  delay(100);
}
```

### MAC Sleep-Scan Mode Example (v2.2.13.0+)

Connect by MAC address using periodic short scans (Sleep-Scan). Ideal for fixed sensor setups with known MAC addresses.

**Important (v2.2.10.0+):** Run Discovery first to find the correct BLE address type (0 or 1) for your sensor!

```cpp
#include <BluetoothBikeSensorServer.h>

const char* HR_SENSOR_MAC = "F0:13:C3:EE:56:97";  // Replace with your sensor MAC
const int8_t HR_ADDR_TYPE = 0;  // Get from Discovery output: [HR] Name (MAC) 0

void onHeartRateChanged(uint16_t hr) {
  Serial.print("Heart rate: ");
  Serial.println(hr);
}

void setup() {
  Serial.begin(115200);
  
  // Initialize in MAC Sleep-Scan mode (periodic scan)
  BluetoothBikeSensorServerBegin("mydevice", SENSOR_TYPE_HEART_RATE, BBS_MODE_MAC);
  
  // Set data callback
  BluetoothBikeSensorServerSetHeartRateCallback(onHeartRateChanged);
  
  // Add sensor by MAC address with explicit address type (RECOMMENDED)
  BikeSensorServerAddSensorByMac(SENSOR_TYPE_HEART_RATE, nullptr, HR_SENSOR_MAC, HR_ADDR_TYPE);
}

void loop() {
  BluetoothBikeSensorServerLoop();
  delay(100);
}
```

**How to find the address type:**
1. Run Discovery (e.g., press 'd' in Serial Monitor)
2. Check the output: `[HR] TICKR FIT (f0:13:c3:ee:56:97) 0` → use `0`
3. Use that value (0 or 1) as the `addrType` parameter

**Advantages of MAC Sleep-Scan Mode:**
- ✅ Periodic short scans reduce power and avoid long connect timeouts
- ✅ MAC address permanently preserved (v2.2.8.0+) - sensors reconnect even after long offline periods
- ✅ Connect only after MAC was seen (more reliable)
- ✅ Ideal for fixed sensor setups

**Note**: In MAC Sleep-Scan mode, the MAC address is permanently preserved. Sensors will reconnect even after long offline periods (e.g., overnight).

---

## API Reference

### Initialization

#### `void BluetoothBikeSensorServerBegin(const char *deviceNamePrefix = nullptr, uint8_t enabledSensorTypes = SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_SPEED_CADENCE | SENSOR_TYPE_POWER_METER, BBSConnectionMode connectionMode = BBS_MODE_SCAN, esp_power_level_t txPower = ESP_PWR_LVL_P9)`

Initializes the library. The device name parameter is used as a prefix for MAC-based identity. The actual BLE device name will be `"{prefix}-xxxxxxxx"` where `xxxxxxxx` is derived from the chip eFuse MAC (lower 32-bit, WiFi-style suffix).

**Parameters:**
- `deviceNamePrefix`: Optional device name prefix (default: "homewind"). Will be converted to lowercase.
- `enabledSensorTypes`: Bitmask of sensor types to enable (default: all)
- `connectionMode`: Connection mode - `BBS_MODE_SCAN` (default) for name-based scanning, or `BBS_MODE_MAC` for MAC-based Sleep-Scan connections (v2.2.13.0+)
- `txPower`: BLE transmit power level (default: `ESP_PWR_LVL_P9`)

**Connection Modes:**
- `BBS_MODE_SCAN` (default): Scan-based mode - use with `BikeSensorServerAddSensor()`
- `BBS_MODE_MAC`: MAC Sleep-Scan mode - use with `BikeSensorServerAddSensorByMac()` (v2.2.13.0+)

**Example:**
```cpp
BluetoothBikeSensorServerBegin();  // Uses default "homewind" prefix, scan mode
BluetoothBikeSensorServerBegin("mydevice");  // Custom prefix: "mydevice-xxxxxxxx", scan mode
BluetoothBikeSensorServerBegin("mydevice", SENSOR_TYPE_HEART_RATE, BBS_MODE_MAC);  // MAC Sleep-Scan mode
```

#### `void BluetoothBikeSensorServerLoop()`

Must be called regularly in your `loop()` function to handle BLE operations.

---

### Sensor Management

#### `bool BikeSensorServerAddSensor(SensorType type, const char* sensorName)`

Add a sensor to connect to by device name. The library will scan for the device and connect automatically when found.

**Parameters:**
- `type`: Sensor type (`SENSOR_TYPE_HEART_RATE`, `SENSOR_TYPE_SPEED_CADENCE`, or `SENSOR_TYPE_POWER_METER`)
- `sensorName`: Device name to search for (exact match required)

**Returns:** `true` if sensor was added successfully

**Example:**
```cpp
BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, "TICKR FIT");
```

#### `bool BikeSensorServerAddSensorByMac(SensorType type, const char* sensorName, const char* macAddress, int8_t addrType)` (v2.2.10.0+)
#### `bool BikeSensorServerAddSensorByMac(SensorType type, const char* sensorName, const char* macAddress)`
#### `bool BikeSensorServerAddSensorByMac(SensorType type, const char* macAddress)`

Add a sensor to connect by MAC address (MAC Sleep-Scan mode). Requires `BBS_MODE_MAC` connection mode.

**Parameters:**
- `type`: Sensor type (`SENSOR_TYPE_HEART_RATE`, `SENSOR_TYPE_SPEED_CADENCE`, or `SENSOR_TYPE_POWER_METER`)
- `sensorName`: Optional device name (can be `nullptr` or empty, used for logging only)
- `macAddress`: MAC address in format "aa:bb:cc:dd:ee:ff" (case-insensitive)
- `addrType`: BLE address type (v2.2.10.0+):
  - `0` = Public address
  - `1` = Random address
  - `-1` = Auto-detect (not recommended - may not work for all devices)
  - **Recommended**: Run Discovery to find the correct type! Discovery output shows: `[HR] Name (MAC) 0` → use `0`

**Returns:** `true` if sensor was added successfully

**Note:** In MAC Sleep-Scan mode, the MAC address is permanently preserved (v2.2.8.0+), even after disconnections and connection failures. The system will continue attempting to reconnect indefinitely.

**Example:**
```cpp
// Initialize in MAC Sleep-Scan mode
BluetoothBikeSensorServerBegin("mydevice", SENSOR_TYPE_HEART_RATE, BBS_MODE_MAC);

// Method 1: With explicit address type (RECOMMENDED - v2.2.10.0+)
// Discovery shows: [HR] TICKR FIT (f0:13:c3:ee:56:97) 0
BikeSensorServerAddSensorByMac(SENSOR_TYPE_HEART_RATE, nullptr, "F0:13:C3:EE:56:97", 0);

// Method 2: Auto-detect (may not work for all devices)
BikeSensorServerAddSensorByMac(SENSOR_TYPE_HEART_RATE, "F0:13:C3:EE:56:97");
```

#### `bool BikeSensorServerRemoveSensor(SensorType type)`

Remove a sensor and disconnect.

**Returns:** `true` if sensor was removed

#### `bool BikeSensorServerIsSensorConnected(SensorType type)`

Check if a specific sensor type is currently connected.

**Returns:** `true` if connected

#### `bool BikeSensorServerGetSensorMac(SensorType type, char* mac, size_t len)`

Get the MAC address of the active sensor.

**Returns:** `true` if sensor is active and MAC was copied

#### `bool BikeSensorServerGetSensorName(SensorType type, char* name, size_t len)`

Get the name of the active sensor.

**Returns:** `true` if sensor is active and name was copied

#### `bool BikeSensorServerGetBatteryLevel(SensorType type, uint8_t* batteryLevel)`

Get the battery level of the active sensor (0-100, 255 = unknown).

**Returns:** `true` if sensor is active and battery level is available

---

### Data Access

#### Heart Rate

```cpp
uint16_t BluetoothBikeSensorServerGetLastHeartRate();
bool BluetoothBikeSensorServerHasHeartRate();
uint32_t BluetoothBikeSensorServerGetLastHeartRateTimestamp();
```

#### Speed/Cadence

```cpp
bool BluetoothBikeSensorServerGetLastSpeedCadence(uint16_t* speed, uint16_t* cadence);
bool BluetoothBikeSensorServerHasSpeedCadence();
uint32_t BluetoothBikeSensorServerGetLastSpeedCadenceTimestamp();
```

**Note:** Speed is in units of 1/256 m/s. To convert to km/h: `speed / 100.0 * 3.6`

#### Power Meter

```cpp
int16_t BluetoothBikeSensorServerGetLastPower();
bool BluetoothBikeSensorServerHasPower();
uint32_t BluetoothBikeSensorServerGetLastPowerTimestamp();
```

---

### Callbacks

#### Data Callbacks

```cpp
void BluetoothBikeSensorServerSetHeartRateCallback(void (*callback)(uint16_t heartRate));
void BluetoothBikeSensorServerSetSpeedCadenceCallback(void (*callback)(uint16_t speed, uint16_t cadence));
void BluetoothBikeSensorServerSetPowerCallback(void (*callback)(int16_t power));
```

#### Sensor Connection Callbacks (Per Type)

```cpp
void BikeSensorServerSetSensorConn(SensorType type, void (*callback)());
void BikeSensorServerSetSensorDisc(SensorType type, void (*callback)());
```

**Example:**
```cpp
void onHRConnected() {
  Serial.println("HR sensor connected!");
}

BikeSensorServerSetSensorConn(SENSOR_TYPE_HEART_RATE, onHRConnected);
```

#### Central Device Callbacks

```cpp
void BikeSensorServerSetClientConn(void (*callback)(uint8_t centralCount));
void BikeSensorServerSetClientDisc(void (*callback)(uint8_t centralCount));
```

---

### Discovery Functions

#### `void BikeSensorServerStartDiscovery(uint8_t sensorTypeMask)`

Start discovery mode to search for Bluetooth sensors. Automatically deletes all previously found sensors before starting.

**Parameters:**
- `sensorTypeMask`: Bitmask of sensor types to search for:
  - `SENSOR_TYPE_HEART_RATE` (0x01)
  - `SENSOR_TYPE_SPEED_CADENCE` (0x02)
  - `SENSOR_TYPE_POWER_METER` (0x04)
  - Can combine: `SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_POWER_METER`

**Example:**
```cpp
BikeSensorServerStartDiscovery(SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_POWER_METER);
```

#### `void BikeSensorServerStopDiscovery()`

Manually stop discovery mode before timeout.

#### `uint8_t BikeSensorServerGetSensorCount()`

Get total count of discovered sensors.

#### `bool BikeSensorServerGetSensor(uint8_t index, SensorType* type, char* name, size_t nameLen, uint8_t mac[6])`

Get sensor information by index. MAC is returned as raw 6-byte array.

#### `const char* BikeSensorServerListSensors(SensorListFormat format)`

Get formatted list of all discovered sensors.

**Format options:**
- `LIST_FORMAT_NAME` - Names only
- `LIST_FORMAT_MAC` - MAC addresses only
- `LIST_FORMAT_FULL` - Name and MAC

#### `void BikeSensorServerSetDiscoveryCompleteCallback(void (*callback)(uint8_t count))`

Set callback function called when discovery completes.

#### `void BikeSensorServerSetDiscoveryDuration(uint32_t durationMs)`

Set discovery duration in milliseconds (default: 20000 = 20 seconds).

#### `void BikeSensorServerSetMaxSensors(uint8_t maxSensors)`

Set maximum number of sensors to discover (default: 12).

---

### Runtime Configuration

### Compile-Time Feature Flags

Feature flags are defined in `src/config/BBSConfig.h` and control optional subsystems.

**Current defaults:**
```cpp
BBS_ENABLE_TELEMETRY   0
BBS_ENABLE_NAME_DISCOVERY 1
BBS_ENABLE_VERBOSE_LOG 0
BBS_ENABLE_GETTERS     0
BBS_ENABLE_CENTRAL_CB  1
```

**Sleep-Scan (MAC Mode) Configuration:**
```cpp
// How long each scan runs (ms)
#define BBS_SLEEP_SCAN_DURATION_MS 2000

// Interval between scans (ms)
#define BBS_SLEEP_SCAN_INTERVAL_MS 30000
```

#### `void BikeSensorServerSetEnabledSensorTypes(uint8_t newEnabledSensorTypes)`

Dynamically change the enabled sensor types at runtime. This will:
- Update the enabled sensor types
- Create/remove services as needed
- Update advertising to reflect new services
- Disconnect all connected central devices (forces them to rediscover)

**Example:**
```cpp
// Enable only HR and Power
BikeSensorServerSetEnabledSensorTypes(SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_POWER_METER);
```

#### `void BikeSensorServerSetPreserveMacOnDisconnect(bool preserve)`

Enable/disable preserving sensor MAC address on disconnect for faster reconnection.

**Parameters:**
- `preserve`: `true` for faster reconnection (preserves MAC), `false` for standard reconnection (rescans)

**Example:**
```cpp
BikeSensorServerSetPreserveMacOnDisconnect(true);  // Enable fast reconnection
```

#### `void BikeSensorServerTriggerReconnect()` (v2.2.13.0+)

Manually trigger an immediate reconnection attempt for disconnected sensors.

**Behavior:**
- **MAC Sleep-Scan mode**: Resets scan timer + starts immediate scan
- **Scan mode**: Starts scanning if not already running

**Example:**
```cpp
// Reconnect button handler
void onReconnectButtonPressed() {
  BikeSensorServerTriggerReconnect();
}
```

#### `void BikeSensorServerSetDeviceNamePrefix(const char* prefix)`

Set the device name prefix for MAC-based identity. Format: `"{prefix}-xxxxxxxx"`.

**Example:**
```cpp
BikeSensorServerSetDeviceNamePrefix("mydevice");  // Device name: "mydevice-da4eb580"
```

#### `void BikeSensorServerPause()`

Pause the BLE Relay Server (stops advertising). Useful when opening sensor configuration UI.

#### `void BikeSensorServerResume()`

Resume the BLE Relay Server (restarts advertising).

#### `bool BikeSensorServerIsAdvertising()`

Check if the BLE Relay Server is currently advertising.

---

### Central Device Status

```cpp
bool BluetoothBikeSensorServerHasCentral();
uint8_t BluetoothBikeSensorServerGetCentralCount();
void BluetoothBikeSensorServerCheckCentralConnections();
```

---

## Examples

The library includes several example sketches demonstrating different use cases:

### 1. HR_Only_Example
**Location:** `examples/HR_Only_Example/HR_Only_Example.ino`

Minimal example connecting to a single Heart Rate sensor.

**Key Features:**
- Single sensor connection
- Basic data callback
- Connection status callbacks

### 2. HR_CSC_Example
**Location:** `examples/HR_CSC_Example/HR_CSC_Example.ino`

Example connecting to both Heart Rate and Speed/Cadence sensors.

**Key Features:**
- Multi-sensor support
- Per-sensor-type callbacks
- Data access methods

### 3. DiscoveryExample
**Location:** `examples/DiscoveryExample/DiscoveryExample.ino`

Demonstrates the discovery feature to find and list available sensors.

**Key Features:**
- Async discovery mode
- Sensor listing and filtering
- Discovery callbacks

### 4. FastReconnectionExample
**Location:** `examples/FastReconnectionExample/FastReconnectionExample.ino`

Shows how to use Sensor MAC Preservation for faster reconnection.

**Key Features:**
- Fast reconnection configuration
- MAC address preservation
- Reconnection time comparison

### 5. WebSocketStreamingExample
**Location:** `examples/WebSocketStreamingExample/WebSocketStreamingExample.ino`

Integration example with WebSocket for real-time data streaming.

**Key Features:**
- WebSocket integration
- Real-time data streaming
- Web UI integration

### 6. WebUI_Integration
**Location:** `examples/WebUI_Integration/WebUI_Integration.ino`

Full WebUI integration example with sensor management.

**Key Features:**
- Web-based sensor configuration
- Real-time monitoring
- Discovery UI integration

### 7. IdentityExample
**Location:** `examples/IdentityExample/IdentityExample.ino`

Demonstrates device identity and advertising configuration.

**Key Features:**
- Custom device name prefix
- Advertising identity management
- MAC-based naming

### 8. ManualSensorExample
**Location:** `examples/ManualSensorExample/ManualSensorExample.ino`

Advanced example with manual sensor management.

**Key Features:**
- Dynamic sensor addition/removal
- Runtime configuration
- Advanced callback usage

---

## Architecture & Design

### Connection Workflow

1. **Sensor Addition**: Call `BikeSensorServerAddSensor()` with sensor name
2. **Discovery**: Library scans for device matching the name
3. **Connection**: When found, automatically connects to sensor
4. **Data Relay**: Sensor data is relayed to connected central devices
5. **Reconnection**: On disconnect, automatically attempts reconnection

### Sensor Types

The library supports three sensor types, each with its own BLE service:

- **Heart Rate (HR)**: Service UUID `0x180D`, Characteristic `0x2A37`
- **Speed/Cadence (CSC)**: Service UUID `0x1816`, Characteristic `0x2A5B`
- **Power Meter (PWR)**: Service UUID `0x1818`, Characteristic `0x2A63`

### GATT Server

The library creates a virtual GATT server that:
- Advertises services based on connected sensors
- Mirrors sensor data to connected central devices
- Supports multiple simultaneous central connections
- Updates advertising dynamically as sensors connect/disconnect

### Device Identity

Device naming uses MAC-based identity:
- Format: `{prefix}-xxxxxxxx` (e.g., "homewind-da4eb580")
- Prefix is configurable (default: "homewind")
- Suffix (`xxxxxxxx`) derived from lower 32 bits of eFuse MAC (WiFi-style suffix)
- Ensures stable, unique device names

---

## Technical Details

### NimBLE-Arduino 2.x Requirement

**IMPORTANT**: This library requires **NimBLE-Arduino version 2.x**. It is **NOT compatible with version 1.x**.

The library uses NimBLE-Arduino 2.x APIs for:
- Name-based sensor connection
- Async discovery
- GATT server management
- Advertising configuration

### Name-Based Connection

The library uses a name-based connection workflow:
1. Sensor is added by device name (e.g., "TICKR FIT")
2. Library scans for devices matching the name
3. When found, connects using the discovered MAC address
4. MAC address is stored for future connections

### Sensor MAC Preservation

When enabled (`BikeSensorServerSetPreserveMacOnDisconnect(true)`):
- Sensor MAC address is preserved on disconnect
- Direct reconnection attempt without scanning
- Faster reconnection (~1 second vs ~5-20 seconds)
- Ideal for sensors with stable MAC addresses

When disabled (default):
- Sensor MAC is cleared on disconnect
- Library rescans to find device by name
- More conservative, ensures fresh device information

### Discovery Mode

Discovery mode:
- Scans for sensors matching specified types
- Stores discovered sensors in internal list
- Can be filtered by sensor type
- Configurable duration and max sensors
- Non-blocking (async)

### Advertising Management

Advertising is automatically managed:
- Starts when sensors are connected
- Stops when all sensors disconnect
- Updates when sensor types change
- Can be paused/resumed manually
- Supports identity refresh for cache bypass

---

## Troubleshooting

### Sensor Not Connecting

**Problem:** Sensor added but never connects

**Solutions:**
- Verify sensor name matches exactly (case-sensitive)
- Ensure sensor is powered on and in range
- Check Serial output for connection attempts
- Try discovery mode to verify sensor is visible
- Check if sensor is already connected to another device

### Discovery Not Finding Sensors

**Problem:** Discovery completes but finds no sensors

**Solutions:**
- Verify sensors are powered on and in range
- Check sensor type mask matches sensor types
- Increase discovery duration: `BikeSensorServerSetDiscoveryDuration(30000)`
- Check Serial output for scan results
- Ensure sensors are advertising (not connected to another device)

### Central Device Not Connecting

**Problem:** Sensor data available but central device doesn't connect

**Solutions:**
- Verify advertising is active: `BikeSensorServerIsAdvertising()`
- Check device name is visible in central's scan
- Ensure correct services are advertised (match connected sensors)
- Try refreshing advertising: `BikeSensorServerRestartAdvertising()`
- For Zwift: Try identity refresh to bypass cache

### Reconnection Too Slow

**Problem:** Sensor reconnection takes too long

**Solutions:**
- Enable Sensor MAC Preservation: `BikeSensorServerSetPreserveMacOnDisconnect(true)`
- Ensure sensor has stable MAC address (not privacy/random)
- Check sensor is in range and powered on

### Memory Issues

**Problem:** Library causes memory problems

**Solutions:**
- Reduce number of sensors
- Disable unused sensor types
- Check available heap: `ESP.getFreeHeap()`
- Ensure sufficient free heap (~50KB minimum)

---

## Best Practices

1. **Sensor Names**: Use exact sensor names (case-sensitive). Use discovery mode to find exact names.

2. **Loop Function**: Always call `BluetoothBikeSensorServerLoop()` regularly in your `loop()` function.

3. **Callbacks**: Use callbacks for real-time data updates rather than polling.

4. **Discovery**: Use discovery mode to find sensor names before adding them.

5. **Reconnection**: Enable Sensor MAC Preservation for faster reconnection if sensors have stable MAC addresses.

6. **Advertising**: Pause advertising when opening sensor configuration UI to prevent interference.

7. **Error Handling**: Check return values from sensor management functions.

8. **Debug Logging**: Enable debug logging (`#define BLE_LIB_DEBUG_GEN`) for troubleshooting.

---

## Limitations

- **NimBLE-Arduino 2.x Only**: Not compatible with version 1.x
- **Name-Based Connection**: Requires exact sensor name match
- **Single Sensor Per Type**: Only one sensor of each type can be connected at a time
- **BLE Range**: Limited by BLE range (~10 meters typical)
- **Power Consumption**: Continuous scanning during reconnection consumes power

---

## Version History

### Version 2.2.21.0 (Current)
- Name-based sensor connection workflow
- Multi-sensor support (HR, CSC, Power)
- Discovery functionality
- Sensor MAC preservation for fast reconnection
- Runtime sensor type configuration
- Homewind BLE Identity integration
- Per-sensor-type callbacks
- Battery level monitoring

---

## License

MIT License – see [LICENSE](LICENSE) for details.

---

## Contributing

[Contributing guidelines to be added]

---

## Related Libraries

- **Homewind**: WebUI library that integrates with BluetoothBikeSensorServer
- **SmartMiFanAsync**: Fan control library (planned integration)
- **GitFirmwareUpdate**: OTA update library (used by Homewind)

---

## Support

For issues, questions, or contributions, please refer to the repository or contact the maintainer.

**Maintainer**: Martin Lihs <martin@example.com>

---

## Next Steps

1. Review the examples in `examples/` directory
2. Start with `HR_Only_Example` for basic usage
3. Try `DiscoveryExample` to find your sensors
4. Check `WebUI_Integration` for full integration example

