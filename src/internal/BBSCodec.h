#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "BBSTypes.h"

// Arduino-free codecs (design §2, §6): AD parser, MAC parse/format, HR/CSC/Power
// measurement parsers. Time is passed in as nowMs; no heap, no String.

// Default lives here for the Arduino-free host tests. Step 3 moves it into
// BBSConfig.h (design §3).
#ifndef BBS_POWER_STALE_MS
#define BBS_POWER_STALE_MS 3000             // cadence → 0 without a new crank event
#endif

namespace BikeSensor {
namespace internal {

inline uint16_t readU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

// ---- Advertising data (§3.3) ----

constexpr uint8_t kAdUuid16Incomplete = 0x02;
constexpr uint8_t kAdUuid16Complete = 0x03;
constexpr uint8_t kAdNameShort = 0x08;
constexpr uint8_t kAdNameComplete = 0x09;

struct AdFields {
  const char* name;   // into the AD buffer, not NUL-terminated; nullptr if absent
  uint8_t nameLen;
  bool    nameComplete;  // true if name came from AD 0x09 (for ADV/SCAN_RSP merge)
  uint8_t typeMask;      // raw typeBit()s; the caller ANDs requested/compiled types
};

inline uint8_t typeBitForService(uint16_t uuid16) {
  switch (uuid16) {
    case 0x180D: return typeBit(SensorType::HEART_RATE);
    case 0x1816: return typeBit(SensorType::SPEED_CADENCE);
    case 0x1818: return typeBit(SensorType::POWER);
    default: return 0;
  }
}

// One ADV or SCAN_RSP payload. A complete name (0x09) wins over a shortened
// one (0x08). A zero length byte ends the payload (padding; later bytes are
// ignored). Returns false at the first malformed element (runs past the end,
// odd 16-bit UUID list); fields from the elements before it are kept.
inline bool parseAd(const uint8_t* data, size_t len, AdFields& out) {
  out = AdFields{};
  if (data == nullptr) len = 0;
  size_t off = 0;
  while (off < len) {
    const uint8_t elemLen = data[off];
    if (elemLen == 0) break;
    if (elemLen > len - off - 1) return false;
    const uint8_t type = data[off + 1];
    const uint8_t* payload = data + off + 2;
    const uint8_t payloadLen = static_cast<uint8_t>(elemLen - 1);
    if (type == kAdUuid16Incomplete || type == kAdUuid16Complete) {
      if (payloadLen % 2 != 0) return false;
      for (uint8_t i = 0; i < payloadLen; i += 2) {
        out.typeMask |= typeBitForService(readU16(payload + i));
      }
    } else if (type == kAdNameComplete || (type == kAdNameShort && !out.nameComplete)) {
      out.name = reinterpret_cast<const char*>(payload);
      out.nameLen = payloadLen;
      out.nameComplete = (type == kAdNameComplete);
    }
    off += 1u + elemLen;
  }
  return true;
}

// ---- MAC "aa:bb:cc:dd:ee:ff"; mac[0] is the first octet as written ----

inline int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Exactly 17 characters, hex in either case, ':' separators.
inline bool parseMac(const char* str, uint8_t mac[6]) {
  if (str == nullptr) return false;
  uint8_t tmp[6];
  for (int i = 0; i < 6; ++i) {
    const char* p = str + i * 3;
    const int hi = hexNibble(p[0]);
    if (hi < 0) return false;
    const int lo = hexNibble(p[1]);
    if (lo < 0) return false;
    if (p[2] != (i < 5 ? ':' : '\0')) return false;
    tmp[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  memcpy(mac, tmp, sizeof(tmp));
  return true;
}

inline void formatMac(const uint8_t mac[6], char out[18]) {
  static const char kHex[] = "0123456789abcdef";
  for (int i = 0; i < 6; ++i) {
    out[i * 3] = kHex[mac[i] >> 4];
    out[i * 3 + 1] = kHex[mac[i] & 0x0F];
    out[i * 3 + 2] = (i < 5) ? ':' : '\0';
  }
}

// ---- Heart Rate Measurement 0x2A37 ----

// Flags bit 0 selects a uint16 value. Energy and RR fields are not read.
inline bool parseHeartRate(const uint8_t* data, size_t len, uint16_t& bpm) {
  if (data == nullptr || len < 2) return false;
  if (data[0] & 0x01) {
    if (len < 3) return false;
    bpm = readU16(data + 1);
  } else {
    bpm = data[1];
  }
  return true;
}

// ---- Crank cadence, shared by CSC and Power (§6) ----

// Per-slot crank state (§3.1). Reset on every transition out of
// SUBSCRIBED/DISCOVERING so a reconnect does not produce a cadence jump.
struct CrankState {
  uint16_t prevCrankRev;
  uint16_t prevCrankTime;   // 1/1024 s
  bool     crankValid;
  uint32_t lastCrankEventMs;
};

inline void resetCrank(CrankState& s) { s = CrankState{}; }

// Writes cadence only on a new crank event and returns true then.
inline bool updateCrank(CrankState& s, uint16_t revs, uint16_t t, uint32_t nowMs,
                        uint16_t& cadence) {
  bool event = false;
  const uint16_t dRevs = static_cast<uint16_t>(revs - s.prevCrankRev);
  const uint16_t dT = static_cast<uint16_t>(t - s.prevCrankTime);
  if (s.crankValid && dRevs > 0 && dT > 0) {
    const uint32_t c = static_cast<uint32_t>(dRevs) * 60u * 1024u / dT;
    cadence = c > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(c);
    s.lastCrankEventMs = nowMs;
    event = true;
  }
  s.prevCrankRev = revs;
  s.prevCrankTime = t;
  s.crankValid = true;
  return event;
}

// ---- CSC Measurement 0x2A5B ----

// Wheel data (bit 0) is skipped, crank data (bit 1) gives the cadence.
// hasCadence is false without crank data and for the first crank packet after
// resetCrank(). A crank packet without a new crank event reports 0 (2.x
// behaviour; no stale timer for CSC). Malformed → false, crank unchanged.
inline bool parseCsc(const uint8_t* data, size_t len, uint32_t nowMs, CrankState& crank,
                     uint16_t& cadence, bool& hasCadence) {
  if (data == nullptr || len < 1) return false;
  const uint8_t flags = data[0];
  size_t off = 1;
  if (flags & 0x01) {
    if (len - off < 6) return false;
    off += 6;
  }
  const bool hasCrank = (flags & 0x02) != 0;
  if (hasCrank && len - off < 4) return false;
  cadence = 0;
  hasCadence = false;
  if (!hasCrank) return true;
  hasCadence = crank.crankValid;
  updateCrank(crank, readU16(data + off), readU16(data + off + 2), nowMs, cadence);
  return true;
}

// ---- Cycling Power Measurement 0x2A63 (§6) ----

constexpr uint16_t kCpsBalance = 1u << 0;
constexpr uint16_t kCpsBalanceLeft = 1u << 1;
constexpr uint16_t kCpsCrank = 1u << 5;
constexpr uint16_t kCpsEnergy = 1u << 11;
// Field size per flag bit 0..11; 0 = no field. Bits 12..15 have no field.
constexpr uint8_t kCpsFieldLen[12] = {1, 0, 2, 0, 6, 4, 4, 4, 3, 2, 2, 2};

// Stale rule: cadence reads as 0 when no crank event for BBS_POWER_STALE_MS.
// Apply to every PowerSample handed out (packet, lastPower(), callback).
inline void applyPowerStale(PowerSample& s, const CrankState& crank, uint32_t nowMs) {
  if (s.hasCadence &&
      static_cast<uint32_t>(nowMs - crank.lastCrankEventMs) > BBS_POWER_STALE_MS) {
    s.cadence = 0;
  }
}

// `sample` is the slot's last measurement (zeroed after a reset). cadence and
// hasCadence carry over to packets without a new crank event (and stay true
// when stale; only cadence drops to 0). Every other field comes from this
// packet. `crankUpdated` is true only when bit 5 produced a valid crank delta
// (same-ms packets are distinguishable; §6b cadence callback). Malformed
// (len < 4 or a flagged field does not fit) → false; sample, crank and
// crankUpdated are unchanged.
inline bool parsePower(const uint8_t* data, size_t len, uint32_t nowMs, CrankState& crank,
                       PowerSample& sample, bool& crankUpdated) {
  if (data == nullptr || len < 4) return false;
  const uint16_t flags = readU16(data);
  size_t off = 4;
  size_t balanceOff = 0, crankOff = 0, energyOff = 0;
  for (uint8_t bit = 0; bit < 12; ++bit) {
    const uint8_t fieldLen = kCpsFieldLen[bit];
    if (fieldLen == 0 || !(flags & (1u << bit))) continue;
    if (len - off < fieldLen) return false;
    if (bit == 0) balanceOff = off;
    if (bit == 5) crankOff = off;
    if (bit == 11) energyOff = off;
    off += fieldLen;
  }

  PowerSample next{};
  next.watts = static_cast<int16_t>(readU16(data + 2));
  next.flags = flags;
  next.balance = (flags & kCpsBalance) ? data[balanceOff] : 0xFF;
  next.balanceIsLeft = (flags & kCpsBalanceLeft) != 0;
  next.hasEnergy = (flags & kCpsEnergy) != 0;
  next.energyKJ = next.hasEnergy ? readU16(data + energyOff) : 0;
  next.cadence = sample.cadence;
  next.hasCadence = sample.hasCadence;
  bool updated = false;
  if ((flags & kCpsCrank) &&
      updateCrank(crank, readU16(data + crankOff), readU16(data + crankOff + 2), nowMs,
                  next.cadence)) {
    next.hasCadence = true;
    updated = true;
  }
  applyPowerStale(next, crank, nowMs);
  sample = next;
  crankUpdated = updated;
  return true;
}

}  // namespace internal
}  // namespace BikeSensor
