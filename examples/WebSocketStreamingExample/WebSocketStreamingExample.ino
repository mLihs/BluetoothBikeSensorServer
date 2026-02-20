/*
 * BluetoothBikeSensorServer - WebSocket Streaming Example
 * 
 * This example demonstrates:
 * - ESPAsyncWebServer with WebSocket support
 * - Live streaming of Heart Rate (HR) and Cycling Speed/Cadence (CSC) data via WebSocket
 * - Web interface displaying real-time sensor data
 * - mDNS hostname based on BLE Relay server name
 * 
 * IMPORTANT: This library requires NimBLE-Arduino 2.x (NOT 1.x compatible).
 * 
 * Required libraries (install via Library Manager):
 * - ESPAsyncWebServer (https://github.com/me-no-dev/ESPAsyncWebServer)
 *   Note: ESPAsyncWebServer requires AsyncTCP (ESP32) - install separately if needed
 * - WiFi (ESP32 Arduino Core - included with ESP32 board package)
 * - ESPmDNS (ESP32 Arduino Core - included with ESP32 board package)
 * 
 * Hardware: ESP32
 * 
 * Features:
 * - Fast reconnection using Sensor MAC Preservation
 * - WebSocket-based real-time data streaming
 * - Modern web interface with live data display
 * - mDNS hostname matching BLE device name (homewind-xxxxxxxx.local)
 */

#include <BluetoothBikeSensorServer.h>
#include <DebugLog.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <HomewindBleIdentity.h>



// WiFi credentials - UPDATE THESE
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Sensor names - replace with your actual sensor names (exact match required)
const char* HR_SENSOR_NAME = "TICKR FIT C571";      // Replace with your HR sensor name
const char* CSC_SENSOR_NAME = "Ridesense";          // Replace with your CSC sensor name (optional)

// Web server
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// Latest sensor data (protected by access patterns)
uint16_t lastHeartRate = 0;
bool heartRateValid = false;
uint16_t lastSpeed = 0;      // in 0.01 m/s units
uint16_t lastCadence = 0;    // in rpm
bool speedCadenceValid = false;
unsigned long lastDataUpdate = 0;

