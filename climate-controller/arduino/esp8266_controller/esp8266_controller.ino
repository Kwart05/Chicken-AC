// =============================================================================
// esp8266_controller.ino — Chicken AC Real-Time Synchronized Controller
// Synchronizes TFT LCD, Local Web Server (192.168.4.1), and Render Cloud
// (chicken-ac.onrender.com) to refresh in lockstep every 2 seconds.
// Immediate screen update upon any manual toggle command execution.
// =============================================================================

#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

// ── Render Cloud Telemetry Endpoint ──────────────────────────────────────────
const char* SERVER_URL = "https://chicken-ac.onrender.com/api/telemetry";

// ── Hardware Pin Definitions ──────────────────────────────────────────────────
#define TFT_CS   D8   // GPIO 15 (TFT Chip Select)
#define TFT_DC   D3   // GPIO 0  (TFT Data/Command)
#define TFT_RST  -1   // TFT Reset wired directly to NodeMCU RST pin

#define DHTPIN       D4   // DHT22 Data Pin (GPIO 2)
#define DHTTYPE      DHT22
#define FAN_PIN      D1   // Ventilation Fan Relay (GPIO 5)
#define PUMP_PIN     D2   // Water Pump Relay (GPIO 4)
#define WATER_SW_PIN D6   // Water Level Float Switch (GPIO 12 to GND)

// ── Relay Logic Configuration (Active-LOW Relays) ────────────────────────────
#define RELAY_ON     LOW
#define RELAY_OFF    HIGH

// ── 16-bit 565 RGB Color Palette ─────────────────────────────────────────────
#define COLOR_BG       0x0824  // Dark Charcoal (#0a0e14)
#define COLOR_CARD     0x18E7  // Slate Card (#182028)
#define COLOR_HEADER   0x0210  // Dark Header Blue
#define COLOR_TEXT     0xFFFF  // Pure White
#define COLOR_CYAN     0x07FF  // Bright Cyan (#00ffff)
#define COLOR_ORANGE   0xFD20  // Warm Amber (#ff9900)
#define COLOR_GREEN    0x2E00  // Emerald Green (#2e7d32)
#define COLOR_RED      0xF800  // Crimson Red (#ff0000)
#define COLOR_GRAY     0x39E7  // Cool Gray (#3a444c)
#define COLOR_WHITE    0xFFFF  // White

// ── System Control Variables & States ────────────────────────────────────────
bool isAutoMode    = true;
bool fanState      = false;
bool pumpState     = false;
bool waterLevelOK  = true; // true = Water available (Float UP / Pin LOW)

float temperature  = NAN;
float humidity     = NAN;

float tempThreshold = 30.0; // Turn on Fan above this temp (°C)
float humThreshold  = 70.0; // Turn on Pump above this humidity limit (%)

DHT dht(DHTPIN, DHTTYPE, 15);
ESP8266WebServer server(80);
ESP8266WiFiMulti wifiMulti;
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);

unsigned long lastSensorRead = 0;
const long readInterval = 2000; // Synchronized 2-second refresh rate

// ── Function Declarations ────────────────────────────────────────────────────
void readSensors();
void processClimateLogic();
void handleRoot();
void handleGetData();
void handleToggle();
void sendHTTPTelemetry();
void processCommand(JsonObject doc);
void applyRelaysAndRefreshScreen();
void initTFTUI();
void updateTFTDisplay();
void drawWifiIcon(uint16_t x, uint16_t y);

// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nInitializing Synchronized Chicken AC Controller...");

  // Initialize GPIO Relay Pins
  pinMode(FAN_PIN, OUTPUT);
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(WATER_SW_PIN, INPUT_PULLUP);

  digitalWrite(FAN_PIN, RELAY_OFF);
  digitalWrite(PUMP_PIN, RELAY_OFF);

  // Initialize 2.8" SPI TFT LCD Display
  tft.begin();
  tft.setRotation(1); // Landscape mode 320x240
  initTFTUI();

  // Initialize DHT22 Temperature & Humidity Sensor
  dht.begin();

  // Configure Dual-Mode Wi-Fi:
  // Connect to Mobile Hotspots ('Kwart's iPhone', 'June') + Broadcast Open AP ('ChickenAC-Setup' @ 192.168.4.1)
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP("ChickenAC-Setup", NULL, 1, 0, 8);

  // Registered Hotspots for Internet Access
  wifiMulti.addAP("Kwart's iPhone", "1234567890..");
  wifiMulti.addAP("June", "senbonzakura");

  Serial.println("Connecting to Wi-Fi hotspots...");
  
  unsigned long startConnect = millis();
  while (wifiMulti.run() != WL_CONNECTED && millis() - startConnect < 4000) {
    delay(200);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi Connected! Local IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Hotspot offline. Standalone AP 'ChickenAC-Setup' ready at 192.168.4.1");
  }

  // HTTP Web Server Endpoints
  server.on("/", handleRoot);
  server.on("/api/data", handleGetData);
  server.on("/api/toggle", handleToggle);
  server.enableCORS(true);
  server.begin();
}

