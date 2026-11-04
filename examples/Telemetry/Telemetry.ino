/**
 * @file Telemetry.ino
 * @brief HR + CSC by MAC with a periodic report: update() timing, heap and
 *        the library's stats() counters.
 *
 * Replaces the 2.x BLETelemetry module. The longest update() is the GATT
 * pass after a connect (bounded by BBS_GATT_BUDGET_MS); outside it update()
 * should stay in the low milliseconds.
 */

#include <BikeSensor.h>

namespace bs = BikeSensor;

// Lowercase, as shown by Discovery. Address type: 0 public, 1 random, -1 auto.
const char* HR_SENSOR_MAC = "f0:13:c3:ee:56:97";
const int8_t HR_ADDR_TYPE = 0;
const char* CSC_SENSOR_MAC = "fc:a6:1f:27:45:16";
const int8_t CSC_ADDR_TYPE = 1;

const uint32_t REPORT_MS = 30000;

uint32_t maxUpdateUs = 0;
uint64_t sumUpdateUs = 0;
uint32_t updates = 0;
uint32_t lastReportMs = 0;

void report() {
  const bs::Stats s = bs::stats();
  Serial.printf("[telemetry] update avg=%luus max=%luus n=%lu\n",
                static_cast<unsigned long>(updates ? sumUpdateUs / updates : 0),
                static_cast<unsigned long>(maxUpdateUs), static_cast<unsigned long>(updates));
  Serial.printf("[telemetry] heap free=%lu largest=%lu min=%lu\n",
                static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(ESP.getMaxAllocHeap()),
                static_cast<unsigned long>(ESP.getMinFreeHeap()));
  Serial.printf("[telemetry] connects=%lu fails=%lu disconnects=%lu notifies=%lu "
                "truncated=%lu malformed=%lu\n",
                s.connects, s.connectFails, s.disconnects, s.notifies, s.notifiesTruncated,
                s.notifiesMalformed);
  Serial.printf("[telemetry] dropped events=%lu notifies=%lu matches=%lu scans=%lu "
                "scanFails=%lu discTimeouts=%lu\n",
                s.eventsDropped, s.notifiesDropped, s.matchesDropped, s.scansStarted,
                s.scanStartFails, s.discoveryTimeouts);
  Serial.printf("[telemetry] gattAborts=%lu hostResets=%lu cancelSafety=%lu advEnforced=%lu "
                "hr=%u csc=%u centrals=%u\n",
                s.gattBudgetAborts, s.hostResets, s.cancelSafetyExits, s.advertisingEnforced,
                bs::connected(bs::SensorType::HEART_RATE),
                bs::connected(bs::SensorType::SPEED_CADENCE), bs::centralCount());
  maxUpdateUs = 0;
  sumUpdateUs = 0;
  updates = 0;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nTelemetry");

  const uint8_t types =
      bs::typeBit(bs::SensorType::HEART_RATE) | bs::typeBit(bs::SensorType::SPEED_CADENCE);
  if (!bs::begin("homewind", types, bs::Mode::MAC)) {
    Serial.println("begin() failed");
    return;
  }
  bs::addByMac(bs::SensorType::HEART_RATE, HR_SENSOR_MAC, HR_ADDR_TYPE, "HR");
  bs::addByMac(bs::SensorType::SPEED_CADENCE, CSC_SENSOR_MAC, CSC_ADDR_TYPE, "CSC");
  lastReportMs = millis();
}

void loop() {
  const uint32_t start = micros();
  bs::update();
  const uint32_t us = micros() - start;
  if (us > maxUpdateUs) maxUpdateUs = us;
  sumUpdateUs += us;
  updates++;

  if (millis() - lastReportMs >= REPORT_MS) {
    lastReportMs = millis();
    report();
  }
  delay(10);
}