// Web page HTML
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
  <title>BLE Bike Sensor - Live Data</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body {
      font-family: Arial, sans-serif;
      text-align: center;
      margin: 0;
      padding: 20px;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      justify-content: center;
      align-items: center;
    }
    .container {
      background: white;
      border-radius: 20px;
      padding: 40px;
      box-shadow: 0 10px 40px rgba(0,0,0,0.2);
      max-width: 600px;
      width: 100%;
    }
    h1 {
      color: #333;
      margin-bottom: 30px;
    }
    .sensor-card {
      background: #f8f9fa;
      border-radius: 15px;
      padding: 30px;
      margin: 20px 0;
      box-shadow: 0 4px 6px rgba(0,0,0,0.1);
    }
    .sensor-title {
      font-size: 18px;
      color: #666;
      margin-bottom: 15px;
      text-transform: uppercase;
      letter-spacing: 1px;
    }
    .sensor-value {
      font-size: 72px;
      font-weight: bold;
      color: #667eea;
      margin: 10px 0;
    }
    .sensor-unit {
      font-size: 24px;
      color: #999;
      margin-left: 10px;
    }
    .sensor-detail {
      font-size: 20px;
      color: #777;
      margin-top: 10px;
    }
    .status {
      margin-top: 30px;
      padding: 15px;
      border-radius: 10px;
      font-size: 14px;
    }
    .status.connected {
      background: #d4edda;
      color: #155724;
    }
    .status.disconnected {
      background: #f8d7da;
      color: #721c24;
    }
    .mdns-info {
      margin-top: 20px;
      padding: 15px;
      background: #e7f3ff;
      border-radius: 10px;
      font-size: 14px;
      color: #004085;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>🚴 BLE Bike Sensor Monitor</h1>
    
    <div class="sensor-card">
      <div class="sensor-title">Heart Rate</div>
      <div>
        <span class="sensor-value" id="hr-value">--</span>
        <span class="sensor-unit">bpm</span>
      </div>
      <div class="sensor-detail" id="hr-status">Waiting for sensor...</div>
    </div>
    
    <div class="sensor-card">
      <div class="sensor-title">Speed & Cadence</div>
      <div>
        <span class="sensor-value" id="speed-value">--</span>
        <span class="sensor-unit">km/h</span>
      </div>
      <div class="sensor-detail">
        <span id="cadence-value">--</span> <span class="sensor-unit">rpm</span>
      </div>
      <div class="sensor-detail" id="csc-status">Waiting for sensor...</div>
    </div>
    
    <div class="status disconnected" id="ws-status">
      WebSocket: Disconnected
    </div>
    
    <div class="mdns-info">
      <strong>mDNS Hostname:</strong> <span id="mdns-name">homewind-xxxxxxxx.local</span><br>
      <small>Access this page via: http://<span id="mdns-name-2">homewind-xxxxxxxx.local</span></small>
    </div>
  </div>
  
  <script>
    var gateway = `ws://${window.location.hostname}/ws`;
    var websocket;
    
    window.addEventListener('load', onload);
    
    function onload(event) {
      initWebSocket();
      // Update mDNS name display (will be updated from server)
      document.getElementById('mdns-name').textContent = window.location.hostname + '.local';
      document.getElementById('mdns-name-2').textContent = window.location.hostname + '.local';
    }
    
    function initWebSocket() {
      console.log('Trying to open a WebSocket connection...');
      websocket = new WebSocket(gateway);
      websocket.onopen = onOpen;
      websocket.onclose = onClose;
      websocket.onmessage = onMessage;
    }
    
    function onOpen(event) {
      console.log('Connection opened');
      document.getElementById('ws-status').className = 'status connected';
      document.getElementById('ws-status').textContent = 'WebSocket: Connected';
    }
    
    function onClose(event) {
      console.log('Connection closed');
      document.getElementById('ws-status').className = 'status disconnected';
      document.getElementById('ws-status').textContent = 'WebSocket: Disconnected';
      setTimeout(initWebSocket, 2000);
    }
    
    function onMessage(event) {
      var data = JSON.parse(event.data);
      console.log('Received:', data);
      
      // Update Heart Rate
      if (data.hr !== undefined) {
        document.getElementById('hr-value').textContent = data.hr;
        document.getElementById('hr-status').textContent = 'Connected';
      } else if (data.hrValid === false) {
        document.getElementById('hr-value').textContent = '--';
        document.getElementById('hr-status').textContent = 'No data';
      }
      
      // Update Speed & Cadence
      if (data.speed !== undefined && data.cadence !== undefined) {
        document.getElementById('speed-value').textContent = data.speed;
        document.getElementById('cadence-value').textContent = data.cadence;
        document.getElementById('csc-status').textContent = 'Connected';
      } else if (data.cscValid === false) {
        document.getElementById('speed-value').textContent = '--';
        document.getElementById('cadence-value').textContent = '--';
        document.getElementById('csc-status').textContent = 'No data';
      }
    }
  </script>
</body>
</html>
)rawliteral";

// Data update callbacks - store latest values
void onHeartRateChanged(uint16_t hr) {
  lastHeartRate = hr;
  heartRateValid = true;
  lastDataUpdate = millis();
  LOGI_F("[HR] Heart rate: %u bpm", hr);
  
  // Send via WebSocket
  sendWebSocketUpdate();
}

void onSpeedCadenceChanged(uint16_t speed, uint16_t cadence) {
  lastSpeed = speed;
  lastCadence = cadence;
  speedCadenceValid = true;
  lastDataUpdate = millis();
  LOGI_F("[CSC] Speed: %.1f km/h, Cadence: %u rpm", speed / 100.0 * 3.6, cadence);
  
  // Send via WebSocket
  sendWebSocketUpdate();
}

