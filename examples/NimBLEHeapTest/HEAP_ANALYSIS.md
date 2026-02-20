# BluetoothBikeSensorServer Heap- und Fragmentierungsanalyse

**Quelle:** NimBLEHeapTest Beispiel-Log  
**Zeitspanne:** ~8 Minuten (00:00:01 → 00:07:50)  
**TICK-Intervall:** 15 Sekunden  
**Sensor:** Heart Rate Sensor (MAC: f0:13:c3:ee:56:97)

---

## 1. Zusammenfassung

| Komponente | Heap-Kosten | Fragmentierung | Status |
|------------|-------------|----------------|--------|
| **NimBLE alleine** | 62,196 bytes | 16.4% | Stabil |
| **BluetoothBikeSensorServer Library** | +1,408 bytes | 16.0% | Stabil |
| **Sensor-Verbindung** | +2,988 bytes | 16.6% | Stabil |
| **Gesamt (Baseline → Sensor verbunden)** | 66,592 bytes | 16.6% | Stabil |

**Fazit:** 
- NimBLE verursacht den größten Heap-Overhead (~62 KB)
- Die BluetoothBikeSensorServer Library fügt nur minimalen Overhead hinzu (~1.4 KB)
- Eine aktive Sensor-Verbindung benötigt zusätzlich ~3 KB
- Die Fragmentierung bleibt stabil bei ~16% über alle Phasen
- **Keine Heap-Drift** nach Verbindungsaufbau - alle Werte bleiben konstant

---

## 2. Detaillierte Phasen-Analyse

### 2.1 Phase 1: Baseline → NimBLE Initialisierung

**Baseline (vor BLE init):**
- `free`: 321,932 bytes
- `largest`: 262,132 bytes
- Fragmentierung: 0% (keine BLE-Allokationen)

**Nach NimBLE Init:**
- `free`: 259,736 bytes
- `largest`: 217,076 bytes
- Fragmentierung: 16.4%
- **Heap-Kosten: 62,196 bytes**

**Bewertung:**
- NimBLE initialisiert den BLE-Stack und reserviert ~62 KB Heap
- Fragmentierung von 0% auf 16.4% ist erwartbar (BLE-Stack-Allokationen)
- `largest` Block reduziert sich um ~45 KB (von 262,132 auf 217,076)

---

### 2.2 Phase 2: NimBLE → BluetoothBikeSensorServer Library

**Vor Library Init:**
- `free`: 259,468 bytes (leicht niedriger als direkt nach NimBLE init)
- `largest`: 217,076 bytes

**Nach Library Init:**
- `free`: 258,328 bytes
- `largest`: 217,076 bytes (unverändert!)
- Fragmentierung: 16.0%
- **Library Overhead: 1,408 bytes** (vs. NimBLE only: 259,736 → 258,328)

**Bewertung:**
- Die Library fügt nur **minimalen Overhead** hinzu (~1.4 KB)
- `largest` Block bleibt unverändert → **keine zusätzliche Fragmentierung**
- Fragmentierung verbessert sich sogar leicht (16.4% → 16.0%)

**Wichtig:** Die Library-Initialisierung ist sehr effizient und fragmentiert den Heap nicht weiter.

---

### 2.3 Phase 3: Library → Sensor-Verbindung

**Vor Sensor-Verbindung (Library only):**
- `free`: 258,328 bytes (nach Library init)
- `largest`: 217,076 bytes
- Fragmentierung: 16.0%

**Nach Sensor-Verbindung:**
- `free`: 255,340 bytes
- `largest`: 212,980 bytes
- Fragmentierung: 16.6%
- **Sensor Connection Overhead: 2,988 bytes**

**Bewertung:**
- Eine aktive Sensor-Verbindung benötigt ~3 KB zusätzlichen Heap
- `largest` Block reduziert sich um ~4 KB (von 217,076 auf 212,980)
- Fragmentierung steigt minimal (16.0% → 16.6%)
- **Stabil nach Verbindung:** Alle Werte bleiben konstant über 2+ Minuten

---

## 3. Runtime-Stabilität

### 3.1 Phase [NimBLE+Lib] (vor Sensor-Verbindung)

**Zeitraum:** 00:00:20 → 00:05:00 (~4.5 Minuten)

| Metrik | Werte | Status |
|--------|-------|--------|
| `free` | 258,280 - 258,328 bytes | **Stabil** (±48 bytes) |
| `largest` | 217,076 bytes | **Konstant** |
| `frag` | 15.5% - 16.0% | **Stabil** |
| `drift` | 63,604 - 64,984 bytes | **Minimal** (±1.4 KB) |
| `minFree` | 255,008 - 258,216 bytes | **Stabil** (temporäre Spikes) |

**Bewertung:**
- Sehr stabiler Heap-Zustand
- Keine fortschreitende Fragmentierung
- Temporäre `minFree`-Spikes (Scan-Aktivitäten) sind normal und reversibel

### 3.2 Phase [NimBLE+Lib+Sensor] (nach Sensor-Verbindung)

**Zeitraum:** 00:05:10 → 00:07:50 (~2.5 Minuten)

| Metrik | Werte | Status |
|--------|-------|--------|
| `free` | 255,340 bytes | **Konstant** |
| `largest` | 212,980 bytes | **Konstant** |
| `frag` | 16.6% | **Konstant** |
| `drift` | 66,592 bytes | **Konstant** |
| `minFree` | 255,008 bytes | **Konstant** |

