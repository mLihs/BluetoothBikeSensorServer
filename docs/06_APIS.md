# API Reference

Complete API reference for BluetoothBikeSensorServer library.

---

## Initialization

### `void BluetoothBikeSensorServerBegin(const char *deviceNamePrefix = nullptr, uint8_t enabledSensorTypes = SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_SPEED_CADENCE | SENSOR_TYPE_POWER_METER, BBSConnectionMode connectionMode = BBS_MODE_SCAN, esp_power_level_t txPower = ESP_PWR_LVL_P9)`

Initialize the library.

**Parameters**:
- `deviceNamePrefix`: Optional device name prefix (default: "homewind"). Will be converted to lowercase.
- `enabledSensorTypes`: Bitmask of sensor types to enable (default: all)
- `connectionMode`: Connection mode - `BBS_MODE_SCAN` (default) for name-based scanning, or `BBS_MODE_MAC` for MAC Sleep-Scan connections
- `txPower`: BLE transmit power level (default: `ESP_PWR_LVL_P9`)

**Connection Modes**:
- `BBS_MODE_SCAN`: Scan-based mode - sensors are found by device name via BLE scanning. Use with `BikeSensorServerAddSensor()`.
- `BBS_MODE_MAC`: MAC Sleep-Scan mode - sensors are connected by MAC address using periodic short scans. Use with `BikeSensorServerAddSensorByMac()`.
  - Sleep-Scan timing is configurable via `BBS_SLEEP_SCAN_DURATION_MS` and `BBS_SLEEP_SCAN_INTERVAL_MS` in `BBSConfig.h`.

**Example**:
```cpp
// Default initialization (scan-based mode)
BluetoothBikeSensorServerBegin();

// Custom prefix, HR and Power only (scan-based)
BluetoothBikeSensorServerBegin("mydevice", 
                               SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_POWER_METER);

// MAC Sleep-Scan mode (periodic scan)
BluetoothBikeSensorServerBegin("mydevice", 
                               SENSOR_TYPE_HEART_RATE, 
                               BBS_MODE_MAC);
```

---

### `void BluetoothBikeSensorServerLoop()`

Must be called regularly in your `loop()` function to handle BLE operations.

**Example**:
```cpp
void loop() {
  BluetoothBikeSensorServerLoop();
  delay(100);
}
```

---

## Sensor Management

### `bool BikeSensorServerAddSensor(SensorType type, const char* sensorName)`

Add a sensor to connect to by device name (scan-based mode).

**Parameters**:
- `type`: Sensor type (`SENSOR_TYPE_HEART_RATE`, `SENSOR_TYPE_SPEED_CADENCE`, or `SENSOR_TYPE_POWER_METER`)
- `sensorName`: Device name to search for (exact match required)

**Returns**: `true` if sensor was added successfully

**Note**: Requires `BBS_MODE_SCAN` connection mode (default). The library will scan for devices matching the name.

**Example**:
```cpp
// Initialize in scan mode (default)
BluetoothBikeSensorServerBegin("mydevice", SENSOR_TYPE_HEART_RATE);
BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, "TICKR FIT");
```

---

### `bool BikeSensorServerAddSensorByMac(SensorType type, const char* sensorName, const char* macAddress, int8_t addrType)`
### `bool BikeSensorServerAddSensorByMac(SensorType type, const char* sensorName, const char* macAddress)`
### `bool BikeSensorServerAddSensorByMac(SensorType type, const char* macAddress)`

Add a sensor to connect by MAC address (MAC Sleep-Scan mode).

**Parameters**:
- `type`: Sensor type (`SENSOR_TYPE_HEART_RATE`, `SENSOR_TYPE_SPEED_CADENCE`, or `SENSOR_TYPE_POWER_METER`)
- `sensorName`: Optional device name (can be `nullptr` or empty, used for logging only)
- `macAddress`: MAC address in format "aa:bb:cc:dd:ee:ff" (case-insensitive)
- `addrType`: BLE address type (v2.2.10.0+):
  - `0` = Public address
  - `1` = Random address
  - `-1` = Auto-detect (not recommended - may not work for all devices)
  - **Recommended**: Run Discovery to find the correct type! Discovery output shows the value at the end: `[HR] TICKR (f0:13:c3:ee:56:97) 0` → use `0`

