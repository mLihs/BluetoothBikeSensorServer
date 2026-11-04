#include "BikeSensor.h"

#include <Arduino.h>
// arduino-esp32 3.3.7+ releases the BT controller memory unless a library
// includes this header (docs/ARDUINO_ESP32_3.3.7_FIX.md).
#include "esp32-hal-bt-mem.h"

#include "internal/BBSCore.h"
#include "internal/BBSNimBLEPort.h"

namespace BikeSensor {

namespace {
internal::NimBLEPort port;
internal::Core core(port, Mode::MAC, 0);
bool begun = false;   // begin() returned true, no successful end() since
}  // namespace

bool initStack(const char* deviceName) { return core.initStack(deviceName); }

bool begin(const char* deviceName, uint8_t typeMask, Mode mode) {
  port.attach(core);
  if (!core.begin(deviceName, typeMask, mode)) return false;
  begun = true;
  return true;
}

void update() {
  if (begun) core.update(millis());
}

bool end() {
  if (!begun) return true;
  if (!core.end()) return false;
  begun = false;
  return true;
}

bool addByName(SensorType type, const char* name) {
  return begun && core.addByName(type, name, millis());
}

bool addByMac(SensorType type, const char* mac, int8_t addrType, const char* name) {
  return begun && core.addByMac(type, mac, addrType, name, millis());
}

// True once the slot is EMPTY or on its way there.
bool remove(SensorType type) {
  if (!begun || internal::slotIndex(type) < 0) return false;
  core.remove(type, millis());
  return true;
}

bool connected(SensorType type) { return core.connected(type); }
bool info(SensorType type, SensorInfo& out) { return core.info(type, out); }

bool startDiscovery(uint8_t typeMask, uint32_t durationMs) {
  return begun && core.startDiscovery(typeMask, durationMs, millis());
}

void stopDiscovery() {
  if (begun) core.stopDiscovery();
}

bool discovering() { return core.discovering(); }
uint8_t discoveredCount() { return core.discoveredCount(); }
bool discovered(uint8_t i, DiscoveredSensor& out) { return core.discovered(i, out); }

bool lastHeartRate(uint16_t& bpm, uint32_t& ageMs) {
  return core.lastHeartRate(bpm, ageMs, millis());
}
bool lastCadence(uint16_t& rpm, uint32_t& ageMs) { return core.lastCadence(rpm, ageMs, millis()); }
bool lastPower(PowerSample& out, uint32_t& ageMs) { return core.lastPower(out, ageMs, millis()); }

void pause() { core.pause(); }
void resume() { core.resume(); }
void quiesce() { core.quiesce(millis()); }
bool quiesced() { return core.quiesced(); }

bool advertising() { return core.advertising(); }
bool refreshAdvertising() { return core.refreshAdvertising(millis()); }
uint8_t centralCount() { return core.centralCount(); }
void setNamePrefix(const char* prefix) { core.setNamePrefix(prefix); }
void serverName(char* out, size_t len) { core.serverName(out, len); }

bool scanning() { return core.scanning(); }
Stats stats() { return core.stats(); }
void setEventCallback(EventCallback cb) { core.setEventCallback(cb); }
void setHeartRateCallback(void (*cb)(uint16_t)) { core.setHeartRateCallback(cb); }
void setCadenceCallback(void (*cb)(uint16_t)) { core.setCadenceCallback(cb); }
void setPowerCallback(void (*cb)(const PowerSample&)) { core.setPowerCallback(cb); }

// 2.x table (Bluetooth Core Spec Vol 2 Part D). NimBLE reports HCI reasons as
// BLE_HS_ERR_HCI_BASE (0x200) + code; other non-zero host codes are not HCI.
const char* disconnectReasonString(int reason) {
  if (reason >= BLE_HS_ERR_HCI_BASE && reason < BLE_HS_ERR_HCI_BASE + 0x100) {
    reason -= BLE_HS_ERR_HCI_BASE;
  } else if (reason != 0) {
    return "Unknown Reason";
  }
  switch (reason) {
    case 0x00: return "Success";
    case 0x01: return "Unknown Command";
    case 0x02: return "Unknown Connection";
    case 0x03: return "Hardware Failure";
    case 0x04: return "Page Timeout";
    case 0x05: return "Authentication Failure";
    case 0x06: return "PIN Missing";
    case 0x07: return "Memory Exceeded";
    case 0x08: return "Connection Timeout";
    case 0x09: return "Connection Limit Exceeded";
    case 0x0A: return "Sync Connection Limit Exceeded";
    case 0x0B: return "Connection Already Exists";
    case 0x0C: return "Command Disallowed";
    case 0x0D: return "Rejected: Limited Resources";
    case 0x0E: return "Rejected: Security Reasons";
    case 0x0F: return "Rejected: Unacceptable BD_ADDR";
    case 0x10: return "Accept Timeout";
    case 0x11: return "Unsupported Feature";
    case 0x12: return "Invalid Parameters";
    case 0x13: return "Remote User Terminated";
    case 0x14: return "Remote Low Resources";
    case 0x15: return "Remote Power Off";
    case 0x16: return "Local Host Terminated";
    case 0x17: return "Repeated Attempts";
    case 0x18: return "Pairing Not Allowed";
    case 0x1A: return "Unsupported Remote Feature";
    case 0x1F: return "Unspecified Error";
    case 0x20: return "Unsupported LMP Parameter";
    case 0x22: return "LMP Response Timeout";
    case 0x23: return "LMP Collision";
    case 0x24: return "LMP PDU Not Allowed";
    case 0x25: return "Encryption Mode Not Acceptable";
    case 0x26: return "Link Key Cannot Be Changed";
    case 0x27: return "Requested QoS Not Supported";
    case 0x28: return "Instant Passed";
    case 0x29: return "Pairing: Unit Key Not Supported";
    case 0x2A: return "Different Transaction Collision";
    case 0x2E: return "Channel Assessment Not Supported";
    case 0x2F: return "Insufficient Security";
    case 0x30: return "Parameter Out Of Range";
    case 0x3A: return "Controller Busy";
    case 0x3B: return "Unacceptable Connection Parameters";
    case 0x3C: return "Advertising Timeout";
    case 0x3D: return "Connection Terminated: MIC Failure";
    case 0x3E: return "Connection Failed To Establish";
    case 0x3F: return "MAC Connection Failed";
    default:   return "Unknown Reason";
  }
}

}  // namespace BikeSensor
