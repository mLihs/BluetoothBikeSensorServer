#include "BBSCodec.h"

#include <cassert>
#include <cstdint>
#include <cstring>

using BikeSensor::PowerSample;
using BikeSensor::SensorType;
using BikeSensor::typeBit;
using BikeSensor::internal::AdFields;
using BikeSensor::internal::CrankState;
namespace bs = BikeSensor::internal;

constexpr uint8_t kHR = typeBit(SensorType::HEART_RATE);
constexpr uint8_t kCSC = typeBit(SensorType::SPEED_CADENCE);
constexpr uint8_t kPWR = typeBit(SensorType::POWER);
static_assert(BBS_POWER_STALE_MS >= 100, "test packet gaps assume >= 100 ms");

struct Buf {
  uint8_t d[64] = {};
  size_t n = 0;
  Buf& u8(uint8_t v) { d[n++] = v; return *this; }
  Buf& u16(uint16_t v) { u8(static_cast<uint8_t>(v)); return u8(static_cast<uint8_t>(v >> 8)); }
  Buf& fill(uint8_t v, size_t count) { while (count--) u8(v); return *this; }
  Buf& str(const char* s) { while (*s) u8(static_cast<uint8_t>(*s++)); return *this; }
};

static bool sameCrank(const CrankState& a, const CrankState& b) {
  return a.prevCrankRev == b.prevCrankRev && a.prevCrankTime == b.prevCrankTime &&
         a.crankValid == b.crankValid && a.lastCrankEventMs == b.lastCrankEventMs;
}

static bool sameSample(const PowerSample& a, const PowerSample& b) {
  return a.watts == b.watts && a.cadence == b.cadence && a.hasCadence == b.hasCadence &&
         a.balance == b.balance && a.balanceIsLeft == b.balanceIsLeft &&
         a.energyKJ == b.energyKJ && a.hasEnergy == b.hasEnergy && a.flags == b.flags;
}

static bool nameIs(const AdFields& f, const char* s) {
  return f.name != nullptr && f.nameLen == strlen(s) && memcmp(f.name, s, f.nameLen) == 0;
}

// ---- AD parser ----

static void testAdNameAndUuids() {
  AdFields f{};
  assert(bs::parseAd(nullptr, 0, f) && f.name == nullptr && f.typeMask == 0);
  assert(bs::parseAd(nullptr, 5, f) && f.name == nullptr);
  Buf empty;
  assert(bs::parseAd(empty.d, 0, f) && f.name == nullptr && f.typeMask == 0);

  // Flags, complete 16-bit list (HR + unrelated battery 0x180F), complete name.
  Buf a;
  a.u8(2).u8(0x01).u8(0x06);
  a.u8(5).u8(0x03).u16(0x180D).u16(0x180F);
  a.u8(6).u8(0x09).str("TICKR");
  assert(bs::parseAd(a.d, a.n, f));
  assert(f.typeMask == kHR && nameIs(f, "TICKR"));

  // Incomplete list with CSC + CPS: raw bits, no AND with compiled types here.
  Buf b;
  b.u8(5).u8(0x02).u16(0x1816).u16(0x1818);
  b.u8(4).u8(0x08).str("Ass");
  assert(bs::parseAd(b.d, b.n, f));
  assert(f.typeMask == (kCSC | kPWR) && nameIs(f, "Ass"));

  // Two lists accumulate; UUIDs are little endian (0x0D18 is not HR).
  Buf c;
  c.u8(3).u8(0x02).u16(0x0D18);
  c.u8(3).u8(0x03).u16(0x180D);
  c.u8(3).u8(0x03).u16(0x1818);
  assert(bs::parseAd(c.d, c.n, f) && f.typeMask == (kHR | kPWR) && f.name == nullptr);

  // Complete name wins over shortened, in either order.
  Buf d;
  d.u8(4).u8(0x08).str("Ass").u8(8).u8(0x09).str("ASSIOMA");
  assert(bs::parseAd(d.d, d.n, f) && nameIs(f, "ASSIOMA"));
  Buf e;
  e.u8(8).u8(0x09).str("ASSIOMA").u8(4).u8(0x08).str("Ass");
  assert(bs::parseAd(e.d, e.n, f) && nameIs(f, "ASSIOMA"));

  // Empty name element and empty UUID list are valid.
  Buf g;
  g.u8(1).u8(0x09).u8(1).u8(0x03);
  assert(bs::parseAd(g.d, g.n, f) && f.name != nullptr && f.nameLen == 0 && f.typeMask == 0);

  // Long name is reported in full; consumers truncate to 31 (pushScanMatch).
  Buf h;
  h.u8(30).u8(0x09).fill('x', 29);
  assert(bs::parseAd(h.d, h.n, f) && f.nameLen == 29);

  // 128-bit and 32-bit lists, manufacturer data: ignored.
  Buf i;
  i.u8(17).u8(0x07).fill(0x18, 16).u8(5).u8(0x05).u16(0x180D).u16(0).u8(3).u8(0xFF).u16(0x180D);
  assert(bs::parseAd(i.d, i.n, f) && f.typeMask == 0 && f.name == nullptr);
}

