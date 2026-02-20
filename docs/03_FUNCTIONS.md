# Core Functions and Workflows

This document describes the core functions and workflows in BluetoothBikeSensorServer.

---

## Initialization Workflow

### Library Initialization

**Function**: `BluetoothBikeSensorServerBegin()`

**Workflow**:
1. Initialize NimBLE device
2. Set device name prefix (default: "homewind")
3. Generate MAC-based device name: `{prefix}-xxxxxxxx`
4. Set connection mode (scan-based or MAC Sleep-Scan)
5. Create GATT server with enabled services
6. Setup advertising with device name and services
7. Initialize active sensor array
8. Set BLE transmit power
9. **Note**: Scan is NOT automatically started (v2.2.7.0+) - it starts only when needed

**Parameters**:
- `deviceNamePrefix`: Optional device name prefix (default: "homewind")
- `enabledSensorTypes`: Bitmask of sensor types to enable (default: all)
- `connectionMode`: Connection mode - `BBS_MODE_SCAN` (default) or `BBS_MODE_MAC` (v2.2.13.0+)
- `txPower`: BLE transmit power level (default: `ESP_PWR_LVL_P9`)

**Connection Modes**:
- `BBS_MODE_SCAN` (default): Scan-based mode - use with `BikeSensorServerAddSensor()`
- `BBS_MODE_MAC`: MAC Sleep-Scan mode - use with `BikeSensorServerAddSensorByMac()` (v2.2.13.0+)
  - Sleep-Scan timing is configurable via `BBS_SLEEP_SCAN_DURATION_MS` and `BBS_SLEEP_SCAN_INTERVAL_MS` in `BBSConfig.h`

**Example**:
```cpp
// Default initialization (all sensors enabled, scan-based mode)
BluetoothBikeSensorServerBegin();

// Custom prefix, HR and Power only (scan-based)
BluetoothBikeSensorServerBegin("mydevice", 
                               SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_POWER_METER);

// MAC Sleep-Scan mode (periodic scan)
BluetoothBikeSensorServerBegin("mydevice", 
                               SENSOR_TYPE_HEART_RATE, 
                               BBS_MODE_MAC);
```

### Main Loop

**Function**: `BluetoothBikeSensorServerLoop()`

**Workflow**:
1. Handle sensor connections (check `doConnect` flags)
2. Process discovery mode (if active)
3. Check central connections
4. Handle reconnection attempts
5. Update advertising if needed

**Must be called regularly** in your `loop()` function.

**Example**:
```cpp
void loop() {
  BluetoothBikeSensorServerLoop();
  delay(100);  // Small delay to prevent watchdog issues
}
```

---

## Connection Workflow

### Connection Modes

The library supports two connection modes:

1. **Scan-Based Mode** (`BBS_MODE_SCAN` - default): Sensors are found by device name via BLE scanning
2. **MAC Sleep-Scan Mode** (`BBS_MODE_MAC`): Sensors are connected by MAC address using periodic short scans

The connection mode is set during initialization via `BluetoothBikeSensorServerBegin()`.

### Adding a Sensor (Scan-Based Mode)

**Function**: `BikeSensorServerAddSensor()`

**Workflow**:
1. Validate sensor type
2. Store sensor name in `ActiveSensor.name`
3. Set `hasTarget = true`
4. Start BLE scan if not already scanning
5. Scan callback matches device name
6. Store `advertisedDevice` and MAC address
7. Set `doConnect = true` flag
8. Main loop calls `connectAndSubscribeSensor()`
9. Connect to sensor and subscribe to notifications

**Parameters**:
- `type`: Sensor type (`SENSOR_TYPE_HEART_RATE`, `SENSOR_TYPE_SPEED_CADENCE`, or `SENSOR_TYPE_POWER_METER`)
- `sensorName`: Device name to search for (exact match required)

**Returns**: `true` if sensor was added successfully

**Example**:
```cpp
// Initialize in scan mode (default)
BluetoothBikeSensorServerBegin("mydevice", SENSOR_TYPE_HEART_RATE);
BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, "TICKR FIT");
```

