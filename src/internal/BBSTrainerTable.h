#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "BBSCodec.h"
#include "BBSConfig.h"
#include "BBSTypes.h"

// Trainer mirror table (TRAINER_PROXY_DESIGN § 2.3), Arduino-free: UUIDs,
// services and characteristics in discovery order, the NVS blob with CRC32,
// the structure hash and the address derived from it, the check before
// storing, and the mirror's advertising data (§ 2.4).

namespace BikeSensor {
namespace internal {

constexpr uint8_t kTrnSvcMax = BBS_TRAINER_MAX_SERVICES;
constexpr uint8_t kTrnChrMax = BBS_TRAINER_MAX_CHRS;
constexpr uint8_t kTrnValueMax = BBS_TRAINER_VALUE_MAX;
constexpr uint8_t kTrnAdCopyMax = 16;   // copied service data (0x16) and appearance (0x19)
// GATT characteristic properties (= NimBLE's BLE_GATT_CHR_F_* for these bits).
constexpr uint8_t kPropRead = 0x02, kPropWriteNr = 0x04, kPropWrite = 0x08;
constexpr uint8_t kPropNotify = 0x10, kPropIndicate = 0x20;
constexpr uint8_t kTrnPropMask = 0x3E;
constexpr uint8_t kTrnBlobVersion = 3;  // raw TrnTable layout; bump on any change of it (3: values 104)

// Little endian as on air; len 2 or 16.
struct TrnUuid {
  uint8_t len;
  uint8_t b[16];
};

struct TrnChr {
  TrnUuid uuid;
  uint8_t svc;                          // index into TrnTable::svc; ascending
  uint8_t props;                        // & kTrnPropMask
  bool cccd;                            // the trainer has a CCCD (0x2902)
  uint8_t len;
  uint8_t value[kTrnValueMax];          // read at discovery; empty if not readable
};

struct TrnTable {
  uint8_t mac[6];                       // trainer, first octet as written
  uint8_t addrType;
  uint8_t adLen;
  uint8_t ad[kTrnAdCopyMax];            // advertising elements copied into the mirror's
  uint8_t svcCount;
  uint8_t chrCount;
  TrnUuid svc[kTrnSvcMax];
  TrnChr chr[kTrnChrMax];
};

// NVS blob `trn_gatt`: the table as is, byte-only members (no padding).
struct TrnBlob {
  uint8_t version;
  uint8_t crc[4];                       // CRC32 of table, little endian
  TrnTable table;
};
static_assert(sizeof(TrnBlob) == 5 + 26 + 17 * kTrnSvcMax + (21 + kTrnValueMax) * kTrnChrMax,
              "TrnBlob must stay byte-only");

// One ADV and one SCAN_RSP payload, each parsed on its own.
struct TrnAdv {
  uint8_t adv[31];
  uint8_t advLen;
  uint8_t rsp[31];
  uint8_t rspLen;
};

// Bluetooth Base UUID 00000000-0000-1000-8000-00805F9B34FB, little endian.
constexpr uint8_t kBaseUuid[16] = {0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
                                   0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// 2, 4 or 16 bytes little endian → 16-bit if it is a Base UUID with a 16-bit
// value, else 128-bit. False for other lengths.
inline bool makeUuid(const uint8_t* p, size_t len, TrnUuid& out) {
  uint8_t full[16];
  memcpy(full, kBaseUuid, sizeof(full));
  if (len == 2 || len == 4) {
    memcpy(full + 12, p, len);
  } else if (len == 16) {
    memcpy(full, p, len);
  } else {
    return false;
  }
  const bool short16 = memcmp(full, kBaseUuid, 12) == 0 && full[14] == 0 && full[15] == 0;
  out = TrnUuid{};
  out.len = short16 ? 2 : 16;
  memcpy(out.b, short16 ? full + 12 : full, out.len);
  return true;
}

inline uint16_t uuid16(const TrnUuid& u) { return u.len == 2 ? readU16(u.b) : 0; }
inline bool sameUuid(const TrnUuid& a, const TrnUuid& b) {
  return a.len == b.len && memcmp(a.b, b.b, a.len) == 0;
}

// § 2.2.2: GAP, GATT, DFU (Nordic Secure DFU, legacy DFU) are not mirrored;
// nor is a trainer HR service: the relay's own HR service stays.
inline bool skippedService(const TrnUuid& u) {
  static constexpr uint8_t kSecureDfu[16] = {0x50, 0xEA, 0xDA, 0x30, 0x88, 0x83, 0xB8, 0x9F,
                                             0x60, 0x4F, 0x15, 0xF3, 0x01, 0x00, 0xC9, 0x8E};
  static constexpr uint8_t kLegacyDfu[16] = {0x23, 0xD1, 0xBC, 0xEA, 0x5F, 0x78, 0x23, 0x15,
                                             0xDE, 0xEF, 0x12, 0x12, 0x30, 0x15, 0x00, 0x00};
  const uint16_t s = uuid16(u);
  return s == 0x1800 || s == 0x1801 || s == 0xFE59 || s == 0x180D ||
         (u.len == 16 && (memcmp(u.b, kSecureDfu, 16) == 0 || memcmp(u.b, kLegacyDfu, 16) == 0));
}

// IEEE CRC32 (zlib), chainable through crc.
inline uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
  crc = ~crc;
  while (n-- > 0) {
    crc ^= *p++;
    for (uint8_t k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

// § 2.3: service and characteristic UUIDs and properties in table order.
// Values, CCCD flags, address and advertising are not part of it.
inline uint32_t structureHash(const TrnTable& t) {
  uint32_t h = 0;
  for (uint8_t s = 0; s < t.svcCount; ++s) {
    const uint8_t tag = 'S';
    h = crc32(&tag, 1, h);
    h = crc32(&t.svc[s].len, 1u + t.svc[s].len, h);
    for (uint8_t i = 0; i < t.chrCount; ++i) {
      const TrnChr& c = t.chr[i];
      if (c.svc != s) continue;
      const uint8_t rec[2] = {'C', c.props};
      h = crc32(rec, sizeof(rec), h);
      h = crc32(&c.uuid.len, 1u + c.uuid.len, h);
    }
  }
  return h;
}

inline uint32_t mix32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x85EBCA6Bu;
  x ^= x >> 13;
  x *= 0xC2B2AE35u;
  return x ^ (x >> 16);
}

// § 2.3 rev 8: the mirror's static random address from the structure hash.
// All 48 bits of the chip's MAC keep two relays with the same trainer model
// apart. out[0] is the first octet as written; its two top bits are set.
inline void mirrorAddress(uint32_t hash, uint64_t chipMac, uint8_t out[6]) {
  const uint32_t chip = mix32(static_cast<uint32_t>(chipMac) ^ mix32(static_cast<uint32_t>(chipMac >> 32)));
  const uint32_t a = mix32(hash ^ chip);
  const uint32_t b = mix32(a ^ 0x5BD1E995u);
  for (uint8_t i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(a >> (24 - 8 * i));
  out[0] |= 0xC0;
  out[4] = static_cast<uint8_t>(b >> 8);
  out[5] = static_cast<uint8_t>(b);
}

inline uint32_t blobCrc(const TrnBlob& b) {
  return crc32(reinterpret_cast<const uint8_t*>(&b.table), sizeof(b.table));
}

inline void sealTable(TrnBlob& b) {
  b.version = kTrnBlobVersion;
  const uint32_t crc = blobCrc(b);
  for (uint8_t i = 0; i < 4; ++i) b.crc[i] = static_cast<uint8_t>(crc >> (8 * i));
}

// False for another length or version, a wrong CRC, counts or lengths beyond
// the limits, a bad UUID length or ungrouped characteristics.
inline bool openTable(const TrnBlob& b, size_t len) {
  const TrnTable& t = b.table;
  const uint32_t crc = readU16(b.crc) | static_cast<uint32_t>(readU16(b.crc + 2)) << 16;
  if (len != sizeof(b) || b.version != kTrnBlobVersion || crc != blobCrc(b) ||
      t.adLen > kTrnAdCopyMax || t.svcCount > kTrnSvcMax || t.chrCount > kTrnChrMax) {
    return false;
  }
  auto bad = [](const TrnUuid& u) { return u.len != 2 && u.len != 16; };
  for (uint8_t s = 0; s < t.svcCount; ++s) {
    if (bad(t.svc[s])) return false;
  }
  for (uint8_t i = 0; i < t.chrCount; ++i) {
    const TrnChr& c = t.chr[i];
    if (bad(c.uuid) || c.svc >= t.svcCount || (i > 0 && c.svc < t.chr[i - 1].svc) ||
        c.len > kTrnValueMax) {
      return false;
    }
  }
  return true;
}

// Calls f(type, payload, len) for each AD element of the ADV, then of the
// SCAN_RSP, each until a zero length (padding) or a malformed element.
template <typename F>
inline void forEachAd(const TrnAdv& a, F f) {
  for (uint8_t part = 0; part < 2; ++part) {
    const uint8_t* ad = part == 0 ? a.adv : a.rsp;
    const size_t len = part == 0 ? a.advLen : a.rspLen;
    for (size_t off = 0; off + 1 < len && ad[off] != 0 && ad[off] <= len - off - 1;
         off += 1u + ad[off]) {
      f(ad[off + 1], ad + off + 2, static_cast<uint8_t>(ad[off] - 1));
    }
  }
}

// Each service UUID of the 16- and 128-bit lists; with serviceData also the
// UUID of 16-bit service data (0x16; the NEO's ADV names FC82 only there).
template <typename F>
inline void forEachAdService(const TrnAdv& a, bool serviceData, F f) {
  forEachAd(a, [&f, serviceData](uint8_t type, const uint8_t* p, uint8_t n) {
    const bool data = serviceData && type == 0x16 && n >= 2;
    if (data) n = 2;
    const uint8_t size = (type == 0x02 || type == 0x03 || data) ? 2 : (type == 0x06 || type == 0x07) ? 16 : 0;
    for (uint8_t k = 0; size != 0 && k + size <= n; k += size) {
      TrnUuid u;
      makeUuid(p + k, size, u);
      f(u);
    }
  });
}

// Appends one AD element; the caller checks the room.
inline void putAd(uint8_t* buf, uint8_t& len, uint8_t type, const void* p, size_t n) {
  buf[len++] = static_cast<uint8_t>(n + 1);
  buf[len++] = type;
  memcpy(buf + len, p, n);
  len = static_cast<uint8_t>(len + n);
}

// Setup: the trainer's service data (0x16) and appearance (0x19) elements
// for the mirror's advertising, as far as they fit.
inline void copyAdFields(const TrnAdv& a, TrnTable& t) {
  t.adLen = 0;
  forEachAd(a, [&t](uint8_t type, const uint8_t* p, uint8_t n) {
    if ((type == 0x16 || type == 0x19) && t.adLen + 2u + n <= kTrnAdCopyMax) putAd(t.ad, t.adLen, type, p, n);
  });
}

inline int findService(const TrnTable& t, const TrnUuid& u) {
  for (uint8_t s = 0; s < t.svcCount; ++s) {
    if (sameUuid(t.svc[s], u)) return s;
  }
  return -1;
}

// Removes services without characteristics (renumbering the rest), except
// those the advertising lists or names in service data (ad may be null), and
// all of them if no SCAN_RSP was captured: checkTable() rejects them.
inline void dropEmptyServices(TrnTable& t, const TrnAdv* ad) {
  uint8_t n = 0;
  for (uint8_t s = 0; s < t.svcCount; ++s) {
    bool keep = false;
    for (uint8_t i = 0; i < t.chrCount; ++i) {
      if (t.chr[i].svc != s) continue;
      t.chr[i].svc = n;                 // n <= s: never matches a later s
      keep = true;
    }
    if (!keep && ad != nullptr) {
      keep = ad->rspLen == 0;
      forEachAdService(*ad, true, [&](const TrnUuid& u) { keep = keep || sameUuid(u, t.svc[s]); });
    }
    if (keep) t.svc[n++] = t.svc[s];
  }
  t.svcCount = n;
}

// G5: a discovery that missed characteristics (FC82 empty) left Zwift at
// "Connecting…". A table is complete only if nothing overflowed, every
// service has a characteristic, every notify/indicate characteristic has a
// CCCD, and every service UUID the trainer advertises (ad may be null) is in
// it, except the skipped ones.
inline TrainerCheck checkTable(const TrnTable& t, bool overflow, const TrnAdv* ad) {
  if (overflow) return TrainerCheck::TOO_LARGE;
  if (t.svcCount == 0 || t.chrCount == 0) return TrainerCheck::EMPTY;
  for (uint8_t s = 0, i = 0; s < t.svcCount; ++s) {
    if (i >= t.chrCount || t.chr[i].svc != s) return TrainerCheck::EMPTY_SERVICE;
    while (i < t.chrCount && t.chr[i].svc == s) ++i;
  }
  for (uint8_t i = 0; i < t.chrCount; ++i) {
    if ((t.chr[i].props & (kPropNotify | kPropIndicate)) != 0 && !t.chr[i].cccd) {
      return TrainerCheck::NO_CCCD;
    }
  }
  bool missing = false;
  if (ad != nullptr) {
    forEachAdService(*ad, false, [&](const TrnUuid& u) {
      missing = missing || (!skippedService(u) && findService(t, u) < 0);
    });
  }
  return missing ? TrainerCheck::MISSING_ADVERTISED : TrainerCheck::OK;
}

// § 2.4 (prototype layout): advertising = flags + the copied elements + the
// first 128-bit service if it fits; scan response = the complete 16-bit list
// (the table's without DIS 0x180A and battery 0x180F, then HR if hr) + the
// complete name. False if the scan response does not fit.
inline bool buildMirrorAdv(const TrnTable& t, const char* name, bool hr, TrnAdv& out) {
  out = TrnAdv{};
  const uint8_t flags[3] = {0x02, 0x01, 0x06};
  memcpy(out.adv, flags, sizeof(flags));
  memcpy(out.adv + sizeof(flags), t.ad, t.adLen);
  out.advLen = static_cast<uint8_t>(sizeof(flags) + t.adLen);
  for (uint8_t s = 0; s < t.svcCount; ++s) {
    if (t.svc[s].len != 16) continue;
    if (out.advLen + 18 <= 31) putAd(out.adv, out.advLen, 0x07, t.svc[s].b, 16);
    break;
  }
  uint8_t list[2 * (kTrnSvcMax + 1)];
  uint8_t n = 0;
  for (uint8_t s = 0; s <= t.svcCount; ++s) {
    const uint16_t u = s < t.svcCount ? uuid16(t.svc[s]) : hr ? 0x180D : 0;
    if (u == 0 || u == 0x180A || u == 0x180F) continue;
    list[n++] = static_cast<uint8_t>(u);
    list[n++] = static_cast<uint8_t>(u >> 8);
  }
  const size_t nameLen = strlen(name);
  if ((n > 0 ? 2u + n : 0u) + 2u + nameLen > 31) return false;
  if (n > 0) putAd(out.rsp, out.rspLen, 0x03, list, n);
  putAd(out.rsp, out.rspLen, 0x09, name, nameLen);
  return true;
}

}  // namespace internal
}  // namespace BikeSensor