static void testAdMalformed() {
  AdFields f{};

  // Zero length ends the payload (padding); later bytes are not parsed.
  Buf pad;
  pad.u8(3).u8(0x03).u16(0x180D).u8(0).u8(0x55).u8(0x66);
  assert(bs::parseAd(pad.d, pad.n, f) && f.typeMask == kHR);

  // Element runs past the end: false, earlier fields kept.
  Buf cut;
  cut.u8(3).u8(0x03).u16(0x1816).u8(9).u8(0x09).str("HRM");
  assert(!bs::parseAd(cut.d, cut.n, f));
  assert(f.typeMask == kCSC && f.name == nullptr);

  // Length byte as last byte (no type).
  Buf lenOnly;
  lenOnly.u8(4).u8(0x09).str("ABC").u8(1);
  assert(!bs::parseAd(lenOnly.d, lenOnly.n, f) && nameIs(f, "ABC"));

  // Single length byte of a bigger element.
  Buf one;
  one.u8(2);
  assert(!bs::parseAd(one.d, one.n, f) && f.name == nullptr && f.typeMask == 0);

  // Odd 16-bit UUID list: malformed, the partial list is not used.
  Buf odd;
  odd.u8(2).u8(0x09).str("A").u8(4).u8(0x03).u16(0x180D).u8(0x18);
  assert(!bs::parseAd(odd.d, odd.n, f) && f.typeMask == 0 && nameIs(f, "A"));

  // Every truncation of a valid payload returns without reading past len.
  Buf full;
  full.u8(2).u8(0x01).u8(0x06).u8(3).u8(0x03).u16(0x1818).u8(5).u8(0x09).str("Kick");
  assert(bs::parseAd(full.d, full.n, f));
  for (size_t n = 0; n < full.n; ++n) {
    const bool ok = bs::parseAd(full.d, n, f);
    assert(ok == (n == 0 || n == 3 || n == 7));
  }
}

// ---- MAC ----

static void testMac() {
  uint8_t mac[6] = {};
  assert(bs::parseMac("aa:BB:0c:d1:Ee:ff", mac));
  const uint8_t want[6] = {0xAA, 0xBB, 0x0C, 0xD1, 0xEE, 0xFF};
  assert(memcmp(mac, want, 6) == 0);

  char out[18];
  bs::formatMac(want, out);
  assert(strcmp(out, "aa:bb:0c:d1:ee:ff") == 0);
  uint8_t back[6];
  assert(bs::parseMac(out, back) && memcmp(back, want, 6) == 0);

  const uint8_t zero[6] = {};
  bs::formatMac(zero, out);
  assert(strcmp(out, "00:00:00:00:00:00") == 0);

  const uint8_t keep[6] = {1, 2, 3, 4, 5, 6};
  const char* bad[] = {
      "", "aa", "aa:bb:cc:dd:ee:f", "aa:bb:cc:dd:ee:ff:", "aa:bb:cc:dd:ee:ff0",
      "aa-bb-cc-dd-ee-ff", "aabbccddeeff", "a:bb:cc:dd:ee:ff", "aa:bb:cc:dd:ee:gg",
      " aa:bb:cc:dd:ee:ff", "aa:bb:cc:dd:ee:ff ", "aa:bb:cc:dd:ee",
  };
  for (const char* s : bad) {
    memcpy(mac, keep, 6);
    assert(!bs::parseMac(s, mac));
    assert(memcmp(mac, keep, 6) == 0);
  }
  assert(!bs::parseMac(nullptr, mac));
}

