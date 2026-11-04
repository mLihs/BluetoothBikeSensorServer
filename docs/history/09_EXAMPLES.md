# Examples Documentation

This document describes all example sketches included with BluetoothBikeSensorServer.

---

## Example Overview

The library includes 8 example sketches demonstrating different use cases:

1. **HR_Only_Example** - Minimal heart rate sensor connection
2. **HR_CSC_Example** - Heart rate and speed/cadence sensors
3. **DiscoveryExample** - Sensor discovery functionality
4. **FastReconnectionExample** - Fast reconnection with MAC preservation
5. **IdentityExample** - Device identity and advertising configuration
6. **ManualSensorExample** - Advanced sensor management
7. **WebSocketStreamingExample** - WebSocket integration for real-time data
8. **WebUI_Integration** - Full WebUI integration example

---

## 1. HR_Only_Example

**Location**: `examples/HR_Only_Example/HR_Only_Example.ino`

**Purpose**: Minimal example connecting to a single Heart Rate sensor.

**Key Features**:
- Single sensor connection
- Basic data callback
- Per-type connection status callbacks

**Code Structure**:
```cpp
// Sensor name
const char* HR_SENSOR_NAME = "TICKR FIT C571";

// Data callback
void onHeartRateChanged(uint16_t hr) {
  LOGI_F("Heart rate: %u bpm", hr);
}

// Connection callbacks
void onSensorConnected() {
  LOGI("✓ Heart rate sensor connected!");
}

void onSensorDisconnected() {
  LOGW("✗ Heart rate sensor disconnected!");
}

void setup() {
  Serial.begin(115200);
  BluetoothBikeSensorServerBegin();
  
  // Set callbacks
  BluetoothBikeSensorServerSetHeartRateCallback(onHeartRateChanged);
  BikeSensorServerSetSensorConn(SENSOR_TYPE_HEART_RATE, onSensorConnected);
  BikeSensorServerSetSensorDisc(SENSOR_TYPE_HEART_RATE, onSensorDisconnected);
  
  // Add sensor
  BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, HR_SENSOR_NAME);
}

void loop() {
  BluetoothBikeSensorServerLoop();
  delay(100);
}
```

**Usage**:
1. Update `HR_SENSOR_NAME` with your sensor name
2. Upload sketch
3. Monitor Serial output for connection status and heart rate data

---

## 2. HR_CSC_Example

**Location**: `examples/HR_CSC_Example/HR_CSC_Example.ino`

**Purpose**: Example connecting to both Heart Rate and Speed/Cadence sensors.

**Key Features**:
- Multi-sensor support
- Per-sensor-type callbacks
- Data access methods
- Speed/cadence conversion

**Code Structure**:
```cpp
// Data callbacks
void onHeartRateChanged(uint16_t hr) {
  Serial.printf("HR: %u bpm\n", hr);
}

void onSpeedCadenceChanged(uint16_t speed, uint16_t cadence) {
  float speedKmh = speed / 100.0 * 3.6;
  Serial.printf("Speed: %.1f km/h, Cadence: %u rpm\n", speedKmh, cadence);
}

// Per-type connection callbacks
void onHRConnected() {
  Serial.println("HR sensor connected!");
}

void onCSCConnected() {
  Serial.println("CSC sensor connected!");
}

void setup() {
  Serial.begin(115200);
  BluetoothBikeSensorServerBegin();
  
  // Set data callbacks
  BluetoothBikeSensorServerSetHeartRateCallback(onHeartRateChanged);
  BluetoothBikeSensorServerSetSpeedCadenceCallback(onSpeedCadenceChanged);
  
  // Set per-type connection callbacks
  BikeSensorServerSetSensorConn(SENSOR_TYPE_HEART_RATE, onHRConnected);
  BikeSensorServerSetSensorConn(SENSOR_TYPE_SPEED_CADENCE, onCSCConnected);
  
  // Add sensors
  BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, "TICKR FIT");
  BikeSensorServerAddSensor(SENSOR_TYPE_SPEED_CADENCE, "Ridesense");
}

void loop() {
  BluetoothBikeSensorServerLoop();
  delay(100);
}
```

**Usage**:
1. Update sensor names with your sensor names
2. Upload sketch
3. Monitor Serial output for connection status and data

---

## 3. DiscoveryExample

**Location**: `examples/DiscoveryExample/DiscoveryExample.ino`

**Purpose**: Demonstrates the discovery feature to find and list available sensors.

**Key Features**:
- Async discovery mode
- Sensor listing and filtering
- Discovery callbacks
- Manual discovery restart