### Adding a Sensor (MAC Sleep-Scan Mode)

**Function**: `BikeSensorServerAddSensorByMac()`

**Workflow**:
1. Validate sensor type and MAC address format
2. Parse MAC address string to binary format
3. Store MAC address in `ActiveSensor.address` and mark `hasResolvedAddress = true`
4. Set `hasTarget = true`, `useMacDirect = true`, `doConnect = false`
5. Sleep-Scan runs periodically and looks for the exact MAC
6. On MAC match, scan callback sets `doConnect = true`
7. Main loop calls `connectAndSubscribeSensor()`
8. Connect to sensor using MAC address and subscribe to notifications

**Parameters**:
- `type`: Sensor type (`SENSOR_TYPE_HEART_RATE`, `SENSOR_TYPE_SPEED_CADENCE`, or `SENSOR_TYPE_POWER_METER`)
- `macAddress`: MAC address in format "aa:bb:cc:dd:ee:ff" (case-insensitive)

**Returns**: `true` if sensor was added successfully

**Example**:
```cpp
// Initialize in MAC Sleep-Scan mode
BluetoothBikeSensorServerBegin("mydevice", SENSOR_TYPE_HEART_RATE, BBS_MODE_MAC);
BikeSensorServerAddSensorByMac(SENSOR_TYPE_HEART_RATE, "F0:13:C3:EE:56:97");
```

**Advantages of MAC Sleep-Scan Mode**:
- ✅ Periodic short scans reduce power and avoid long connect timeouts
- ✅ Connect only after MAC was seen (more reliable)
- ✅ MAC address is permanently preserved (v2.2.8.0+)
- ✅ Sensors reconnect even after long offline periods
- ✅ Ideal for fixed sensor setups with known MAC addresses

**v2.2.8.0 Fix**: In MAC Sleep-Scan mode, the MAC address is never deleted, even after multiple connection failures. This ensures sensors can reconnect after being offline for extended periods (e.g., overnight).

### Connection Process

**Internal Function**: `connectAndSubscribeSensor()`

**Workflow**:
1. Check if sensor has target (`hasTarget`)
2. Check if connection triggered (`doConnect`)
3. Create BLE client if needed
4. Set connection parameters (12, 24, 0, 200)
5. Connect to sensor using `advertisedDevice`
6. Discover services and characteristics
7. Subscribe to characteristic notifications
8. Read battery level (if available)
9. Set `isConnected = true`
10. Call connection callback

**Connection Parameters**:
- Min interval: 12 (15ms)
- Max interval: 24 (30ms)
- Latency: 0
- Timeout: 200 (2 seconds)

### Removing a Sensor

**Function**: `BikeSensorServerRemoveSensor()`

**Workflow**:
1. Validate sensor type
2. Disconnect BLE client
3. Clear sensor state (`hasTarget = false`)
4. Clear MAC address and `advertisedDevice`
5. Call disconnection callback
6. Update advertising if needed

**Returns**: `true` if sensor was removed

**Example**:
```cpp
BikeSensorServerRemoveSensor(SENSOR_TYPE_HEART_RATE);
```

---

## Reconnection Handling

### Standard Reconnection (Scan-Based Mode, Default)

**Workflow**:
1. Sensor disconnects (callback: `onDisconnect()`)
2. Clear sensor MAC address (`macString.clear()`)
3. Delete `advertisedDevice` object
4. Set `doConnect = false`
5. Restart BLE scan
6. Scan finds device by name
7. Store `advertisedDevice` and MAC
8. Set `doConnect = true`
9. Main loop connects to sensor

**Reconnection Time**: ~5-20 seconds (depends on scan duration)

### Fast Reconnection (MAC Preservation in Scan Mode)

**Enable**: `BikeSensorServerSetPreserveMacOnDisconnect(true)`

**Workflow**:
1. Sensor disconnects (callback: `onDisconnect()`)
2. **Preserve** sensor MAC address and `advertisedDevice`
3. Set `doConnect = true` (trigger reconnection)
4. Main loop attempts direct connection
5. If successful: ~1 second reconnection time
6. If failed: Falls back to scan-based reconnection (after 3 attempts)

