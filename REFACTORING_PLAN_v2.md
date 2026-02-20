# Refactoring-Plan: BluetoothBikeSensorServer v2

**Datum**: 2026-02-04  
**Ziele**:
1. **Höhere Modularisierung** - Neue Sensortypen einfacher hinzufügen, Compile-Time Exclusion via `#define`
2. **Geringere Speicherauslastung** - Flash (Code-Duplikate) und RAM reduzieren
3. **Weniger Fragmentierung** - Dynamische Allokationen minimieren

**Basierend auf**: Verifizierte `DEEP_COMPLEXITY_ANALYSIS.md` + erweiterte Analyse

---

## Executive Summary

| Phase | Maßnahme | Flash-Ersparnis | RAM-Ersparnis | Risiko |
|-------|----------|-----------------|---------------|--------|
| 1 | Compile-Time Sensor Flags | ~2-4 KB pro deaktiviertem Typ | ~200 Bytes pro Typ | ⚠️ Niedrig |
| 2 | SensorConfig Registry | ~400 Bytes | - | ⚠️ Niedrig |
| 3 | Mirror-Funktion Template | ~200 Bytes | - | ✅ Sehr niedrig |
| 4 | Scan-Konfig zentralisieren | ~150 Bytes | - | ✅ Sehr niedrig |
| 5 | String → char[] | - | ~100 Bytes | ⚠️ Niedrig |

---

## Phase 1: Compile-Time Sensor Type Flags (PRIORITÄT HOCH)

### 1.1 Problem

Aktuell werden alle 3 Sensortypen (HR, CSC, Power) immer kompiliert, auch wenn sie nicht benötigt werden:
- Globale Variablen: `hrServiceLocal`, `cscServiceLocal`, `powerServiceLocal` (+ Chars)
- Mirror-Funktionen: `mirrorHeartRatePacket()`, `mirrorCSCPacket()`, `mirrorPowerPacket()`
- Parse-Funktionen, Callbacks, Service-Erstellung

### 1.2 Lösung: Feature-Flags in BBSConfig.h

**Datei**: `src/config/BBSConfig.h`

```cpp
// ============================================================================
// SENSOR TYPE FLAGS - Compile-Time Configuration
// ============================================================================
// Set to 0 to completely exclude a sensor type from compilation.
// This saves Flash (~2-4KB per type) and RAM (~200 bytes per type).
// Note: Runtime enabledSensorTypes mask is still respected if compiled in.

#ifndef BBS_COMPILE_HEART_RATE
  #define BBS_COMPILE_HEART_RATE 1
#endif

#ifndef BBS_COMPILE_CSC
  #define BBS_COMPILE_CSC 1
#endif

#ifndef BBS_COMPILE_POWER
  #define BBS_COMPILE_POWER 1
#endif

// Derived: Count of compiled sensor types (for array sizing)
#define BBS_COMPILED_SENSOR_COUNT \
  (BBS_COMPILE_HEART_RATE + BBS_COMPILE_CSC + BBS_COMPILE_POWER)
```

### 1.3 Anwendung in BluetoothBikeSensorServer.cpp

**Globale Variablen** (Zeilen 162-167):
```cpp
NimBLEServer *server = nullptr;

#if BBS_COMPILE_HEART_RATE
NimBLEService *hrServiceLocal = nullptr;
NimBLECharacteristic *hrCharLocal = nullptr;
#endif

#if BBS_COMPILE_CSC
NimBLEService *cscServiceLocal = nullptr;
NimBLECharacteristic *cscCharLocal = nullptr;
#endif

#if BBS_COMPILE_POWER
NimBLEService *powerServiceLocal = nullptr;
NimBLECharacteristic *powerCharLocal = nullptr;
#endif
```

**Mirror-Funktionen** (Zeilen 247-308):
```cpp
#if BBS_COMPILE_HEART_RATE
void mirrorHeartRatePacket(const uint8_t *data, size_t len) {
  // ... existing code
}
#endif

#if BBS_COMPILE_CSC
void mirrorCSCPacket(const uint8_t *data, size_t len) {
  // ... existing code
}
#endif

#if BBS_COMPILE_POWER
void mirrorPowerPacket(const uint8_t *data, size_t len) {
  // ... existing code
}
#endif
```

