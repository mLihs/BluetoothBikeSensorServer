#pragma once

/**
 * SensorRegistry.h - Zentrale Sensor-Typ-Konfiguration
 *
 * Jeder kompilierte Sensortyp wird EINMAL in SENSOR_REGISTRY definiert.
 * Alle switch(type)-Blöcke verwenden stattdessen getSensorConfig(type).
 *
 * Neuen Sensortyp hinzufügen:
 *   1. SensorType-Enum erweitern (BluetoothBikeSensorServer.h)
 *   2. BBS_COMPILE_* Flag in BBSConfig.h hinzufügen
 *   3. Parse/Mirror-Funktionen implementieren
 *   4. EINEN Eintrag in SENSOR_REGISTRY (BluetoothBikeSensorServer.cpp) hinzufügen - fertig!
 */

#include "../BluetoothBikeSensorServer.h"
#include "../config/BBSConfig.h"

/**
 * Configuration entry for one sensor type.
 * Stored in flash (const) - zero RAM cost.
 */
struct SensorTypeConfig {
  SensorType type;           // e.g. SENSOR_TYPE_HEART_RATE
  uint8_t    typeBit;        // Same as type (bitmask for enabledSensorTypes)
  const char* shortName;     // "HR", "CSC", "Power"
  const char* serviceName;   // "0x180D (HR)" - for log messages
  uint16_t   serviceUUID16;  // 0x180D
  uint16_t   charUUID16;     // 0x2A37
  void (*parseFunc)(const uint8_t*, size_t);
  void (*mirrorFunc)(const uint8_t*, size_t);
};
