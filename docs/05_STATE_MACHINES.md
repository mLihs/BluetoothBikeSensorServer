# State Machines

This document describes all state machines used in BluetoothBikeSensorServer.

---

## Sensor Connection State Machine

**File**: `src/BluetoothBikeSensorServer.cpp`

**Purpose**: Manages sensor connection lifecycle from addition to connection to reconnection.

### States

1. **NO_TARGET** - No sensor target set
2. **SCANNING** - Scanning for sensor by name
3. **DEVICE_FOUND** - Sensor device found, ready to connect
4. **CONNECTING** - Attempting to connect to sensor
5. **CONNECTED** - Connected to sensor, subscribed to notifications
6. **DISCONNECTED** - Disconnected from sensor
7. **RECONNECTING** - Attempting to reconnect

### State Transition Diagram

```
         ┌──────────────┐
         │  NO_TARGET   │ ◄─── Initial state
         └──────┬───────┘
                │
                │ BikeSensorServerAddSensor()
                ▼
         ┌──────────────┐
         │  SCANNING    │ ◄─── startScan() called
         └──────┬───────┘
                │
                │ Device found (name match)
                ▼
         ┌──────────────┐
         │ DEVICE_FOUND │ ◄─── advertisedDevice stored
         └──────┬───────┘      doConnect = true
                │
                │ connectAndSubscribeSensor()
                ▼
         ┌──────────────┐
         │  CONNECTING  │ ◄─── client->connect()
         └──────┬───────┘
                │
         ┌──────┴──────┐
         │             │
    Success      Failure
         │             │
         ▼             ▼
┌──────────────┐ ┌──────────────┐
│  CONNECTED   │ │  SCANNING    │ ◄─── Retry scan
│              │ │              │
│ • Subscribed │ └──────────────┘
│ • Receiving  │
│   data       │
└──────┬───────┘
       │
       │ onDisconnect() callback
       ▼
┌──────────────┐
│ DISCONNECTED │
└──────┬───────┘
       │
       │ preserveMacOnDisconnect?
       │
   ┌───┴───┐
   │       │
  Yes     No
   │       │
   ▼       ▼
┌──────────────┐ ┌──────────────┐
│ RECONNECTING │ │  SCANNING    │ ◄─── Clear MAC, rescan
│              │ │              │
│ Direct       │ └──────────────┘
│ connection   │
│ attempt      │
└──────┬───────┘
       │
   ┌───┴───┐
   │       │
Success  Failure
   │       │
   ▼       ▼
┌──────────────┐ ┌──────────────┐
│  CONNECTED   │ │  SCANNING    │ ◄─── After 3 failures
└──────────────┘ └──────────────┘
```

### Transition Conditions

| From | To | Condition |
|------|----|-----------|
| `NO_TARGET` | `SCANNING` | `BikeSensorServerAddSensor()` called |
| `SCANNING` | `DEVICE_FOUND` | Device found with matching name |
| `DEVICE_FOUND` | `CONNECTING` | `connectAndSubscribeSensor()` called |
| `CONNECTING` | `CONNECTED` | Connection successful, subscribed |
| `CONNECTING` | `SCANNING` | Connection failed, retry scan |
| `CONNECTED` | `DISCONNECTED` | `onDisconnect()` callback |
| `DISCONNECTED` | `RECONNECTING` | `preserveMacOnDisconnect = true` |
| `DISCONNECTED` | `SCANNING` | `preserveMacOnDisconnect = false` |
| `RECONNECTING` | `CONNECTED` | Direct connection successful |
| `RECONNECTING` | `SCANNING` | Direct connection failed (after 3 attempts) |

### State Variables

- `hasTarget`: Whether sensor target is set (bool)
- `doConnect`: Flag to trigger connection (bool)
- `isConnected`: Connection state (bool)
- `advertisedDevice`: Stored device object (NimBLEAdvertisedDevice*)
- `macString`: Sensor MAC address (std::string)
- `reconnectFailureCount`: Reconnection failure counter (uint8_t)

---

## Discovery State Machine

**File**: `src/BluetoothBikeSensorServer.cpp`

**Purpose**: Manages discovery mode lifecycle from start to completion.

### States

1. **IDLE** - Discovery not active
2. **PREPARING** - Saving server mode state, clearing discovered sensors
3. **SCANNING** - Actively scanning for sensors
4. **PROCESSING** - Processing scan results
5. **COMPLETE** - Discovery completed, callback called
6. **RESTORING** - Restoring server mode state

### State Transition Diagram