// Send latest sensor data via WebSocket
void sendWebSocketUpdate() {
  if (ws.count() == 0) {
    return; // No clients connected
  }
  
  // Build JSON message
  String json = "{";
  
  // Heart Rate
  if (heartRateValid) {
    json += "\"hr\":" + String(lastHeartRate) + ",";
  } else {
    json += "\"hrValid\":false,";
  }
  
  // Speed & Cadence
  if (speedCadenceValid) {
    float speedKmh = lastSpeed / 100.0 * 3.6;
    json += "\"speed\":" + String(speedKmh, 1) + ",";
    json += "\"cadence\":" + String(lastCadence) + ",";
  } else {
    json += "\"cscValid\":false,";
  }
  
  // Remove trailing comma and close JSON
  if (json.endsWith(",")) {
    json.remove(json.length() - 1);
  }
  json += "}";
  
  // Send to all connected clients
  ws.textAll(json);
}

// WebSocket event handler
void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, 
                      AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    LOGI_F("[WebSocket] Client #%u connected from %s", client->id(), client->remoteIP().toString().c_str());
    // Send current data immediately on connect
    sendWebSocketUpdate();
  } else if (type == WS_EVT_DISCONNECT) {
    LOGI_F("[WebSocket] Client #%u disconnected", client->id());
  } else if (type == WS_EVT_DATA) {
    // Handle incoming data if needed
    LOGD_F("[WebSocket] Data received from client #%u", client->id());
  }
}

// Sensor connection callbacks
void onHRConnected() {
  LOGI("[HR] ✓ Heart rate sensor connected!");
  
  char mac[18];
  if (BikeSensorServerGetSensorMac(SENSOR_TYPE_HEART_RATE, mac, sizeof(mac))) {
    LOGI_F("[HR] MAC address: %s", mac);
  }
}

void onHRDisconnected() {
  LOGW("[HR] ✗ Heart rate sensor disconnected!");
  heartRateValid = false;
  sendWebSocketUpdate(); // Notify clients
}

void onCSCConnected() {
  LOGI("[CSC] ✓ Speed/Cadence sensor connected!");
  
  char mac[18];
  if (BikeSensorServerGetSensorMac(SENSOR_TYPE_SPEED_CADENCE, mac, sizeof(mac))) {
    LOGI_F("[CSC] MAC address: %s", mac);
  }
}

void onCSCDisconnected() {
  LOGW("[CSC] ✗ Speed/Cadence sensor disconnected!");
  speedCadenceValid = false;
  sendWebSocketUpdate(); // Notify clients
}

// Central device callbacks
void onCentralConnected(uint8_t count) {
  LOGI_F("[Central] Device connected. Total: %u", count);
}