**Code Structure**:
```cpp
// Discovery complete callback
void onDiscoveryComplete(uint8_t count) {
  LOGI_F("=== Discovery complete: %u sensors found ===", count);
  
  // List all sensors
  LOGI("All sensors:");
  LOGI(BikeSensorServerListSensors(LIST_FORMAT_FULL));
  
  // List individual sensors
  for (uint8_t i = 0; i < count; i++) {
    SensorType type;
    char name[64];
    uint8_t mac[6];
    char macStr[18];
    if (BikeSensorServerGetSensor(i, &type, name, sizeof(name), mac)) {
      snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      LOGI_F("Sensor %u: Type=%d, Name=%s, MAC=%s", i, type, name, macStr);
    }
  }
  
  // Schedule next discovery
  nextDiscoveryStartMs = millis() + DISCOVERY_PAUSE_MS;
  discoveryScheduled = true;
}

void setup() {
  Serial.begin(115200);
  BluetoothBikeSensorServerBegin();
  
  // Set discovery callback
  BikeSensorServerSetDiscoveryCompleteCallback(onDiscoveryComplete);
  
  // Configure discovery
  BikeSensorServerSetDiscoveryDuration(15000);
  BikeSensorServerSetMaxSensors(50);
  
  // Start first discovery
  BikeSensorServerStartDiscovery(SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_POWER_METER);
}

void loop() {
  BluetoothBikeSensorServerLoop();
  
  // Restart discovery after pause
  if (discoveryScheduled && millis() >= nextDiscoveryStartMs) {
    discoveryScheduled = false;
    BikeSensorServerStartDiscovery(SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_POWER_METER);
  }
  
  delay(100);
}
```

**Usage**:
1. Upload sketch
2. Monitor Serial output for discovered sensors
3. Discovery automatically restarts after pause

---

## 4. FastReconnectionExample

**Location**: `examples/FastReconnectionExample/FastReconnectionExample.ino`

**Purpose**: Shows how to use Sensor MAC Preservation for faster reconnection.

**Key Features**:
- Fast reconnection configuration
- MAC address preservation
- Reconnection time comparison
- Connection state monitoring

**Code Structure**:
```cpp
void setup() {
  Serial.begin(115200);
  BluetoothBikeSensorServerBegin();
  
  // Enable fast reconnection
  BikeSensorServerSetPreserveMacOnDisconnect(true);
  
  // Set callbacks
  BluetoothBikeSensorServerSetHeartRateCallback(onHeartRateChanged);
  BikeSensorServerSetSensorConn(SENSOR_TYPE_HEART_RATE, onHRConnected);
  BikeSensorServerSetSensorDisc(SENSOR_TYPE_HEART_RATE, onHRDisconnected);
  
  // Add sensor
  BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, "TICKR FIT");
}

void loop() {
  BluetoothBikeSensorServerLoop();
  
  // Monitor connection state
  static bool lastConnected = false;
  bool connected = BikeSensorServerIsSensorConnected(SENSOR_TYPE_HEART_RATE);
  
  if (connected != lastConnected) {
    if (connected) {
      Serial.println("Sensor connected (fast reconnection enabled)");
    } else {
      Serial.println("Sensor disconnected - will attempt fast reconnection");
    }
    lastConnected = connected;
  }
  
  delay(100);
}
```

**Usage**:
1. Update sensor name
2. Upload sketch
3. Power off sensor to test reconnection
4. Monitor Serial output for reconnection time

---

## 5. IdentityExample

**Location**: `examples/IdentityExample/IdentityExample.ino`

**Purpose**: Demonstrates device identity and advertising configuration.

**Key Features**:
- Custom device name prefix
- Advertising identity management
- MAC-based naming
- Device name display

**Code Structure**:
```cpp
void setup() {
  Serial.begin(115200);
  
  // Set custom device name prefix
  BikeSensorServerSetDeviceNamePrefix("mydevice");
  
  // Initialize library
  BluetoothBikeSensorServerBegin();
  
  // Display device name
  Serial.printf("Device name: %s\n", 
                homewind::GetBleDeviceNameLower().c_str());
  
  // Add sensor
  BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, "TICKR FIT");
}

void loop() {
  BluetoothBikeSensorServerLoop();
  delay(100);
}
```

**Usage**:
1. Update device name prefix
2. Upload sketch
3. Check Serial output for device name
4. Verify device name in BLE scanner

---

## 6. ManualSensorExample

**Location**: `examples/ManualSensorExample/ManualSensorExample.ino`

**Purpose**: Advanced example with manual sensor management.

**Key Features**:
- Dynamic sensor addition/removal
- Runtime configuration
- Advanced callback usage
- Sensor status monitoring

