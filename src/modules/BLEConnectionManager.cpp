#include "BLEConnectionManager.h"

#include "../BLETelemetry.h"
#include <cstring>

namespace {
ActiveSensor g_activeSensors[3];
BLEConnectionManagerContext g_context = {};

bool getPreserveMacOnDisconnect() {
  return g_context.preserveMacOnDisconnect ? *g_context.preserveMacOnDisconnect : true;
}

bool isInitialised() {
  return g_context.isInitialised ? *g_context.isInitialised : false;
}

void startScanIfAvailable() {
  if (g_context.startScan) {
    g_context.startScan();
  }
}

void callSensorDisconnectedCallback(int index) {
  if (g_context.sensorDisconnectedCallbacks && g_context.sensorDisconnectedCallbacks[index]) {
    g_context.sensorDisconnectedCallbacks[index]();
  }
}
}  // namespace

ActiveSensor::ActiveSensor()
    : type(SENSOR_TYPE_HEART_RATE),
      address((uint8_t*)"\0\0\0\0\0\0", 6),
      name{0},  // v2.2.5: Zero-initialize fixed buffer
      mac{0, 0, 0, 0, 0, 0},
      hasMac(false),
      addrType(0),
      doConnect(false),
      useMacDirect(false),
      client(nullptr),
      characteristic(nullptr),
      isConnected(false),
      hasTarget(false),
      reconnectFailureCount(0),
      batteryLevel(255),
      hasBatteryService(false),
      batteryCharacteristic(nullptr),
      lastDisconnectReason(0),
      hasResolvedAddress(false),
      nameCheckCount(0) {}

// v2.2.5: Heap-free name handling
void ActiveSensor::setName(const char* newName) {
  if (newName) {
    strncpy(name, newName, SENSOR_NAME_MAX_LEN - 1);
    name[SENSOR_NAME_MAX_LEN - 1] = '\0';
  } else {
    name[0] = '\0';
  }
}

void ActiveSensor::clearName() {
  name[0] = '\0';
}

void BLEConnectionManagerSetContext(const BLEConnectionManagerContext& context) {
  g_context = context;
}

void BLEConnectionManagerInitActiveSensors() {
  g_activeSensors[0].type = SENSOR_TYPE_HEART_RATE;
  g_activeSensors[1].type = SENSOR_TYPE_SPEED_CADENCE;
  g_activeSensors[2].type = SENSOR_TYPE_POWER_METER;
}

ActiveSensor* BLEConnectionManagerGetSensors() {
  return g_activeSensors;
}

int BLEConnectionManagerGetSensorIndex(SensorType type) {
  if (type == SENSOR_TYPE_HEART_RATE) return 0;
  if (type == SENSOR_TYPE_SPEED_CADENCE) return 1;
  if (type == SENSOR_TYPE_POWER_METER) return 2;
  return -1;
}

SensorType BLEConnectionManagerGetSensorTypeFromIndex(int index) {
  if (index == 0) return SENSOR_TYPE_HEART_RATE;
  if (index == 1) return SENSOR_TYPE_SPEED_CADENCE;
  if (index == 2) return SENSOR_TYPE_POWER_METER;
  return (SensorType)0;
}

class SensorClientCallbacks : public NimBLEClientCallbacks {
  SensorType sensorType;

 public:
  explicit SensorClientCallbacks(SensorType type) : sensorType(type) {}

  void onConnect(NimBLEClient* pclient) override {
    (void)pclient;
    int index = BLEConnectionManagerGetSensorIndex(sensorType);
    if (index >= 0) {
      const char* sensorTypeStr = "";
      switch (sensorType) {
        case SENSOR_TYPE_HEART_RATE: sensorTypeStr = "HR"; break;
        case SENSOR_TYPE_SPEED_CADENCE: sensorTypeStr = "CSC"; break;
        case SENSOR_TYPE_POWER_METER: sensorTypeStr = "Power"; break;
      }
      BLE_LIB_LOGI_F("[CLIENT] Connected to %s sensor.", sensorTypeStr);
    }
  }