**Service-Erstellung** (Zeilen 676-701):
```cpp
void setupLocalBLEServer() {
  // ... logging ...
  server = NimBLEDevice::createServer();

#if BBS_COMPILE_HEART_RATE
  if (enabledSensorTypes & SENSOR_TYPE_HEART_RATE) {
    hrServiceLocal = server->createService(HR_SERVICE_UUID);
    hrCharLocal = hrServiceLocal->createCharacteristic(HR_MEAS_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);
    hrServiceLocal->start();
    BLE_LIB_LOGI_F("[SERVER] Heart Rate service created");
  }
#endif

#if BBS_COMPILE_CSC
  if (enabledSensorTypes & SENSOR_TYPE_SPEED_CADENCE) {
    // ... CSC service creation
  }
#endif

#if BBS_COMPILE_POWER
  if (enabledSensorTypes & SENSOR_TYPE_POWER_METER) {
    // ... Power service creation
  }
#endif
  // ...
}
```

### 1.4 Risiko-Minimierung

- **Backward-compatible**: Alle Flags default auf `1` (wie bisher)
- **Runtime-Check bleibt**: `enabledSensorTypes` Bitmask funktioniert weiterhin
- **Inkrementell**: Jeder Sensor kann einzeln migriert werden

### 1.5 Betroffene Stellen (vollständige Liste)

| Datei | Zeilen | Was |
|-------|--------|-----|
| BluetoothBikeSensorServer.cpp | 28-33 | UUIDs |
| BluetoothBikeSensorServer.cpp | 162-167 | Service/Char Pointer |
| BluetoothBikeSensorServer.cpp | 180-189 | Data structs |
| BluetoothBikeSensorServer.cpp | 192-198 | Callbacks |
| BluetoothBikeSensorServer.cpp | 247-308 | Mirror-Funktionen |
| BluetoothBikeSensorServer.cpp | 676-701 | setupLocalBLEServer() |
| BluetoothBikeSensorServer.cpp | 876-897 | Switch für UUIDs |
| BluetoothBikeSensorServer.cpp | 1401-1419 | Dynamic enable |
| modules/SensorDataParser.cpp | * | Parse-Funktionen |

---

## Phase 2: SensorConfig Registry (PRIORITÄT MITTEL)

### 2.1 Problem

4 Switch-Statements mit identischem Mapping (Zeilen 776, 801, 876, 905):
```cpp
switch(type) {
  case SENSOR_TYPE_HEART_RATE: sensorTypeStr = "HR"; break;
  case SENSOR_TYPE_SPEED_CADENCE: sensorTypeStr = "CSC"; break;
  case SENSOR_TYPE_POWER_METER: sensorTypeStr = "Power"; break;
}
```

### 2.2 Lösung: Statisches Config-Array

**Neue Datei**: `src/config/SensorRegistry.h`

```cpp
#pragma once
#include "../BluetoothBikeSensorServer.h"
#include <NimBLEUUID.h>

// Forward declarations
void parseAndLogHR(const uint8_t*, size_t);
void parseAndLogCSC(const uint8_t*, size_t);
void parseAndLogPower(const uint8_t*, size_t);
void mirrorHeartRatePacket(const uint8_t*, size_t);
void mirrorCSCPacket(const uint8_t*, size_t);
void mirrorPowerPacket(const uint8_t*, size_t);

struct SensorConfig {
  SensorType type;
  const char* shortName;      // "HR", "CSC", "Power"
  const char* serviceName;    // "0x180D (HR)"
  uint16_t serviceUUID16;
  uint16_t charUUID16;
  void (*parseFunc)(const uint8_t*, size_t);
  void (*mirrorFunc)(const uint8_t*, size_t);
};

// Sensor configurations - compile-time conditional
static const SensorConfig SENSOR_CONFIGS[] = {
#if BBS_COMPILE_HEART_RATE
  { SENSOR_TYPE_HEART_RATE, "HR", "0x180D (HR)", 0x180D, 0x2A37, parseAndLogHR, mirrorHeartRatePacket },
#endif
#if BBS_COMPILE_CSC
  { SENSOR_TYPE_SPEED_CADENCE, "CSC", "0x1816 (CSC)", 0x1816, 0x2A5B, parseAndLogCSC, mirrorCSCPacket },
#endif
#if BBS_COMPILE_POWER
  { SENSOR_TYPE_POWER_METER, "Power", "0x1818 (Power)", 0x1818, 0x2A63, parseAndLogPower, mirrorPowerPacket },
#endif
};

static constexpr size_t SENSOR_CONFIG_COUNT = sizeof(SENSOR_CONFIGS) / sizeof(SENSOR_CONFIGS[0]);

// Lookup function
inline const SensorConfig* getSensorConfig(SensorType type) {
  for (size_t i = 0; i < SENSOR_CONFIG_COUNT; i++) {
    if (SENSOR_CONFIGS[i].type == type) {
      return &SENSOR_CONFIGS[i];
    }
  }
  return nullptr;
}

inline const char* getSensorTypeName(SensorType type) {
  const SensorConfig* cfg = getSensorConfig(type);
  return cfg ? cfg->shortName : "UNKNOWN";
}
```