// ---- Heart rate ----

static void testHeartRate() {
  uint16_t bpm = 7;
  assert(!bs::parseHeartRate(nullptr, 2, bpm));
  const uint8_t one[1] = {0x00};
  assert(!bs::parseHeartRate(one, 0, bpm) && !bs::parseHeartRate(one, 1, bpm));
  assert(bpm == 7);

  const uint8_t u8v[2] = {0x00, 142};
  assert(bs::parseHeartRate(u8v, 2, bpm) && bpm == 142);

  const uint8_t u16v[3] = {0x01, 0x2C, 0x01};
  assert(bs::parseHeartRate(u16v, 3, bpm) && bpm == 300);
  bpm = 7;
  assert(!bs::parseHeartRate(u16v, 2, bpm) && bpm == 7);

  // Energy (bit 3) and RR (bit 4) fields are ignored, also when cut.
  const uint8_t rr[7] = {0x18, 95, 0x10, 0x00, 0x00, 0x04, 0x00};
  assert(bs::parseHeartRate(rr, 7, bpm) && bpm == 95);
  assert(bs::parseHeartRate(rr, 3, bpm) && bpm == 95);
}

// ---- CSC ----

static Buf cscCrank(uint16_t revs, uint16_t t) {
  Buf b;
  b.u8(0x02).u16(revs).u16(t);
  return b;
}

static void testCscLengths() {
  CrankState crank{};
  uint16_t cad = 9;
  bool has = true;
  assert(!bs::parseCsc(nullptr, 5, 0, crank, cad, has));
  const uint8_t z[1] = {0};
  assert(!bs::parseCsc(z, 0, 0, crank, cad, has));

  assert(bs::parseCsc(z, 1, 0, crank, cad, has) && !has && cad == 0);

  Buf wheel;
  wheel.u8(0x01).fill(0x11, 6);
  assert(bs::parseCsc(wheel.d, 7, 0, crank, cad, has) && !has);
  assert(!bs::parseCsc(wheel.d, 6, 0, crank, cad, has));
  assert(!crank.crankValid);

  Buf crankOnly = cscCrank(1, 2);
  assert(!bs::parseCsc(crankOnly.d, 4, 0, crank, cad, has));
  assert(!crank.crankValid);
  assert(bs::parseCsc(crankOnly.d, 5, 0, crank, cad, has) && !has && crank.crankValid);

  Buf both;
  both.u8(0x03).fill(0x22, 6).u16(3).u16(4);
  bs::resetCrank(crank);
  assert(!bs::parseCsc(both.d, 10, 0, crank, cad, has));
  assert(!crank.crankValid);
  assert(bs::parseCsc(both.d, 11, 0, crank, cad, has));
  assert(crank.prevCrankRev == 3 && crank.prevCrankTime == 4);
}