**Code Structure**:
```cpp
void setup() {
  Serial.begin(115200);
  BluetoothBikeSensorServerBegin();
  
  // Set all callbacks
  setupCallbacks();
  
  // Add sensors dynamically
  addSensors();
}

void loop() {
  BluetoothBikeSensorServerLoop();
  
  // Monitor sensor status
  monitorSensorStatus();
  
  // Example: Remove sensor after 30 seconds
  static unsigned long startTime = millis();
  if (millis() - startTime > 30000) {
    Serial.println("Removing HR sensor...");
    BikeSensorServerRemoveSensor(SENSOR_TYPE_HEART_RATE);
    startTime = millis() + 30000; // Prevent repeated removal
  }
  
  delay(100);
}

void addSensors() {
  BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, "TICKR FIT");
  BikeSensorServerAddSensor(SENSOR_TYPE_SPEED_CADENCE, "Ridesense");
  BikeSensorServerAddSensor(SENSOR_TYPE_POWER_METER, "Power Meter");
}

void monitorSensorStatus() {
  // Check connection status for each sensor type
  if (BikeSensorServerIsSensorConnected(SENSOR_TYPE_HEART_RATE)) {
    char mac[18];
    if (BikeSensorServerGetSensorMac(SENSOR_TYPE_HEART_RATE, mac, sizeof(mac))) {
      Serial.printf("HR sensor MAC: %s\n", mac);
    }
  }
}
```

**Usage**:
1. Update sensor names
2. Upload sketch
3. Monitor Serial output for sensor management operations

---

## 7. WebSocketStreamingExample

**Location**: `examples/WebSocketStreamingExample/WebSocketStreamingExample.ino`

**Purpose**: Integration example with WebSocket for real-time data streaming.

**Key Features**:
- WebSocket integration
- Real-time data streaming
- Web UI integration
- CPU load monitoring

**Code Structure**:
```cpp
#include <BluetoothBikeSensorServer.h>
#include <ESPAsyncWebServer.h>
#include <WebSocketsServer.h>

WebSocketsServer webSocket(81);

void onHeartRateChanged(uint16_t hr) {
  // Stream to WebSocket clients
  String json = "{\"hr\":" + String(hr) + "}";
  webSocket.broadcastTXT(json);
}

void setup() {
  Serial.begin(115200);
  
  // Initialize library
  BluetoothBikeSensorServerBegin();
  
  // Set callbacks
  BluetoothBikeSensorServerSetHeartRateCallback(onHeartRateChanged);
  
  // Setup WebSocket
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  
  // Add sensor
  BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, "TICKR FIT");
}

void loop() {
  BluetoothBikeSensorServerLoop();
  webSocket.loop();
  delay(10);
}
```

**Usage**:
1. Update sensor name
2. Upload sketch
3. Connect to WebSocket from web client
4. Receive real-time sensor data

---

## 8. WebUI_Integration

**Location**: `examples/WebUI_Integration/WebUI_Integration.ino`

**Purpose**: Full WebUI integration example with sensor management.

**Key Features**:
- Web-based sensor configuration
- Real-time monitoring
- Discovery UI integration
- Full WebUI features

**Code Structure**:
```cpp
#include <BluetoothBikeSensorServer.h>
#include <Homewind.h>  // WebUI library

void setup() {
  Serial.begin(115200);
  
  // Initialize WebUI
  Homewind.begin();
  
  // Initialize sensor server
  BluetoothBikeSensorServerBegin();
  
  // Setup WebUI integration
  setupWebUI();
  
  // Add sensors
  BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, "TICKR FIT");
}

void loop() {
  BluetoothBikeSensorServerLoop();
  Homewind.loop();
  delay(10);
}
```

**Usage**:
1. Requires Homewind library
2. Update sensor names
3. Upload sketch
4. Access WebUI via browser
5. Configure sensors via web interface

---

## Common Patterns

### Minimal Setup

```cpp
#include <BluetoothBikeSensorServer.h>

void setup() {
  Serial.begin(115200);
  BluetoothBikeSensorServerBegin();
  BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, "TICKR FIT");
}

void loop() {
  BluetoothBikeSensorServerLoop();
  delay(100);
}
```

### With Callbacks

```cpp
void onHeartRateChanged(uint16_t hr) {
  Serial.printf("HR: %u bpm\n", hr);
}

void setup() {
  Serial.begin(115200);
  BluetoothBikeSensorServerBegin();
  BluetoothBikeSensorServerSetHeartRateCallback(onHeartRateChanged);
  BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, "TICKR FIT");
}

void loop() {
  BluetoothBikeSensorServerLoop();
  delay(100);
}
```

### Multi-Sensor

```cpp
void setup() {
  Serial.begin(115200);
  BluetoothBikeSensorServerBegin();
  
  BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, "TICKR FIT");
  BikeSensorServerAddSensor(SENSOR_TYPE_SPEED_CADENCE, "Ridesense");
  BikeSensorServerAddSensor(SENSOR_TYPE_POWER_METER, "Power Meter");
}

void loop() {
  BluetoothBikeSensorServerLoop();
  delay(100);
}
```

---

## Related Documentation

- [01_PROJECT_OVERVIEW.md](./01_PROJECT_OVERVIEW.md) - Project overview
- [03_FUNCTIONS.md](./03_FUNCTIONS.md) - Core functions
- [06_APIS.md](./06_APIS.md) - API reference