void onCentralDisconnected(uint8_t count) {
  LOGI_F("[Central] Device disconnected. Remaining: %u", count);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  
  LOGI("");
  LOGI("========================================");
  LOGI("BluetoothBikeSensorServer");
  LOGI("WebSocket Streaming Example");
  LOGI("========================================");
  LOGI("");
  
  // Initialize the BLE library first
  // Device name is now MAC-based: "{prefix}-xxxxxxxx" (where xxxxxxxx is the WiFi-style suffix from eFuse MAC)
  BluetoothBikeSensorServerBegin(nullptr, 
                                 SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_SPEED_CADENCE);
  
  // Enable Sensor MAC Preservation for fast reconnection
  BikeSensorServerSetPreserveMacOnDisconnect(true);
  LOGI("✓ Fast reconnection enabled (Sensor MAC Preservation)");
  LOGI("");
  
  // Set data callbacks
  BluetoothBikeSensorServerSetHeartRateCallback(onHeartRateChanged);
  BluetoothBikeSensorServerSetSpeedCadenceCallback(onSpeedCadenceChanged);
  
  // Set sensor connection callbacks
  BikeSensorServerSetSensorConn(SENSOR_TYPE_HEART_RATE, onHRConnected);
  BikeSensorServerSetSensorDisc(SENSOR_TYPE_HEART_RATE, onHRDisconnected);
  BikeSensorServerSetSensorConn(SENSOR_TYPE_SPEED_CADENCE, onCSCConnected);
  BikeSensorServerSetSensorDisc(SENSOR_TYPE_SPEED_CADENCE, onCSCDisconnected);
  
  // Set central device callbacks
  BikeSensorServerSetClientConn(onCentralConnected);
  BikeSensorServerSetClientDisc(onCentralDisconnected);
  
  LOGI("Adding sensors...");
  
  // Add Heart Rate sensor
  if (BikeSensorServerAddSensor(SENSOR_TYPE_HEART_RATE, HR_SENSOR_NAME)) {
    LOGI_F("  ✓ HR sensor added: %s", HR_SENSOR_NAME);
  } else {
    LOGE("  ✗ Failed to add HR sensor");
  }
  
  // Add Speed/Cadence sensor (optional - comment out if you only have HR)
  if (BikeSensorServerAddSensor(SENSOR_TYPE_SPEED_CADENCE, CSC_SENSOR_NAME)) {
    LOGI_F("  ✓ CSC sensor added: %s", CSC_SENSOR_NAME);
  } else {
    LOGE("  ✗ Failed to add CSC sensor");
  }
  
  LOGI("");
  LOGI("BLE Relay Server initialized. Library will scan and connect automatically.");
  LOGI("");
  
  // Connect to WiFi (BLE continues running during WiFi connection)
  LOGI_F("Connecting to WiFi: %s", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    LOGV(".");
    // Keep BLE running during WiFi connection
    BluetoothBikeSensorServerLoop();
  }
  
  LOGI("");
  IPAddress localIP = WiFi.localIP();
  LOGI_F("WiFi connected! IP address: %d.%d.%d.%d", localIP[0], localIP[1], localIP[2], localIP[3]);
  LOGI("");
  
  // Get mDNS hostname from BLE identity (matches BLE device name)
  String mdnsHostname = homewind::GetMdnsHostnameLower();
  LOGI_F("BLE Relay Server name: %s", BluetoothBikeSensorServerGetServerName());
  LOGI_F("mDNS hostname: %s", mdnsHostname.c_str());
  LOGI("");
  
  // Setup mDNS
  if (MDNS.begin(mdnsHostname.c_str())) {
    MDNS.addService("http", "tcp", 80);
    LOGI_F("✓ mDNS started: http://%s.local/", mdnsHostname.c_str());
  } else {
    LOGW("✗ mDNS failed to start");
  }
  LOGI("");
  
  // Setup WebSocket
  ws.onEvent(onWebSocketEvent);
  server.addHandler(&ws);
  
  // Route for root / web page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html);
  });
  
  // Start web server
  server.begin();
  LOGI("✓ Web server started on port 80");
  LOGI_F("  Access via: http://%s.local/ or http://%d.%d.%d.%d/", 
         mdnsHostname.c_str(), localIP[0], localIP[1], localIP[2], localIP[3]);
  LOGI("");
  
  // Check initial advertising status
  delay(500);
  if (BikeSensorServerIsAdvertising()) {
    LOGI("✓ Relay server is advertising and should be visible in BLE scanners/Zwift");
  } else {
    LOGW("⚠️  Relay server is NOT advertising - attempting to start...");
    if (BikeSensorServerRestartAdvertising()) {
      LOGI("✓ Advertising started successfully");
    } else {
      LOGE("✗ Failed to start advertising");
    }
  }
  LOGI("");
}

void loop() {
  // Must call this regularly to handle BLE operations
  BluetoothBikeSensorServerLoop();
  
  // Periodically send updates even if no new data (to keep connection alive)
  static uint32_t lastPeriodicUpdate = 0;
  if (millis() - lastPeriodicUpdate >= 5000) { // Every 5 seconds
    lastPeriodicUpdate = millis();
    if (ws.count() > 0) {
      sendWebSocketUpdate();
    }
  }



  
  // Clean up WebSocket connections if needed
  ws.cleanupClients();
  
  delay(10); // Small delay to prevent overwhelming
}

