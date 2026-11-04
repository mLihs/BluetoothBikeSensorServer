# BluetoothBikeSensorServer Documentation Index

**Version**: 2.2.21.0
**Platform**: ESP32 (Arduino Core)  
**Author**: Martin Lihs  
**Last Updated**: 2026-10-31

---

## Overview

This documentation provides comprehensive information about the BluetoothBikeSensorServer ESP32 Arduino library, covering architecture, implementation details, APIs, state machines, and development guidelines. The documentation is structured for both human developers and AI agents.

---

## Documentation Structure

### 📋 Quick Start
- **[01_PROJECT_OVERVIEW.md](./01_PROJECT_OVERVIEW.md)** - What is BluetoothBikeSensorServer? Purpose, features, and use cases

### 🏗️ Architecture & Design
- **[02_ARCHITECTURE.md](./02_ARCHITECTURE.md)** - System architecture, module boundaries, design principles
- **[03_FUNCTIONS.md](./03_FUNCTIONS.md)** - Core functions and workflows (connection, discovery, data relay)

### 🔧 Implementation Details
- **[04_COMPONENTS.md](./04_COMPONENTS.md)** - Detailed documentation of all components and classes
- **[05_STATE_MACHINES.md](./05_STATE_MACHINES.md)** - All state machines (sensor connection, discovery, reconnection)
- **[06_APIS.md](./06_APIS.md)** - Complete API reference with function signatures and examples

### 👨‍💻 Development
- **[07_DEVELOPMENT.md](./07_DEVELOPMENT.md)** - Build instructions, configuration, extending the library
- **[09_EXAMPLES.md](./09_EXAMPLES.md)** - Example sketches documentation and usage
- **[ARDUINO_ESP32_3.3.7_FIX.md](./ARDUINO_ESP32_3.3.7_FIX.md)** - arduino-esp32 3.3.7 NimBLE boot crash fix (TLSF assert)
- **[MIGRATION_HOMEWIND_IDENTITY.md](./MIGRATION_HOMEWIND_IDENTITY.md)** - Migration guide for HomewindBleIdentity → BBSIdentity modules
- **[MIGRATION_HOMEWIND_ADDRTYPE.md](./MIGRATION_HOMEWIND_ADDRTYPE.md)** - Migration guide for BLE Address Type support (v2.2.10.0+)
- **[MIGRATION_CONNECTION_MODE_ALIASES.md](./MIGRATION_CONNECTION_MODE_ALIASES.md)** - Migration guide for removing `BBS_CONN_MODE_*` aliases → `BBS_MODE_*`

### 📝 Status & Future
- **[08_OPEN_TOPICS.md](./08_OPEN_TOPICS.md)** - TODOs, known issues, planned features, open questions

---

## Quick Navigation by Topic

### For New Developers
1. Start with [01_PROJECT_OVERVIEW.md](./01_PROJECT_OVERVIEW.md) to understand what BluetoothBikeSensorServer does
2. Read [02_ARCHITECTURE.md](./02_ARCHITECTURE.md) for system design
3. Review [03_FUNCTIONS.md](./03_FUNCTIONS.md) to understand core workflows
4. Check [07_DEVELOPMENT.md](./07_DEVELOPMENT.md) for setup instructions
5. Explore [09_EXAMPLES.md](./09_EXAMPLES.md) for usage examples

### For AI Agents
- **Architecture queries**: [02_ARCHITECTURE.md](./02_ARCHITECTURE.md), [04_COMPONENTS.md](./04_COMPONENTS.md)
- **State machine queries**: [05_STATE_MACHINES.md](./05_STATE_MACHINES.md), [03_FUNCTIONS.md](./03_FUNCTIONS.md)
- **API queries**: [06_APIS.md](./06_APIS.md)
- **Implementation status**: [08_OPEN_TOPICS.md](./08_OPEN_TOPICS.md)
- **Example usage**: [09_EXAMPLES.md](./09_EXAMPLES.md)

### For Contributors
- **Adding features**: [07_DEVELOPMENT.md](./07_DEVELOPMENT.md) → "Extending the Library"
- **Understanding existing code**: [04_COMPONENTS.md](./04_COMPONENTS.md)
- **API changes**: [06_APIS.md](./06_APIS.md)
- **Current work**: [08_OPEN_TOPICS.md](./08_OPEN_TOPICS.md)

---

## Key Concepts