### 2.3 Anwendung: Switch ersetzen

**Vorher** (Zeilen 775-781):
```cpp
const char* sensorTypeStr = "";
switch(type) {
  case SENSOR_TYPE_HEART_RATE: sensorTypeStr = "HR"; break;
  case SENSOR_TYPE_SPEED_CADENCE: sensorTypeStr = "CSC"; break;
  case SENSOR_TYPE_POWER_METER: sensorTypeStr = "Power"; break;
}
BLE_LIB_LOGI_F("[CLIENT] Created client for %s sensor", sensorTypeStr);
```

**Nachher**:
```cpp
BLE_LIB_LOGI_F("[CLIENT] Created client for %s sensor", getSensorTypeName(type));
```

**Vorher** (Zeilen 876-897):
```cpp
switch(type) {
  case SENSOR_TYPE_HEART_RATE:
    serviceUUID = HR_SERVICE_UUID;
    charUUID = HR_MEAS_CHAR_UUID;
    parseFunc = parseAndLogHR;
    mirrorFunc = mirrorHeartRatePacket;
    break;
  // ... CSC, Power
}
```

**Nachher**:
```cpp
const SensorConfig* cfg = getSensorConfig(type);
if (!cfg) return false;

NimBLEUUID serviceUUID((uint16_t)cfg->serviceUUID16);
NimBLEUUID charUUID((uint16_t)cfg->charUUID16);
auto parseFunc = cfg->parseFunc;
auto mirrorFunc = cfg->mirrorFunc;
```

### 2.4 Vorteile

1. **Neuer Sensortyp = 1 Zeile** im Array (statt 4+ Switch-Stellen)
2. **Compile-Time-bedingt**: Deaktivierte Typen sind nicht im Array
3. **Zentrale Wartung**: Alle Mappings an einem Ort
4. **Flash-Ersparnis**: ~400 Bytes durch wegfallende Switch-Duplikate

---

## Phase 3: Mirror-Funktion Template (PRIORITÄT NIEDRIG)

### 3.1 Problem

3 fast identische Funktionen (Zeilen 247-308):
```cpp
void mirrorHeartRatePacket(const uint8_t *data, size_t len) {
  if (!hrCharLocal) return;
  hrCharLocal->setValue(data, len);
  if (server && server->getConnectedCount() > 0) {
    bool success = hrCharLocal->notify();
    if (success) { hrNotifySuccessCount++; }
    else { hrNotifyFailureCount++; /* log */ }
  }
}
// CSC und Power sind identisch, nur mit anderen Variablen
```

### 3.2 Lösung: Template mit Makro

```cpp
// Generic mirror function
template<typename CharType, typename SuccessCounter, typename FailCounter>
void mirrorPacketGeneric(CharType*& charLocal, 
                         const uint8_t* data, size_t len,
                         SuccessCounter& successCount, FailCounter& failCount,
                         const char* tag) {
  if (!charLocal) return;
  charLocal->setValue(data, len);
  if (server && server->getConnectedCount() > 0) {
    bool success = charLocal->notify();
    if (success) {
      successCount++;
    } else {
      failCount++;
      if (failCount % 10 == 1) {
        BLE_LIB_LOGHOT_F("[MIRROR] %s notify failed (count: %lu)", tag, failCount);
      }
    }
  }
}

// Wrapper-Funktionen (für bestehende API-Kompatibilität)
#if BBS_COMPILE_HEART_RATE
void mirrorHeartRatePacket(const uint8_t *data, size_t len) {
  mirrorPacketGeneric(hrCharLocal, data, len, hrNotifySuccessCount, hrNotifyFailureCount, "HR");
}
#endif

#if BBS_COMPILE_CSC
void mirrorCSCPacket(const uint8_t *data, size_t len) {
  mirrorPacketGeneric(cscCharLocal, data, len, cscNotifySuccessCount, cscNotifyFailureCount, "CSC");
}
#endif

#if BBS_COMPILE_POWER
void mirrorPowerPacket(const uint8_t *data, size_t len) {
  mirrorPacketGeneric(powerCharLocal, data, len, powerNotifySuccessCount, powerNotifyFailureCount, "Power");
}
#endif
```