// =============================================================================
void loop() {
  server.handleClient();

  // Auto-reconnect to Wi-Fi hotspots in background
  if (WiFi.status() != WL_CONNECTED) {
    wifiMulti.run();
  }

  unsigned long now = millis();
  if (now - lastSensorRead >= readInterval) {
    lastSensorRead = now;
    readSensors();
    processClimateLogic();
  }

  // Listen for USB Serial Commands
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.length() > 0) {
      JsonDocument cmdDoc;
      DeserializationError err = deserializeJson(cmdDoc, input);
      if (!err) {
        processCommand(cmdDoc.as<JsonObject>());
        applyRelaysAndRefreshScreen();
      }
    }
  }

  yield();
}

// =============================================================================
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
  }

  // FLOAT SWITCH LOGIC:
  // When tank is FULL with water, float arm floats UP -> closes circuit to GND (Pin reads LOW -> Water OK).
  // When tank is EMPTY, float arm hangs DOWN -> open circuit (Pin reads HIGH -> Low Water).
  waterLevelOK = (digitalRead(WATER_SW_PIN) == LOW);
}

// =============================================================================
void processClimateLogic() {
  // COORDINATED EVAPORATIVE COOLING LOGIC:
  // In AUTO mode, update Fan and Pump based on temperature and humidity thresholds.
  if (isAutoMode) {
    if (!isnan(temperature) && !isnan(humidity)) {
      fanState = (temperature >= tempThreshold);
      pumpState = (humidity >= humThreshold) && fanState && waterLevelOK;
    } else {
      fanState = false;
      pumpState = false;
    }
  }

  applyRelaysAndRefreshScreen();

  // Stream JSON telemetry over USB Serial
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

  // Send telemetry to Render Cloud Server
  if (WiFi.status() == WL_CONNECTED) {
    sendHTTPTelemetry();
  }
}

// =============================================================================
void applyRelaysAndRefreshScreen() {
  // Safety rule: Never allow pump to run if water level is LOW
  if (!waterLevelOK) {
    pumpState = false;
  }

  digitalWrite(FAN_PIN,  fanState  ? RELAY_ON : RELAY_OFF);
  digitalWrite(PUMP_PIN, pumpState ? RELAY_ON : RELAY_OFF);

  // Immediately refresh TFT LCD screen with exact live states
  updateTFTDisplay();
}

// =============================================================================
void sendHTTPTelemetry() {
  WiFiClientSecure client;
  client.setInsecure(); // Disable SSL certificate verification for ESP8266 HTTPS
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
      bool stateChanged = false;
      for (JsonObject cmdObj : respDoc["cmds"].as<JsonArray>()) {
        processCommand(cmdObj);
        stateChanged = true;
      }
      if (stateChanged) {
        applyRelaysAndRefreshScreen();
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
    if (doc["val"].is<const char*>()) {
      const char* val = doc["val"];
      isAutoMode = (strcmp(val, "AUTO") == 0);
    } else if (doc["val"].is<int>()) {
      isAutoMode = (doc["val"].as<int>() == 1);
    }
  } else if (strcmp(cmd, "pump") == 0) {
    int val = doc["val"].as<int>();
    isAutoMode = false; // Toggling pump manually automatically engages MANUAL mode
    pumpState = (val == 1) && waterLevelOK;
  } else if (strcmp(cmd, "fan") == 0) {
    int val = doc["val"].as<int>();
    isAutoMode = false; // Toggling fan manually automatically engages MANUAL mode
    fanState = (val == 1);
  } else if (strcmp(cmd, "temp_thr") == 0) {
    float val = doc["val"].as<float>();
    if (val > 0 && val < 60) tempThreshold = val;
  } else if (strcmp(cmd, "hum_thr") == 0) {
    float val = doc["val"].as<float>();
    if (val > 0 && val <= 100) humThreshold = val;
  }
}