**Reconnection Time**: ~1 second (if successful)

**Use Cases**:
- Sensors with stable MAC addresses (not privacy/random)
- Faster reconnection required
- Sensors that disconnect/reconnect frequently

**Example**:
```cpp
// Enable fast reconnection
BikeSensorServerSetPreserveMacOnDisconnect(true);

// Add sensor
BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, "TICKR FIT");
```

### MAC Sleep-Scan Mode Reconnection (v2.2.13.0+)

**Mode**: `BBS_MODE_MAC` (set during `BluetoothBikeSensorServerBegin()`)

**Workflow**:
1. Sensor disconnects (callback: `onDisconnect()`)
2. **MAC address is ALWAYS preserved** (regardless of `preserveMacOnDisconnect` setting)
3. Sleep-Scan runs periodically and looks for the exact MAC
4. On MAC match, scan callback sets `doConnect = true`
5. Main loop attempts direct connection
6. If connection fails: MAC remains preserved, system retries on next scan cycle

**Reconnection Time**: Sleep-Scan interval + connection time (~1-2 seconds)

**Key Features**:
- ✅ MAC address is permanently preserved (never deleted)
- ✅ Works even after long offline periods (e.g., overnight)
- ✅ Periodic scan avoids long blocking connects
- ✅ Ideal for fixed sensor setups

**v2.2.8.0 Fix**: Previously, after 3 connection failures, the MAC address was deleted, causing sensors to never reconnect. Now, the MAC is always preserved in MAC Sleep-Scan mode.

**Example**:
```cpp
// Initialize in MAC Sleep-Scan mode
BluetoothBikeSensorServerBegin("mydevice", SENSOR_TYPE_HEART_RATE, BBS_MODE_MAC);
BikeSensorServerAddSensorByMac(SENSOR_TYPE_HEART_RATE, "F0:13:C3:EE:56:97");
// MAC will be preserved forever, even after disconnections
```

### Manual Reconnect Trigger (v2.2.13.0+)

**Function**: `BikeSensorServerTriggerReconnect()`

**Purpose**: Manually trigger an immediate reconnection attempt for disconnected sensors. Useful for implementing a "Reconnect" button in UI.

**Workflow**:
1. Check if any sensor is disconnected
2. If no disconnected sensors: Log message and return
3. **MAC Sleep-Scan mode**:
   - Reset scan timer (`g_lastSleepScanMs = 0`)
   - Start immediate Sleep-Scan
4. **Scan mode**:
   - Set `doConnect = true` for disconnected sensors
   - Start scan if not already running

**Behavior by Mode**:

| Mode | Action |
|------|--------|
| `BBS_MODE_MAC` | Resets scan timer + starts immediate Sleep-Scan |
| `BBS_MODE_SCAN` | Starts/restarts scanning if not already running |

**Properties**:
- **Non-blocking**: Returns immediately, connection happens asynchronously in `Loop()`
- **Safe**: Only affects disconnected sensors, connected sensors remain unaffected
- **Idempotent**: Multiple calls don't cause issues

**Example**:
```cpp
// Button handler in UI
void onReconnectButtonPressed() {
  BikeSensorServerTriggerReconnect();
  Serial.println("Reconnect triggered!");
}

// WebSocket command handler
void handleWebSocketMessage(const char* message) {
  if (strcmp(message, "reconnect") == 0) {
    BikeSensorServerTriggerReconnect();
  }
}
```

---

## Discovery Workflow

### Starting Discovery

**Function**: `BikeSensorServerStartDiscovery()`

**Workflow**:
1. Save current server mode state (scanning, advertising)
2. Clear all previously discovered sensors
3. Set discovery type mask
4. Pause normal mode operations
5. Start discovery scan with type filter
6. Scan callback checks for matching services
7. Store discovered sensors in `g_discoveredSensors[]`
8. Discovery completes after timeout or manual stop
9. Call discovery complete callback
10. Restore server mode state (if not manually paused)