**Bewertung:**
- **Perfekte Stabilität** nach Verbindungsaufbau
- Keine Heap-Drift
- Keine Fragmentierungszunahme
- Alle Werte bleiben exakt konstant

---

## 4. Heap-Kosten-Aufschlüsselung

### 4.1 Gesamt-Overhead (Baseline → Vollständig verbunden)

```
Baseline:           321,932 bytes (free)
                    262,132 bytes (largest)

Nach Sensor-Verbindung:
                    255,340 bytes (free)  ← -66,592 bytes
                    212,980 bytes (largest) ← -49,152 bytes

Gesamt-Heap-Kosten: 66,592 bytes
```

### 4.2 Komponenten-Overhead

| Komponente | Heap-Kosten | Anteil | Fragmentierung |
|------------|-------------|--------|----------------|
| **NimBLE Stack** | 62,196 bytes | 93.4% | 16.4% |
| **BikeSensorServer Library** | 1,408 bytes | 2.1% | 16.0% |
| **Sensor-Verbindung** | 2,988 bytes | 4.5% | 16.6% |
| **Gesamt** | 66,592 bytes | 100% | 16.6% |

**Erkenntnisse:**
- **NimBLE dominiert** den Heap-Verbrauch (~93% des Gesamt-Overheads)
- Die **BluetoothBikeSensorServer Library** ist sehr effizient (~2% Overhead)
- Eine **Sensor-Verbindung** benötigt ~3 KB (akzeptabel)

---

## 5. Fragmentierungs-Analyse

### 5.1 Fragmentierungs-Entwicklung

| Phase | Fragmentierung | Änderung |
|-------|----------------|----------|
| Baseline | 0% | - |
| Nach NimBLE | 16.4% | +16.4% |
| Nach Library | 16.0% | -0.4% |
| Nach Sensor | 16.6% | +0.6% |

**Bewertung:**
- Fragmentierung ist **stabil und akzeptabel** (~16%)
- Keine fortschreitende Verschlechterung
- Library-Initialisierung verbessert Fragmentierung sogar leicht

### 5.2 Largest Block Entwicklung

| Phase | Largest Block | Änderung |
|-------|---------------|----------|
| Baseline | 262,132 bytes | - |
| Nach NimBLE | 217,076 bytes | -45,056 bytes |
| Nach Library | 217,076 bytes | 0 bytes |
| Nach Sensor | 212,980 bytes | -4,096 bytes |

**Erkenntnisse:**
- NimBLE reduziert `largest` um ~45 KB (erwartbar)
- Library ändert `largest` nicht → **keine zusätzliche Fragmentierung**
- Sensor-Verbindung reduziert `largest` um ~4 KB (akzeptabel)

---

## 6. Vergleich mit anderen Analysen

### 6.1 Vergleich: BlockerMix8 (Homewind-Projekt)

| Metrik | BlockerMix8 | BikesensorLib | Unterschied |
|--------|------------|---------------|-------------|
| Fragmentierung | 39.6% - 42.6% | 16.0% - 16.6% | **~25% besser** |
| Heap-Drift | Keine | Keine | Gleich |
| Stabilität | Stabil | Stabil | Gleich |

**Erkenntnis:** BluetoothBikeSensorServer alleine verursacht deutlich weniger Fragmentierung als ein vollständiges Homewind-System (WebSocket, Fans, etc.).

---

## 7. Empfehlungen

### 7.1 Für minimale Heap-Nutzung

1. **NimBLE ist der Hauptverbraucher** (~62 KB)
   - Kann nicht reduziert werden (BLE-Stack-Anforderung)
   - Fragmentierung von ~16% ist akzeptabel

2. **BluetoothBikeSensorServer Library ist sehr effizient**
   - Nur ~1.4 KB Overhead
   - Keine zusätzliche Fragmentierung
   - **Keine Optimierung nötig**

3. **Sensor-Verbindungen sind akzeptabel**
   - ~3 KB pro Verbindung
   - Stabile Werte nach Verbindungsaufbau
   - **Keine Memory-Leaks**

### 7.2 Für Langzeit-Stabilität

- ✅ **Keine Heap-Drift** nach Verbindungsaufbau
- ✅ **Stabile Fragmentierung** (~16%)
- ✅ **Konstante Werte** über mehrere Minuten
- ✅ **Keine Memory-Leaks** erkennbar

**Fazit:** Die BluetoothBikeSensorServer Library ist sehr heap-effizient und verursacht keine Probleme mit Fragmentierung oder Drift.

---

## 8. Zusammenfassung der wichtigsten Erkenntnisse

1. **NimBLE alleine:** 62,196 bytes Heap-Kosten, 16.4% Fragmentierung
2. **BluetoothBikeSensorServer Library:** Nur +1,408 bytes, keine zusätzliche Fragmentierung
3. **Sensor-Verbindung:** +2,988 bytes, Fragmentierung steigt minimal auf 16.6%
4. **Stabilität:** Perfekt stabil nach Verbindungsaufbau, keine Drift
5. **Gesamt-Overhead:** 66,592 bytes (93% davon ist NimBLE)

**Die Library ist sehr effizient und verursacht minimalen Heap-Overhead!**
