/**
 * @file Power.ino
 * @brief One power meter by name: watts, cadence, balance, energy.
 *
 * build_opt.h (next to this sketch; ESP32 core 3.x passes it to every
 * source, including the libraries) sets BBS_COMPILE_POWER=1.
 *
 * At start a Power discovery runs; the first named meter is added by name.
 * Samples are printed from the power callback.
 */

#include <BikeSensor.h>

#if !BBS_COMPILE_POWER
#error "Power needs -DBBS_COMPILE_POWER=1 (examples/Power/build_opt.h)"
#endif

namespace bs = BikeSensor;

const uint8_t POWER_MASK = bs::typeBit(bs::SensorType::POWER);

char meterName[32] = "";
bool discoveryDone = false;

const char* eventName(bs::EventType type) {
  switch (type) {
    case bs::EventType::SENSOR_FOUND: return "SENSOR_FOUND";
    case bs::EventType::SENSOR_CONNECTED: return "SENSOR_CONNECTED";
    case bs::EventType::SENSOR_DISCONNECTED: return "SENSOR_DISCONNECTED";
    case bs::EventType::SENSOR_FAILED: return "SENSOR_FAILED";
    case bs::EventType::DISCOVERY_DONE: return "DISCOVERY_DONE";
    case bs::EventType::CENTRAL_CONNECTED: return "CENTRAL_CONNECTED";
    case bs::EventType::CENTRAL_DISCONNECTED: return "CENTRAL_DISCONNECTED";
    case bs::EventType::CENTRAL_SUBSCRIBED: return "CENTRAL_SUBSCRIBED";
    case bs::EventType::CENTRAL_UNSUBSCRIBED: return "CENTRAL_UNSUBSCRIBED";
    case bs::EventType::HOST_RESET: return "HOST_RESET";
    default: return "?";
  }
}

void onEvent(const bs::EventInfo& info) {
  if (info.type == bs::EventType::DATA) return;
  Serial.printf("[%lu] %s type=0x%02x reason=0x%x count=%u\n", millis(), eventName(info.type),
                bs::typeBit(info.sensorType), info.reason, info.count);
  if (info.type == bs::EventType::DISCOVERY_DONE) discoveryDone = true;
}

void onPower(const bs::PowerSample& s) {
  Serial.printf("power %d W cadence %u%s balance ", s.watts, s.cadence,
                s.hasCadence ? "" : " (none)");
  if (s.balance == 0xFF) {
    Serial.print("-");
  } else {
    Serial.printf("%u.%u %%%s", s.balance / 2, (s.balance % 2) * 5,
                  s.balanceIsLeft ? " left" : "");
  }
  if (s.hasEnergy) Serial.printf(" energy %u kJ", s.energyKJ);
  Serial.println();
}

void startDiscovery() {
  const bool ok = bs::startDiscovery(POWER_MASK, 15000);
  Serial.printf("startDiscovery(POWER, 15 s): %s\n", ok ? "ok" : "rejected");
}

void addMeter() {
  const bool ok = bs::addByName(bs::SensorType::POWER, meterName);
  Serial.printf("addByName(\"%s\"): %s\n", meterName, ok ? "ok" : "rejected");
}

// Named entries come first: take the first one with a name.
void pickMeter() {
  const uint8_t n = bs::discoveredCount();
  bs::DiscoveredSensor d;
  for (uint8_t i = 0; i < n; ++i) {
    if (!bs::discovered(i, d)) continue;
    Serial.printf("  %u: \"%s\" %02x:%02x:%02x:%02x:%02x:%02x type %u\n", i, d.name, d.mac[0],
                  d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5], d.addrType);
  }
  if (n == 0 || !bs::discovered(0, d) || d.name[0] == '\0') {
    Serial.println("No named power meter found; press d to search again");
    return;
  }
  memcpy(meterName, d.name, sizeof(meterName));
  addMeter();
}

void printStatus() {
  bs::SensorInfo info;
  if (bs::info(bs::SensorType::POWER, info)) {
    Serial.printf("power \"%s\" state=%u battery=%s%u encrypted=%u cadenceSource=%u "
                  "lastReason=0x%x\n",
                  info.name, static_cast<unsigned>(info.state), info.hasBattery ? "" : "?",
                  info.hasBattery ? info.battery : 0, info.encrypted, info.providesCadence,
                  info.lastDisconnectReason);
  }
  const bs::Stats s = bs::stats();
  Serial.printf("gattAborts=%lu centrals=%u heap=%lu\n", s.gattBudgetAborts,
                bs::centralCount(), static_cast<unsigned long>(ESP.getFreeHeap()));
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nPower");

  if (!bs::begin("homewind", POWER_MASK, bs::Mode::NAME)) {
    Serial.println("begin() failed");
    return;
  }
  bs::setEventCallback(onEvent);
  bs::setPowerCallback(onPower);
  startDiscovery();
  Serial.println("Commands: d=discovery, s=status");
}

void loop() {
  bs::update();

  if (discoveryDone) {
    discoveryDone = false;
    if (meterName[0] == '\0') pickMeter();
  }

  if (Serial.available()) {
    switch (Serial.read()) {
      case 'd': startDiscovery(); break;
      case 's': printStatus(); break;
      default: break;
    }
  }
  delay(10);
}
