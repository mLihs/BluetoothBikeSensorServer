# arduino-esp32 3.3.7 NimBLE Boot Fix

**Betrifft**: arduino-esp32 Core 3.3.7+ mit NimBLE-Arduino  
**Symptom**: Boot-Crash mit `assert failed: block_locate_free tlsf_control_functions.h:618`  
**Status**: ✅ Fix implementiert in `BluetoothBikeSensorServer.cpp`

---

## Problem

Seit arduino-esp32 **3.3.7** (PR #12287) gibt der ESP32-Core den BT-Controller-Speicher (~36 KB) frei, falls `btInUse()` `false` zurückgibt. 

`btInUse()` wird über einen Constructor in `esp32-hal-bt-mem.h` gesteuert – nur Bibliotheken, die diesen Header einbinden, signalisieren: „BT wird genutzt“.

**NimBLE-Arduino** (h2zero) ist eine externe Bibliothek und bindet `esp32-hal-bt-mem.h` **nicht** ein. Dadurch:

1. `_btLibraryInUse` bleibt `false`
2. Der Core gibt BT-Speicher beim Boot frei
3. Beim Aufruf von `NimBLEDevice::init()` fehlt der BT-Speicher
4. **TLSF-Assert** beim Heap-Alloc: `block_size(block) >= *size`

### Fehlermeldung (Auszug)

```
[BBS][Heap] BEGIN:STACK:BEFORE_NIMBLE_INIT: free=104740 largest=63476 ...
btdm: bss start 0x3fcef180, len 36
MAGIC fadebead VERSION 0001000a

assert failed: block_locate_free tlsf_control_functions.h:618 (block_size(block) >= *size)
Rebooting...
```

---

## Lösung (implementiert)

In `BluetoothBikeSensorServer.cpp` wird `esp32-hal-bt-mem.h` eingebunden:

```cpp
#if defined(ARDUINO_ARCH_ESP32)
#include "esp32-hal-bt-mem.h"
#endif
```

Der Constructor setzt `_btLibraryInUse = true` → der Core behält den BT-Speicher bei.

---

## Alternative Lösung

Falls der Include nicht greift, kann `btInUse()` überschrieben werden:

```cpp
extern "C" bool btInUse(void) {
  return true;
}
```

Den Include entfernen und die Funktion stattdessen bereitstellen.

---

## Referenzen

- **arduino-esp32 PR #12287**: „Make BLE memory management automatic“
- **arduino-esp32 Issue #12284**: Heap-Regression 3.3.5→3.3.6 (BT-Memory)
- **Code-Stelle**: `BluetoothBikeSensorServer.cpp` – Block-Kommentar vor `#include "esp32-hal-bt-mem.h"`

---

## Zukünftige Updates

 Wenn **NimBLE-Arduino** den Include selbst einbindet, kann der hiesige Fix entfernt werden. Ein doppelter Include ist unkritisch (Constructor setzt nur `true`).

---

**Zuletzt aktualisiert**: 2026-02-20
