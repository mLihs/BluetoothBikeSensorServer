# Component Documentation

This document provides detailed documentation for all major components in BluetoothBikeSensorServer.

---

## Core Data Structures

### ActiveSensor

**File**: `src/BluetoothBikeSensorServer.cpp` (internal structure)

**Purpose**: Tracks state for a single connected sensor (one per sensor type).

**Structure**:
```cpp
struct ActiveSensor {
  SensorType type;                    // Sensor type (HR, CSC, Power)
  NimBLEAddress address;              // Sensor MAC address
  std::string name;                  // Sensor device name
  std::string macString;             // MAC address as string
  uint8_t addrType;                  // Address type (public/random)
  bool doConnect;                    // Flag to trigger connection
  NimBLEAdvertisedDevice* advertisedDevice;  // Advertised device object
  NimBLEClient *client;              // BLE client for sensor
  NimBLERemoteCharacteristic *characteristic; // Characteristic for notifications
  bool isConnected;                   // Connection state
  bool hasTarget;                    // Whether we want to connect
  uint8_t reconnectFailureCount;     // Reconnection failure counter
  uint8_t batteryLevel;              // Battery level (0-100, 255 = unknown)
  bool hasBatteryService;            // Whether sensor has Battery Service
  NimBLERemoteCharacteristic *batteryCharacteristic; // Battery characteristic
};
```

**Array**: `activeSensors[3]`
- Index 0: Heart Rate sensor
- Index 1: Speed/Cadence sensor
- Index 2: Power Meter sensor

**Key Functions**:
- `getSensorIndex(SensorType type)` - Get array index from sensor type
- `connectAndSubscribeSensor(SensorType type)` - Connect to sensor

---

### DiscoveredSensor

**File**: `src/BluetoothBikeSensorServer.cpp` (internal structure)

**Purpose**: Stores information about discovered sensors during discovery mode.

**Structure**:
```cpp
struct DiscoveredSensor {
  String name;                       // Sensor device name
  String mac;                        // Sensor MAC address
  bool hasHR;                        // Has Heart Rate service
  bool hasCSC;                       // Has Speed/Cadence service
  bool hasPWR;                       // Has Power Meter service
  
  SensorType getType() const;        // Get primary sensor type
};
```

**Array**: `g_discoveredSensors[MAX_DISCOVERED_SENSORS]` (max 50)

**Key Functions**:
- `BikeSensorServerGetSensor()` - Get sensor by index
- `BikeSensorServerListSensors()` - List all sensors

---

## BLE Components

### NimBLEServer (GATT Server)

**Purpose**: Virtual GATT server that mirrors sensor data to central devices.

**Services Created**:
- **HR Service** (0x180D): If `SENSOR_TYPE_HEART_RATE` enabled
- **CSC Service** (0x1816): If `SENSOR_TYPE_SPEED_CADENCE` enabled
- **Power Service** (0x1818): If `SENSOR_TYPE_POWER_METER` enabled

**Characteristics**:
- All characteristics have `NOTIFY` property
- CCCD (Client Characteristic Configuration Descriptor) handled automatically by NimBLE

**Key Functions**:
- `setupLocalBLEServer()` - Create server and services
- `updateAdvertisingServices()` - Update advertising based on enabled sensors

---

### NimBLEClient (Sensor Connections)

**Purpose**: BLE client for connecting to sensors.

**Per-Sensor Clients**:
- Each sensor type has its own `NimBLEClient` instance
- Stored in `ActiveSensor.client`

**Connection Parameters**:
- Min interval: 12 (15ms)
- Max interval: 24 (30ms)
- Latency: 0
- Timeout: 200 (2 seconds)

**Callbacks**: `SensorClientCallbacks`
- `onConnect()` - Called when connected to sensor
- `onDisconnect()` - Called when disconnected from sensor

