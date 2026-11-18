/**
 * @file Trainer.ino
 * @brief Trainer mirror: the relay shows a smart trainer's GATT to one app.
 *
 * build_opt.h (next to this sketch) sets BBS_COMPILE_TRAINER=1.
 *
 * Serial: "t <mac>" reads the trainer's GATT into NVS (setup), then reboot;
 * "f" forgets it; "i" prints the state. With a stored table the relay
 * connects to the trainer and advertises the mirror while the link is ready.
 */

#include <BikeSensor.h>

#if !BBS_COMPILE_TRAINER
#error "Trainer needs -DBBS_COMPILE_TRAINER=1 (examples/Trainer/build_opt.h)"
#endif

namespace bs = BikeSensor;

const char* eventName(bs::EventType type) {
  switch (type) {
    case bs::EventType::DISCOVERY_DONE: return "DISCOVERY_DONE";
    case bs::EventType::CENTRAL_CONNECTED: return "CENTRAL_CONNECTED";
    case bs::EventType::CENTRAL_DISCONNECTED: return "CENTRAL_DISCONNECTED";
    case bs::EventType::HOST_RESET: return "HOST_RESET";
    case bs::EventType::TRAINER_CONNECTED: return "TRAINER_CONNECTED";
    case bs::EventType::TRAINER_DISCONNECTED: return "TRAINER_DISCONNECTED";
    case bs::EventType::TRAINER_SETUP_DONE: return "TRAINER_SETUP_DONE";
    default: return nullptr;
  }
}

void onEvent(const bs::EventInfo& info) {
  const char* name = eventName(info.type);
  if (name != nullptr) Serial.printf("[%lu] %s reason=%d\n", millis(), name, info.reason);
  if (info.type == bs::EventType::TRAINER_SETUP_DONE && info.reason == 0) {
    Serial.println("Stored; restarting");
    delay(100);
    ESP.restart();
  }
}

void printInfo() {
  bs::TrainerInfo t;
  bs::trainerInfo(t);
  Serial.printf("state=%u mirror=%u app=%u trainer=%02x:%02x:%02x:%02x:%02x:%02x "
                "address=%02x:%02x:%02x:%02x:%02x:%02x hash=%08lx svcs=%u chrs=%u mtu=%u/%u "
                "up=%lu down=%lu dropped=%lu writeErrors=%lu oversize=%lu\n",
                static_cast<unsigned>(t.state), t.mirror, t.appConnected, t.mac[0], t.mac[1],
                t.mac[2], t.mac[3], t.mac[4], t.mac[5], t.address[0], t.address[1], t.address[2],
                t.address[3], t.address[4], t.address[5], static_cast<unsigned long>(t.hash),
                t.services, t.characteristics, t.mtu, t.appMtu, static_cast<unsigned long>(t.up),
                static_cast<unsigned long>(t.down), static_cast<unsigned long>(t.dropped),
                static_cast<unsigned long>(t.writeErrors), static_cast<unsigned long>(t.oversize));
}

void command(String line) {
  line.trim();
  if (line.startsWith("t ")) {
    const bool ok = bs::trainerSetup(line.substring(2).c_str());
    Serial.printf("trainerSetup: %s\n", ok ? "started" : "rejected");
  } else if (line == "f") {
    Serial.printf("trainerForget: %s\n", bs::trainerForget() ? "ok" : "failed");
  } else if (line == "i") {
    printInfo();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nTrainer");

  bs::setTrainerMirror(true);
  if (!bs::begin("homewind", bs::typeBit(bs::SensorType::HEART_RATE), bs::Mode::MAC)) {
    Serial.println("begin() failed");
    return;
  }
  bs::setEventCallback(onEvent);
  printInfo();
  Serial.println("Commands: t <mac>=setup, f=forget, i=info");
}

void loop() {
  bs::update();
  static String line;
  while (Serial.available()) {   // never blocks update()
    const char c = static_cast<char>(Serial.read());
    if (c != '\n') {
      line += c;
      continue;
    }
    command(line);
    line = "";
  }
  delay(5);
}
