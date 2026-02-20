# BluetoothBikeSensorServer Architecture

## System Architecture Overview

BluetoothBikeSensorServer follows a **modular, event-driven architecture** with clear separation between sensor management, GATT server, and discovery functionality. The design emphasizes:
- **Name-based connection workflow** (no MAC address management)
- **MAC Sleep-Scan mode** for fixed MAC-based setups
- **Non-blocking operations** throughout
- **Multi-sensor support** with independent per-type handling
- **Resource efficiency** (static buffers, minimal heap allocation)

---

## Architecture Layers

```
┌─────────────────────────────────────────────────────────────────┐
│                    Application Layer                            │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐        │
│  │   User Code  │  │   Callbacks   │  │   Examples   │        │
│  └──────────────┘  └──────────────┘  └──────────────┘        │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                  Public API Layer                               │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐        │
│  │  Sensor Mgmt │  │   Discovery   │  │  Data Access │        │
│  │   Functions  │  │   Functions   │  │   Functions  │        │
│  └──────────────┘  └──────────────┘  └──────────────┘        │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    Core Components                               │
│  ┌──────────────────────────────────────────────────────────┐ │
│  │              ActiveSensor Management                       │ │
│  │  • Per-sensor-type tracking (HR, CSC, Power)              │ │
│  │  • Connection state management                             │ │
│  │  • Reconnection handling                                   │ │
│  │  • Battery level monitoring                                │ │
│  └──────────────────────────────────────────────────────────┘ │
│  ┌──────────────────────────────────────────────────────────┐ │
│  │              Discovery Manager                             │ │
│  │  • Async scanning                                          │ │
│  │  • Sensor filtering by type                                 │ │
│  │  • Discovered sensor list                                 │ │
│  └──────────────────────────────────────────────────────────┘ │
│  ┌──────────────────────────────────────────────────────────┐ │
│  │              GATT Server (Virtual Sensor)                 │ │
│  │  • Service creation (HR, CSC, Power)                       │ │
│  │  • Data mirroring to centrals                              │ │
│  │  • Advertising management                                  │ │
│  │  • Central connection tracking                             │ │
│  └──────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    BLE Layer (NimBLE 2.x)                      │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐        │
│  │   BLE Client │  │  BLE Server  │  │   BLE Scan   │        │
│  │  (Sensors)  │  │  (Centrals)   │  │  (Discovery) │        │
│  └──────────────┘  └──────────────┘  └──────────────┘        │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
                    ┌─────────────────┐
                    │   ESP32 BLE     │
                    │   (Hardware)    │
                    └─────────────────┘
```

---

## Module Organization

### Public API (`BluetoothBikeSensorServer.h`)

**Initialization Functions**
- `BluetoothBikeSensorServerBegin()` - Initialize library
- `BluetoothBikeSensorServerLoop()` - Main loop (must be called regularly)

**Sensor Management Functions**
- `BikeSensorServerAddSensor()` - Add sensor by name
- `BikeSensorServerRemoveSensor()` - Remove sensor
- `BikeSensorServerIsSensorConnected()` - Check connection status
- `BikeSensorServerGetSensorMac()` - Get sensor MAC address
- `BikeSensorServerGetSensorName()` - Get sensor name
- `BikeSensorServerGetBatteryLevel()` - Get battery level

**Discovery Functions**
- `BikeSensorServerStartDiscovery()` - Start discovery scan
- `BikeSensorServerStopDiscovery()` - Stop discovery
- `BikeSensorServerGetSensorCount()` - Get discovered sensor count
- `BikeSensorServerGetSensor()` - Get sensor by index
- `BikeSensorServerListSensors()` - List all sensors

**Data Access Functions**
- `BluetoothBikeSensorServerGetLastHeartRate()` - Get last HR value
- `BluetoothBikeSensorServerGetLastSpeedCadence()` - Get last speed/cadence
- `BluetoothBikeSensorServerGetLastPower()` - Get last power value

**Configuration Functions**
- `BikeSensorServerSetEnabledSensorTypes()` - Enable/disable sensor types
- `BikeSensorServerSetPreserveMacOnDisconnect()` - Fast reconnection mode
- `BikeSensorServerSetDeviceNamePrefix()` - Set device name prefix
- `BikeSensorServerPause()` - Pause advertising
- `BikeSensorServerResume()` - Resume advertising

**Callback Registration Functions**
- `BluetoothBikeSensorServerSetHeartRateCallback()` - HR data callback
- `BluetoothBikeSensorServerSetSpeedCadenceCallback()` - CSC data callback
- `BluetoothBikeSensorServerSetPowerCallback()` - Power data callback
- `BikeSensorServerSetSensorConn()` - Sensor connection callback
- `BikeSensorServerSetSensorDisc()` - Sensor disconnection callback
- `BikeSensorServerSetClientConn()` - Central connection callback
- `BikeSensorServerSetClientDisc()` - Central disconnection callback

### Core Implementation (`BluetoothBikeSensorServer.cpp`)