static void testCscCadence() {
  CrankState crank{};
  uint16_t cad = 0;
  bool has = false;

  Buf p = cscCrank(100, 1000);
  assert(bs::parseCsc(p.d, p.n, 10, crank, cad, has) && !has && cad == 0);

  p = cscCrank(101, 2024);  // 1 rev in 1 s
  assert(bs::parseCsc(p.d, p.n, 1010, crank, cad, has) && has && cad == 60);
  assert(crank.lastCrankEventMs == 1010);

  // Same crank data again: no new crank event → 0 (2.x behaviour).
  assert(bs::parseCsc(p.d, p.n, 1500, crank, cad, has) && has && cad == 0);
  assert(crank.lastCrankEventMs == 1010);

  // Wrap of both counters at 65535.
  p = cscCrank(65535, 65000);
  assert(bs::parseCsc(p.d, p.n, 2000, crank, cad, has));
  p = cscCrank(1, 65000 + 2048 - 65536);  // 2 revs in 2 s across the wrap
  assert(bs::parseCsc(p.d, p.n, 3000, crank, cad, has) && has && cad == 60);

  // Wheel + crank: crank offset follows the wheel field.
  Buf both;
  both.u8(0x03).fill(0xEE, 6).u16(4).u16(1512 + 1024);  // 3 revs in 1 s
  assert(bs::parseCsc(both.d, both.n, 4000, crank, cad, has) && has && cad == 180);

  // A malformed packet leaves the crank state untouched.
  const CrankState before = crank;
  Buf cut = cscCrank(50, 50);
  assert(!bs::parseCsc(cut.d, cut.n - 1, 5000, crank, cad, has));
  assert(sameCrank(before, crank));

  // Clamp to 0xFFFF.
  p = cscCrank(4, 2537);  // 0 revs → no event
  assert(bs::parseCsc(p.d, p.n, 5100, crank, cad, has) && cad == 0);
  p = cscCrank(2004, 2538);  // 2000 revs in 1/1024 s
  assert(bs::parseCsc(p.d, p.n, 5200, crank, cad, has) && has && cad == 0xFFFF);

  // Reset after disconnect: no jump on the first packet.
  bs::resetCrank(crank);
  p = cscCrank(9000, 30000);
  assert(bs::parseCsc(p.d, p.n, 6000, crank, cad, has) && !has && cad == 0);
  p = cscCrank(9002, 31024);
  assert(bs::parseCsc(p.d, p.n, 7000, crank, cad, has) && has && cad == 120);
}

// ---- Power 0x2A63 ----

static const uint8_t kFieldLen[16] = {1, 0, 2, 0, 6, 4, 4, 4, 3, 2, 2, 2, 0, 0, 0, 0};

static Buf powerBase(uint16_t flags, int16_t watts) {
  Buf b;
  b.u16(flags).u16(static_cast<uint16_t>(watts));
  return b;
}

static void testPowerShortAndMinimal() {
  CrankState crank{};
  PowerSample s{};
  const PowerSample empty{};
  Buf b = powerBase(0, -5);
  assert(!bs::parsePower(nullptr, 4, 0, crank, s));
  for (size_t n = 0; n < 4; ++n) {
    assert(!bs::parsePower(b.d, n, 0, crank, s));
    assert(sameSample(s, empty));
  }
  assert(bs::parsePower(b.d, 4, 0, crank, s));
  assert(s.watts == -5 && s.flags == 0 && s.balance == 0xFF && !s.balanceIsLeft);
  assert(!s.hasEnergy && s.energyKJ == 0 && !s.hasCadence && s.cadence == 0);
  assert(!crank.crankValid);

  // Trailing bytes beyond the flagged fields are ignored.
  b.u8(0x77).u8(0x88);
  assert(bs::parsePower(b.d, b.n, 0, crank, s) && s.watts == -5);
}

static void testPowerEachFlagBit() {
  for (uint8_t bit = 0; bit < 16; ++bit) {
    const uint16_t flags = static_cast<uint16_t>(1u << bit);
    Buf b = powerBase(flags, 250);
    if (bit == 0) b.u8(100);
    else if (bit == 5) b.u16(7).u16(512);
    else if (bit == 11) b.u16(1234);
    else b.fill(0xA5, kFieldLen[bit]);
    assert(b.n == 4u + kFieldLen[bit]);

    CrankState crank{};
    PowerSample s{};
    assert(bs::parsePower(b.d, b.n, 0, crank, s));
    assert(s.watts == 250 && s.flags == flags);
    assert(s.balance == (bit == 0 ? 100 : 0xFF));
    assert(s.balanceIsLeft == (bit == 1));
    assert(s.hasEnergy == (bit == 11) && s.energyKJ == (bit == 11 ? 1234 : 0));
    assert(!s.hasCadence && s.cadence == 0);
    assert(crank.crankValid == (bit == 5));
    if (bit == 5) assert(crank.prevCrankRev == 7 && crank.prevCrankTime == 512);

    // The field cut by one byte: malformed, nothing changes.
    if (kFieldLen[bit] > 0) {
      CrankState c2{};
      PowerSample s2{};
      assert(!bs::parsePower(b.d, b.n - 1, 0, c2, s2));
      assert(!c2.crankValid && sameSample(s2, PowerSample{}));
    }
  }
}

