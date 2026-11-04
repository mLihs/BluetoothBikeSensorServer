/**
 * @file NimBLEHeapTest.ino
 * @brief Example to evaluate heap fragmentation and usage caused by NimBLE alone,
 *        the additional impact of BluetoothBikeSensorServer library, and sensor connections
 * 
 * This example first initializes only NimBLE (no services, no advertising) 
 * and monitors heap fragmentation. After a few ticks, it then initializes
 * BluetoothBikeSensorServer to show the additional overhead of the library.
 * Finally, it connects to a sensor by MAC address to show the overhead of active connections.
 * 
 * It prints heap statistics in the format:
 * [Heap] TICK 07:26:02: free=110136, largest=57332, frag=47.9%, drift=70096, minFree=60324
 * 
 * Hardware: ESP32
 * 
 * Required libraries:
 * - NimBLE-Arduino (2.x required)
 * - BluetoothBikeSensorServer
 * 
 * Usage:
 * 1. Update HR_SENSOR_MAC and HR_ADDR_TYPE below with your sensor's MAC address and type
 * 2. Upload to ESP32
 * 3. Open Serial Monitor at 115200 baud
 * 4. Observe heap statistics printed every 30 seconds
 * 
 * The example will:
 * - Capture baseline heap before BLE initialization
 * - Initialize NimBLE with minimal configuration (no services, no advertising)
 * - Monitor heap for a few ticks (NimBLE only)
 * - Initialize BluetoothBikeSensorServer (shows library overhead)
 * - Monitor heap for a few more ticks (Library only, no sensor)
 * - Connect to a sensor by MAC address (shows connection overhead)
 * - Continue monitoring to see the difference
 * - Print statistics in the requested format
 */

#include <NimBLEDevice.h>
#include <BikeSensor.h>

// Sensor configuration (MAC mode - no name needed, just MAC address)
// 
// IMPORTANT: BLE Address Type
// ===========================
// Each BLE device has an address type shown in Discovery output:
//   [HR] TICKR FIT (f0:13:c3:ee:56:97) 0  <- use 0
//   [CSC] Speed Sensor (fc:a6:1f:27:45:16) 1  <- use 1
//
// If your sensor doesn't connect, try the other address type (0 or 1)!
// Set to nullptr to skip sensor connection phase
const char* HR_SENSOR_MAC = "f0:13:c3:ee:56:97";  // Replace with your sensor's MAC address
const int8_t HR_ADDR_TYPE = 0;  // Use 0 or 1 from Discovery output! (or -1 to skip sensor phase)

// Heap monitoring configuration
const uint32_t HEAP_TICK_INTERVAL_MS = 15000;  // Print every 30 seconds
const uint32_t TICKS_BEFORE_LIB_INIT = 2;      // Number of ticks before initializing library
const uint32_t TICKS_BEFORE_SENSOR_ADD = 2;     // Number of ticks after lib init before adding sensor

// Baseline heap (captured before BLE init)
uint32_t baselineFree = 0;
uint32_t baselineLargest = 0;

// NimBLE-only heap (captured after NimBLE init, before library init)
uint32_t nimbleOnlyFree = 0;
uint32_t nimbleOnlyLargest = 0;

// Library-only heap (captured after library init, before sensor connection)
uint32_t libraryOnlyFree = 0;
uint32_t libraryOnlyLargest = 0;

// State tracking
bool libraryInitialized = false;
bool sensorAdded = false;
uint32_t tickCount = 0;
uint32_t ticksSinceLibInit = 0;

// Helper function to format uptime as HH:MM:SS
void formatUptime(uint32_t uptimeMs, char* buffer, size_t bufferSize) {
  uint32_t totalSeconds = uptimeMs / 1000;
  uint32_t hours = totalSeconds / 3600;
  uint32_t minutes = (totalSeconds % 3600) / 60;
  uint32_t seconds = totalSeconds % 60;
  
  snprintf(buffer, bufferSize, "%02lu:%02lu:%02lu", hours, minutes, seconds);
}

// Helper function to calculate fragmentation percentage
float calcFragmentation(uint32_t largest, uint32_t free) {
  if (free == 0) return 0.0f;
  return (1.0f - ((float)largest / (float)free)) * 100.0f;
}