**ActiveSensor Management**
- `ActiveSensor` structure - Tracks connected sensors (one per type)
- `connectAndSubscribeSensor()` - Connect to sensor and subscribe to notifications
- `startScan()` - Start scanning for sensors
- `handleScanResults()` - Process scan results and match by name

**Discovery Management**
- `DiscoveredSensor` structure - Stores discovered sensor information
- `handleDiscoveryResults()` - Process discovery scan results
- `g_discoveredSensors[]` - Array of discovered sensors (max 50)

**GATT Server Management**
- `createGATTServer()` - Create virtual GATT server
- `updateAdvertisingServices()` - Update advertising based on enabled sensors
- `mirrorHeartRatePacket()` - Mirror HR data to centrals
- `mirrorCSCPacket()` - Mirror CSC data to centrals
- `mirrorPowerPacket()` - Mirror Power data to centrals

**Data Parsing**
- `parseAndLogHR()` - Parse HR measurement characteristic
- `parseAndLogCSC()` - Parse CSC measurement characteristic
- `parseAndLogPower()` - Parse Power measurement characteristic

**Device Identity** (modularized):
- `modules/BBSIdentity.*` - Core identity (prefix, instanceId, deviceName) - no NimBLE dependency
- `modules/BBSAdvertisingIdentity.*` - Advertising identity (beacon service data, AdvSnapshot) - requires NimBLE
- `HomewindBleIdentity.*` - Compatibility wrapper (maintains `homewind::` namespace)

**Key Functions**:
- `bbs::identity::GetBleDeviceNameLower()` - Get MAC-based device name
- `bbs::identity::GetInstanceId()` - Get compact instance ID (derived from eFuse MAC)
- `bbs::adv_identity::ConfigureAdvertising()` - Configure advertising with identity
- `bbs::adv_identity::UpdateAdvertisingIfChanged()` - Update advertising only if changed

---

## Multi-Sensor Architecture

The library supports **three sensor types simultaneously**, each handled independently:

### Sensor Type Enumeration
```cpp
enum SensorType {
  SENSOR_TYPE_HEART_RATE = 0x01,      // 0x01
  SENSOR_TYPE_SPEED_CADENCE = 0x02,   // 0x02
  SENSOR_TYPE_POWER_METER = 0x04      // 0x04
};
```

### ActiveSensor Array
- **Index 0**: Heart Rate sensor
- **Index 1**: Speed/Cadence sensor
- **Index 2**: Power Meter sensor

Each `ActiveSensor` structure tracks:
- Sensor type
- Device name and MAC address
- Connection state
- BLE client and characteristics
- Battery level (if available)
- Reconnection state

### Per-Sensor-Type Services

**Heart Rate Service**
- UUID: `0x180D`
- Characteristic: `0x2A37` (Heart Rate Measurement)
- Data format: 8-bit or 16-bit HR value + optional RR intervals

**Speed/Cadence Service**
- UUID: `0x1816`
- Characteristic: `0x2A5B` (CSC Measurement)
- Data format: Cumulative wheel/crank revolutions + event times

**Power Meter Service**
- UUID: `0x1818`
- Characteristic: `0x2A63` (Cycling Power Measurement)
- Data format: Power value (watts) + optional torque/crank data

---

## GATT Server (Virtual Sensor)

The library creates a **virtual GATT server** that acts as a relay between sensors and central devices:

### Server Structure
```
NimBLEServer
├── HR Service (0x180D) [if HR sensor connected]
│   └── HR Characteristic (0x2A37)
│       └── Notifications → Centrals
├── CSC Service (0x1816) [if CSC sensor connected]
│   └── CSC Characteristic (0x2A5B)
│       └── Notifications → Centrals
└── Power Service (0x1818) [if Power sensor connected]
    └── Power Characteristic (0x2A63)
        └── Notifications → Centrals
```

### Data Flow
1. **Sensor → Library**: Sensor sends notification via BLE Client
2. **Library → GATT Server**: Library updates characteristic value
3. **GATT Server → Centrals**: Library sends notification to connected centrals

### Advertising
- Advertises services based on **enabled sensor types** (not connected sensors)
- Services are added/removed dynamically when sensor types are enabled/disabled
- Device name: `{prefix}-xxxxxxxx` (MAC-based)

---

## Connection Workflow

### Name-Based Connection

1. **User adds sensor**: `BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, "TICKR FIT")`
2. **Library stores target**: Sensor name stored in `ActiveSensor.name`
3. **Library starts scan**: `startScan()` begins BLE scanning
4. **Scan finds device**: Scan callback matches device name
5. **Library stores device**: `advertisedDevice` and MAC address stored
6. **Library connects**: `connectAndSubscribeSensor()` connects to sensor
7. **Library subscribes**: Subscribes to characteristic notifications
8. **Data flows**: Sensor → Library → GATT Server → Centrals

### Reconnection Handling

**Standard Reconnection** (default):
1. Sensor disconnects
2. Library clears sensor MAC and `advertisedDevice`
3. Library restarts scan
4. Library finds device by name
5. Library reconnects

