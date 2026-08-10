#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h> // Install "ArduinoJson" by Benoit Blanchon via Library Manager
#include "DHT.h"

// --- Wi-Fi Credentials ---
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// --- Pin Definitions ---
#define DHTPIN       D5   // Data pin for DHT22
#define DHTTYPE      DHT22
#define FAN_PIN      D1   // Relay controlling the Fan
#define PUMP_PIN     D2   // Relay controlling the Pump
#define WATER_SW_PIN D6   // Water Level Float Switch (Connected to GND)

// --- Relay Logic Configuration ---
// Set to LOW if using standard Active-LOW relay modules, or HIGH for Active-HIGH
#define RELAY_ON     LOW
#define RELAY_OFF    HIGH

// --- Automatic Control Thresholds ---
float tempThreshold = 28.0; // Turn on Fan above this temp (°C)
float humThreshold  = 60.0; // Turn on Pump below this humidity (%)

// --- Global Variables & States ---
bool isAutoMode    = true;
bool fanState      = false;
bool pumpState     = false;
bool waterLevelOK  = false; // true = Water available, false = Tank empty

float temperature  = 0.0;
float humidity     = 0.0;

DHT dht(DHTPIN, DHTTYPE);
ESP8266WebServer server(80);

unsigned long lastSensorRead = 0;
const long readInterval = 2000; // Read sensors every 2 seconds

// --- Function Declarations ---
void readSensors();
void processClimateLogic();
void handleRoot();
void handleGetData();
void handleToggle();

void setup() {
  Serial.begin(115200);
  
  // Initialize GPIO Pins
  pinMode(FAN_PIN, OUTPUT);
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(WATER_SW_PIN, INPUT_PULLUP); // Uses internal pullup resistor

  digitalWrite(FAN_PIN, RELAY_OFF);
  digitalWrite(PUMP_PIN, RELAY_OFF);

  // Initialize DHT Sensor
  dht.begin();

  // Connect to Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: http://");
  Serial.println(WiFi.localIP());

  // Define Web Server Endpoints
  server.on("/", handleRoot);
  server.on("/api/data", handleGetData);     // Fetch current telemetry (JSON)
  server.on("/api/toggle", handleToggle);   // Control endpoints via API
  
  // Enable CORS so your external web app can hit this API directly
  server.enableCORS(true);
  server.begin();
}

void loop() {
  server.handleClient();

  // Periodic Sensor Readings & Logic Loop
  if (millis() - lastSensorRead >= readInterval) {
    lastSensorRead = millis();
    readSensors();
    processClimateLogic();
  }
}

// Read DHT22 and Water Level Switch
void readSensors() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (!isnan(h) && !isnan(t)) {
    humidity = h;
    temperature = t;
  } else {
    Serial.println(F("Failed to read from DHT sensor!"));
  }

  // Float Switch logic: Reading LOW means circuit is pulled down to GND (Water OK)
  waterLevelOK = (digitalRead(WATER_SW_PIN) == LOW);
}

// Execute Control Logic
void processClimateLogic() {
  // WATER SAFETY LOCKOUT: Force pump OFF if water level is low
  if (!waterLevelOK) {
    pumpState = false;
  }

  // AUTO MODE LOGIC
  if (isAutoMode) {
    // Fan Logic: Turn ON if temperature exceeds limit
    fanState = (temperature >= tempThreshold);

    // Pump Logic: Turn ON if humidity drops below limit AND water level is safe
    if (waterLevelOK) {
      pumpState = (humidity < humThreshold);
    }
  }

  // Apply Hardware States
  digitalWrite(FAN_PIN, fanState ? RELAY_ON : RELAY_OFF);
  digitalWrite(PUMP_PIN, pumpState ? RELAY_ON : RELAY_OFF);
}

// Dynamic API Endpoint to return JSON to your Web App
void handleGetData() {
  StaticJsonDocument<256> doc;
  doc["temperature"]  = temperature;
  doc["humidity"]     = humidity;
  doc["fan"]          = fanState;
  doc["pump"]         = pumpState;
  doc["waterLevelOK"] = waterLevelOK;
  doc["autoMode"]     = isAutoMode;

  String jsonResponse;
  serializeJson(doc, jsonResponse);
  server.send(200, "application/json", jsonResponse);
}

// Handler for incoming toggle commands from web interface
void handleToggle() {
  if (server.hasArg("device")) {
    String device = server.arg("device");

    if (device == "mode") {
      isAutoMode = !isAutoMode;
    } 
    else if (device == "fan" && !isAutoMode) {
      fanState = !fanState;
    } 
    else if (device == "pump" && !isAutoMode) {
      // Allow turning pump ON only if water safety switch is OK
      if (waterLevelOK) {
        pumpState = !pumpState;
      }
    }
  }
  server.send(200, "text/plain", "OK");
}

// Built-in Web Control Dashboard
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>Evaporative Cooler Controller</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial, sans-serif; text-align: center; background: #f2f2f2; padding: 20px; }
        .card { background: white; max-width: 400px; margin: 0 auto; padding: 20px; border-radius: 10px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); }
        .btn { padding: 10px 20px; font-size: 16px; margin: 10px; border: none; border-radius: 5px; cursor: pointer; }
        .on { background-color: #4CAF50; color: white; }
        .off { background-color: #f44336; color: white; }
        .status { font-weight: bold; padding: 4px 8px; border-radius: 4px; }
        .ok { background: #d4edda; color: #155724; }
        .warn { background: #f8d7da; color: #721c24; }
    </style>
</head>
<body>
    <div class="card">
        <h2>Evaporative Cooler</h2>
        <p>Temp: <span id="temp">--</span> &deg;C</p>
        <p>Humidity: <span id="hum">--</span> %</p>
        <p>Water Level: <span id="water" class="status">--</span></p>
        <hr>
        <p>System Mode: <button id="modeBtn" class="btn" onclick="toggle('mode')">--</button></p>
        <p>Fan: <button id="fanBtn" class="btn" onclick="toggle('fan')">--</button></p>
        <p>Pump: <button id="pumpBtn" class="btn" onclick="toggle('pump')">--</button></p>
    </div>

    <script>
        function updateData() {
            fetch('/api/data')
                .then(res => res.json())
                .then(data => {
                    document.getElementById('temp').innerText = data.temperature.toFixed(1);
                    document.getElementById('hum').innerText = data.humidity.toFixed(1);
                    
                    const w = document.getElementById('water');
                    w.innerText = data.waterLevelOK ? "NORMAL" : "LOW (PUMP LOCKED)";
                    w.className = "status " + (data.waterLevelOK ? "ok" : "warn");

                    document.getElementById('modeBtn').innerText = data.autoMode ? "AUTO" : "MANUAL";
                    
                    const fanBtn = document.getElementById('fanBtn');
                    fanBtn.innerText = data.fan ? "ON" : "OFF";
                    fanBtn.className = "btn " + (data.fan ? "on" : "off");
                    fanBtn.disabled = data.autoMode;

                    const pumpBtn = document.getElementById('pumpBtn');
                    pumpBtn.innerText = data.pump ? "ON" : "OFF";
                    pumpBtn.className = "btn " + (data.pump ? "on" : "off");
                    pumpBtn.disabled = data.autoMode || !data.waterLevelOK;
                });
        }

        function toggle(device) {
            fetch('/api/toggle?device=' + device)
                .then(() => updateData());
        }

        setInterval(updateData, 2000);
        updateData();
    </script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}