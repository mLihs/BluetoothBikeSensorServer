# Development Guide

This document provides development instructions, build configuration, and guidelines for extending BluetoothBikeSensorServer.

---

## Build Requirements

### Required Dependencies

1. **ESP32 Arduino Core**
   - Version: Latest stable (tested with 2.0.x, 3.3.6, 3.3.7)
   - Installation: Via Arduino IDE Board Manager or PlatformIO
   - **3.3.7+**: Library includes fix for BT memory release issue. Siehe [ARDUINO_ESP32_3.3.7_FIX.md](./ARDUINO_ESP32_3.3.7_FIX.md)

2. **NimBLE-Arduino**
   - Version: **2.x** (REQUIRED, NOT compatible with 1.x)
   - Installation: Via Arduino IDE Library Manager or PlatformIO
   - Library ID: `h2zero/NimBLE-Arduino`

### Optional Dependencies

- **DebugLog**: For debug logging (optional, can use Serial directly)

---

## Build Configuration

### Arduino IDE

1. **Install ESP32 Board Support**:
   - File → Preferences → Additional Board Manager URLs
   - Add: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   - Tools → Board → Boards Manager → Search "ESP32" → Install

2. **Install NimBLE-Arduino**:
   - Sketch → Include Library → Manage Libraries
   - Search "NimBLE-Arduino"
   - Install version **2.x** (NOT 1.x)

3. **Select Board**:
   - Tools → Board → ESP32 Arduino → Select your board variant

4. **Configure Serial Monitor**:
   - Tools → Serial Monitor → Set baud rate to 115200

### PlatformIO

**platformio.ini**:
```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
lib_deps = 
    h2zero/NimBLE-Arduino@^2.0.0
    file://../libraries/BluetoothBikeSensorServer
monitor_speed = 115200
```

---

## Debug Logging

### Compile-Time Flags

Enable debug logging via preprocessor defines:

```cpp
// General lifecycle/errors
#define BLE_LIB_DEBUG_GEN

// High-frequency (prints once per measurement window, default ~1 Hz)
#define BLE_LIB_DEBUG_HOT

// Reserved for future
// #define BLE_LIB_DEBUG_NET
```

### Debug Levels

**BLE_LIB_DEBUG_GEN**:
- General lifecycle events
- Errors and warnings
- Connection/disconnection events
- Discovery events

**BLE_LIB_DEBUG_HOT**:
- High-frequency data updates
- Sensor notifications
- Connection attempts
- Advertising updates

### Usage

**In Sketch**:
```cpp
#define BLE_LIB_DEBUG_GEN
#include <BluetoothBikeSensorServer.h>
```

**Via Build Flags** (PlatformIO):
```ini
build_flags = -DBLE_LIB_DEBUG_GEN
```

**Via DebugConfig.h**:
Create `DebugConfig.h` in your sketch directory:
```cpp
#define BLE_LIB_DEBUG_GEN
#define BLE_LIB_DEBUG_HOT
```

---

## Code Structure

### File Organization

```
BluetoothBikeSensorServer/
├── src/
│   ├── BluetoothBikeSensorServer.h      # Public API header
│   ├── BluetoothBikeSensorServer.cpp     # Main implementation
│   ├── HomewindBleIdentity.h            # Compatibility wrapper (legacy API)
│   ├── modules/
│   │   ├── BBSIdentity.h                # Core identity (no NimBLE)
│   │   ├── BBSAdvertisingIdentity.h     # Advertising identity (requires NimBLE)
│   └── HomewindBleIdentity.cpp          # Device identity implementation
├── examples/                             # Example sketches
├── docs/                                 # Documentation
└── library.properties                    # Arduino library metadata
```

### Key Components

**BluetoothBikeSensorServer.h**:
- Public API declarations
- Sensor type enumerations
- Function signatures
- Inline documentation

**BluetoothBikeSensorServer.cpp**:
- Main implementation
- ActiveSensor management
- Discovery handling
- GATT server management
- Data parsing and mirroring

