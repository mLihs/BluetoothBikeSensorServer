#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "BBSEventQueue.h"

// The only path from NimBLE callbacks (host task) into the library: build one
// Event and push it. Never touch slot state, never call the application (§2).

namespace BikeSensor {
namespace internal {

inline Event makeEvent(Ev kind, uint8_t slot, int16_t reason, uint16_t gen) {
  Event ev{};
  ev.kind = kind;
  ev.slot = slot;
  ev.reason = reason;
  ev.gen = gen;
  return ev;
}

// name is not NUL-terminated (AD data); kept to 31 bytes plus NUL.
inline bool pushScanMatch(EventQueue& q, uint16_t epoch, const uint8_t addr[6],
                          uint8_t addrType, const char* name, size_t nameLen) {
  Event ev = makeEvent(Ev::SCAN_MATCH, 0, 0, epoch);
  memcpy(ev.match.addr, addr, sizeof(ev.match.addr));
  ev.match.addrType = addrType;
  if (name == nullptr) nameLen = 0;
  if (nameLen > sizeof(ev.match.name) - 1) nameLen = sizeof(ev.match.name) - 1;
  if (nameLen > 0) memcpy(ev.match.name, name, nameLen);
  ev.match.name[nameLen] = '\0';
  return q.push(ev);
}

inline bool pushScanEnd(EventQueue& q, uint16_t epoch, int16_t reason) {
  return q.push(makeEvent(Ev::SCAN_END, 0, reason, epoch));
}

inline bool pushHostReset(EventQueue& q) {
  return q.push(makeEvent(Ev::HOST_RESET, 0, 0, 0));
}

inline bool pushConnected(EventQueue& q, uint8_t slot, uint16_t attemptGen) {
  return q.push(makeEvent(Ev::CONNECTED, slot, 0, attemptGen));
}

inline bool pushConnectFail(EventQueue& q, uint8_t slot, uint16_t attemptGen,
                            int16_t reason) {
  return q.push(makeEvent(Ev::CONNECT_FAIL, slot, reason, attemptGen));
}

inline bool pushDisconnected(EventQueue& q, uint8_t slot, uint16_t attemptGen,
                             int16_t reason) {
  return q.push(makeEvent(Ev::DISCONNECTED, slot, reason, attemptGen));
}

// Copies at most BBS_NOTIFY_MAX_LEN bytes; reason keeps the original length,
// so update() can detect truncation (reason > notify.len).
inline bool pushNotify(EventQueue& q, uint8_t slot, uint16_t attemptGen,
                       const uint8_t* data, size_t len) {
  if (data == nullptr) len = 0;
  const int16_t original = len > INT16_MAX ? INT16_MAX : static_cast<int16_t>(len);
  Event ev = makeEvent(Ev::NOTIFY, slot, original, attemptGen);
  const size_t copied = len > BBS_NOTIFY_MAX_LEN ? BBS_NOTIFY_MAX_LEN : len;
  if (copied > 0) memcpy(ev.notify.data, data, copied);
  ev.notify.len = static_cast<uint8_t>(copied);
  return q.push(ev);
}

inline bool pushCentralConnect(EventQueue& q) {
  return q.push(makeEvent(Ev::CENTRAL_CONNECT, 0, 0, 0));
}

inline bool pushCentralDisconnect(EventQueue& q, int16_t reason) {
  return q.push(makeEvent(Ev::CENTRAL_DISCONNECT, 0, reason, 0));
}

inline bool pushSubscribe(EventQueue& q, uint16_t subValue) {
  Event ev = makeEvent(Ev::SUBSCRIBE, 0, 0, 0);
  ev.sub.subValue = subValue;
  return q.push(ev);
}

}  // namespace internal
}  // namespace BikeSensor