### 3.3 Alternative: Makro (falls Template-Overhead vermieden werden soll)

```cpp
#define DEFINE_MIRROR_FUNC(NAME, CHAR_LOCAL, SUCCESS_CNT, FAIL_CNT, TAG) \
void NAME(const uint8_t *data, size_t len) { \
  if (!CHAR_LOCAL) return; \
  CHAR_LOCAL->setValue(data, len); \
  if (server && server->getConnectedCount() > 0) { \
    bool success = CHAR_LOCAL->notify(); \
    if (success) { SUCCESS_CNT++; } \
    else { \
      FAIL_CNT++; \
      if (FAIL_CNT % 10 == 1) { \
        BLE_LIB_LOGHOT_F("[MIRROR] " TAG " notify failed (count: %lu)", FAIL_CNT); \
      } \
    } \
  } \
}

#if BBS_COMPILE_HEART_RATE
DEFINE_MIRROR_FUNC(mirrorHeartRatePacket, hrCharLocal, hrNotifySuccessCount, hrNotifyFailureCount, "HR")
#endif

#if BBS_COMPILE_CSC
DEFINE_MIRROR_FUNC(mirrorCSCPacket, cscCharLocal, cscNotifySuccessCount, cscNotifyFailureCount, "CSC")
#endif

#if BBS_COMPILE_POWER
DEFINE_MIRROR_FUNC(mirrorPowerPacket, powerCharLocal, powerNotifySuccessCount, powerNotifyFailureCount, "Power")
#endif
```

---

## Phase 4: Scan-Konfiguration zentralisieren (PRIORITÄT NIEDRIG)

### 4.1 Problem

Scan-Parameter werden 3x identisch gesetzt (Zeilen 1030-1038, 1086-1092, 1129-1133):
```cpp
pBLEScan->setActiveScan(true);
pBLEScan->setInterval(97);
pBLEScan->setWindow(67);
pBLEScan->setDuplicateFilter(true);
```

**Hinweis**: `BLEScanManager::configureScanParameters()` existiert bereits (Zeile 70-82), wird aber NICHT verwendet!

### 4.2 Lösung: Bestehende Funktion nutzen

**In `startScan()`** (Zeile 1028-1039):
```cpp
// VORHER:
if (!pBLEScan) {
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(97);
  pBLEScan->setWindow(67);
  pBLEScan->setDuplicateFilter(true);
  BLETelemetry::setScanDuplicateFilter(true);
}

// NACHHER:
if (!pBLEScan) {
  pBLEScan = NimBLEDevice::getScan();
  BLEScanManager::configureScanParameters(pBLEScan);
  BLETelemetry::setScanDuplicateFilter(true);
}
```

**Analog in `startSleepScan()` und `startDiscoveryScan()`**

### 4.3 Discovery hat andere Parameter

`startDiscoveryScan()` verwendet leicht andere Parameter (interval=45, window=30).  
→ **Lösung**: Parameter-Overload oder Enum für Scan-Modus:

```cpp
enum ScanMode { SCAN_MODE_SERVER, SCAN_MODE_DISCOVERY };

void configureScanParameters(NimBLEScan* pScan, ScanMode mode = SCAN_MODE_SERVER) {
  pScan->setActiveScan(true);
  if (mode == SCAN_MODE_DISCOVERY) {
    pScan->setInterval(45);
    pScan->setWindow(30);
  } else {
    pScan->setInterval(97);
    pScan->setWindow(67);
  }
  pScan->setDuplicateFilter(true);
}
```

---

## Phase 5: Fragmentierung reduzieren (PRIORITÄT NIEDRIG)