**Fast Reconnection** (if `preserveMacOnDisconnect = true`):
1. Sensor disconnects
2. Library preserves sensor MAC and `advertisedDevice`
3. Library attempts direct reconnection
4. If successful: ~1 second reconnection time
5. If failed: Falls back to scan-based reconnection

---

## Discovery Workflow

### Discovery Mode

1. **User starts discovery**: `BikeSensorServerStartDiscovery(SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_POWER_METER)`
2. **Library pauses normal mode**: Saves current state (scanning, advertising)
3. **Library starts scan**: Begins discovery scan with type filter
4. **Scan finds devices**: Scan callback checks for matching services
5. **Library stores devices**: Adds to `g_discoveredSensors[]` array
6. **Discovery completes**: After timeout or manual stop
7. **Library calls callback**: `onDiscoveryComplete(count)`
8. **Library restores state**: Resumes normal mode (if not manually paused)

### Discovery State

- **Running**: `g_discoveryRunning = true`
- **Duration**: Configurable (default: 20 seconds)
- **Max Sensors**: Configurable (default: 12, max: 50)
- **Type Filter**: Bitmask of sensor types to discover

---

## Device Identity

### MAC-Based Naming

Device name format: `{prefix}-xxxxxxxx`

- **Prefix**: Configurable (default: "homewind")
- **Identity suffix**: lower 32 bits of eFuse MAC (hex, WiFi-style)
- **Example**: "homewind-a1b2" (where a1b2 is from MAC)

### HomewindBleIdentity Module

The `HomewindBleIdentity` module provides:
- Stable device naming based on BT MAC
- Advertising identity with Beacon Service UUID
- Service Data payload with device information
- Guard snapshot for efficient advertising updates

See: [04_COMPONENTS.md](./04_COMPONENTS.md) → "HomewindBleIdentity"

---

## Memory Management

### Static Buffers
- **ActiveSensor array**: Fixed-size array (3 sensors)
- **DiscoveredSensor array**: Fixed-size array (max 50 sensors)
- **Scan results**: Handled by NimBLE (internal buffers)
- **Advertising data**: Rebuilt on changes (no persistent buffer)

### Heap Allocation
- **BLE Clients**: Allocated per sensor (deleted on disconnect)
- **BLE Server**: Allocated once during initialization
- **AdvertisedDevice objects**: Allocated when device found (deleted on disconnect if MAC not preserved)

### String Management
- **Device names**: `std::string` (heap allocated, but minimal)
- **MAC addresses**: `std::string` (heap allocated, but minimal)
- **Discovery lists**: Static buffer for formatted output

---

## Thread Safety

### Single-Threaded Main Loop
All code runs in the main `loop()` function (single thread). BLE callbacks run in NimBLE context but should not directly modify shared state.

### Callback Safety
BLE callbacks (sensor notifications, scan results) should:
- Mark state as dirty (if needed)
- Store data in buffers
- Avoid blocking operations
- Let main loop handle state updates

### State Updates
- **From callbacks**: Store data, mark dirty flags
- **From main loop**: Process dirty flags, update state, send notifications

---

## Error Handling

### Connection Failures
- **Connection timeout**: Retry with exponential backoff
- **Service not found**: Log error, retry scan
- **Characteristic not found**: Log error, disconnect
- **Subscription failure**: Log error, retry connection

### Discovery Failures
- **Scan timeout**: Call discovery complete callback with current results
- **Max sensors reached**: Stop adding sensors, continue scan
- **Memory issues**: Log warning, continue with available memory

### GATT Server Failures
- **Service creation failure**: Log error, continue without service
- **Characteristic creation failure**: Log error, continue without characteristic
- **Notification failure**: Log warning (CCCD may not be enabled), continue

---

## Extension Points

### Adding a New Sensor Type

1. **Add to SensorType enum**:
   ```cpp
   SENSOR_TYPE_NEW_SENSOR = 0x08
   ```

2. **Add service UUID and characteristic UUID**:
   ```cpp
   const NimBLEUUID NEW_SERVICE_UUID((uint16_t)0xXXXX);
   const NimBLEUUID NEW_CHAR_UUID((uint16_t)0xXXXX);
   ```

3. **Extend ActiveSensor array** (if needed):
   - Currently supports 3 sensors (HR, CSC, Power)
   - Would need to increase array size for more types

4. **Add parsing function**:
   ```cpp
   void parseAndLogNewSensor(const uint8_t *data, size_t len);
   ```

5. **Add mirroring function**:
   ```cpp
   void mirrorNewSensorPacket(const uint8_t *data, size_t len);
   ```

6. **Add to GATT server creation**:
   - Create service and characteristic
   - Add to advertising

---

## Related Documentation

- [03_FUNCTIONS.md](./03_FUNCTIONS.md) - Core functions and workflows
- [04_COMPONENTS.md](./04_COMPONENTS.md) - Component documentation
- [05_STATE_MACHINES.md](./05_STATE_MACHINES.md) - State machines
- [07_DEVELOPMENT.md](./07_DEVELOPMENT.md) - Development guide