**Returns**: `true` if sensor was added successfully

**Note**: 
- Requires `BBS_MODE_MAC` connection mode. Scanning runs periodically in short windows (Sleep-Scan).
- The MAC address is preserved permanently - even after disconnections and connection failures, the system will continue attempting to reconnect.
- Ideal for fixed sensor setups where the MAC address is known.
- **v2.2.10.0+**: Use explicit `addrType` parameter for reliable connections. Run Discovery first to find the correct type (0 or 1).

**Example**:
```cpp
// Initialize in MAC Sleep-Scan mode
BluetoothBikeSensorServerBegin("mydevice", SENSOR_TYPE_HEART_RATE, BBS_MODE_MAC);

// Method 1: With explicit address type (RECOMMENDED)
// Discovery shows: [HR] TICKR FIT (f0:13:c3:ee:56:97) 0
BikeSensorServerAddSensorByMac(SENSOR_TYPE_HEART_RATE, nullptr, "F0:13:C3:EE:56:97", 0);

// Method 2: Auto-detect (may not work for all devices)
BikeSensorServerAddSensorByMac(SENSOR_TYPE_HEART_RATE, "F0:13:C3:EE:56:97");

// Method 3: Short form with auto-detect
BikeSensorServerAddSensorByMac(SENSOR_TYPE_HEART_RATE, "F0:13:C3:EE:56:97");
```

**v2.2.8.0 Fix**: In MAC Sleep-Scan mode, the MAC address is never deleted, even after multiple connection failures. This ensures sensors can reconnect after long periods of being offline.

**v2.2.10.0 Addition**: Discovery now shows the BLE address type (0 or 1) for each sensor. Use this value as the `addrType` parameter for reliable connections.

---

### `bool BikeSensorServerRemoveSensor(SensorType type)`

Remove a sensor and disconnect.

**Parameters**:
- `type`: Sensor type to remove

**Returns**: `true` if sensor was removed

**Example**:
```cpp
BikeSensorServerRemoveSensor(SENSOR_TYPE_HEART_RATE);
```

---

### `bool BikeSensorServerIsSensorConnected(SensorType type)`

Check if a specific sensor type is currently connected.

**Parameters**:
- `type`: Sensor type to check

**Returns**: `true` if connected

**Example**:
```cpp
if (BikeSensorServerIsSensorConnected(SENSOR_TYPE_HEART_RATE)) {
  Serial.println("HR sensor connected");
}
```

---

### `bool BikeSensorServerGetSensorMac(SensorType type, char* mac, size_t len)`

Get the MAC address of the active sensor.

**Parameters**:
- `type`: Sensor type
- `mac`: Output buffer for MAC address (must be at least 18 bytes)
- `len`: Size of output buffer

**Returns**: `true` if sensor is active and MAC was copied

**Example**:
```cpp
char mac[18];
if (BikeSensorServerGetSensorMac(SENSOR_TYPE_HEART_RATE, mac, sizeof(mac))) {
  Serial.printf("HR sensor MAC: %s\n", mac);
}
```

---

### `bool BikeSensorServerGetSensorName(SensorType type, char* name, size_t len)`

Get the name of the active sensor.

**Parameters**:
- `type`: Sensor type
- `name`: Output buffer for sensor name
- `len`: Size of output buffer

**Returns**: `true` if sensor is active and name was copied

**Example**:
```cpp
char name[64];
if (BikeSensorServerGetSensorName(SENSOR_TYPE_HEART_RATE, name, sizeof(name))) {
  Serial.printf("HR sensor name: %s\n", name);
}
```

---

### `bool BikeSensorServerGetBatteryLevel(SensorType type, uint8_t* batteryLevel)`

Get the battery level of the active sensor.

**Parameters**:
- `type`: Sensor type
- `batteryLevel`: Output parameter for battery level (0-100, 255 = unknown)

**Returns**: `true` if sensor is active and battery level is available

