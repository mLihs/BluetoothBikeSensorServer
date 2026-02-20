# BluetoothBikeSensorServer Project Overview

## What is BluetoothBikeSensorServer?

**BluetoothBikeSensorServer** is an ESP32 Arduino library that acts as a **Bluetooth Low Energy (BLE) relay** for fitness sensors. It connects to BLE sensors (Heart Rate monitors, Speed/Cadence sensors, Power meters) and relays their data to connected central devices (fitness apps, trainers, etc.) via a local GATT server.

The library provides a simple, **name-based connection workflow** using **NimBLE-Arduino 2.x**, with an optional **MAC-based Sleep-Scan mode** for fixed sensor setups.

---

## Purpose

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
- **Sensor Aggregation**: Combine multiple sensors into a single virtual device

---

## Key Features

### ✅ Implemented Features

1. **Multi-Sensor Support**
   - Heart Rate (HR) sensors (BLE Service 0x180D)
   - Speed/Cadence (CSC) sensors (BLE Service 0x1816)
   - Power Meter sensors (BLE Service 0x1818)
   - Support for all three sensor types simultaneously

2. **Connection Modes**
   - **Scan-Based Mode**: Connect to sensors by device name (no MAC address required)
   - **MAC Sleep-Scan Mode**: Connect by MAC address using periodic short scans
   - Robust reconnection handling with permanent MAC preservation

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
   - Sensor MAC preservation option for faster reconnection
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

## Project Status

### Current State: **Version 2.2.13.0 (Stable)**

- **Name-Based Connection**: ✅ Fully functional
- **Multi-Sensor Support**: ✅ Fully functional
- **Discovery**: ✅ Fully functional
- **GATT Server**: ✅ Fully functional
- **Reconnection**: ✅ Fully functional
- **Device Identity**: ✅ Fully functional
- **Battery Monitoring**: ✅ Fully functional

### Compilation Status
- ✅ Compiles successfully with NimBLE-Arduino 2.x
- ✅ All features implemented and tested
- ✅ Multiple example sketches provided

---

## Design Principles

### 1. **Flexible Connection Modes**
- Name-based scanning for simple setups
- MAC-based Sleep-Scan for fixed sensors and predictable reconnects

### 2. **Non-Blocking Design**
- No `delay()` calls in main code paths
- Async operations where possible
- Event-driven callbacks

### 3. **Multi-Sensor Support**
- Each sensor type handled independently
- Per-sensor-type callbacks and status
- Dynamic service advertising

### 4. **Resource Efficiency**
- Static buffers where possible
- Minimal heap allocation
- Efficient BLE operations

### 5. **Maintainability**
- Clear module boundaries
- Consistent naming conventions
- Comprehensive documentation

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│              Application Layer (User Code)              │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │   Callbacks  │  │   Examples   │  │  Integration │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
└─────────────────────────────────────────────────────────┘
                        │
                        ▼
┌─────────────────────────────────────────────────────────┐
│              BluetoothBikeSensorServer API                │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │  Sensor Mgmt │  │   Discovery   │  │  Data Access  │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
└─────────────────────────────────────────────────────────┘
                        │
                        ▼
┌─────────────────────────────────────────────────────────┐
│                    Core Components                       │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │ ActiveSensor │  │   Discovery   │  │  GATT Server │  │
│  │  Management  │  │    Manager    │  │   (Mirror)   │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
└─────────────────────────────────────────────────────────┘
                        │
                        ▼
┌─────────────────────────────────────────────────────────┐
│                    BLE Layer (NimBLE)                    │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │   BLE Client  │  │  BLE Server   │  │   BLE Scan   │  │
│  │  (Sensors)   │  │  (Centrals)   │  │  (Discovery) │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
└─────────────────────────────────────────────────────────┘
                        │
                        ▼
                    ┌─────────┐
                    │  ESP32  │
                    │   BLE   │
                    └─────────┘
```

See [02_ARCHITECTURE.md](./02_ARCHITECTURE.md) for detailed architecture documentation.

---

## File Structure

```
BluetoothBikeSensorServer/
├── library.properties        # Arduino library metadata
├── README.md                 # Quick start guide (legacy)
├── docs/                     # Comprehensive documentation (this directory)
├── examples/                 # Example sketches
│   ├── HR_Only_Example/
│   ├── HR_CSC_Example/
│   ├── DiscoveryExample/
│   ├── FastReconnectionExample/
│   ├── IdentityExample/
│   ├── ManualSensorExample/
│   ├── WebSocketStreamingExample/
│   └── WebUI_Integration/
└── src/
    ├── BluetoothBikeSensorServer.h    # Main header file
    ├── BluetoothBikeSensorServer.cpp # Main implementation
    ├── HomewindBleIdentity.h         # Compatibility wrapper (legacy API)
    ├── HomewindBleIdentity.cpp       # Compatibility wrapper implementation
    └── modules/
        ├── BBSIdentity.h              # Core identity module (no NimBLE)
        ├── BBSIdentity.cpp
        ├── BBSAdvertisingIdentity.h   # Advertising identity module (requires NimBLE)
        ├── BBSAdvertisingIdentity.cpp
        ├── BLEConnectionManager.*     # Connection management
        ├── BLEScanManager.*           # Scan management
        ├── BLEAdvertisingManager.*    # Advertising management
        └── SensorDataParser.*         # Data parsing
```

---

## Version Information

- **Current Version**: 2.2.13.0
- **Version Source**: `library.properties`
- **Release Status**: Stable

---

## Related Libraries

- **Homewind**: WebUI library that integrates with BluetoothBikeSensorServer
- **SmartMiFanAsync**: Fan control library (planned integration)
- **GitFirmwareUpdate**: OTA update library (used by Homewind)

---

## Next Steps

1. Read [02_ARCHITECTURE.md](./02_ARCHITECTURE.md) for system design details
2. Review [03_FUNCTIONS.md](./03_FUNCTIONS.md) to understand core workflows
3. Check [07_DEVELOPMENT.md](./07_DEVELOPMENT.md) for setup instructions
4. Explore [09_EXAMPLES.md](./09_EXAMPLES.md) for usage examples