```
         ┌──────────────┐
         │    IDLE      │ ◄─── Initial state
         └──────┬───────┘
                │
                │ BikeSensorServerStartDiscovery()
                ▼
         ┌──────────────┐
         │  PREPARING  │ ◄─── Save state, clear sensors
         └──────┬───────┘
                │
                │ Start scan
                ▼
         ┌──────────────┐
         │   SCANNING   │ ◄─── BLE scan active
         └──────┬───────┘
                │
                │ Device found
                ▼
         ┌──────────────┐
         │  PROCESSING │ ◄─── Check service UUIDs
         └──────┬───────┘      Add to discovered list
                │
                │ Continue scan
                │
         ┌──────┴──────┐
         │             │
    Timeout      Manual Stop
         │             │
         ▼             ▼
┌──────────────┐ ┌──────────────┐
│   COMPLETE   │ │   COMPLETE   │
│              │ │              │
│ Call callback│ │ Call callback│
└──────┬───────┘ └──────┬───────┘
       │                │
       └──────┬─────────┘
              │
              │ Restore state (if not manually paused)
              ▼
       ┌──────────────┐
       │  RESTORING   │
       └──────┬───────┘
              │
              │ State restored
              ▼
       ┌──────────────┐
       │    IDLE      │
       └──────────────┘
```

### Transition Conditions

| From | To | Condition |
|------|----|-----------|
| `IDLE` | `PREPARING` | `BikeSensorServerStartDiscovery()` called |
| `PREPARING` | `SCANNING` | State saved, scan started |
| `SCANNING` | `PROCESSING` | Device found with matching service |
| `PROCESSING` | `SCANNING` | Device processed, continue scan |
| `SCANNING` | `COMPLETE` | Timeout reached or manual stop |
| `COMPLETE` | `RESTORING` | Callback called, restore state |
| `RESTORING` | `IDLE` | State restored |

### State Variables

- `g_discoveryRunning`: Whether discovery is active (bool)
- `g_discoveryDone`: Whether discovery completed (bool)
- `g_discoveryTypeMask`: Sensor type mask (uint8_t)
- `g_discoveredCount`: Number of discovered sensors (int)
- `savedState`: Saved server mode state (ServerModeState)

---

## Reconnection State Machine

**File**: `src/BluetoothBikeSensorServer.cpp`

**Purpose**: Manages sensor reconnection after disconnect.

### States

1. **CONNECTED** - Sensor connected
2. **DISCONNECTED** - Sensor disconnected
3. **FAST_RECONNECT** - Direct reconnection attempt (MAC preserved)
4. **SCAN_RECONNECT** - Scan-based reconnection (MAC cleared)

### State Transition Diagram (Fast Reconnection)

```
         ┌──────────────┐
         │  CONNECTED   │
         └──────┬───────┘
                │
                │ onDisconnect()
                ▼
         ┌──────────────┐
         │ DISCONNECTED │ ◄─── preserveMacOnDisconnect = true
         └──────┬───────┘      MAC preserved
                │
                │ doConnect = true
                ▼
         ┌──────────────┐
         │FAST_RECONNECT│ ◄─── Direct connection attempt
         └──────┬───────┘
                │
         ┌──────┴──────┐
         │             │
    Success      Failure (attempt 1-3)
         │             │
         ▼             ▼
┌──────────────┐ ┌──────────────┐
│  CONNECTED   │ │FAST_RECONNECT│ ◄─── Retry (up to 3 times)
└──────────────┘ └──────┬───────┘
                        │
                        │ After 3 failures
                        ▼
                ┌──────────────┐
                │SCAN_RECONNECT│ ◄─── Clear MAC, rescan
                └──────┬───────┘
                       │
                       │ Device found
                       ▼
                ┌──────────────┐
                │  CONNECTED   │
                └──────────────┘
```

### State Transition Diagram (Standard Reconnection)

```
         ┌──────────────┐
         │  CONNECTED   │
         └──────┬───────┘
                │
                │ onDisconnect()
                ▼
         ┌──────────────┐
         │ DISCONNECTED │ ◄─── preserveMacOnDisconnect = false
         └──────┬───────┘      MAC cleared
                │
                │ startScan()
                ▼
         ┌──────────────┐
         │SCAN_RECONNECT│ ◄─── Scan for device by name
         └──────┬───────┘
                │
                │ Device found
                ▼
         ┌──────────────┐
         │  CONNECTED   │
         └──────────────┘
```

### Transition Conditions

| From | To | Condition |
|------|----|-----------|
| `CONNECTED` | `DISCONNECTED` | `onDisconnect()` callback |
| `DISCONNECTED` | `FAST_RECONNECT` | `preserveMacOnDisconnect = true` |
| `DISCONNECTED` | `SCAN_RECONNECT` | `preserveMacOnDisconnect = false` |
| `FAST_RECONNECT` | `CONNECTED` | Direct connection successful |
| `FAST_RECONNECT` | `SCAN_RECONNECT` | Direct connection failed (3 attempts) |
| `SCAN_RECONNECT` | `CONNECTED` | Device found and connected |

