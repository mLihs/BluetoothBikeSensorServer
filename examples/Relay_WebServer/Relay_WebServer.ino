/*
 * BluetoothBikeSensorServer - Relay_WebServer
 *
 * HR + CSC sensors by name, relayed to BLE centrals, plus a web page with live
 * heart rate and cadence over a WebSocket. The mDNS hostname is the relay
 * server name (serverName(), "homewind-xxxxxxxx").
 *
 * Required libraries: ESPAsyncWebServer and AsyncTCP. WiFi and ESPmDNS come
 * with the ESP32 core.
 *
 * AsyncTCP callbacks never call BikeSensor: the WebSocket connect handler only
 * sets a flag, and loop() builds and sends every frame.
 */

#include <BikeSensor.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

namespace bs = BikeSensor;

// WiFi credentials - UPDATE THESE
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Sensor names - replace with your actual sensor names (exact match required)
const char* HR_SENSOR_NAME = "TICKR FIT C571";
const char* CSC_SENSOR_NAME = "Ridesense";

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// Set from the AsyncTCP task, read in loop().
volatile bool sendRequested = false;

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
      <div class="sensor-title">Cadence</div>
      <div>
        <span class="sensor-value" id="cadence-value">--</span>
        <span class="sensor-unit">rpm</span>
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
      
      // Update Cadence
      if (data.cadence !== undefined) {
        document.getElementById('cadence-value').textContent = data.cadence;
        document.getElementById('csc-status').textContent = 'Connected';
      } else if (data.cscValid === false) {
        document.getElementById('cadence-value').textContent = '--';
        document.getElementById('csc-status').textContent = 'No data';
      }
    }
  </script>
</body>
</html>
)rawliteral";

// Latest values, read in loop() only (the getters copy under the library's lock).
void sendUpdate() {
  if (ws.count() == 0) return;
  uint16_t value;
  uint32_t ageMs;
  char hr[20] = "\"hrValid\":false";
  char cadence[20] = "\"cscValid\":false";
  if (bs::lastHeartRate(value, ageMs)) snprintf(hr, sizeof(hr), "\"hr\":%u", value);
  if (bs::lastCadence(value, ageMs)) snprintf(cadence, sizeof(cadence), "\"cadence\":%u", value);
  char json[48];
  snprintf(json, sizeof(json), "{%s,%s}", hr, cadence);
  ws.textAll(json);
}

void onHeartRate(uint16_t) { sendRequested = true; }
void onCadence(uint16_t) { sendRequested = true; }

void onEvent(const bs::EventInfo& info) {
  switch (info.type) {
    case bs::EventType::SENSOR_CONNECTED:
      Serial.printf("[0x%02x] connected\n", bs::typeBit(info.sensorType));
      break;
    case bs::EventType::SENSOR_DISCONNECTED:
      Serial.printf("[0x%02x] disconnected: %s\n", bs::typeBit(info.sensorType),
                    bs::disconnectReasonString(info.reason));
      sendRequested = true;
      break;
    case bs::EventType::CENTRAL_CONNECTED:
    case bs::EventType::CENTRAL_DISCONNECTED:
      Serial.printf("[Central] count %u\n", info.count);
      break;
    default:
      break;
  }
}

// AsyncTCP task: no BikeSensor calls here.
void onWebSocketEvent(AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type, void*,
                      uint8_t*, size_t) {
  const unsigned long id = client->id();
  if (type == WS_EVT_CONNECT) {
    Serial.printf("[WebSocket] client #%lu connected\n", id);
    sendRequested = true;
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("[WebSocket] client #%lu disconnected\n", id);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nRelay_WebServer");

  const uint8_t types =
      bs::typeBit(bs::SensorType::HEART_RATE) | bs::typeBit(bs::SensorType::SPEED_CADENCE);
  if (!bs::begin("homewind", types, bs::Mode::NAME)) {
    Serial.println("begin() failed");
    return;
  }
  bs::setEventCallback(onEvent);
  bs::setHeartRateCallback(onHeartRate);
  bs::setCadenceCallback(onCadence);
  Serial.printf("HR \"%s\": %s\n", HR_SENSOR_NAME,
                bs::addByName(bs::SensorType::HEART_RATE, HR_SENSOR_NAME) ? "ok" : "rejected");
  Serial.printf("CSC \"%s\": %s\n", CSC_SENSOR_NAME,
                bs::addByName(bs::SensorType::SPEED_CADENCE, CSC_SENSOR_NAME) ? "ok" : "rejected");

  Serial.printf("Connecting to WiFi: %s\n", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    bs::update();   // BLE keeps running while WiFi connects
    delay(10);
  }
  Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());

  char hostname[48];
  bs::serverName(hostname, sizeof(hostname));
  if (MDNS.begin(hostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS: http://%s.local/\n", hostname);
  } else {
    Serial.println("mDNS failed to start");
  }

  ws.onEvent(onWebSocketEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET,
            [](AsyncWebServerRequest* request) { request->send(200, "text/html", index_html); });
  server.begin();
  Serial.printf("Relay server advertising: %u\n", bs::advertising());
}

void loop() {
  bs::update();

  static uint32_t lastPeriodicMs = 0;
  if (millis() - lastPeriodicMs >= 5000) {   // keep idle pages fresh
    lastPeriodicMs = millis();
    sendRequested = true;
  }
  if (sendRequested) {
    sendRequested = false;
    sendUpdate();
  }
  ws.cleanupClients();
  delay(10);
}