**Parameters**:
- `sensorTypeMask`: Bitmask of sensor types to search for:
  - `SENSOR_TYPE_HEART_RATE` (0x01)
  - `SENSOR_TYPE_SPEED_CADENCE` (0x02)
  - `SENSOR_TYPE_POWER_METER` (0x04)
  - Can combine: `SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_POWER_METER`

**Example**:
```cpp
// Discover HR and Power sensors
BikeSensorServerStartDiscovery(SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_POWER_METER);
```

### Discovery Configuration

**Duration**: `BikeSensorServerSetDiscoveryDuration(uint32_t durationMs)`
- Default: 20000 (20 seconds)
- Minimum: 1000 (1 second)
- Maximum: 60000 (60 seconds)

**Max Sensors**: `BikeSensorServerSetMaxSensors(uint8_t maxSensors)`
- Default: 12
- Maximum: 50

**Example**:
```cpp
// Set discovery to 15 seconds, max 50 sensors
BikeSensorServerSetDiscoveryDuration(15000);
BikeSensorServerSetMaxSensors(50);
```

### Discovery Complete Callback

**Function**: `BikeSensorServerSetDiscoveryCompleteCallback()`

**Workflow**:
1. Discovery completes (timeout or manual stop)
2. Library calls callback with sensor count
3. User can list sensors, filter by type, etc.
4. User can start new discovery if needed

**Example**:
```cpp
void onDiscoveryComplete(uint8_t count) {
  Serial.printf("Found %u sensors\n", count);
  
  // List all sensors
  Serial.println(BikeSensorServerListSensors(LIST_FORMAT_FULL));
  
  // Start new discovery
  BikeSensorServerStartDiscovery(SENSOR_TYPE_HEART_RATE);
}

BikeSensorServerSetDiscoveryCompleteCallback(onDiscoveryComplete);
```

### Accessing Discovered Sensors

**Get Count**: `BikeSensorServerGetSensorCount()`
- Returns total count of discovered sensors

**Get Sensor**: `BikeSensorServerGetSensor(uint8_t index, ...)`
- Returns sensor information by index (MAC returned as raw 6-byte array)

**List Sensors**: `BikeSensorServerListSensors(SensorListFormat format)`
- Returns formatted string with all sensors

**Example**:
```cpp
uint8_t count = BikeSensorServerGetSensorCount();
for (uint8_t i = 0; i < count; i++) {
  SensorType type;
  char name[64];
  uint8_t mac[6];
  char macStr[18];
  if (BikeSensorServerGetSensor(i, &type, name, sizeof(name), mac)) {
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.printf("Sensor %u: Type=%d, Name=%s, MAC=%s\n", i, type, name, macStr);
  }
}
```

---

## Data Relay Workflow

### Sensor Data Flow

**Workflow**:
1. Sensor sends notification via BLE Client
2. Characteristic callback receives data
3. Parse sensor data (HR, CSC, or Power)
4. Update internal state (last value, timestamp)
5. Call user callback (if registered)
6. Mirror data to GATT server characteristic
7. Send notification to connected centrals

### Data Parsing

**Heart Rate**: `parseAndLogHR()`
- Parses 8-bit or 16-bit HR value
- Optional RR intervals (if present)
- Updates `lastHeartRate`, `heartRateValid`, `lastHeartRateTimestamp`

**Speed/Cadence**: `parseAndLogCSC()`
- Parses cumulative wheel/crank revolutions
- Calculates speed and cadence
- Updates `speedCadenceData` structure

**Power Meter**: `parseAndLogPower()`
- Parses power value (watts)
- Optional torque/crank data (if present)
- Updates `powerData` structure

### Data Mirroring

**Function**: `mirrorHeartRatePacket()`, `mirrorCSCPacket()`, `mirrorPowerPacket()`

**Workflow**:
1. Update GATT server characteristic value
2. Check if centrals are connected
3. Send notification to all connected centrals
4. Track notification success/failure (for debugging)

**Note**: Notifications only sent if centrals have enabled CCCD (Client Characteristic Configuration Descriptor).

