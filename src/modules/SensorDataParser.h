#pragma once

#include <Arduino.h>

#include "../BluetoothBikeSensorServer.h"

#if BBS_COMPILE_CSC
struct SpeedCadenceData {
  uint16_t speed;        // Speed in 0.01 m/s (or km/h * 100)
  uint16_t cadence;       // Cadence in rpm
  bool valid;
  uint32_t timestamp;
  uint32_t cumulativeWheelRevolutions;
  uint16_t lastWheelEventTime;
  uint16_t cumulativeCrankRevolutions;
  uint16_t lastCrankEventTime;
};
#endif

#if BBS_COMPILE_POWER
struct PowerData {
  int16_t power;          // Power in watts
  bool valid;
  uint32_t timestamp;
};
#endif

struct SensorDataParserContext {
#if BBS_COMPILE_HEART_RATE
  uint16_t* lastHeartRate;
  bool* heartRateValid;
  uint32_t* lastHeartRateTimestamp;
  void (**heartRateCallback)(uint16_t);
#endif
#if BBS_COMPILE_CSC
  SpeedCadenceData* speedCadenceData;
  void (**speedCadenceCallback)(uint16_t, uint16_t);
#endif
#if BBS_COMPILE_POWER
  PowerData* powerData;
  void (**powerCallback)(int16_t);
#endif
};

void SensorDataParserSetContext(const SensorDataParserContext& context);

#if BBS_COMPILE_HEART_RATE
void parseAndLogHR(const uint8_t* data, size_t len);
#endif
#if BBS_COMPILE_CSC
void parseAndLogCSC(const uint8_t* data, size_t len);
#endif
#if BBS_COMPILE_POWER
void parseAndLogPower(const uint8_t* data, size_t len);
#endif