**Example**:
```cpp
uint8_t battery;
if (BikeSensorServerGetBatteryLevel(SENSOR_TYPE_HEART_RATE, &battery)) {
  if (battery != 255) {
    Serial.printf("HR sensor battery: %u%%\n", battery);
  }
}
```

---

## Data Access

### Heart Rate

#### `uint16_t BluetoothBikeSensorServerGetLastHeartRate()`

Get the last heart rate value.

**Returns**: Heart rate in bpm, or 0 if not available

**Example**:
```cpp
uint16_t hr = BluetoothBikeSensorServerGetLastHeartRate();
Serial.printf("Heart rate: %u bpm\n", hr);
```

---

#### `bool BluetoothBikeSensorServerHasHeartRate()`

Check if heart rate data is available.

**Returns**: `true` if heart rate data is valid

**Example**:
```cpp
if (BluetoothBikeSensorServerHasHeartRate()) {
  Serial.println("HR data available");
}
```

---

#### `uint32_t BluetoothBikeSensorServerGetLastHeartRateTimestamp()`

Get the timestamp of the last heart rate update.

**Returns**: Timestamp in milliseconds (from `millis()`)

---

### Speed/Cadence

#### `bool BluetoothBikeSensorServerGetLastSpeedCadence(uint16_t* speed, uint16_t* cadence)`

Get the last speed and cadence values.

**Parameters**:
- `speed`: Output parameter for speed (in units of 1/256 m/s)
- `cadence`: Output parameter for cadence (in rpm)

**Returns**: `true` if data is available

**Note**: Speed is in units of 1/256 m/s. To convert to km/h: `speed / 100.0 * 3.6`

**Example**:
```cpp
uint16_t speed, cadence;
if (BluetoothBikeSensorServerGetLastSpeedCadence(&speed, &cadence)) {
  float speedKmh = speed / 100.0 * 3.6;
  Serial.printf("Speed: %.1f km/h, Cadence: %u rpm\n", speedKmh, cadence);
}
```

---

#### `bool BluetoothBikeSensorServerHasSpeedCadence()`

Check if speed/cadence data is available.

**Returns**: `true` if data is valid

---

#### `uint32_t BluetoothBikeSensorServerGetLastSpeedCadenceTimestamp()`

Get the timestamp of the last speed/cadence update.

**Returns**: Timestamp in milliseconds

---

### Power Meter

#### `int16_t BluetoothBikeSensorServerGetLastPower()`

Get the last power value.

**Returns**: Power in watts, or 0 if not available

**Example**:
```cpp
int16_t power = BluetoothBikeSensorServerGetLastPower();
Serial.printf("Power: %d W\n", power);
```

---

#### `bool BluetoothBikeSensorServerHasPower()`

Check if power data is available.

**Returns**: `true` if power data is valid

---

#### `uint32_t BluetoothBikeSensorServerGetLastPowerTimestamp()`

Get the timestamp of the last power update.

**Returns**: Timestamp in milliseconds

---

## Callbacks

### Data Callbacks

#### `void BluetoothBikeSensorServerSetHeartRateCallback(void (*callback)(uint16_t heartRate))`

Set callback function for heart rate updates.

**Parameters**:
- `callback`: Function pointer: `void callback(uint16_t heartRate)`

**Example**:
```cpp
void onHeartRateChanged(uint16_t hr) {
  Serial.printf("Heart rate: %u bpm\n", hr);
}

BluetoothBikeSensorServerSetHeartRateCallback(onHeartRateChanged);
```

---

#### `void BluetoothBikeSensorServerSetSpeedCadenceCallback(void (*callback)(uint16_t speed, uint16_t cadence))`

Set callback function for speed/cadence updates.

**Parameters**:
- `callback`: Function pointer: `void callback(uint16_t speed, uint16_t cadence)`

**Example**:
```cpp
void onSpeedCadenceChanged(uint16_t speed, uint16_t cadence) {
  float speedKmh = speed / 100.0 * 3.6;
  Serial.printf("Speed: %.1f km/h, Cadence: %u rpm\n", speedKmh, cadence);
}

BluetoothBikeSensorServerSetSpeedCadenceCallback(onSpeedCadenceChanged);
```

---