static void testPowerFieldOrder() {
  // Balance, crank and energy around skipped fields of every size.
  const uint16_t flags = 0x0FFF;
  Buf b = powerBase(flags, 321);
  b.u8(60);                 // 0  balance
  b.fill(0xB1, 2);          // 2  torque
  b.fill(0xB2, 6);          // 4  wheel
  b.u16(500).u16(2048);     // 5  crank
  b.fill(0xB3, 4);          // 6  force
  b.fill(0xB4, 4);          // 7  torque magnitudes
  b.fill(0xB5, 3);          // 8  angles
  b.fill(0xB6, 2);          // 9  top dead spot
  b.fill(0xB7, 2);          // 10 bottom dead spot
  b.u16(4321);              // 11 energy
  assert(b.n == 34);

  CrankState crank{};
  PowerSample s{};
  assert(bs::parsePower(b.d, b.n, 0, crank, s));
  assert(s.watts == 321 && s.balance == 60 && s.balanceIsLeft);
  assert(s.hasEnergy && s.energyKJ == 4321 && s.flags == flags);
  assert(crank.crankValid && crank.prevCrankRev == 500 && crank.prevCrankTime == 2048);

  // Reserved bits 12..15 add no field and are kept in the raw flags.
  b.d[1] = 0xFF;
  PowerSample r{};
  CrankState rc{};
  assert(bs::parsePower(b.d, 34, 0, rc, r) && r.flags == 0xFFFF && r.energyKJ == 4321);

  // Every shorter length of the full packet is malformed.
  for (size_t n = 0; n < 34; ++n) {
    CrankState c2{};
    PowerSample s2{};
    assert(!bs::parsePower(b.d, n, 0, c2, s2));
    assert(!c2.crankValid);
  }

  // Energy cut while crank fits: still malformed, crank state unchanged.
  const CrankState before = crank;
  const PowerSample last = s;
  Buf c = powerBase(static_cast<uint16_t>((1u << 5) | (1u << 11)), 1);
  c.u16(501).u16(3072).u8(0x01);
  assert(!bs::parsePower(c.d, c.n, 100, crank, s));
  assert(sameCrank(before, crank) && sameSample(last, s));
}

static Buf powerCrank(uint16_t revs, uint16_t t, int16_t watts = 200) {
  Buf b = powerBase(1u << 5, watts);
  b.u16(revs).u16(t);
  return b;
}

static void testPowerCadence() {
  CrankState crank{};
  PowerSample s{};

  Buf p = powerCrank(10, 0);
  assert(bs::parsePower(p.d, p.n, 1000, crank, s) && !s.hasCadence && s.cadence == 0);

  p = powerCrank(11, 1024);
  assert(bs::parsePower(p.d, p.n, 2000, crank, s) && s.hasCadence && s.cadence == 60);
  assert(crank.lastCrankEventMs == 2000);

  // Same crank data: cadence kept, timer not refreshed.
  assert(bs::parsePower(p.d, p.n, 2050, crank, s) && s.cadence == 60);
  assert(crank.lastCrankEventMs == 2000);

  // Packet without bit 5 carries the last cadence.
  Buf noCrank = powerBase(0, 180);
  assert(bs::parsePower(noCrank.d, noCrank.n, 2100, crank, s));
  assert(s.watts == 180 && s.hasCadence && s.cadence == 60);

  // Crank wrap at 65535 for revs and time.
  p = powerCrank(65535, 64512);
  assert(bs::parsePower(p.d, p.n, 3000, crank, s));
  p = powerCrank(1, 1024);  // 2 revs in 2 s across both wraps
  assert(bs::parsePower(p.d, p.n, 4000, crank, s) && s.cadence == 60);

  // Clamp to 0xFFFF.
  p = powerCrank(2001, 1025);
  assert(bs::parsePower(p.d, p.n, 4100, crank, s) && s.cadence == 0xFFFF);
}

