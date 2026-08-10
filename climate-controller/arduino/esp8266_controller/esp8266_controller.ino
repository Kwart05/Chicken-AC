// =============================================================================
// esp8266_controller.ino — Chicken AC ESP8266 WiFi Climate Controller
// Tailored for Active-LOW Relays, D5 (DHT22), D1 (Fan), D2 (Pump), D6 (Water SW)
// =============================================================================

#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <DHT.h>

// ── Python Backend / Render Server URL ────────────────────────────────────────
const char* SERVER_URL = "https://chicken-ac.onrender.com/api/telemetry";

// ── Pin Definitions ──────────────────────────────────────────────────────────
#define DHTPIN       D5   // Data pin for DHT22
#define DHTTYPE      DHT22
#define FAN_PIN      D1   // Relay controlling Ventilation Fan
#define PUMP_PIN     D2   // Relay controlling Water Pump
#define WATER_SW_PIN D6   // Water Level Float Switch (Connected to GND)

// ── Relay Logic Configuration (Active-LOW relays) ────────────────────────────
#define RELAY_ON     LOW
#define RELAY_OFF    HIGH

// ── Automatic Control Thresholds ─────────────────────────────────────────────
float tempThreshold = 30.0; // Turn on Fan above this temp (°C)
float humThreshold  = 70.0; // Turn on Pump above/below humidity limit (%)

// ── Global Variables & States ────────────────────────────────────────────────
bool isAutoMode    = true;
bool fanState      = false;
bool pumpState     = false;
bool waterLevelOK  = true; // true = Water available, false = Tank empty

float temperature  = NAN;
float humidity     = NAN;

DHT dht(DHTPIN, DHTTYPE, 15);
ESP8266WebServer server(80);
ESP8266WiFiMulti wifiMulti;

unsigned long lastSensorRead = 0;
const long readInterval = 2000; // Read sensors every 2 seconds

// ── Function Declarations ────────────────────────────────────────────────────
void readSensors();
void processClimateLogic();
void handleRoot();
void handleGetData();
void handleToggle();
void sendHTTPTelemetry();
void processCommand(JsonObject doc);

// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("\nStarting Chicken AC ESP8266 Controller...");

  // Initialize GPIO Pins
  pinMode(FAN_PIN, OUTPUT);
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(WATER_SW_PIN, INPUT_PULLUP);

  digitalWrite(FAN_PIN, RELAY_OFF);
  digitalWrite(PUMP_PIN, RELAY_OFF);

  // Initialize DHT Sensor
  dht.begin();

  // Method 1: Register Multiple Known Wi-Fi Hotspots
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP("Kwart's iPhone", "1234567890..");
  wifiMulti.addAP("June", "senbonzakura");

  Serial.println("Attempting connection to known Wi-Fi hotspots...");
  
  // Try connecting for 10 seconds to known APs
  unsigned long startConnect = millis();
  while (wifiMulti.run() != WL_CONNECTED && millis() - startConnect < 10000) {
    delay(500);
    Serial.print(".");
  }

  // Method 2: Fallback to WiFiManager Captive Portal if no known AP is reachable
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nKnown Wi-Fi not found! Launching 'ChickenAC-Setup' portal...");
    WiFi.mode(WIFI_AP_STA);
    WiFiManager wm;
    wm.setConfigPortalTimeout(180); // Stay open for 3 minutes
    wm.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    if (!wm.startConfigPortal("ChickenAC-Setup")) {
      Serial.println("Portal timeout. Proceeding without Wi-Fi...");
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to Wi-Fi!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  }

  // Define Web Server Endpoints
  server.on("/", handleRoot);
  server.on("/api/data", handleGetData);
  server.on("/api/toggle", handleToggle);
  
  server.enableCORS(true);
  server.begin();
}

// =============================================================================
void loop() {
  server.handleClient();

  // Auto-reconnect to best available hotspot ("Kwart's iPhone" or "June")
  if (WiFi.status() != WL_CONNECTED) {
    wifiMulti.run();
  }

  unsigned long now = millis();

  // Periodic Sensor Readings & Logic Loop
  if (now - lastSensorRead >= readInterval) {
    lastSensorRead = now;
    readSensors();
    processClimateLogic();
  }

  // Check incoming Serial commands from Python server if connected via USB
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.length() > 0) {
      JsonDocument cmdDoc;
      DeserializationError err = deserializeJson(cmdDoc, input);
      if (!err) {
        processCommand(cmdDoc.as<JsonObject>());
      }
    }
  }

  yield(); // Feed ESP8266 Watchdog
}