**BBSIdentity.h/cpp** (Core Identity):
- MAC-based device naming
- Prefix sanitization
- Instance ID generation
- No NimBLE dependency

**BBSAdvertisingIdentity.h/cpp** (Advertising Identity):
- Beacon Service Data payload
- Advertising configuration
- Change detection (AdvSnapshot)
- Requires NimBLE

**HomewindBleIdentity.h/cpp** (Compatibility Wrapper):
- Maintains `homewind::` namespace for backward compatibility
- Maps to `bbs::identity::` and `bbs::adv_identity::`
- See `docs/MIGRATION_HOMEWIND_IDENTITY.md` for details

---

## Extending the Library

### Adding a New Sensor Type

1. **Add to SensorType enum** (in `BluetoothBikeSensorServer.h`):
   ```cpp
   enum SensorType {
     SENSOR_TYPE_HEART_RATE = 0x01,
     SENSOR_TYPE_SPEED_CADENCE = 0x02,
     SENSOR_TYPE_POWER_METER = 0x04,
     SENSOR_TYPE_NEW_SENSOR = 0x08  // New sensor type
   };
   ```

2. **Add service and characteristic UUIDs** (in `BluetoothBikeSensorServer.cpp`):
   ```cpp
   const NimBLEUUID NEW_SERVICE_UUID((uint16_t)0xXXXX);
   const NimBLEUUID NEW_CHAR_UUID((uint16_t)0xXXXX);
   ```

3. **Extend ActiveSensor array** (if needed):
   - Currently supports 3 sensors (HR, CSC, Power)
   - Increase array size: `ActiveSensor activeSensors[4];`
   - Update `getSensorIndex()` and `getSensorTypeFromIndex()`

4. **Add parsing function**:
   ```cpp
   void parseAndLogNewSensor(const uint8_t *data, size_t len) {
     // Parse sensor data
     // Update internal state
     // Call user callback if registered
   }
   ```

5. **Add mirroring function**:
   ```cpp
   void mirrorNewSensorPacket(const uint8_t *data, size_t len) {
     if (!newSensorCharLocal) return;
     newSensorCharLocal->setValue(data, len);
     if (server && server->getConnectedCount() > 0) {
       newSensorCharLocal->notify();
     }
   }
   ```

6. **Add to GATT server creation**:
   ```cpp
   if (enabledSensorTypes & SENSOR_TYPE_NEW_SENSOR) {
     newSensorServiceLocal = server->createService(NEW_SERVICE_UUID);
     newSensorCharLocal = newSensorServiceLocal->createCharacteristic(
       NEW_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);
     newSensorServiceLocal->start();
   }
   ```

7. **Add to advertising**:
   ```cpp
   if (enabledSensorTypes & SENSOR_TYPE_NEW_SENSOR) {
     advData.addServiceUUID(NEW_SERVICE_UUID);
   }
   ```

8. **Add public API functions**:
   - Data access functions
   - Callback registration functions
   - Sensor management functions

---

## Testing

### Unit Testing

Currently, the library does not include unit tests. Testing is done via example sketches.

### Manual Testing

1. **Connection Test**:
   - Add sensor by name
   - Verify connection
   - Check data callbacks

2. **Reconnection Test**:
   - Disconnect sensor (power off)
   - Verify reconnection
   - Check reconnection time

3. **Discovery Test**:
   - Start discovery
   - Verify sensors found
   - Check sensor list

4. **Multi-Sensor Test**:
   - Add multiple sensors
   - Verify all connect
   - Check data from all sensors

5. **Central Connection Test**:
   - Connect central device (fitness app)
   - Verify data relay
   - Check notifications

---

## Debugging Tips

### Common Issues

1. **Boot-Crash (arduino-esp32 3.3.7+)**: `assert failed: block_locate_free tlsf_control_functions.h:618`
   - Betrifft NimBLE mit arduino-esp32 3.3.7. Die Library enthält den Fix (esp32-hal-bt-mem.h).
   - Falls trotzdem Crash: siehe [ARDUINO_ESP32_3.3.7_FIX.md](./ARDUINO_ESP32_3.3.7_FIX.md)

