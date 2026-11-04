/**
 * @file Discovery.ino
 * @brief Lists nearby HR / CSC sensors with name, MAC and address type.
 *
 * A 15 s discovery runs every 30 s. Use the printed MAC and address type with
 * addByMac() (Basic_MAC) or the name with addByName() (Basic_Name). Only
 * compiled types are found; Power needs -DBBS_COMPILE_POWER=1 (see Power).
 */

#include <BikeSensor.h>

namespace bs = BikeSensor;

const uint8_t TYPES = bs::typeBit(bs::SensorType::HEART_RATE) |
                      bs::typeBit(bs::SensorType::SPEED_CADENCE) |
                      bs::typeBit(bs::SensorType::POWER);
const uint32_t DISCOVERY_MS = 15000;
const uint32_t PAUSE_MS = 15000;

bool restartPending = false;
uint32_t doneMs = 0;

void printResults() {
  bs::DiscoveredSensor d;
  for (uint8_t i = 0; i < bs::discoveredCount(); ++i) {
    if (!bs::discovered(i, d)) break;
    Serial.printf("  [%s%s%s] \"%s\" %02x:%02x:%02x:%02x:%02x:%02x addrType %u\n",
                  (d.typeMask & bs::typeBit(bs::SensorType::HEART_RATE)) ? "HR " : "",
                  (d.typeMask & bs::typeBit(bs::SensorType::SPEED_CADENCE)) ? "CSC " : "",
                  (d.typeMask & bs::typeBit(bs::SensorType::POWER)) ? "PWR " : "",
                  d.name[0] != '\0' ? d.name : "(no name)", d.mac[0], d.mac[1], d.mac[2],
                  d.mac[3], d.mac[4], d.mac[5], d.addrType);
  }
}

// The table is sorted (named first) before DISCOVERY_DONE is emitted.
void onEvent(const bs::EventInfo& info) {
  if (info.type != bs::EventType::DISCOVERY_DONE) return;
  Serial.printf("Discovery %lu done (reason %d): %u sensors\n",
                static_cast<unsigned long>(info.generation), info.reason, info.count);
  printResults();
  restartPending = true;
  doneMs = millis();
}

void startDiscovery() {
  Serial.printf("startDiscovery: %s\n", bs::startDiscovery(TYPES, DISCOVERY_MS) ? "ok" : "rejected");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nDiscovery");

  if (!bs::begin("homewind", TYPES, bs::Mode::MAC)) {
    Serial.println("begin() failed");
    return;
  }
  bs::setEventCallback(onEvent);
  startDiscovery();
}

void loop() {
  bs::update();
  if (restartPending && millis() - doneMs >= PAUSE_MS) {
    restartPending = false;
    startDiscovery();
  }
  delay(10);
}