static void testPowerStale() {
  CrankState crank{};
  PowerSample s{};
  Buf p = powerCrank(10, 0);
  assert(bs::parsePower(p.d, p.n, 1000, crank, s));
  p = powerCrank(12, 1024);  // 120 rpm
  assert(bs::parsePower(p.d, p.n, 2000, crank, s) && s.cadence == 120);

  Buf noCrank = powerBase(0, 150);
  const uint32_t edge = 2000 + BBS_POWER_STALE_MS;
  assert(bs::parsePower(noCrank.d, noCrank.n, edge, crank, s) && s.cadence == 120);
  assert(bs::parsePower(noCrank.d, noCrank.n, edge + 1, crank, s));
  assert(s.cadence == 0 && s.hasCadence);

  // Packet with bit 5 but no new crank event after stale: still 0.
  assert(bs::parsePower(p.d, p.n, edge + 500, crank, s) && s.cadence == 0);

  // lastPower()/callback path: stale applied to a copy without a packet.
  PowerSample fresh{};
  CrankState c2{};
  Buf q = powerCrank(1, 0);
  assert(bs::parsePower(q.d, q.n, 100, c2, fresh));
  q = powerCrank(2, 1024);
  assert(bs::parsePower(q.d, q.n, 200, c2, fresh) && fresh.cadence == 60);
  PowerSample view = fresh;
  bs::applyPowerStale(view, c2, 200 + BBS_POWER_STALE_MS);
  assert(view.cadence == 60);
  view = fresh;
  bs::applyPowerStale(view, c2, 200 + BBS_POWER_STALE_MS + 1);
  assert(view.cadence == 0 && fresh.cadence == 60);

  // A new crank event revives the cadence.
  p = powerCrank(14, 2048 + 1024);  // 2 revs in 2 s
  assert(bs::parsePower(p.d, p.n, edge + 1000, crank, s) && s.cadence == 60);

  // millis() wrap: elapsed time is unsigned.
  CrankState w{};
  PowerSample ws{};
  q = powerCrank(1, 0);
  assert(bs::parsePower(q.d, q.n, 0xFFFFFFA0u, w, ws));
  q = powerCrank(2, 1024);
  assert(bs::parsePower(q.d, q.n, 0xFFFFFFC0u, w, ws) && ws.cadence == 60);
  assert(bs::parsePower(noCrank.d, noCrank.n, 0x00000020u, w, ws) && ws.cadence == 60);
  const uint32_t staleAfterWrap = 0xFFFFFFC0u + BBS_POWER_STALE_MS + 1u;
  assert(bs::parsePower(noCrank.d, noCrank.n, staleAfterWrap, w, ws) && ws.cadence == 0);
}

static void testPowerResetAfterDisconnect() {
  CrankState crank{};
  PowerSample s{};
  Buf p = powerCrank(100, 0);
  assert(bs::parsePower(p.d, p.n, 1000, crank, s));
  p = powerCrank(101, 1024);
  assert(bs::parsePower(p.d, p.n, 2000, crank, s) && s.cadence == 60);

  // Without a reset a reconnect would compare against stale counters.
  CrankState noReset = crank;
  PowerSample noResetSample = s;
  Buf jump = powerCrank(160, 2048);
  assert(bs::parsePower(jump.d, jump.n, 2500, noReset, noResetSample));
  assert(noResetSample.cadence == 3540);

  // Disconnect: crank state and last measurement are cleared.
  bs::resetCrank(crank);
  s = PowerSample{};
  assert(!crank.crankValid);
  assert(bs::parsePower(jump.d, jump.n, 2500, crank, s));
  assert(!s.hasCadence && s.cadence == 0);
  p = powerCrank(161, 3072);
  assert(bs::parsePower(p.d, p.n, 3500, crank, s) && s.hasCadence && s.cadence == 60);
}

int main() {
  testAdNameAndUuids();
  testAdMalformed();
  testMac();
  testHeartRate();
  testCscLengths();
  testCscCadence();
  testPowerShortAndMinimal();
  testPowerEachFlagBit();
  testPowerFieldOrder();
  testPowerCadence();
  testPowerStale();
  testPowerResetAfterDisconnect();
  return 0;
}
