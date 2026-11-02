/**
 * @file Basic_MAC_V3.ino
 * @brief V3 hardware checkpoint: one heart-rate sensor by MAC.
 *
 * EXPERIMENTAL: drives the V3 internals (src/internal) directly. This is not
 * the final public API; it will be replaced by the public BikeSensor facade.
 * The 2.x API and examples are unaffected.
 *
 * Core::begin() starts the stack and the relay server, advertised as
 * "homewind-<chip id>". Sleep scan finds the MAC, connects, subscribes and
 * prints the lifecycle events; heart-rate notifies are relayed to centrals.
 */

#include <NimBLEDevice.h>
// Only puts this library on the include path (Arduino resolves libraries by
// their top-level headers); nothing from the 2.x API is used.
#include <BluetoothBikeSensorServer.h>
#include <internal/BBSCore.h>
#include <internal/BBSNimBLEPort.h>

// V3 names stay qualified: the 2.x header also declares a global SensorType.
namespace bs = BikeSensor;

// Lowercase, as shown by Discovery. Address type: 0 public, 1 random, -1 auto.
const char* HR_SENSOR_MAC = "f0:13:c3:ee:56:97";
const int8_t HR_ADDR_TYPE = 0;

bs::internal::NimBLEPort port;
bs::internal::Core core(port, bs::Mode::MAC, bs::typeBit(bs::SensorType::HEART_RATE));

const char* eventName(bs::EventType type) {
  switch (type) {
    case bs::EventType::SENSOR_FOUND: return "SENSOR_FOUND";
    case bs::EventType::SENSOR_CONNECTED: return "SENSOR_CONNECTED";
    case bs::EventType::SENSOR_DISCONNECTED: return "SENSOR_DISCONNECTED";
    case bs::EventType::SENSOR_FAILED: return "SENSOR_FAILED";
    case bs::EventType::HOST_RESET: return "HOST_RESET";
    default: return "?";
  }
}

void onEvent(const bs::EventInfo& info) {
  Serial.printf("[%lu] %s type=0x%02x reason=0x%x\n", millis(), eventName(info.type),
                bs::typeBit(info.sensorType), info.reason);
}

void addSensor() {
  const bool ok =
      core.addByMac(bs::SensorType::HEART_RATE, HR_SENSOR_MAC, HR_ADDR_TYPE, "HR", millis());
  Serial.printf("addByMac(%s): %s\n", HR_SENSOR_MAC, ok ? "ok" : "rejected");
}

void printStatus() {
  static_assert(bs::internal::slotIndex(bs::SensorType::HEART_RATE) >= 0,
                "Basic_MAC_V3 needs BBS_COMPILE_HEART_RATE");
  const bs::internal::SensorSlot& slot =
      core.slot(bs::internal::slotIndex(bs::SensorType::HEART_RATE));
  const bs::Stats s = core.stats();
  Serial.printf("state=%u battery=%u failCount=%u lastReason=0x%x heap=%lu\n",
                static_cast<unsigned>(slot.state), slot.battery, slot.failCount,
                slot.lastDisconnectReason, static_cast<unsigned long>(ESP.getFreeHeap()));
  Serial.printf("connects=%lu fails=%lu disconnects=%lu scans=%lu scanFails=%lu "
                "gattAborts=%lu hostResets=%lu cancelSafety=%lu\n",
                s.connects, s.connectFails, s.disconnects, s.scansStarted, s.scanStartFails,
                s.gattBudgetAborts, s.hostResets, s.cancelSafetyExits);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nBasic_MAC_V3 (experimental V3 internals)");

  port.attach(core);
  if (!core.begin("homewind", bs::typeBit(bs::SensorType::HEART_RATE), bs::Mode::MAC)) {
    Serial.println("Core::begin failed");
    return;
  }
  core.setEventCallback(onEvent);
  addSensor();
  Serial.println("Commands: a=add/replace, r=remove, s=status");
}

void loop() {
  core.update(millis());

  if (Serial.available()) {
    switch (Serial.read()) {
      case 'a': addSensor(); break;
      case 'r':
        core.remove(bs::SensorType::HEART_RATE, millis());
        Serial.println("remove()");
        break;
      case 's': printStatus(); break;
      default: break;
    }
  }
  delay(10);
}