**Key Functions**:
- `connectAndSubscribeSensor()` - Connect and subscribe to notifications
- `NimBLEDevice::createClient()` - Create client instance
- `NimBLEDevice::deleteClient()` - Delete client instance

---

### NimBLEScan (Discovery)

**Purpose**: BLE scanning for sensor discovery and connection.

**Scan Modes**:
- **Normal Mode**: Scanning for sensors to connect (name-based)
- **MAC Sleep-Scan Mode**: Periodic short scans to find known MACs
- **Discovery Mode**: Scanning for all sensors of specified types

**Scan Callbacks**: `AdvertisedDeviceCallbacks`
- `onResult()` - Called for each scanned device
- Filters by device name (normal mode) or service UUID (discovery mode)

**Key Functions**:
- `startScan()` - Start scanning for sensors
- `BikeSensorServerStartDiscovery()` - Start discovery mode
- `BikeSensorServerStopDiscovery()` - Stop discovery mode

---

### NimBLEAdvertising (GATT Server Advertising)

**Purpose**: Advertises the virtual GATT server to central devices.

**Advertising Data**:
- Device name: `{prefix}-xxxxxxxx` (MAC-based)
- Service UUIDs: Based on enabled sensor types
- Scan response: Empty (cleared)

**Advertising State**:
- Started when sensors are connected (or manually)
- Stopped when paused or during discovery
- Restarted when resumed

**Key Functions**:
- `updateAdvertisingServices()` - Update advertising services
- `refreshAdvertisingName()` - Update advertising name
- `BikeSensorServerPause()` - Pause advertising
- `BikeSensorServerResume()` - Resume advertising

---

## Device Identity Components

The identity system has been modularized into two components:

### BBSIdentity (Core Identity)

**File**: `src/modules/BBSIdentity.h`, `src/modules/BBSIdentity.cpp`

**Purpose**: Core device identity generation (no NimBLE dependency).

**Namespace**: `bbs::identity::`

**Key Functions**:

#### `GetBleDeviceNameLower()`
Returns MAC-based device name in lowercase format.

**Format**: `{prefix}-xxxxxxxx`
- Prefix: Configurable (default: "homewind")
- xxxxxxxx: Hex representation of lower 32 bits of eFuse MAC

**Example**: "homewind-da4eb580"

**Returns**: `const String&` (reference, no copy overhead)

#### `GetInstanceId()`
Returns 16-bit instance ID derived from the same stable base identity (lower 16 bits of the 32-bit suffix).

**Source**: Lower 16 bits of eFuse MAC lower 32-bit identity

#### `SetDeviceNamePrefix(const char* prefix)`
Sets the device name prefix (sanitized, converted to lowercase, hostname-safe).

**Default**: "homewind"

**Sanitization**: Invalid characters replaced with '-', duplicates removed, trimmed

#### `GetMdnsHostnameLower()`
Returns the same format as `GetBleDeviceNameLower()` for mDNS hostname use.

### BBSAdvertisingIdentity (Advertising Identity)

**File**: `src/modules/BBSAdvertisingIdentity.h`, `src/modules/BBSAdvertisingIdentity.cpp`

**Purpose**: BLE advertising configuration with Homewind beacon service data (requires NimBLE).

**Namespace**: `bbs::adv_identity::`

**Key Functions**:

#### `BuildBeaconServiceData(uint8_t[8], ...)`
Builds 8-byte beacon service data payload.

**Format**:
- [0] = Protocol version (0x01)
- [1] = Device type (0x01 = SensorHub)
- [2] = sensorMask (HR=0x01, CSC=0x02, PWR=0x04, ZWIFT_CTRL=0x08)
- [3] = state
- [4] = fwMajor
- [5] = fwMinor
- [6-7] = instanceId (16-bit, LSB first)

#### `ConfigureAdvertising(NimBLEAdvertising* adv, ...)`
Configures BLE advertising with Homewind identity.

