#pragma once

#include <cstdint>

#include "BBSTypes.h"

// The NimBLE calls the state machine needs, behind an Arduino-free interface.
// Device: NimBLEPort (BBSNimBLEPort.h). Host tests: a fake. All methods run on
// the update() task, except discActive() (any task, for scanning()).

namespace BikeSensor {
namespace internal {

// NimBLE host return codes used for decisions (ble_hs.h; BBSNimBLEPort.h
// static_asserts that they match).
constexpr int kHsEAlready = 2;
constexpr int kHsEBusy = 15;
constexpr int kHsENotSynced = 22;
constexpr uint16_t kConnHandleNone = 0xFFFF;         // BLE_HS_CONN_HANDLE_NONE

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
  // Kept while DISCONNECTING; kConnHandleNone only after NimBLE has called
  // onDisconnect/onConnectFail (NimBLEClient.cpp:1081–1087).
  virtual uint16_t connHandle(uint8_t slot) = 0;

  // One blocking GATT pass (§5.2): service, characteristic, subscribe, battery.
  // battery = 255 if not readable.
  virtual bool gattPass(uint8_t slot, SensorType type, uint8_t& battery) = 0;
  virtual void gattTimerStart(uint32_t ms) = 0;      // one-shot esp_timer
  virtual void gattTimerStop() = 0;

  // Local GATT server (§5.4). serverStart() creates the server, the local
  // measurement characteristics of the typeBit()s in typeMask and their
  // callbacks once, then starts the GATT server; true if already started.
  virtual bool serverStart(uint8_t typeMask) = 0;
  // Replaces the advertising data with name + the service UUIDs of typeMask.
  // Takes effect at the next advStart(). False if a field did not fit.
  virtual bool advSetData(const char* name, uint8_t typeMask) = 0;
  // Lower 32 bits of the eFuse MAC (2.x BBSIdentity suffix). Any task.
  virtual uint32_t chipId() = 0;
  virtual uint8_t centralCount() = 0;                // NimBLEServer::getConnectedCount()
  virtual bool advActive() = 0;                      // ble_gap_adv_active()
  virtual bool advStart() = 0;                       // NimBLEAdvertising::start()
  virtual bool advStop() = 0;                        // NimBLEAdvertising::stop()
  // setValue() + notify() on the local measurement characteristic of the
  // slot's type; no-op if there is none.
  virtual void relay(uint8_t slot, const uint8_t* data, uint8_t len) = 0;

  // Lifecycle (§4 initStack()/begin()/end()).
  virtual bool stackReady() = 0;                     // NimBLEDevice::isInitialized()
  virtual bool stackInit(const char* gapName) = 0;   // NimBLEDevice::init()
  // Per-slot clients, reset detector, GATT timer; existing ones are kept.
  virtual bool open() = 0;
  // end() steps 5 and 6: delete the GATT timer, NimBLEDevice::deinit(true).
  // False without either while a client still has a connection handle.
  virtual bool close() = 0;
  // end() drives update() itself: the clock and a one-tick wait between passes.
  virtual uint32_t nowMs() = 0;
  virtual void waitTick() = 0;

 protected:
  ~BlePort() = default;
};

}  // namespace internal
}  // namespace BikeSensor