### 5.1 Problem: String in DiscoveredSensor

**Zeilen 92-100**:
```cpp
struct DiscoveredSensor {
  String name;   // ← Arduino String (dynamisch)
  String mac;    // ← Arduino String (dynamisch)
  bool hasHR;
  bool hasCSC;
  bool hasPWR;
  uint8_t addrType;
  // ...
};
```

### 5.2 Lösung: Statische char-Arrays

```cpp
struct DiscoveredSensor {
  char name[32];   // Max BLE device name length
  char mac[18];    // "XX:XX:XX:XX:XX:XX\0"
  bool hasHR;
  bool hasCSC;
  bool hasPWR;
  uint8_t addrType;
  
  // Helper für Kompatibilität
  void setName(const char* n) {
    strncpy(name, n ? n : "", sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
  }
  void setMac(const char* m) {
    strncpy(mac, m ? m : "", sizeof(mac) - 1);
    mac[sizeof(mac) - 1] = '\0';
  }
};
```

### 5.3 Weitere String-Stellen

| Zeile | Variable | Empfehlung |
|-------|----------|------------|
| 169 | `std::string advertisingName` | Behalten (wird nur 1x gesetzt) |
| 92-94 | `DiscoveredSensor::name/mac` | → `char[]` |

---

## Umsetzungsreihenfolge

### Schritt 1: BBSConfig.h erweitern (5 Minuten)
- Sensor-Type-Flags hinzufügen
- Keine Code-Änderungen nötig (Defaults = 1)

### Schritt 2: Phase 1 inkrementell (je 15 Minuten pro Sensortyp)
- Mit Heart Rate anfangen (meist genutzt)
- `#if BBS_COMPILE_HEART_RATE` um alle HR-spezifischen Blöcke
- Kompilieren, testen
- CSC und Power analog

### Schritt 3: SensorRegistry.h erstellen (30 Minuten)
- Neue Datei mit Config-Array
- Switch-Statements schrittweise ersetzen
- Backward-compatible (alte Funktionen rufen neue auf)

### Schritt 4: Mirror-Template (15 Minuten)
- Template/Makro definieren
- Wrapper-Funktionen beibehalten (API-Kompatibilität)

### Schritt 5: Scan-Konfig (10 Minuten)
- `BLEScanManager::configureScanParameters()` überall nutzen

### Schritt 6: String → char[] (20 Minuten)
- DiscoveredSensor ändern
- Alle Zugriffe anpassen

---

## Testplan

### Nach jeder Phase:

1. **Kompilieren**: Keine Fehler/Warnings
2. **Flash-Größe prüfen**: `pio run -v | grep "Flash:"`
3. **Heap-Test**: NimBLEHeapTest Example ausführen
4. **Funktionstest**: HR_CSC_Example mit echtem Sensor

### Spezifische Tests:

| Phase | Test |
|-------|------|
| 1 | Mit `BBS_COMPILE_CSC=0` kompilieren, HR-Only testen |
| 2 | Alle 3 Sensortypen verbinden, Logs prüfen |
| 3 | Mirror-Notify verifizieren |
| 4 | Discovery + Server-Scan nacheinander |
| 5 | Discovery mit vielen Geräten (>10), Heap prüfen |

---

## Risiken und Mitigationen

| Risiko | Mitigation |
|--------|------------|
| Breaking Change für Homewind | Homewind nutzt nur öffentliche API → kein Risiko |
| Template-Bloat | Makro-Alternative bereit |
| Vergessene #if-Blöcke | Kompilieren mit jeder Kombination testen |
| Discovery-Scan anders konfiguriert | Enum-Parameter für Scan-Modus |

---

## Erwartete Ergebnisse

### Bei Deaktivierung eines Sensortyps:
- **Flash**: ~2-4 KB weniger
- **RAM**: ~200 Bytes weniger (Service + Char + Data + Callbacks)

### Bei allen Sensortypen aktiv:
- **Flash**: ~800 Bytes weniger (Code-Deduplizierung)
- **Wartbarkeit**: Neuer Sensortyp = 1 Zeile im Registry + 1 Mirror-Makro

---

**Erstellt**: 2026-02-04  
**Status**: ✅ BEREIT FÜR UMSETZUNG  
**Empfohlener Start**: Phase 1 (BBSConfig.h erweitern)