// =============================================================================
// Read DHT22 and Water Level Switch
void readSensors() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(h) || isnan(t)) {
    delay(50);
    h = dht.readHumidity();
    t = dht.readTemperature();
  }

  if (!isnan(h) && !isnan(t)) {
    humidity = h;
    temperature = t;
  } else {
    Serial.println(F("Failed to read from DHT sensor!"));
  }

  // Float Switch logic: Reading LOW means circuit is pulled down to GND (Water OK)
  waterLevelOK = (digitalRead(WATER_SW_PIN) == LOW);
}

// =============================================================================
// Execute Control Logic
void processClimateLogic() {
  // AUTO MODE LOGIC: Calculate automatic actuator states
  if (isAutoMode) {
    if (!isnan(temperature) && !isnan(humidity)) {
      fanState  = (temperature >= tempThreshold);
      pumpState = (humidity >= humThreshold);
    }
    // WATER SAFETY LOCKOUT (In AUTO mode, force pump OFF if water level is low)
    if (!waterLevelOK) {
      pumpState = false;
    }
  }

  // Apply Hardware States to Active-LOW Relays
  digitalWrite(FAN_PIN,  fanState  ? RELAY_ON : RELAY_OFF);
  digitalWrite(PUMP_PIN, pumpState ? RELAY_ON : RELAY_OFF);

  // Print JSON telemetry to Serial (for USB COM9 mode)
  JsonDocument doc;
  if (isnan(temperature)) doc["t"] = nullptr; else doc["t"] = round(temperature * 10.0) / 10.0;
  if (isnan(humidity))    doc["h"] = nullptr; else doc["h"] = round(humidity    * 10.0) / 10.0;
  doc["p"]  = pumpState    ? 1 : 0;
  doc["f"]  = fanState     ? 1 : 0;
  doc["m"]  = isAutoMode   ? "AUTO" : "MANUAL";
  doc["w"]  = waterLevelOK ? 1 : 0;
  doc["tt"] = tempThreshold;
  doc["ht"] = humThreshold;

  String jsonStr;
  serializeJson(doc, jsonStr);
  Serial.println(jsonStr);

  // Send HTTP Telemetry over WiFi to Python Backend if connected
  if (WiFi.status() == WL_CONNECTED) {
    sendHTTPTelemetry();
  }
}

// =============================================================================
// Post Telemetry to Python App /api/telemetry
void sendHTTPTelemetry() {
  WiFiClient client;
  HTTPClient http;

  if (!http.begin(client, SERVER_URL)) return;

  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  if (isnan(temperature)) doc["t"] = nullptr; else doc["t"] = round(temperature * 10.0) / 10.0;
  if (isnan(humidity))    doc["h"] = nullptr; else doc["h"] = round(humidity    * 10.0) / 10.0;
  doc["p"]  = pumpState    ? 1 : 0;
  doc["f"]  = fanState     ? 1 : 0;
  doc["m"]  = isAutoMode   ? "AUTO" : "MANUAL";
  doc["w"]  = waterLevelOK ? 1 : 0;
  doc["tt"] = tempThreshold;
  doc["ht"] = humThreshold;

  String jsonStr;
  serializeJson(doc, jsonStr);

  int httpCode = http.POST(jsonStr);

  if (httpCode == HTTP_CODE_OK) {
    String response = http.getString();
    JsonDocument respDoc;
    DeserializationError err = deserializeJson(respDoc, response);
    if (!err && respDoc["cmds"].is<JsonArray>()) {
      JsonArray cmds = respDoc["cmds"].as<JsonArray>();
      for (JsonObject cmdObj : cmds) {
        processCommand(cmdObj);
      }
    }
  }

  http.end();
}

// =============================================================================
void processCommand(JsonObject doc) {
  const char* cmd = doc["cmd"];
  if (!cmd) return;

  if (strcmp(cmd, "mode") == 0) {
    const char* val = doc["val"];
    if (val && strcmp(val, "MANUAL") == 0) isAutoMode = false;
    else                                   isAutoMode = true;

  } else if (strcmp(cmd, "pump") == 0) {
    if (!isAutoMode) {
      pumpState = (doc["val"].as<int>() == 1);
    }

  } else if (strcmp(cmd, "fan") == 0) {
    if (!isAutoMode) {
      fanState = (doc["val"].as<int>() == 1);
    }

  } else if (strcmp(cmd, "temp_thr") == 0) {
    float val = doc["val"].as<float>();
    if (val > 0 && val < 60) tempThreshold = val;

  } else if (strcmp(cmd, "hum_thr") == 0) {
    float val = doc["val"].as<float>();
    if (val > 0 && val <= 100) humThreshold = val;
  }
}



// =============================================================================
// Handler for incoming toggle commands directly on ESP8266 Web Server
void handleToggle() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.hasArg("device")) {
    String device = server.arg("device");

    if (device == "mode") {
      isAutoMode = !isAutoMode;
    } 
    else if (device == "fan") {
      isAutoMode = false;
      fanState = !fanState;
    } 
    else if (device == "pump") {
      isAutoMode = false;
      pumpState = !pumpState;
    }
  }
  server.send(200, "text/plain", "OK");
}