### Reconnection Timing

**Fast Reconnection** (MAC preserved):
- Reconnection time: ~1 second
- Attempts: Up to 3 direct connection attempts
- Fallback: Scan-based reconnection after 3 failures

**Standard Reconnection** (MAC cleared):
- Reconnection time: ~5-20 seconds (depends on scan duration)
- Attempts: Continuous scanning until device found
- No fallback needed (scan continues until found)

---

## GATT Server State Machine

**File**: `src/BluetoothBikeSensorServer.cpp`

**Purpose**: Manages GATT server lifecycle and advertising state.

### States

1. **NOT_INITIALIZED** - Server not created
2. **INITIALIZED** - Server created, services added
3. **ADVERTISING** - Advertising active
4. **PAUSED** - Advertising paused (manual pause)
5. **STOPPED** - Advertising stopped (discovery mode)

### State Transition Diagram

```
         ┌──────────────┐
         │NOT_INITIALIZED│
         └──────┬───────┘
                │
                │ BluetoothBikeSensorServerBegin()
                ▼
         ┌──────────────┐
         │ INITIALIZED  │ ◄─── Server created, services added
         └──────┬───────┘
                │
                │ Start advertising
                ▼
         ┌──────────────┐
         │ ADVERTISING  │ ◄─── Advertising active
         └──────┬───────┘
                │
         ┌──────┴──────┐
         │             │
    Pause         Discovery Start
         │             │
         ▼             ▼
┌──────────────┐ ┌──────────────┐
│   PAUSED     │ │   STOPPED    │
│              │ │              │
│ Manual pause│ │ Discovery    │
│              │ │ mode active  │
└──────┬───────┘ └──────┬───────┘
       │                │
       │ Resume         │ Discovery End
       │                │
       ▼                ▼
┌──────────────┐ ┌──────────────┐
│ ADVERTISING  │ │ ADVERTISING  │
└──────────────┘ └──────────────┘
```

### Transition Conditions

| From | To | Condition |
|------|----|-----------|
| `NOT_INITIALIZED` | `INITIALIZED` | `BluetoothBikeSensorServerBegin()` called |
| `INITIALIZED` | `ADVERTISING` | Advertising started |
| `ADVERTISING` | `PAUSED` | `BikeSensorServerPause()` called |
| `ADVERTISING` | `STOPPED` | Discovery mode started |
| `PAUSED` | `ADVERTISING` | `BikeSensorServerResume()` called |
| `STOPPED` | `ADVERTISING` | Discovery mode ended |

### State Variables

- `server`: NimBLE server instance (NimBLEServer*)
- `pAdvertising`: Advertising instance (NimBLEAdvertising*)
- `g_serverManuallyPaused`: Manual pause flag (bool)
- `g_serverWasAdvertisingBeforePause`: Pre-pause state (bool)

---

## Central Connection State Machine

**File**: `src/BluetoothBikeSensorServer.cpp`

**Purpose**: Tracks central device connections to GATT server.

### States

1. **NO_CENTRALS** - No central devices connected
2. **CENTRALS_CONNECTED** - One or more centrals connected

### State Transition Diagram

```
         ┌──────────────┐
         │ NO_CENTRALS  │ ◄─── Initial state
         └──────┬───────┘
                │
                │ Central connects
                ▼
         ┌──────────────┐
         │CENTRALS_CONN │ ◄─── onConnect() callback
         │   ECTED      │      centralCount > 0
         └──────┬───────┘
                │
                │ Central disconnects
                ▼
         ┌──────────────┐
         │ NO_CENTRALS  │ ◄─── onDisconnect() callback
         └──────────────┘      centralCount = 0
```

### Transition Conditions

| From | To | Condition |
|------|----|-----------|
| `NO_CENTRALS` | `CENTRALS_CONNECTED` | Central connects (`centralCount > 0`) |
| `CENTRALS_CONNECTED` | `NO_CENTRALS` | All centrals disconnect (`centralCount = 0`) |

### State Variables

- `lastCentralCount`: Last known central count (uint8_t)
- `server->getConnectedCount()`: Current central count

### Callbacks

- `onCentralConnected()`: Called when central connects
- `onCentralDisconnected()`: Called when central disconnects

---

## Related Documentation

- [03_FUNCTIONS.md](./03_FUNCTIONS.md) - Core functions and workflows
- [04_COMPONENTS.md](./04_COMPONENTS.md) - Component documentation
- [02_ARCHITECTURE.md](./02_ARCHITECTURE.md) - System architecture

