/**
 * @file Power.ino
 * @brief V3 hardware checkpoint: one power meter by name, with sensor
 * security (Just Works bonding, design §6a).
 *
 * EXPERIMENTAL: drives the V3 internals (src/internal) directly. This is not
 * the final public API; it will be ported to the public BikeSensor facade.
 * The 2.x API and examples are unaffected.
 *
 * build_opt.h (next to this sketch; ESP32 core 3.x passes it to every
 * source, including the libraries) sets BBS_COMPILE_POWER=1, which turns on
 * BBS_SENSOR_SECURITY, and sizes the bond store: 6 bonds, 6 CSFC records
 * (at least one per bond) and 20 CCCD records (4 per bonded central).
 *
 * At start a Power discovery runs; the first named meter is added by name.
 * Samples are printed from the power callback.
 */

#include <NimBLEDevice.h>
// Only puts this library on the include path (Arduino resolves libraries by
// their top-level headers); nothing from the 2.x API is used.
#include <BluetoothBikeSensorServer.h>
#include <internal/BBSCore.h>
#include <internal/BBSNimBLEPort.h>

#if !BBS_COMPILE_POWER
#error "Power needs -DBBS_COMPILE_POWER=1 (examples/Power/build_opt.h)"
#endif

// V3 names stay qualified: the 2.x header also declares a global SensorType.
namespace bs = BikeSensor;

const uint8_t POWER_MASK = bs::typeBit(bs::SensorType::POWER);

bs::internal::NimBLEPort port;
bs::internal::Core core(port, bs::Mode::NAME, POWER_MASK);

char meterName[32] = "";
bool discoveryDone = false;
bool repairing = false;          // 'c': waiting for the slot to reach EMPTY

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
  const bool ok = core.startDiscovery(POWER_MASK, 15000, millis());
  Serial.printf("startDiscovery(POWER, 15 s): %s\n", ok ? "ok" : "rejected");
}

void addMeter() {
  const bool ok = core.addByName(bs::SensorType::POWER, meterName, millis());
  Serial.printf("addByName(\"%s\"): %s\n", meterName, ok ? "ok" : "rejected");
}

// Named entries come first: take the first one with a name.
void pickMeter() {
  const uint8_t n = core.discoveredCount();
  bs::DiscoveredSensor d;
  for (uint8_t i = 0; i < n; ++i) {
    if (!core.discovered(i, d)) continue;
    Serial.printf("  %u: \"%s\" %02x:%02x:%02x:%02x:%02x:%02x type %u\n", i, d.name, d.mac[0],
                  d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5], d.addrType);
  }
  if (n == 0 || !core.discovered(0, d) || d.name[0] == '\0') {
    Serial.println("No named power meter found; press d to search again");
    return;
  }
  memcpy(meterName, d.name, sizeof(meterName));
  addMeter();
}

void printStatus() {
  bs::SensorInfo info;
  if (core.info(bs::SensorType::POWER, info)) {
    Serial.printf("power \"%s\" state=%u battery=%s%u encrypted=%u bonded=%u cadenceSource=%u "
                  "lastReason=0x%x\n",
                  info.name, static_cast<unsigned>(info.state), info.hasBattery ? "" : "?",
                  info.hasBattery ? info.battery : 0, info.encrypted, info.bonded,
                  info.providesCadence, info.lastDisconnectReason);
  }
  const bs::Stats s = core.stats();
  Serial.printf("bonds=%d securityOk=%lu securityFails=%lu bondEvictions=%lu "
                "bondStoreRejects=%lu gattAborts=%lu centrals=%u heap=%lu\n",
                NimBLEDevice::getNumBonds(), s.securityOk, s.securityFails, s.bondEvictions,
                s.bondStoreRejects, s.gattBudgetAborts, core.centralCount(),
                static_cast<unsigned long>(ESP.getFreeHeap()));
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nPower (experimental V3 internals)");

  port.attach(core);
  if (!core.begin("homewind", POWER_MASK, bs::Mode::NAME)) {
    Serial.println("Core::begin failed");
    return;
  }
  core.setEventCallback(onEvent);
  core.setPowerCallback(onPower);
  startDiscovery();
  Serial.println("Commands: c=re-pair (remove, deleteBonds, addByName), d=discovery, s=status");
}

void loop() {
  core.update(millis());

  if (discoveryDone) {
    discoveryDone = false;
    if (meterName[0] == '\0') pickMeter();
  }
  // Re-pairing (§4): remove(POWER), wait until the slot has left its link,
  // deleteBonds(), addByName().
  if (repairing) {
    bs::SensorInfo info;
    if (core.info(bs::SensorType::POWER, info) && info.state == bs::SlotState::EMPTY) {
      repairing = false;
      Serial.printf("deleteBonds(): %s, bonds=%d\n", core.deleteBonds() ? "ok" : "refused",
                    NimBLEDevice::getNumBonds());
      addMeter();
    }
  }

  if (Serial.available()) {
    switch (Serial.read()) {
      case 'c':
        if (meterName[0] == '\0') {
          Serial.println("No meter configured yet");
          break;
        }
        core.remove(bs::SensorType::POWER, millis());
        repairing = true;
        Serial.println("remove(POWER)");
        break;
      case 'd': startDiscovery(); break;
      case 's': printStatus(); break;
      default: break;
    }
  }
  delay(10);
}
