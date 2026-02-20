#include "SensorDataParser.h"

namespace {
SensorDataParserContext g_context = {};

#if BBS_COMPILE_CSC
SpeedCadenceData* getSpeedCadenceData() {
  return g_context.speedCadenceData;
}
#endif

#if BBS_COMPILE_POWER
PowerData* getPowerData() {
  return g_context.powerData;
}
#endif
}  // namespace

void SensorDataParserSetContext(const SensorDataParserContext& context) {
  g_context = context;
}

#if BBS_COMPILE_HEART_RATE
void parseAndLogHR(const uint8_t* data, size_t len) {
  if (!g_context.lastHeartRate || !g_context.heartRateValid ||
      !g_context.lastHeartRateTimestamp || !g_context.heartRateCallback || len < 2) {
    return;
  }

  uint8_t flags = data[0];
  bool is16 = flags & 0x01;
  uint16_t hr = is16 ? (data[1] | (data[2] << 8)) : data[1];
  BLE_LIB_LOGHOT_F("HR: %u bpm", hr);
  bool changed = !*g_context.heartRateValid || hr != *g_context.lastHeartRate;
  *g_context.lastHeartRate = hr;
  *g_context.heartRateValid = true;
  *g_context.lastHeartRateTimestamp = millis();
  if (changed && *g_context.heartRateCallback) {
    (*g_context.heartRateCallback)(hr);
  }

  int off = is16 ? 3 : 2;
  if (flags & 0x10) {
#if defined(BLE_LIB_DEBUG_HOT)
    // v2.2.4.7 FIX: Stack-Buffer statt String-Concatenation (eliminiert Heap-Reallocs)
    char rrBuf[256];  // Sufficient for multiple RR intervals
    int pos = snprintf(rrBuf, sizeof(rrBuf), "RR [ms]: ");
    bool first = true;
    while (off + 1 < static_cast<int>(len) && pos < (int)sizeof(rrBuf) - 20) {
      uint16_t rr = data[off] | (data[off + 1] << 8);
      off += 2;
      float rr_ms = rr * 1000.0f / 1024.0f;
      if (!first) {
        pos += snprintf(rrBuf + pos, sizeof(rrBuf) - pos, ", ");
      }
      pos += snprintf(rrBuf + pos, sizeof(rrBuf) - pos, "%.1f", rr_ms);
      first = false;
    }
    BLE_LIB_LOGHOT_F("%s", rrBuf);
#else
    // Skip RR interval parsing in release builds
    while (off + 1 < static_cast<int>(len)) {
      off += 2;
    }
#endif
  }
}
#endif

// Parse Speed/Cadence data according to Bluetooth SIG CSC Measurement Characteristic spec
#if BBS_COMPILE_CSC
void parseAndLogCSC(const uint8_t* data, size_t len) {
  SpeedCadenceData* speedCadenceData = getSpeedCadenceData();
  if (!speedCadenceData || !g_context.speedCadenceCallback || len < 1) {
    return;
  }

  uint8_t flags = data[0];
  int offset = 1;

  bool wheelRevolutionPresent = (flags & 0x01) != 0;
  bool crankRevolutionPresent = (flags & 0x02) != 0;

  uint32_t cumulativeWheelRevolutions = 0;
  uint16_t lastWheelEventTime = 0;
  uint16_t cumulativeCrankRevolutions = 0;
  uint16_t lastCrankEventTime = 0;

  // Parse wheel revolution data (if present)
  if (wheelRevolutionPresent && len >= static_cast<size_t>(offset + 6)) {
    cumulativeWheelRevolutions = data[offset] | (data[offset + 1] << 8) |
                                 (data[offset + 2] << 16) | (data[offset + 3] << 24);
    lastWheelEventTime = data[offset + 4] | (data[offset + 5] << 8);
    offset += 6;
  }

  // Parse crank revolution data (if present)
  if (crankRevolutionPresent && len >= static_cast<size_t>(offset + 4)) {
    cumulativeCrankRevolutions = data[offset] | (data[offset + 1] << 8);
    lastCrankEventTime = data[offset + 2] | (data[offset + 3] << 8);
    offset += 4;
  }

  // Calculate speed and cadence (simplified - would need previous values for accurate calculation)
  // For now, store raw values
  speedCadenceData->cumulativeWheelRevolutions = cumulativeWheelRevolutions;
  speedCadenceData->lastWheelEventTime = lastWheelEventTime;
  speedCadenceData->cumulativeCrankRevolutions = cumulativeCrankRevolutions;
  speedCadenceData->lastCrankEventTime = lastCrankEventTime;

  // Calculate cadence from crank revolutions (rpm)
  // This is a simplified calculation - in practice you'd need time delta
  static uint32_t lastCrankRevolutions = 0;
  static uint32_t lastCrankTime = 0;
  if (lastCrankTime > 0 && lastCrankEventTime != lastCrankTime) {
    uint16_t timeDelta = (lastCrankEventTime >= lastCrankTime) ?
                         (lastCrankEventTime - lastCrankTime) :
                         (65535 - lastCrankTime + lastCrankEventTime);
    uint16_t revDelta = (cumulativeCrankRevolutions >= lastCrankRevolutions) ?
                        (cumulativeCrankRevolutions - lastCrankRevolutions) : 0;
    if (timeDelta > 0 && revDelta > 0) {
      // Cadence = (revolutions / time) * (1024 / 60) = rpm
      speedCadenceData->cadence = (revDelta * 1024 * 60) / timeDelta;
    } else {
      speedCadenceData->cadence = 0;  // No pedaling
    }
  } else if (lastCrankTime > 0) {
    speedCadenceData->cadence = 0;  // No new crank events
  }
  lastCrankRevolutions = cumulativeCrankRevolutions;
  lastCrankTime = lastCrankEventTime;

  // Calculate speed from wheel revolutions (simplified)
  // Would need wheel circumference and time delta for accurate speed
  speedCadenceData->speed = 0; // Placeholder - would calculate from wheel revolutions

  speedCadenceData->valid = true;
  speedCadenceData->timestamp = millis();

  BLE_LIB_LOGHOT_F("CSC: WheelRev=%lu, CrankRev=%u, Cadence=%u rpm",
                   cumulativeWheelRevolutions, cumulativeCrankRevolutions,
                   speedCadenceData->cadence);

  if (*g_context.speedCadenceCallback) {
    (*g_context.speedCadenceCallback)(speedCadenceData->speed, speedCadenceData->cadence);
  }
}
#endif

// Parse Power Meter data according to Bluetooth SIG Cycling Power Measurement Characteristic spec
#if BBS_COMPILE_POWER
void parseAndLogPower(const uint8_t* data, size_t len) {
  PowerData* powerData = getPowerData();
  if (!powerData || !g_context.powerCallback || len < 3) {
    return;
  }

  uint16_t flags = data[0] | (data[1] << 8);
  int offset = 2;

  // Power value is always present (sint16, watts)
  int16_t power = data[offset] | (data[offset + 1] << 8);
  offset += 2;

  powerData->power = power;
  powerData->valid = true;
  powerData->timestamp = millis();

  BLE_LIB_LOGHOT_F("Power: %d W", power);

  if (*g_context.powerCallback) {
    (*g_context.powerCallback)(power);
  }
}
#endif