#### `void BluetoothBikeSensorServerSetPowerCallback(void (*callback)(int16_t power))`

Set callback function for power updates.

**Parameters**:
- `callback`: Function pointer: `void callback(int16_t power)`

**Example**:
```cpp
void onPowerChanged(int16_t power) {
  Serial.printf("Power: %d W\n", power);
}

BluetoothBikeSensorServerSetPowerCallback(onPowerChanged);
```

---

### Sensor Connection Callbacks (Per Type)

#### `void BikeSensorServerSetSensorConn(SensorType type, void (*callback)())`

Set callback function for sensor connection (per sensor type).

**Parameters**:
- `type`: Sensor type
- `callback`: Function pointer: `void callback()`

**Example**:
```cpp
void onHRConnected() {
  Serial.println("HR sensor connected!");
}

BikeSensorServerSetSensorConn(SENSOR_TYPE_HEART_RATE, onHRConnected);
```

---

#### `void BikeSensorServerSetSensorDisc(SensorType type, void (*callback)())`

Set callback function for sensor disconnection (per sensor type).

**Parameters**:
- `type`: Sensor type
- `callback`: Function pointer: `void callback()`

**Example**:
```cpp
void onHRDisconnected() {
  Serial.println("HR sensor disconnected!");
}

BikeSensorServerSetSensorDisc(SENSOR_TYPE_HEART_RATE, onHRDisconnected);
```

---

### Central Device Callbacks

#### `void BikeSensorServerSetClientConn(void (*callback)(uint8_t centralCount))`

Set callback function for central device connection.

**Parameters**:
- `callback`: Function pointer: `void callback(uint8_t centralCount)`

**Example**:
```cpp
void onCentralConnected(uint8_t count) {
  Serial.printf("Central connected. Total: %u\n", count);
}

BikeSensorServerSetClientConn(onCentralConnected);
```

---

#### `void BikeSensorServerSetClientDisc(void (*callback)(uint8_t centralCount))`

Set callback function for central device disconnection.

**Parameters**:
- `callback`: Function pointer: `void callback(uint8_t centralCount)`

**Example**:
```cpp
void onCentralDisconnected(uint8_t count) {
  Serial.printf("Central disconnected. Remaining: %u\n", count);
}

BikeSensorServerSetClientDisc(onCentralDisconnected);
```

---

## Discovery Functions

### `void BikeSensorServerStartDiscovery(uint8_t sensorTypeMask)`

Start discovery mode to search for Bluetooth sensors.

**Parameters**:
- `sensorTypeMask`: Bitmask of sensor types to search for:
  - `SENSOR_TYPE_HEART_RATE` (0x01)
  - `SENSOR_TYPE_SPEED_CADENCE` (0x02)
  - `SENSOR_TYPE_POWER_METER` (0x04)
  - Can combine: `SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_POWER_METER`

**Example**:
```cpp
BikeSensorServerStartDiscovery(SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_POWER_METER);
```

---

### `void BikeSensorServerStopDiscovery()`

Manually stop discovery mode before timeout.

---

### `uint8_t BikeSensorServerGetSensorCount()`

Get total count of discovered sensors.

**Returns**: Number of discovered sensors

---

### `bool BikeSensorServerGetSensor(uint8_t index, SensorType* type, char* name, size_t nameLen, uint8_t mac[6])`

Get sensor information by index. The MAC address is returned as raw 6-byte array.

**Parameters**:
- `index`: Sensor index (0 to count-1)
- `type`: Output parameter for sensor type
- `name`: Output buffer for sensor name
- `nameLen`: Size of name buffer
- `mac`: Output buffer for MAC address

**Returns**: `true` if sensor found

**Example**:
```cpp
SensorType type;
char name[64];
uint8_t mac[6];
char macStr[18];
if (BikeSensorServerGetSensor(0, &type, name, sizeof(name), mac)) {
  snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.printf("Sensor: Type=%d, Name=%s, MAC=%s\n", type, name, macStr);
}
```

---

### `const char* BikeSensorServerListSensors(SensorListFormat format)`

Get formatted list of all discovered sensors.