// =============================================================================
void handleToggle() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.hasArg("device")) {
    String device = server.arg("device");
    if (device == "mode") {
      isAutoMode = !isAutoMode;
    } else if (device == "fan") {
      isAutoMode = false;
      fanState = !fanState;
    } else if (device == "pump") {
      isAutoMode = false;
      pumpState = (!pumpState) && waterLevelOK;
    }
    applyRelaysAndRefreshScreen();
  }
  server.send(200, "text/plain", "OK");
}

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
// Embedded Mobile Dark-Mode Web Dashboard (Serves at http://192.168.4.1)
// =============================================================================
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>Chicken AC — Climate Controller</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: 'Segoe UI', system-ui, sans-serif; text-align: center; background: #0e1215; color: #e8e4da; padding: 20px; margin:0; }
        .card { background: #182026; max-width: 440px; margin: 20px auto; padding: 25px; border-radius: 14px; box-shadow: 0 8px 24px rgba(0,0,0,0.5); border: 1px solid #28343e; }
        .btn { padding: 12px 24px; font-size: 15px; margin: 8px; border: none; border-radius: 8px; cursor: pointer; font-weight: bold; transition: background 0.2s; }
        .on { background-color: #2e7d32; color: #e8f5e9; box-shadow: 0 0 10px rgba(46,125,50,0.4); }
        .off { background-color: #263238; color: #90a4ae; border: 1px solid #37474f; }
        .mode-btn { background-color: #0277bd; color: white; }
        .status { font-weight: bold; padding: 6px 14px; border-radius: 6px; font-size: 0.95em; }
        .ok { background: #1b382b; color: #4caf50; border: 1px solid #2e7d32; }
        .warn { background: #3e1b1b; color: #ef5350; border: 1px solid #c62828; }
        h2 { color: #f59e0b; margin-top:0; font-size: 1.5rem; letter-spacing: 0.05em; }
        .val { font-size: 2rem; font-weight: bold; color: #38bdf8; }
        .row { display: flex; justify-content: space-between; align-items: center; margin: 16px 0; padding: 10px 15px; background: #12181c; border-radius: 8px; }
    </style>
</head>
<body>
    <div class="card">
        <h2>CHICKEN AC CONTROLLER</h2>
        <p><span id="water" class="status ok">WATER LEVEL: OK</span></p>

        <div class="row">
            <div>TEMPERATURE</div>
            <div class="val"><span id="temp">--</span> °C</div>
        </div>
        <div class="row">
            <div>HUMIDITY</div>
            <div class="val"><span id="hum">--</span> %</div>
        </div>

        <button id="modeBtn" class="btn mode-btn" onclick="toggle('mode')">MODE</button>
        <hr style="border: 0; border-top: 1px solid #28343e; margin: 20px 0;">

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
                    w.innerText = data.waterLevelOK ? "WATER LEVEL: OK" : "WATER LEVEL: LOW";
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

        // Synchronized 2-second refresh rate (matching screen & cloud telemetry)
        setInterval(updateData, 2000);
        updateData();
    </script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

// =============================================================================
// 2.8" SPI TFT LCD Dashboard UI Layout (320x240 pixels)
// =============================================================================
void initTFTUI() {
  tft.fillScreen(COLOR_BG);

  // 1. Top Title Bar — Perfectly Centered Title
  tft.fillRect(0, 0, 320, 30, COLOR_HEADER);
  tft.setTextColor(COLOR_WHITE);
  tft.setTextSize(2);
  tft.setCursor(34, 7);
  tft.print("CHICKEN AC CONTROLLER");

  // 2. Temperature & Humidity Card Outlines (Y=34 to 128, H=94px)
  tft.drawRoundRect(8, 34, 148, 94, 6, COLOR_CARD);
  tft.drawRoundRect(164, 34, 148, 94, 6, COLOR_CARD);

  // Full Inscription Headers (TextSize 1)
  tft.setTextColor(COLOR_ORANGE);
  tft.setTextSize(1);
  tft.setCursor(14, 42);
  tft.print("TEMPERATURE");

  tft.setTextColor(COLOR_CYAN);
  tft.setTextSize(1);
  tft.setCursor(170, 42);
  tft.print("HUMIDITY");
}

void updateTFTDisplay() {
  // ── 1. Update Temperature Display (Largest TextSize 4) ───────────────────────
  tft.fillRect(12, 56, 140, 68, COLOR_BG);
  tft.setTextColor(COLOR_ORANGE);
  tft.setTextSize(4);
  tft.setCursor(14, 72);
  
  if (isnan(temperature)) {
    tft.print("--.-");
  } else {
    tft.print(temperature, 1);
  }

  // Draw Crisp Degree Circle (°) and Uppercase 'C'
  uint16_t curX = tft.getCursorX();
  uint16_t curY = tft.getCursorY();
  tft.drawCircle(curX + 3, curY + 3, 2, COLOR_ORANGE);
  tft.drawCircle(curX + 3, curY + 3, 3, COLOR_ORANGE);
  tft.setCursor(curX + 10, curY);
  tft.print("C");

  // ── 2. Update Humidity Display (Largest TextSize 4) ──────────────────────────
  tft.fillRect(168, 56, 140, 68, COLOR_BG);
  tft.setTextColor(COLOR_CYAN);
  tft.setTextSize(4);
  tft.setCursor(170, 72);
  
  if (isnan(humidity)) {
    tft.print("--.-");
  } else {
    tft.print(humidity, 1);
  }
  tft.print("%");

  // ── 3. MODE / FAN / PUMP Control Panel (Centered Values, H=52px) ───────────
  tft.fillRect(8, 132, 304, 52, COLOR_CARD);
  tft.drawRoundRect(8, 132, 304, 52, 6, COLOR_CYAN);

  // Sub-Box 1: MODE (Centered)
  tft.fillRect(12, 136, 92, 44, COLOR_BG);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_GRAY);
  tft.setCursor(42, 140);
  tft.print("MODE");
  tft.setTextSize(2);
  tft.setTextColor(isAutoMode ? COLOR_CYAN : COLOR_ORANGE);
  tft.setCursor(isAutoMode ? 34 : 34, 158);
  tft.print(isAutoMode ? "AUTO" : "MANU");

  // Sub-Box 2: FAN (Centered)
  tft.fillRect(112, 136, 92, 44, fanState ? COLOR_GREEN : COLOR_BG);
  tft.setTextSize(1);
  tft.setTextColor(fanState ? COLOR_WHITE : COLOR_GRAY);
  tft.setCursor(147, 140);
  tft.print("FAN");
  tft.setTextSize(2);
  tft.setTextColor(COLOR_WHITE);
  tft.setCursor(fanState ? 146 : 142, 158);
  tft.print(fanState ? "ON" : "OFF");

  // Sub-Box 3: PUMP (Centered)
  tft.fillRect(212, 136, 94, 44, pumpState ? COLOR_GREEN : COLOR_BG);
  tft.setTextSize(1);
  tft.setTextColor(pumpState ? COLOR_WHITE : COLOR_GRAY);
  tft.setCursor(244, 140);
  tft.print("PUMP");
  tft.setTextSize(2);
  tft.setTextColor(COLOR_WHITE);
  tft.setCursor(pumpState ? 247 : 243, 158);
  tft.print(pumpState ? "ON" : "OFF");

  // ── 4. Dynamic Water Tank Bar & Wi-Fi Icon Partitioning ───────────────────
  bool wifiConnected = (WiFi.status() == WL_CONNECTED);

  if (wifiConnected) {
    // Wi-Fi Available: Partition Bar (Bar W=252px, Wi-Fi Icon Box W=48px)
    tft.fillRect(8, 190, 252, 38, waterLevelOK ? COLOR_GREEN : COLOR_RED);
    tft.setTextColor(COLOR_WHITE);
    tft.setTextSize(2);
    tft.setCursor(waterLevelOK ? 44 : 38, 202);
    tft.print(waterLevelOK ? "WATER LEVEL: OK" : "WATER LEVEL: LOW");

    // Right Side Wi-Fi Icon Box
    tft.fillRect(264, 190, 48, 38, COLOR_BG);
    tft.drawRoundRect(264, 190, 48, 38, 6, COLOR_CARD);
    drawWifiIcon(288, 209);
  } else {
    // No Wi-Fi: Transverse Full Horizontal Width (W=304px)
    tft.fillRect(8, 190, 304, 38, waterLevelOK ? COLOR_GREEN : COLOR_RED);
    tft.setTextColor(COLOR_WHITE);
    tft.setTextSize(2);
    tft.setCursor(waterLevelOK ? 70 : 64, 202);
    tft.print(waterLevelOK ? "WATER LEVEL: OK" : "WATER LEVEL: LOW");
  }
}

// Draw Vector Wi-Fi Icon (3 Curved Arcs + Center Dot)
void drawWifiIcon(uint16_t x, uint16_t y) {
  tft.fillCircle(x, y, 2, COLOR_CYAN);
  tft.drawCircleHelper(x, y, 6, 1, COLOR_CYAN);
  tft.drawCircleHelper(x, y, 6, 2, COLOR_CYAN);
  tft.drawCircleHelper(x, y, 11, 1, COLOR_CYAN);
  tft.drawCircleHelper(x, y, 11, 2, COLOR_CYAN);
}