// =============================================================================
// Dynamic API Endpoint to return JSON directly to web browsers
void handleGetData() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  JsonDocument doc;
  doc["temperature"]  = isnan(temperature) ? 0.0 : temperature;
  doc["humidity"]     = isnan(humidity) ? 0.0 : humidity;
  doc["fan"]          = fanState;
  doc["pump"]         = pumpState;
  doc["waterLevelOK"] = waterLevelOK;
  doc["autoMode"]     = isAutoMode;

  String jsonResponse;
  serializeJson(doc, jsonResponse);
  server.send(200, "application/json", jsonResponse);
}

// =============================================================================
// Built-in Web Control Dashboard (at http://<ESP8266_IP>/)
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>Chicken AC — Climate Controller</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: 'Segoe UI', system-ui, sans-serif; text-align: center; background: #0e1215; color: #e8e4da; padding: 20px; margin:0; }
        .card { background: #182026; max-width: 440px; margin: 30px auto; padding: 25px; border-radius: 14px; box-shadow: 0 8px 24px rgba(0,0,0,0.5); border: 1px solid #28343e; }
        .btn { padding: 12px 24px; font-size: 15px; margin: 8px; border: none; border-radius: 8px; cursor: pointer; font-weight: bold; transition: background 0.2s; }
        .on { background-color: #2e7d32; color: #e8f5e9; box-shadow: 0 0 10px rgba(46,125,50,0.4); }
        .off { background-color: #263238; color: #90a4ae; border: 1px solid #37474f; }
        .mode-btn { background-color: #0277bd; color: white; }
        .status { font-weight: bold; padding: 4px 10px; border-radius: 4px; font-size: 0.9em; }
        .ok { background: #1b382b; color: #4caf50; border: 1px solid #2e7d32; }
        .warn { background: #3e1b1b; color: #ef5350; border: 1px solid #c62828; }
        h2 { color: #f59e0b; margin-top:0; font-size: 1.5rem; letter-spacing: 0.05em; }
        .val { font-size: 1.8rem; font-weight: bold; color: #38bdf8; }
        .row { display: flex; justify-content: space-between; align-items: center; margin: 16px 0; padding: 10px; background: #12181c; border-radius: 8px; }
    </style>
</head>
<body>
    <div class="card">
        <h2>CHICKEN AC</h2>
        <p style="color:#64748b; font-size:0.85rem; margin-top:-10px;">Direct ESP8266 Controller</p>
        
        <div class="row">
            <span>Temperature</span>
            <span class="val"><span id="temp">--</span> &deg;C</span>
        </div>
        <div class="row">
            <span>Humidity</span>
            <span class="val"><span id="hum">--</span> %</span>
        </div>
        <div class="row">
            <span>Water Reservoir</span>
            <span id="water" class="status">--</span>
        </div>
        
        <hr style="border-color:#28343e; margin: 20px 0;">
        
        <div class="row">
            <span>Control Mode</span>
            <button id="modeBtn" class="btn mode-btn" onclick="toggle('mode')">--</button>
        </div>
        <div class="row">
            <span>Ventilation Fan</span>
            <button id="fanBtn" class="btn" onclick="toggle('fan')">--</button>
        </div>
        <div class="row">
            <span>Water Pump</span>
            <button id="pumpBtn" class="btn" onclick="toggle('pump')">--</button>
        </div>
    </div>

    <script>
        function updateData() {
            fetch('/api/data')
                .then(res => res.json())
                .then(data => {
                    document.getElementById('temp').innerText = (data.temperature || 0).toFixed(1);
                    document.getElementById('hum').innerText = (data.humidity || 0).toFixed(1);
                    
                    const w = document.getElementById('water');
                    w.innerText = data.waterLevelOK ? "WATER OK" : "LOW WATER";
                    w.className = "status " + (data.waterLevelOK ? "ok" : "warn");

                    document.getElementById('modeBtn').innerText = data.autoMode ? "AUTO MODE" : "MANUAL MODE";
                    
                    const fanBtn = document.getElementById('fanBtn');
                    fanBtn.innerText = data.fan ? "FAN ON" : "FAN OFF";
                    fanBtn.className = "btn " + (data.fan ? "on" : "off");

                    const pumpBtn = document.getElementById('pumpBtn');
                    pumpBtn.innerText = data.pump ? "PUMP ON" : "PUMP OFF";
                    pumpBtn.className = "btn " + (data.pump ? "on" : "off");
                });
        }

        function toggle(device) {
            fetch('/api/toggle?device=' + device)
                .then(() => updateData());
        }

        setInterval(updateData, 1500);
        updateData();
    </script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}