**Parameters**:
- `format`: Format type:
  - `LIST_FORMAT_NAME` - Names only
  - `LIST_FORMAT_MAC` - MAC addresses only
  - `LIST_FORMAT_FULL` - Name, MAC, and address type (v2.2.10.0+)

**Returns**: Formatted string (static buffer, overwritten on next call)

**Example**:
```cpp
Serial.println(BikeSensorServerListSensors(LIST_FORMAT_FULL));
// Output (v2.2.10.0+):
// [HR] TICKR FIT (f0:13:c3:ee:56:97) 0
// [CSC] Ridesense (fc:a6:1f:27:45:16) 1
// The last number (0 or 1) is the BLE address type for use with BikeSensorServerAddSensorByMac()
```

**v2.2.10.0 Addition**: `LIST_FORMAT_FULL` now includes the BLE address type (0 = Public, 1 = Random) at the end of each line. This value can be used directly as the `addrType` parameter in `BikeSensorServerAddSensorByMac()`.

---

### `void BikeSensorServerSetDiscoveryCompleteCallback(void (*callback)(uint8_t count))`

Set callback function called when discovery completes.

**Parameters**:
- `callback`: Function pointer: `void callback(uint8_t count)`

**Example**:
```cpp
void onDiscoveryComplete(uint8_t count) {
  Serial.printf("Discovery complete: %u sensors found\n", count);
}

BikeSensorServerSetDiscoveryCompleteCallback(onDiscoveryComplete);
```

---

### `bool BikeSensorServerIsDiscoveryActive()`

Check if discovery mode is currently active.

**Returns**: `true` if discovery is running, `false` otherwise

**Example**:
```cpp
if (BikeSensorServerIsDiscoveryActive()) {
  Serial.println("Discovery in progress...");
}
```

---

### `bool BikeSensorServerIsScanning()`

Check if server-mode scanning is currently active.

**Returns**: `true` if scan is running, `false` otherwise

**Note**: In MAC Sleep-Scan mode (`BBS_MODE_MAC`), scanning runs in short windows. This may return `true` during an active scan cycle.

**Example**:
```cpp
if (BikeSensorServerIsScanning()) {
  Serial.println("Scanning for sensors...");
} else if (BikeSensorServerGetConnectionMode() == BBS_MODE_MAC) {
  Serial.println("MAC Sleep-Scan mode: periodic scanning");
}
```

---

### `void BikeSensorServerSetDiscoveryDuration(uint32_t durationMs)`

Set discovery duration in milliseconds.

**Parameters**:
- `durationMs`: Discovery duration (default: 20000 = 20 seconds)

---

### `void BikeSensorServerSetMaxSensors(uint8_t maxSensors)`

Set maximum number of sensors to discover.

**Parameters**:
- `maxSensors`: Maximum sensors (default: 12, max: 50)

---

## Runtime Configuration

### `void BikeSensorServerSetEnabledSensorTypes(uint8_t newEnabledSensorTypes)`

Dynamically change the enabled sensor types at runtime.

**Parameters**:
- `newEnabledSensorTypes`: Bitmask of sensor types to enable

**Note**: This will disconnect all connected central devices (forces them to rediscover).

**Example**:
```cpp
// Enable only HR and Power
BikeSensorServerSetEnabledSensorTypes(SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_POWER_METER);
```

---

### `void BikeSensorServerSetPreserveMacOnDisconnect(bool preserve)`

Enable/disable preserving sensor MAC address on disconnect for faster reconnection.

**Parameters**:
- `preserve`: `true` for faster reconnection (preserves MAC), `false` for standard reconnection (rescans)

**Default**: `true`

**Example**:
```cpp
BikeSensorServerSetPreserveMacOnDisconnect(true);  // Enable fast reconnection
```

---

### `void BikeSensorServerSetConnectionMode(BBSConnectionMode mode)`

Set the connection mode (scan-based or MAC Sleep-Scan).

**Parameters**:
- `mode`: Connection mode - `BBS_MODE_SCAN` or `BBS_MODE_MAC`

**Note**: Usually set during `BluetoothBikeSensorServerBegin()`, but can be changed at runtime.

