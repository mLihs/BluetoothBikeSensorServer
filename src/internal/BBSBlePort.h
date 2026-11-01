#pragma once

#include <cstdint>

#include "BBSTypes.h"

// The NimBLE calls the state machine needs, behind an Arduino-free interface.
// Device: NimBLEPort (BBSNimBLEPort.h). Host tests: a fake. All methods run on
// the update() task.

namespace BikeSensor {
namespace internal {

// NimBLE host return codes used for decisions (ble_hs.h; BBSNimBLEPort.h
// static_asserts that they match).
constexpr int kHsEAlready = 2;
constexpr int kHsEBusy = 15;
constexpr int kHsENotSynced = 22;

class BlePort {
 public:
  virtual bool hsSynced() = 0;                       // ble_hs_synced()
  virtual bool discActive() = 0;                     // ble_gap_disc_active()
  virtual bool connActive() = 0;                     // ble_gap_conn_active()
  // ble_gap_disc() with cb_arg = epoch; returns its rc.
  virtual int discStart(uint16_t epoch, uint32_t durationMs) = 0;
  virtual int discCancel() = 0;                      // ble_gap_disc_cancel() rc

  // Per-slot client. connect() sets the timeout and 0 retries, then calls
  // connect(addr, true, true). mac[0] is the first octet as written.
  virtual bool connect(uint8_t slot, const uint8_t mac[6], uint8_t addrType) = 0;
  virtual int lastError(uint8_t slot) = 0;
  virtual bool cancelConnect(uint8_t slot) = 0;
  virtual bool disconnect(uint8_t slot) = 0;
  virtual bool isConnected(uint8_t slot) = 0;
  virtual uint16_t connHandle(uint8_t slot) = 0;

  // One blocking GATT pass (§5.2): service, characteristic, subscribe, battery.
  // battery = 255 if not readable.
  virtual bool gattPass(uint8_t slot, SensorType type, uint8_t& battery) = 0;
  virtual void gattTimerStart(uint32_t ms) = 0;      // one-shot esp_timer
  virtual void gattTimerStop() = 0;

 protected:
  ~BlePort() = default;
};

}  // namespace internal
}  // namespace BikeSensor