**Parameters**:
- `sensorMask`: Bit flags for sensor types (HR=0x01, CSC=0x02, PWR=0x04, ZWIFT_CTRL=0x08)
- `state`: Device state byte
- `fwMajor`, `fwMinor`: Firmware version
- `advertiseZwiftLegacyUuidToo`: Whether to advertise legacy Zwift UUID

**Features**:
- Beacon Service UUID with Service Data payload
- Dynamic service advertising based on sensorMask
- Optional legacy Zwift UUID support

#### `UpdateAdvertisingIfChanged(...)`
Updates advertising only if configuration changed (efficient updates with debouncing).

**Uses**: `AdvSnapshot` structure for change detection

**Debouncing**: Minimum 300ms between rebuilds

### HomewindBleIdentity (Compatibility Wrapper)

**File**: `src/HomewindBleIdentity.h`, `src/HomewindBleIdentity.cpp`

**Purpose**: Compatibility wrapper maintaining `homewind::` namespace for backward compatibility.

**Status**: Thin wrapper mapping `homewind::` functions to `bbs::identity::` and `bbs::adv_identity::`

**Note**: Library internally uses `bbs::identity::` directly. This wrapper is kept for external code compatibility.

**See**: `docs/MIGRATION_HOMEWIND_IDENTITY.md` for migration guide

---

## Callback System

### Data Callbacks

**Heart Rate Callback**:
```cpp
void (*heartRateCallback)(uint16_t heartRate);
```
- Called when HR value changes
- Parameter: Heart rate in bpm

**Speed/Cadence Callback**:
```cpp
void (*speedCadenceCallback)(uint16_t speed, uint16_t cadence);
```
- Called when speed/cadence changes
- Parameters: Speed (1/256 m/s), Cadence (rpm)

**Power Callback**:
```cpp
void (*powerCallback)(int16_t power);
```
- Called when power value changes
- Parameter: Power in watts

### Sensor Connection Callbacks

**Per-Type Callbacks**:
```cpp
void (*sensorConnectedCallbacks[3])();
void (*sensorDisconnectedCallbacks[3])();
```
- Array index: 0=HR, 1=CSC, 2=Power
- Called when sensor connects/disconnects

**Registration**:
```cpp
BikeSensorServerSetSensorConn(SENSOR_TYPE_HEART_RATE, onHRConnected);
BikeSensorServerSetSensorDisc(SENSOR_TYPE_HEART_RATE, onHRDisconnected);
```

### Central Device Callbacks

**Connection Callback**:
```cpp
void (*centralConnectedCallback)(uint8_t centralCount);
```
- Called when central device connects
- Parameter: Current central count

**Disconnection Callback**:
```cpp
void (*centralDisconnectedCallback)(uint8_t centralCount);
```
- Called when central device disconnects
- Parameter: Remaining central count

**Registration**:
```cpp
BikeSensorServerSetClientConn(onCentralConnected);
BikeSensorServerSetClientDisc(onCentralDisconnected);
```

### Discovery Callback

**Discovery Complete Callback**:
```cpp
void (*g_discoveryCompleteCallback)(uint8_t count);
```
- Called when discovery completes
- Parameter: Number of discovered sensors

**Registration**:
```cpp
BikeSensorServerSetDiscoveryCompleteCallback(onDiscoveryComplete);
```

---

## Data Storage

### Heart Rate Data

**Variables**:
- `lastHeartRate`: Last HR value (uint16_t)
- `heartRateValid`: Whether HR data is valid (bool)
- `lastHeartRateTimestamp`: Timestamp of last update (uint32_t)

**Access Functions**:
- `BluetoothBikeSensorServerGetLastHeartRate()` - Get last HR value
- `BluetoothBikeSensorServerHasHeartRate()` - Check if HR data available
- `BluetoothBikeSensorServerGetLastHeartRateTimestamp()` - Get timestamp

### Speed/Cadence Data

