/**
 * @file Basic_Name.ino
 * @brief One heart-rate sensor by name, relayed to centrals.
 *
 * NAME mode scans in windows until a sensor advertises exactly this name
 * (case-sensitive, as shown by the Discovery example), then connects and
 * subscribes. After a drop the next window finds it again, also with a new
 * address.
 */

#include <BikeSensor.h>

namespace bs = BikeSensor;

const char* HR_SENSOR_NAME = "TICKR FIT C571";

void onEvent(const bs::EventInfo& info) {
  switch (info.type) {
    case bs::EventType::SENSOR_FOUND: Serial.println("SENSOR_FOUND"); break;
    case bs::EventType::SENSOR_CONNECTED: Serial.println("SENSOR_CONNECTED"); break;
    case bs::EventType::SENSOR_DISCONNECTED:
      Serial.printf("SENSOR_DISCONNECTED 0x%x (%s)\n", info.reason,
                    bs::disconnectReasonString(info.reason));
      break;
    case bs::EventType::SENSOR_FAILED: Serial.printf("SENSOR_FAILED 0x%x\n", info.reason); break;
    case bs::EventType::CENTRAL_CONNECTED: Serial.printf("centrals: %u\n", info.count); break;
    case bs::EventType::CENTRAL_DISCONNECTED: Serial.printf("centrals: %u\n", info.count); break;
    default: break;
  }
}

void onHeartRate(uint16_t bpm) { Serial.printf("HR %u bpm\n", bpm); }

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nBasic_Name");

  if (!bs::begin("homewind", bs::typeBit(bs::SensorType::HEART_RATE), bs::Mode::NAME)) {
    Serial.println("begin() failed");
    return;
  }
  bs::setEventCallback(onEvent);
  bs::setHeartRateCallback(onHeartRate);
  Serial.printf("addByName(\"%s\"): %s\n", HR_SENSOR_NAME,
                bs::addByName(bs::SensorType::HEART_RATE, HR_SENSOR_NAME) ? "ok" : "rejected");
}

void loop() {
  bs::update();
  delay(10);
}