### Connection Workflows
Two connection modes are supported:
- **Scan-Based Mode** (`BBS_MODE_SCAN` - default): Connect to sensors by device name. Library scans for devices matching the name and connects automatically.
- **MAC Sleep-Scan Mode** (`BBS_MODE_MAC`): Connect by MAC address using periodic short scans. MAC address is permanently preserved (v2.2.8.0+).

See: [03_FUNCTIONS.md](./03_FUNCTIONS.md) → "Connection Workflow", [05_STATE_MACHINES.md](./05_STATE_MACHINES.md) → "Sensor Connection State Machine", [06_APIS.md](./06_APIS.md) → "Initialization"

### BLE Sensor Relay
Acts as a relay between BLE sensors (HR, CSC, Power) and central devices (fitness apps, trainers). Creates a virtual GATT server that mirrors sensor data.

See: [01_PROJECT_OVERVIEW.md](./01_PROJECT_OVERVIEW.md) → "What is BluetoothBikeSensorServer?", [02_ARCHITECTURE.md](./02_ARCHITECTURE.md) → "GATT Server"

### Discovery Mode
Async discovery mode to find and list available BLE sensors. Can filter by sensor type and stores discovered sensors in a list.

See: [03_FUNCTIONS.md](./03_FUNCTIONS.md) → "Discovery Workflow", [05_STATE_MACHINES.md](./05_STATE_MACHINES.md) → "Discovery State Machine"

### Sensor MAC Preservation
- **Scan-Based Mode**: Option to preserve sensor MAC address on disconnect for faster reconnection. When enabled, direct reconnection without scanning (~1 second vs ~5-20 seconds).
- **MAC Sleep-Scan Mode** (v2.2.8.0+): MAC address is always preserved permanently, even after multiple connection failures. Sensors can reconnect after long offline periods.

See: [03_FUNCTIONS.md](./03_FUNCTIONS.md) → "Reconnection Handling", [05_STATE_MACHINES.md](./05_STATE_MACHINES.md) → "Reconnection State Machine"

### Multi-Sensor Support
Support for multiple sensor types simultaneously (Heart Rate, Speed/Cadence, Power Meter). Each sensor type has its own connection and data handling.

See: [02_ARCHITECTURE.md](./02_ARCHITECTURE.md) → "Multi-Sensor Architecture", [04_COMPONENTS.md](./04_COMPONENTS.md) → "ActiveSensor"

### Device Identity
MAC-based device naming: `{prefix}-xxxxxxxx` (e.g., "homewind-da4eb580"). Prefix is configurable, suffix derived from eFuse MAC (lower 32-bit, WiFi-style).

See: [02_ARCHITECTURE.md](./02_ARCHITECTURE.md) → "Device Identity", [04_COMPONENTS.md](./04_COMPONENTS.md) → "HomewindBleIdentity"

---

## Related Documentation

### In Repository
- `README.md` - Quick start and installation (legacy, see [01_PROJECT_OVERVIEW.md](./01_PROJECT_OVERVIEW.md))
- `library.properties` - Arduino library metadata
- `src/BluetoothBikeSensorServer.h` - Header file with inline documentation
- `examples/` - Example sketches (see [09_EXAMPLES.md](./09_EXAMPLES.md))

### External Libraries
- **NimBLE-Arduino** - Version 2.x (REQUIRED, NOT compatible with 1.x)
- **HomewindBleIdentity** - MAC-based device identity (included in library)
- **DebugLog** - Optional debug logging library

---

## Documentation Conventions

### Code Examples
- C++ code uses `cpp` language identifier
- Configuration examples show actual values
- Function signatures include parameter types and return values

### Status Indicators
- ✅ **Implemented** - Feature is complete and functional
- 🚧 **In Progress** - Feature is partially implemented
- 📋 **Planned** - Feature is planned but not started
- ⚠️ **Stub/Placeholder** - Interface exists but implementation is missing

### File References
- Relative paths from repository root: `src/BluetoothBikeSensorServer.h`
- Absolute paths for clarity: `/src/BluetoothBikeSensorServer.h` (when needed)

### Sensor Type Notation
- **HR** - Heart Rate sensor (SENSOR_TYPE_HEART_RATE)
- **CSC** - Speed/Cadence sensor (SENSOR_TYPE_SPEED_CADENCE)
- **PWR** - Power Meter sensor (SENSOR_TYPE_POWER_METER)

---

## Feedback & Updates

This documentation is maintained alongside the codebase. If you find errors or gaps, please update the relevant documentation file.

**Last Documentation Review**: 2026-01-24

