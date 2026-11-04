/**
 * @file Basic_MAC.ino
 * @brief One heart-rate sensor by MAC, relayed to centrals.
 *
 * begin() starts the stack and the relay server, advertised as
 * "homewind-<chip id>". The sleep scan finds the MAC, connects, subscribes and
 * prints the lifecycle events; heart-rate notifies are relayed to centrals.
 * Take the MAC and address type from the Discovery example.
 */

#include <BikeSensor.h>

namespace bs = BikeSensor;

// Lowercase, as shown by Discovery. Address type: 0 public, 1 random, -1 auto.
const char* HR_SENSOR_MAC = "f0:13:c3:ee:56:97";
const int8_t HR_ADDR_TYPE = 0;

const char* eventName(bs::EventType type) {
  switch (type) {
    case bs::EventType::SENSOR_FOUND: return "SENSOR_FOUND";
    case bs::EventType::SENSOR_CONNECTED: return "SENSOR_CONNECTED";
    case bs::EventType::SENSOR_DISCONNECTED: return "SENSOR_DISCONNECTED";
    case bs::EventType::SENSOR_FAILED: return "SENSOR_FAILED";
    case bs::EventType::CENTRAL_CONNECTED: return "CENTRAL_CONNECTED";
    case bs::EventType::CENTRAL_DISCONNECTED: return "CENTRAL_DISCONNECTED";
    case bs::EventType::HOST_RESET: return "HOST_RESET";
    default: return nullptr;
  }
}

void onEvent(const bs::EventInfo& info) {
  const char* name = eventName(info.type);
  if (name == nullptr) return;
  Serial.printf("[%lu] %s type=0x%02x reason=0x%x", millis(), name, bs::typeBit(info.sensorType),
                info.reason);
  // Only disconnect reasons are HCI codes; SENSOR_FAILED carries 0 or a host rc.
  if (info.type == bs::EventType::SENSOR_DISCONNECTED ||
      info.type == bs::EventType::CENTRAL_DISCONNECTED) {
    Serial.printf(" (%s)", bs::disconnectReasonString(info.reason));
  }
  Serial.println();
}

void onHeartRate(uint16_t bpm) { Serial.printf("HR %u bpm\n", bpm); }

void addSensor() {
  const bool ok = bs::addByMac(bs::SensorType::HEART_RATE, HR_SENSOR_MAC, HR_ADDR_TYPE, "HR");
  Serial.printf("addByMac(%s): %s\n", HR_SENSOR_MAC, ok ? "ok" : "rejected");
}

void printStatus() {
  bs::SensorInfo info;
  bs::info(bs::SensorType::HEART_RATE, info);
  const bs::Stats s = bs::stats();
  Serial.printf("state=%u battery=%s%u lastReason=0x%x centrals=%u advertising=%u heap=%lu\n",
                static_cast<unsigned>(info.state), info.hasBattery ? "" : "?",
                info.hasBattery ? info.battery : 0, info.lastDisconnectReason, bs::centralCount(),
                bs::advertising(), static_cast<unsigned long>(ESP.getFreeHeap()));
  Serial.printf("connects=%lu fails=%lu disconnects=%lu scans=%lu scanFails=%lu "
                "gattAborts=%lu hostResets=%lu cancelSafety=%lu\n",
                s.connects, s.connectFails, s.disconnects, s.scansStarted, s.scanStartFails,
                s.gattBudgetAborts, s.hostResets, s.cancelSafetyExits);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nBasic_MAC");

  if (!bs::begin("homewind", bs::typeBit(bs::SensorType::HEART_RATE), bs::Mode::MAC)) {
    Serial.println("begin() failed");
    return;
  }
  char name[48];
  bs::serverName(name, sizeof(name));
  Serial.printf("Relay server: %s\n", name);
  bs::setEventCallback(onEvent);
  bs::setHeartRateCallback(onHeartRate);
  addSensor();
  Serial.println("Commands: a=add/replace, r=remove, s=status");
}

void loop() {
  bs::update();

  if (Serial.available()) {
    switch (Serial.read()) {
      case 'a': addSensor(); break;
      case 'r': Serial.printf("remove(): %u\n", bs::remove(bs::SensorType::HEART_RATE)); break;
      case 's': printStatus(); break;
      default: break;
    }
  }
  delay(10);
}