---

## GATT Server Management

### Server Creation

**Function**: `setupLocalBLEServer()`

**Workflow**:
1. Create NimBLE server
2. Create services for enabled sensor types:
   - HR Service (0x180D) if `SENSOR_TYPE_HEART_RATE` enabled
   - CSC Service (0x1816) if `SENSOR_TYPE_SPEED_CADENCE` enabled
   - Power Service (0x1818) if `SENSOR_TYPE_POWER_METER` enabled
3. Create characteristics with NOTIFY property
4. Start services
5. Setup advertising with device name and services
6. Start advertising

### Advertising Management

**Update Services**: `updateAdvertisingServices()`

**Workflow**:
1. Stop advertising (if running)
2. Rebuild advertising data from scratch
3. Add service UUIDs for enabled sensor types
4. Set device name
5. Restart advertising (if was running)

**Update Name**: `refreshAdvertisingName()`

**Workflow**:
1. Get MAC-based device name from `HomewindBleIdentity`
2. Update advertising name if changed
3. Restart advertising if needed

### Runtime Configuration

**Enable/Disable Sensor Types**: `BikeSensorServerSetEnabledSensorTypes()`

**Workflow**:
1. Update `enabledSensorTypes` bitmask
2. Recreate GATT server services
3. Update advertising services
4. Disconnect all centrals (force rediscovery)
5. Restart advertising

**Example**:
```cpp
// Enable only HR and Power
BikeSensorServerSetEnabledSensorTypes(SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_POWER_METER);
```

### Pause/Resume

**Pause**: `BikeSensorServerPause()`
- Stops advertising
- Saves current state
- Useful when opening sensor configuration UI

**Resume**: `BikeSensorServerResume()`
- Restarts advertising (if was running)
- Restores saved state

**Example**:
```cpp
// Pause when opening UI
BikeSensorServerPause();

// ... configure sensors ...

// Resume after closing UI
BikeSensorServerResume();
```

---

## Device Identity Workflow

### MAC-Based Naming

**Function**: `HomewindBleIdentity::GetBleDeviceNameLower()`

**Workflow**:
1. Read stable base identity (eFuse MAC lower 32-bit, WiFi-style)
2. Convert to hex string (lowercase)
3. Format: `{prefix}-xxxxxxxx`
4. Example: "homewind-da4eb580"

### Setting Device Name Prefix

**Function**: `BikeSensorServerSetDeviceNamePrefix()`

**Workflow**:
1. Set prefix in `HomewindBleIdentity`
2. Convert to lowercase
3. Update advertising name
4. Restart advertising

**Example**:
```cpp
BikeSensorServerSetDeviceNamePrefix("mydevice");
// Device name: "mydevice-da4eb580"
```

---

## Central Device Management

### Connection Tracking

**Function**: `BluetoothBikeSensorServerCheckCentralConnections()`

**Workflow**:
1. Get current central count from server
2. Compare with last known count
3. Call connection/disconnection callbacks if changed
4. Update last known count

**Called automatically** in `BluetoothBikeSensorServerLoop()`.

### Central Callbacks

**Connection**: `BikeSensorServerSetClientConn()`
- Called when central connects
- Parameter: current central count

**Disconnection**: `BikeSensorServerSetClientDisc()`
- Called when central disconnects
- Parameter: remaining central count

**Example**:
```cpp
void onCentralConnected(uint8_t count) {
  Serial.printf("Central connected. Total: %u\n", count);
}

void onCentralDisconnected(uint8_t count) {
  Serial.printf("Central disconnected. Remaining: %u\n", count);
}

BikeSensorServerSetClientConn(onCentralConnected);
BikeSensorServerSetClientDisc(onCentralDisconnected);
```

---

## Related Documentation

- [02_ARCHITECTURE.md](./02_ARCHITECTURE.md) - System architecture
- [04_COMPONENTS.md](./04_COMPONENTS.md) - Component details
- [05_STATE_MACHINES.md](./05_STATE_MACHINES.md) - State machines
- [06_APIS.md](./06_APIS.md) - Complete API reference