2. **Sensor Not Connecting**:
   - Verify sensor name matches exactly (case-sensitive)
   - Check Serial output for connection attempts
   - Enable `BLE_LIB_DEBUG_GEN` for detailed logs
   - Verify sensor is powered on and in range
   - Check if sensor is already connected to another device

3. **Discovery Not Finding Sensors**:
   - Verify sensors are powered on and in range
   - Check sensor type mask matches sensor types
   - Increase discovery duration
   - Enable `BLE_LIB_DEBUG_GEN` for scan results

4. **Central Not Connecting**:
   - Verify advertising is active: `BikeSensorServerIsAdvertising()`
   - Check device name is visible in central's scan
   - Ensure correct services are advertised
   - Try refreshing advertising: `BikeSensorServerRestartAdvertising()`

5. **Reconnection Too Slow**:
   - Enable Sensor MAC Preservation: `BikeSensorServerSetPreserveMacOnDisconnect(true)`
   - Verify sensor has stable MAC address (not privacy/random)
   - Check sensor is in range and powered on

6. **Memory Issues**:
   - Check available heap: `ESP.getFreeHeap()`
   - Reduce number of sensors
   - Disable unused sensor types
   - Ensure sufficient free heap (~50KB minimum)

### Debug Output

Enable debug logging to see detailed operation:

```cpp
#define BLE_LIB_DEBUG_GEN
#define BLE_LIB_DEBUG_HOT
#include <BluetoothBikeSensorServer.h>
```

**Log Prefixes**:
- `[I]` - Info (general events)
- `[W]` - Warning
- `[E]` - Error
- `[H]` - Hot path (high-frequency events)
- `[CLIENT]` - BLE client operations
- `[SERVER]` - GATT server operations
- `[ADV]` - Advertising operations
- `[MIRROR]` - Data mirroring operations
- `[DISCOVERY]` - Discovery operations

---

## Code Style

### Naming Conventions

- **Functions**: `PascalCase` for public API, `camelCase` for internal
- **Variables**: `camelCase` for local, `g_` prefix for globals
- **Constants**: `UPPER_SNAKE_CASE`
- **Types**: `PascalCase`

### Code Organization

- **Header files**: Public API only
- **Implementation files**: Internal implementation
- **Namespaces**: Use for grouping (e.g., `homewind` namespace)

### Documentation

- **Function documentation**: Doxygen-style comments
- **Inline comments**: Explain complex logic
- **API documentation**: See [06_APIS.md](./06_APIS.md)

---

## Contributing

### Code Contributions

1. **Fork the repository**
2. **Create a feature branch**
3. **Make changes**:
   - Follow code style guidelines
   - Add documentation
   - Test thoroughly
4. **Submit pull request**:
   - Describe changes
   - Include test results
   - Update documentation if needed

### Documentation Contributions

- Update relevant documentation files in `docs/`
- Keep documentation in sync with code changes
- Add examples for new features

---

## Version Management

### Version Numbering

- **Format**: `MAJOR.MINOR.PATCH`
- **Current**: 2.0.0
- **Location**: `library.properties`

### Version History

**Version 2.0.0**:
- Name-based sensor connection workflow
- Multi-sensor support (HR, CSC, Power)
- Discovery functionality
- Sensor MAC preservation for fast reconnection
- Runtime sensor type configuration
- Homewind BLE Identity integration
- Per-sensor-type callbacks
- Battery level monitoring

---

## Related Documentation

- [01_PROJECT_OVERVIEW.md](./01_PROJECT_OVERVIEW.md) - Project overview
- [02_ARCHITECTURE.md](./02_ARCHITECTURE.md) - System architecture
- [06_APIS.md](./06_APIS.md) - API reference
- [09_EXAMPLES.md](./09_EXAMPLES.md) - Example usage

