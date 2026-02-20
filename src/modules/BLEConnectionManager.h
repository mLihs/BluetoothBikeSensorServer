#pragma once

#include <NimBLEDevice.h>
#include <cstring>

#include "../BluetoothBikeSensorServer.h"

// v2.2.5: Fixed-size name buffer to prevent heap fragmentation from std::string
#define SENSOR_NAME_MAX_LEN 32

struct ActiveSensor {
  SensorType type;
  NimBLEAddress address;
  char name[SENSOR_NAME_MAX_LEN];  // v2.2.5: Fixed buffer instead of std::string (heap fragmentation fix)
  uint8_t mac[6];         // MAC address bytes for connection
  bool hasMac;            // Whether MAC bytes are valid
  uint8_t addrType;       // Address type (public/random)
  bool doConnect;         // Flag to trigger connection in loop
  bool useMacDirect;      // If true, connect directly by MAC address (skip scan)
  NimBLEClient *client;
  NimBLERemoteCharacteristic *characteristic;
  bool isConnected;
  bool hasTarget;  // Whether we want to connect to this sensor
  uint8_t reconnectFailureCount;  // Count of consecutive reconnection failures
  uint8_t batteryLevel;  // Battery level (0-100), 255 = unknown/not available
  bool hasBatteryService;  // Whether sensor has Battery Service
  NimBLERemoteCharacteristic *batteryCharacteristic;  // Battery Level characteristic
  int lastDisconnectReason;  // Last BLE disconnect reason code (0 = never disconnected)

  // v2.2.4: Explicit resolution tracking for guaranteed "only once" name matching
  bool hasResolvedAddress;   // True = MAC is locked in and trustworthy (use MAC-only matching)
  uint16_t nameCheckCount;   // Telemetry: count of name checks performed
  // Note: v2.2.4.3 removed lastNameCheckMs (rate-limiting was blocking valid sensors)

  ActiveSensor();
  
  // v2.2.5: Helper methods for name handling (heap-free)
  void setName(const char* newName);
  void clearName();
  const char* getName() const { return name; }
};

struct BLEConnectionManagerContext {
  bool* isInitialised;
  bool* preserveMacOnDisconnect;
  void (*startScan)();
  void (**sensorDisconnectedCallbacks)();
};

void BLEConnectionManagerSetContext(const BLEConnectionManagerContext& context);

void BLEConnectionManagerInitActiveSensors();
ActiveSensor* BLEConnectionManagerGetSensors();

int BLEConnectionManagerGetSensorIndex(SensorType type);
SensorType BLEConnectionManagerGetSensorTypeFromIndex(int index);

NimBLEClientCallbacks* BLEConnectionManagerCreateClientCallbacks(SensorType type);