// Print heap statistics in the requested format
void printHeapTick(const char* phase = nullptr) {
  uint32_t now = millis();
  uint32_t free = ESP.getFreeHeap();
  uint32_t largest = ESP.getMaxAllocHeap();
  uint32_t minFree = ESP.getMinFreeHeap();
  
  // Calculate fragmentation
  float frag = calcFragmentation(largest, free);
  
  // Calculate drift from baseline
  int32_t drift = baselineFree > 0 ? (int32_t)(baselineFree - free) : 0;
  
  // Format uptime
  char uptimeBuf[12];
  formatUptime(now, uptimeBuf, sizeof(uptimeBuf));
  
  // Print in the exact format requested
  Serial.printf("[Heap] TICK %s: free=%u, largest=%u, frag=%.1f%%, drift=%d, minFree=%u",
                uptimeBuf, free, largest, frag, drift, minFree);
  
  // Add phase indicator if provided
  if (phase) {
    Serial.printf(" [%s]", phase);
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("");
  Serial.println("========================================");
  Serial.println("NimBLE Heap Fragmentation Test");
  Serial.println("(NimBLE only - no services, no advertising)");
  Serial.println("========================================");
  Serial.println("");
  
  // Capture baseline heap BEFORE BLE initialization
  baselineFree = ESP.getFreeHeap();
  baselineLargest = ESP.getMaxAllocHeap();
  
  Serial.printf("[Heap] BASELINE (before BLE init): free=%u, largest=%u\n", 
                baselineFree, baselineLargest);
  Serial.println("");
  
  // Initialize NimBLE with minimal configuration
  // This initializes the BLE stack but doesn't create any services or start advertising
  Serial.println("Initializing NimBLE (minimal configuration)...");
  
  // Initialize NimBLE device (this allocates the BLE stack)
  NimBLEDevice::init("NimBLEHeapTest");
  
  // Set power level (optional, but matches typical usage)
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  
  Serial.println("NimBLE initialized (no services, no advertising)");
  Serial.println("");
  
  // Capture heap after BLE initialization
  uint32_t afterInitFree = ESP.getFreeHeap();
  uint32_t afterInitLargest = ESP.getMaxAllocHeap();
  int32_t initCost = (int32_t)(baselineFree - afterInitFree);
  float initFrag = calcFragmentation(afterInitLargest, afterInitFree);
  
  Serial.printf("[Heap] AFTER_BLE_INIT: free=%u, largest=%u, frag=%.1f%%, cost=%d\n",
                afterInitFree, afterInitLargest, initFrag, initCost);
  Serial.println("");
  
  // Store NimBLE-only heap state
  nimbleOnlyFree = afterInitFree;
  nimbleOnlyLargest = afterInitLargest;
  
  Serial.println("Starting continuous heap monitoring...");
  Serial.printf("Will initialize BluetoothBikeSensorServer after %u tick(s) (~%u seconds)\n",
                TICKS_BEFORE_LIB_INIT, TICKS_BEFORE_LIB_INIT * (HEAP_TICK_INTERVAL_MS / 1000));
  Serial.println("Heap statistics will be printed every 30 seconds");
  Serial.println("");
  Serial.println("Format: [Heap] TICK HH:MM:SS: free=..., largest=..., frag=...%, drift=..., minFree=... [PHASE]");
  Serial.println("");
  Serial.println("Phases:");
  Serial.println("  [NimBLE] = Only NimBLE initialized");
  Serial.println("  [NimBLE+Lib] = NimBLE + BluetoothBikeSensorServer initialized (no sensors)");
  if (HR_SENSOR_MAC != nullptr && HR_ADDR_TYPE >= 0) {
    Serial.printf("  [NimBLE+Lib+Sensor] = NimBLE + Library + Sensor connected (MAC: %s)\n", HR_SENSOR_MAC);
  }
  Serial.println("");
  
  // Print first tick immediately
  printHeapTick("NimBLE");
}

void loop() {
  // Print heap statistics periodically
  static uint32_t lastTickMs = 0;
  uint32_t now = millis();
  
  if (now - lastTickMs >= HEAP_TICK_INTERVAL_MS) {
    lastTickMs = now;
    tickCount++;
    
    // Initialize library after specified number of ticks
    if (!libraryInitialized && tickCount >= TICKS_BEFORE_LIB_INIT) {
      Serial.println("");
      Serial.println("========================================");
      Serial.println("Initializing BluetoothBikeSensorServer...");
      Serial.println("========================================");
      Serial.println("");
      
      // Capture heap before library initialization
      uint32_t beforeLibFree = ESP.getFreeHeap();
      uint32_t beforeLibLargest = ESP.getMaxAllocHeap();
      
      Serial.printf("[Heap] BEFORE_LIB_INIT: free=%u, largest=%u\n",
                    beforeLibFree, beforeLibLargest);
      Serial.println("");
      
      // Initialize BluetoothBikeSensorServer in MAC mode (required for MAC-based sensor connection)
      // Enable Heart Rate sensor type (needed for sensor connection)
      Serial.println("Initializing BluetoothBikeSensorServer (MAC mode, HR enabled)...");
      // NimBLE is already up, so begin() skips the stack init
      if (BikeSensor::begin("NimBLEHeapTest", BikeSensor::typeBit(BikeSensor::SensorType::HEART_RATE),
                            BikeSensor::Mode::MAC)) {
        Serial.println("BluetoothBikeSensorServer initialized");
      } else {
        Serial.println("ERROR: begin() failed!");
      }
      Serial.println("");
      
      // Capture heap after library initialization
      uint32_t afterLibFree = ESP.getFreeHeap();
      uint32_t afterLibLargest = ESP.getMaxAllocHeap();
      int32_t libCost = (int32_t)(beforeLibFree - afterLibFree);
      float libFrag = calcFragmentation(afterLibLargest, afterLibFree);
      
      Serial.printf("[Heap] AFTER_LIB_INIT: free=%u, largest=%u, frag=%.1f%%, cost=%d\n",
                    afterLibFree, afterLibLargest, libFrag, libCost);
      Serial.println("");
      
      // Calculate total cost from NimBLE-only state
      int32_t totalCostFromNimble = (int32_t)(nimbleOnlyFree - afterLibFree);
      Serial.printf("[Heap] LIBRARY_OVERHEAD (vs NimBLE only): cost=%d bytes\n",
                    totalCostFromNimble);
      Serial.println("");
      
      // Store library-only heap state
      libraryOnlyFree = afterLibFree;
      libraryOnlyLargest = afterLibLargest;
      
      libraryInitialized = true;
      ticksSinceLibInit = 0;
    }
    
    // Add sensor after library has been initialized for a few ticks
    if (libraryInitialized && !sensorAdded && HR_SENSOR_MAC != nullptr && HR_ADDR_TYPE >= 0) {
      ticksSinceLibInit++;
      
      if (ticksSinceLibInit >= TICKS_BEFORE_SENSOR_ADD) {
        Serial.println("");
        Serial.println("========================================");
        Serial.println("Adding Sensor by MAC Address...");
        Serial.println("========================================");
        Serial.println("");
        
        // Capture heap before adding sensor
        uint32_t beforeSensorFree = ESP.getFreeHeap();
        uint32_t beforeSensorLargest = ESP.getMaxAllocHeap();
        
        Serial.printf("[Heap] BEFORE_SENSOR_ADD: free=%u, largest=%u\n",
                      beforeSensorFree, beforeSensorLargest);
        Serial.println("");
        
        Serial.printf("Adding Heart Rate sensor: MAC=%s, addrType=%d\n",
                      HR_SENSOR_MAC, HR_ADDR_TYPE);
        
        // Add sensor by MAC address
        if (BikeSensor::addByMac(BikeSensor::SensorType::HEART_RATE, HR_SENSOR_MAC, HR_ADDR_TYPE)) {
          Serial.println("Sensor added successfully (will connect when found)");
        } else {
          Serial.println("ERROR: Failed to add sensor!");
        }
        Serial.println("");
        
        // Capture heap after adding sensor (before connection)
        uint32_t afterAddFree = ESP.getFreeHeap();
        uint32_t afterAddLargest = ESP.getMaxAllocHeap();
        int32_t addCost = (int32_t)(beforeSensorFree - afterAddFree);
        float addFrag = calcFragmentation(afterAddLargest, afterAddFree);
        
        Serial.printf("[Heap] AFTER_SENSOR_ADD (not connected yet): free=%u, largest=%u, frag=%.1f%%, cost=%d\n",
                      afterAddFree, afterAddLargest, addFrag, addCost);
        Serial.println("");
        
        Serial.println("Waiting for sensor connection...");
        Serial.println("(Heap will be monitored when sensor connects)");
        Serial.println("");
        
        sensorAdded = true;
      }
    }
    
    // Check if sensor is connected and capture heap when connection is established
    static bool sensorConnectionLogged = false;
    if (sensorAdded && !sensorConnectionLogged) {
      if (BikeSensor::connected(BikeSensor::SensorType::HEART_RATE)) {
        Serial.println("");
        Serial.println("========================================");
        Serial.println("Sensor Connected!");
        Serial.println("========================================");
        Serial.println("");
        
        // Capture heap after sensor connection
        uint32_t afterConnFree = ESP.getFreeHeap();
        uint32_t afterConnLargest = ESP.getMaxAllocHeap();
        int32_t connCost = (int32_t)(libraryOnlyFree - afterConnFree);
        float connFrag = calcFragmentation(afterConnLargest, afterConnFree);
        
        Serial.printf("[Heap] AFTER_SENSOR_CONNECT: free=%u, largest=%u, frag=%.1f%%, cost=%d\n",
                      afterConnFree, afterConnLargest, connFrag, connCost);
        Serial.println("");
        
        // Calculate total cost from library-only state
        Serial.printf("[Heap] SENSOR_CONNECTION_OVERHEAD (vs Library only): cost=%d bytes\n",
                      connCost);
        Serial.println("");
        
        sensorConnectionLogged = true;
      }
    }
    
    // Print heap tick with appropriate phase indicator
    if (sensorAdded && BikeSensor::connected(BikeSensor::SensorType::HEART_RATE)) {
      printHeapTick("NimBLE+Lib+Sensor");
    } else if (libraryInitialized) {
      printHeapTick("NimBLE+Lib");
    } else {
      printHeapTick("NimBLE");
    }
  }
  
  // Call library loop if initialized (keeps it running)
  if (libraryInitialized) {
    BikeSensor::update();
  }
  
  delay(10);  // Small delay to prevent overwhelming the system
}