**Example**:
```cpp
BikeSensorServerSetConnectionMode(BBS_MODE_MAC);  // Switch to MAC Sleep-Scan mode
```

---

### `BBSConnectionMode BikeSensorServerGetConnectionMode()`

Get the current connection mode.

**Returns**: Current connection mode (`BBS_MODE_SCAN` or `BBS_MODE_MAC`)

**Example**:
```cpp
BBSConnectionMode mode = BikeSensorServerGetConnectionMode();
if (mode == BBS_MODE_MAC) {
  Serial.println("Currently in MAC Sleep-Scan mode");
} else {
  Serial.println("Currently in scan-based mode");
}
```

**Use Case**: Check if system is in MAC Sleep-Scan mode, similar to `BikeSensorServerIsScanning()` for scan mode.

---

### `void BikeSensorServerTriggerReconnect()` (v2.2.13.0+)

Manually trigger an immediate reconnection attempt for disconnected sensors.

**Behavior by Mode**:
- **MAC Sleep-Scan mode (`BBS_MODE_MAC`)**: Resets the scan timer and starts an immediate Sleep-Scan
- **Scan mode (`BBS_MODE_SCAN`)**: Sets `doConnect = true` for disconnected sensors and starts scanning if not already running

**Properties**:
- **Non-blocking**: Returns immediately, connection happens asynchronously in `Loop()`
- **Safe**: Only affects disconnected sensors, connected sensors remain unaffected
- **Idempotent**: Multiple calls don't cause issues

**Example**:
```cpp
// Reconnect button handler
void onReconnectButtonPressed() {
  BikeSensorServerTriggerReconnect();
}

// WebSocket command handler
void handleWebSocketMessage(const char* msg) {
  if (strcmp(msg, "reconnect") == 0) {
    BikeSensorServerTriggerReconnect();
  }
}
```

**Use Case**: Implement a "Reconnect" button in UI to manually trigger reconnection without waiting for the automatic interval.

---

### `void BikeSensorServerSetDeviceNamePrefix(const char* prefix)`

Set the device name prefix for MAC-based identity.

**Parameters**:
- `prefix`: Device name prefix (will be converted to lowercase)

**Example**:
```cpp
BikeSensorServerSetDeviceNamePrefix("mydevice");  // Device name: "mydevice-a1b2"
```

---

### `void BikeSensorServerPause()`

Pause the BLE Relay Server (stops advertising).

**Use Case**: Useful when opening sensor configuration UI to prevent interference.

---

### `void BikeSensorServerResume()`

Resume the BLE Relay Server (restarts advertising).

---

### `bool BikeSensorServerIsAdvertising()`

Check if the BLE Relay Server is currently advertising.

**Returns**: `true` if advertising is active

---

## Central Device Status

### `bool BluetoothBikeSensorServerHasCentral()`

Check if any central devices are connected.

**Returns**: `true` if at least one central is connected

---

### `uint8_t BluetoothBikeSensorServerGetCentralCount()`

Get the number of connected central devices.

**Returns**: Number of connected centrals

---

### `void BluetoothBikeSensorServerCheckCentralConnections()`

Manually check central connections and trigger callbacks if changed.

**Note**: Called automatically in `BluetoothBikeSensorServerLoop()`.

---

## Sensor Type Definitions

### `enum SensorType`

```cpp
enum SensorType {
  SENSOR_TYPE_HEART_RATE = 0x01,      // Heart Rate sensor
  SENSOR_TYPE_SPEED_CADENCE = 0x02,   // Speed/Cadence sensor
  SENSOR_TYPE_POWER_METER = 0x04      // Power Meter sensor
};
```

---

### `enum SensorListFormat`

```cpp
enum SensorListFormat {
  LIST_FORMAT_NAME,   // Names only
  LIST_FORMAT_MAC,    // MAC addresses only
  LIST_FORMAT_FULL     // Name and MAC
};
```

---

## Related Documentation

- [03_FUNCTIONS.md](./03_FUNCTIONS.md) - Core functions and workflows
- [04_COMPONENTS.md](./04_COMPONENTS.md) - Component documentation
- [09_EXAMPLES.md](./09_EXAMPLES.md) - Example usage