**Structure**: `SpeedCadenceData`
```cpp
struct SpeedCadenceData {
  uint16_t speed;                    // Speed in 0.01 m/s
  uint16_t cadence;                  // Cadence in rpm
  bool valid;                        // Whether data is valid
  uint32_t timestamp;                // Timestamp of last update
  uint32_t cumulativeWheelRevolutions;
  uint16_t lastWheelEventTime;
  uint16_t cumulativeCrankRevolutions;
  uint16_t lastCrankEventTime;
};
```

**Access Functions**:
- `BluetoothBikeSensorServerGetLastSpeedCadence()` - Get last speed/cadence
- `BluetoothBikeSensorServerHasSpeedCadence()` - Check if data available
- `BluetoothBikeSensorServerGetLastSpeedCadenceTimestamp()` - Get timestamp

### Power Data

**Structure**: `PowerData`
```cpp
struct PowerData {
  int16_t power;                     // Power in watts
  bool valid;                        // Whether data is valid
  uint32_t timestamp;                // Timestamp of last update
};
```

**Access Functions**:
- `BluetoothBikeSensorServerGetLastPower()` - Get last power value
- `BluetoothBikeSensorServerHasPower()` - Check if data available
- `BluetoothBikeSensorServerGetLastPowerTimestamp()` - Get timestamp

---

## Configuration State

### Enabled Sensor Types

**Variable**: `enabledSensorTypes` (uint8_t)

**Bitmask**:
- `SENSOR_TYPE_HEART_RATE` (0x01)
- `SENSOR_TYPE_SPEED_CADENCE` (0x02)
- `SENSOR_TYPE_POWER_METER` (0x04)

**Default**: All sensors enabled (0x07)

**Functions**:
- `BikeSensorServerSetEnabledSensorTypes()` - Set enabled types
- Used to create/remove GATT server services

### Sensor MAC Preservation

**Variable**: `preserveMacOnDisconnect` (bool)

**Default**: `false` (standard reconnection)

**When `true`**:
- Sensor MAC preserved on disconnect
- Faster reconnection (~1 second)
- Falls back to scan after 3 failures

**Functions**:
- `BikeSensorServerSetPreserveMacOnDisconnect()` - Enable/disable
- `BikeSensorServerGetPreserveMacOnDisconnect()` - Get current setting

### Device Name Prefix

**Variable**: Stored in `HomewindBleIdentity` module

**Default**: "homewind"

**Functions**:
- `BikeSensorServerSetDeviceNamePrefix()` - Set prefix
- `HomewindBleIdentity::GetBleDeviceNameLower()` - Get device name

---

## Discovery State

### Discovery Variables

**State**:
- `g_discoveryRunning`: Whether discovery is active (bool)
- `g_discoveryDone`: Whether discovery completed (bool)
- `g_discoveryTypeMask`: Sensor type mask for discovery (uint8_t)

**Configuration**:
- `discoveryDuration`: Discovery duration in ms (uint32_t, default: 20000)
- `maxSensors`: Maximum sensors to discover (uint8_t, default: 12, max: 50)

**Saved State**:
- `savedState`: Server mode state saved before discovery
  - `wasScanning`: Whether scan was active
  - `wasAdvertising`: Whether advertising was active
  - `hadTarget`: Whether sensors had targets
  - `wasConnected`: Whether sensors were connected

**Functions**:
- `BikeSensorServerStartDiscovery()` - Start discovery
- `BikeSensorServerStopDiscovery()` - Stop discovery
- `BikeSensorServerSetDiscoveryDuration()` - Set duration
- `BikeSensorServerSetMaxSensors()` - Set max sensors
- `BikeSensorServerIsDiscoveryActive()` - Check if active

---

## Related Documentation

- [02_ARCHITECTURE.md](./02_ARCHITECTURE.md) - System architecture
- [03_FUNCTIONS.md](./03_FUNCTIONS.md) - Core functions
- [05_STATE_MACHINES.md](./05_STATE_MACHINES.md) - State machines
- [06_APIS.md](./06_APIS.md) - API reference

