# Open Topics

This document tracks TODOs, known issues, planned features, and open questions for BluetoothBikeSensorServer.

---

## Known Issues

### ⚠️ Sensor Name Matching

**Issue**: Sensor name matching is case-sensitive and requires exact match.

**Impact**: Users must know exact sensor name (including capitalization).

**Workaround**: Use discovery mode to find exact sensor names.

**Status**: Working as designed, but could be improved with fuzzy matching.

---

### ⚠️ Reconnection Timing

**Issue**: Standard reconnection can take 5-20 seconds (depends on scan duration).

**Impact**: Long reconnection time if sensor disconnects.

**Workaround**: Enable Sensor MAC Preservation for faster reconnection (~1 second).

**Status**: Working as designed, but could be optimized further.

---

### ⚠️ Discovery Mode Interrupts Normal Operation

**Issue**: Discovery mode pauses normal sensor connections.

**Impact**: Sensors cannot connect during discovery.

**Workaround**: Discovery is typically short (20 seconds), and sensors reconnect after.

**Status**: Working as designed, but could be improved with parallel scanning.

---

## Planned Features

### 📋 Fuzzy Sensor Name Matching

**Description**: Support partial or case-insensitive sensor name matching.

**Priority**: Medium

**Implementation Notes**:
- Add fuzzy matching option to `BikeSensorServerAddSensor()`
- Use string comparison with case-insensitive option
- Support partial matching (substring)

---

### 📋 Parallel Discovery and Connection

**Description**: Allow discovery mode to run in parallel with normal sensor connections.

**Priority**: Low

**Implementation Notes**:
- Use separate BLE scan instances
- Coordinate scan callbacks
- Manage scan state separately

---

### 📋 Sensor Connection Priority

**Description**: Support priority-based sensor connection (connect to preferred sensor first).

**Priority**: Low

**Implementation Notes**:
- Add priority field to `ActiveSensor`
- Sort sensors by priority
- Connect to highest priority first

---

### 📋 Advanced Reconnection Strategies

**Description**: Support multiple reconnection strategies (exponential backoff, adaptive timing).

**Priority**: Low

**Implementation Notes**:
- Add reconnection strategy enum
- Implement different strategies
- Make configurable via API

---

### 📋 Sensor Data Filtering

**Description**: Filter or smooth sensor data (e.g., moving average for HR).

**Priority**: Low

**Implementation Notes**:
- Add filtering options to data callbacks
- Implement common filters (moving average, median)
- Make configurable per sensor type

---

### 📋 Connection Quality Metrics

**Description**: Track and report connection quality (RSSI, packet loss, etc.).

**Priority**: Low

**Implementation Notes**:
- Track RSSI from scan results
- Monitor notification success/failure rates
- Provide API to query connection quality

---

### 📋 Multi-Instance Support

**Description**: Support multiple instances of the library (e.g., multiple GATT servers).

**Priority**: Very Low

**Implementation Notes**:
- Refactor to class-based design
- Support multiple instances
- Coordinate BLE resources

---

## Open Questions

### ❓ Sensor Type Extension

**Question**: How to best support additional sensor types beyond HR, CSC, and Power?

**Considerations**:
- Current design supports 3 sensor types (array size)
- Would need to increase array size or use dynamic allocation
- Service UUID and characteristic UUID need to be defined

**Status**: Open for discussion

---

### ❓ Discovery Mode UI Integration

**Question**: How to best integrate discovery mode with WebUI?

**Considerations**:
- Discovery should be non-blocking for UI
- Results should be available via API
- Should support real-time updates during discovery

**Status**: Open for discussion

---

### ❓ Advertising Identity Refresh

**Question**: When should advertising identity be refreshed for Zwift cache bypass?

**Considerations**:
- Currently manual via `BikeSensorServerRefreshAdvertisingWithIdentity()`
- Could be automatic on certain events
- Need to balance cache bypass vs. stability

**Status**: Open for discussion

---

### ❓ Battery Level Monitoring

**Question**: Should battery level monitoring be more prominent?

**Considerations**:
- Currently only available via `BikeSensorServerGetBatteryLevel()`
- Could add battery level callbacks
- Could add battery level to discovery results

**Status**: Open for discussion

---

## Technical Debt

### 🔧 Code Organization

**Issue**: Some functions are quite long (e.g., `connectAndSubscribeSensor()`).

**Action**: Consider refactoring into smaller functions.

**Priority**: Low

---

### 🔧 Error Handling

**Issue**: Some error paths could be more robust.

**Action**: Add more error checking and recovery.

**Priority**: Medium

---

### 🔧 Documentation

**Issue**: Some internal functions lack documentation.

**Action**: Add inline documentation for internal functions.

**Priority**: Low

---

### 🔧 Testing

**Issue**: No automated tests.

**Action**: Add unit tests for core functionality.

**Priority**: Medium

---

## Performance Optimizations

### ⚡ Scan Optimization

**Description**: Optimize BLE scanning for faster device discovery.

**Priority**: Low

**Notes**:
- Current scan duration is configurable
- Could use passive scanning for lower power
- Could optimize scan interval

---

### ⚡ Memory Optimization

**Description**: Reduce memory usage (heap and stack).

**Priority**: Low

**Notes**:
- Current memory usage is reasonable (~50KB heap)
- Could optimize string usage
- Could use PROGMEM for constants

---

### ⚡ Connection Optimization

**Description**: Optimize connection parameters for faster connection.

**Priority**: Low

**Notes**:
- Current connection parameters are standard
- Could experiment with different parameters
- Could use connection parameter negotiation

---

## Integration Opportunities

### 🔗 Homewind Integration

**Status**: Planned

**Description**: Full integration with Homewind WebUI library.

**Notes**:
- Discovery UI integration
- Sensor management via WebUI
- Real-time telemetry

---

### 🔗 SmartMiFanAsync Integration

**Status**: Planned

**Description**: Integration with SmartMiFanAsync for fan control based on sensor data.

**Notes**:
- Heart rate-based fan speed
- Power-based fan speed
- Configurable thresholds

---

## Related Documentation

- [01_PROJECT_OVERVIEW.md](./01_PROJECT_OVERVIEW.md) - Project overview
- [02_ARCHITECTURE.md](./02_ARCHITECTURE.md) - System architecture
- [07_DEVELOPMENT.md](./07_DEVELOPMENT.md) - Development guide