  void onDisconnect(NimBLEClient* pclient, int reason) override {
    int index = BLEConnectionManagerGetSensorIndex(sensorType);
    if (index < 0) {
      return;
    }

    ActiveSensor& sensor = g_activeSensors[index];
    sensor.isConnected = false;
    sensor.lastDisconnectReason = reason;  // Store disconnect reason for API access

    const char* sensorTypeStr = "";
    switch (sensorType) {
      case SENSOR_TYPE_HEART_RATE: sensorTypeStr = "HR"; break;
      case SENSOR_TYPE_SPEED_CADENCE: sensorTypeStr = "CSC"; break;
      case SENSOR_TYPE_POWER_METER: sensorTypeStr = "Power"; break;
    }

    BLE_LIB_LOGI_F("[CLIENT] Disconnected from %s sensor (reason: 0x%02X = %s).",
                   sensorTypeStr, reason, BikeSensorServerGetDisconnectReasonString(reason));

    // Telemetry: Record sensor disconnection
    BLETelemetry::recordDisconnection();

    // Optionally preserve sensor MAC for faster reconnection
    // v2.2.7 FIX: For MAC-based sensors (useMacDirect=true), ALWAYS preserve MAC regardless of preserveMacOnDisconnect setting
    if (sensor.useMacDirect) {
      // MAC-based sensor: user explicitly set the MAC - ALWAYS preserve it
      sensor.doConnect = true;  // Trigger reconnection attempt
      BLE_LIB_LOGHOT_F("[CLIENT] MAC-based sensor: MAC preserved - will attempt direct reconnection");
    } else if (!getPreserveMacOnDisconnect()) {
      // Scan-based mode with preservation disabled: clear MAC and rescan
      sensor.hasMac = false;
      memset(sensor.mac, 0, sizeof(sensor.mac));
      sensor.doConnect = false;
      BLE_LIB_LOGHOT_F("[CLIENT] Sensor MAC cleared - will rescan for device");
    } else {
      // Scan-based mode with preservation enabled: preserve MAC for fast reconnection
      sensor.doConnect = true;  // Trigger reconnection attempt
      BLE_LIB_LOGHOT_F("[CLIENT] Sensor MAC preserved - will attempt direct reconnection");
    }

    // v2.2.5 FIX: DO NOT delete client on disconnect to prevent heap fragmentation!
    // The client will be reused for reconnection. Only clear transient state.
    // Client is only deleted when:
    // 1. Explicitly removing a sensor (BikeSensorServerRemoveSensor)
    // 2. BLE stack restart (BluetoothBikeSensorServerEnd)
    // 
    // This eliminates the main source of heap fragmentation from repeated
    // createClient/deleteClient cycles during reconnections.
    
    // Clear characteristic pointer (will be re-obtained on reconnect)
    sensor.characteristic = nullptr;
    sensor.batteryCharacteristic = nullptr;
    
    // Verify client pointer consistency (defensive check)
    if (sensor.client != pclient) {
      BLE_LIB_LOGW_F("[CLIENT] Warning: Client pointer mismatch in onDisconnect (stored=%p, callback=%p)",
                     sensor.client, pclient);
      // In case of mismatch, the callback's client is the one that disconnected.
      // We keep our stored client pointer as-is since it may still be valid.
    }

    // Call disconnect callback (only if BLE is still initialized)
    if (isInitialised()) {
      callSensorDisconnectedCallback(index);
    }

    // Restart scan only if MAC was cleared
    if (!getPreserveMacOnDisconnect() && isInitialised()) {
      startScanIfAvailable();
    }
  }
};

// v2.2.5: Static callbacks - allocated once, never freed
// This eliminates heap allocations from repeated new SensorClientCallbacks()
static SensorClientCallbacks* g_staticCallbacks[3] = {nullptr, nullptr, nullptr};

NimBLEClientCallbacks* BLEConnectionManagerCreateClientCallbacks(SensorType type) {
  int index = BLEConnectionManagerGetSensorIndex(type);
  if (index < 0 || index >= 3) {
    return nullptr;
  }
  
  // Create callback only once per sensor type (lazy initialization)
  if (!g_staticCallbacks[index]) {
    g_staticCallbacks[index] = new SensorClientCallbacks(type);
    BLE_LIB_LOGI_F("[CLIENT] Created static callbacks for sensor type %d", (int)type);
  }
  
  return g_staticCallbacks[index];
}
