// =============================================================================
// chicken_ac_esp32.ino — Chicken AC Standalone ESP32 Firmware
// Replaces Arduino Nano + HC-05 completely!
//
// Hardware: ESP32 Dev Module
// Sensors:
//   - DHT22 temperature/humidity on GPIO 4
//   - Relay 1 (Pump) on GPIO 16 (active HIGH)
//   - Relay 2 (Fan) on GPIO 17 (active HIGH)
//   - Float switch on GPIO 34 (INPUT with external or internal pullup)
// =============================================================================

#include "BluetoothSerial.h"
#include <DHT.h>
#include <ArduinoJson.h>

// ── Pin definitions ──────────────────────────────────────────────────────────
#define DHT_PIN       4    // GPIO4  — DHT22 data
#define PUMP_PIN      16   // GPIO16 — Relay 1 (water pump), active HIGH
#define FAN_PIN       17   // GPIO17 — Relay 2 (ventilation), active HIGH
#define FLOAT_PIN     34   // GPIO34 — Float switch (input only pin, needs pullup/LOW = water OK)

// ── DHT sensor ───────────────────────────────────────────────────────────────
#define DHT_TYPE DHT22
DHT dht(DHT_PIN, DHT_TYPE);

// ── Bluetooth Serial ─────────────────────────────────────────────────────────
BluetoothSerial SerialBT;

// ── Default thresholds ───────────────────────────────────────────────────────
float tempThreshold = 30.0;  // °C
float humThreshold  = 70.0;  // %

// ── Runtime state ────────────────────────────────────────────────────────────
enum Mode { AUTO_MODE, MANUAL_MODE };
Mode  currentMode  = AUTO_MODE;
bool  pumpState    = false;
bool  fanState     = false;
bool  waterOK      = true;

// ── Timing ───────────────────────────────────────────────────────────────────
unsigned long lastRead = 0;
const unsigned long READ_INTERVAL = 2000;   // ms

float lastTemp = NAN;
float lastHum  = NAN;

// =============================================================================
void setup() {
  // Debug serial over USB
  Serial.begin(9600);

  // Bluetooth Serial (Advertises as "ChickenAC-ESP32")
  SerialBT.begin("ChickenAC-ESP32");
  Serial.println("Bluetooth Started! Ready to pair with PC as ChickenAC-ESP32");

  pinMode(PUMP_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  pinMode(FLOAT_PIN, INPUT); // Note: GPIO34 doesn't have internal pullup, wire with 10k to 3.3V if floating

  digitalWrite(PUMP_PIN, LOW);
  digitalWrite(FAN_PIN,  LOW);

  dht.begin();
}

// =============================================================================
void loop() {
  processBTSerial();

  unsigned long now = millis();
  if (now - lastRead >= READ_INTERVAL) {
    lastRead = now;
    readAndControl();
  }
}

// =============================================================================
void readAndControl() {
  // Read float switch (LOW = water present)
  waterOK = (digitalRead(FLOAT_PIN) == LOW);

  // Read DHT22
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t)) lastTemp = t;
  if (!isnan(h)) lastHum  = h;

  // AUTO mode logic
  if (currentMode == AUTO_MODE) {
    if (!isnan(lastTemp) && !isnan(lastHum)) {
      bool shouldRun = (lastTemp >= tempThreshold) || (lastHum >= humThreshold);
      pumpState = shouldRun;
      fanState  = shouldRun;
    }
  }

  // Safety override: empty reservoir cuts pump
  if (!waterOK) {
    pumpState = false;
  }

  // Drive relays
  digitalWrite(PUMP_PIN, pumpState ? HIGH : LOW);
  digitalWrite(FAN_PIN,  fanState  ? HIGH : LOW);

  emitStatus();
}

// =============================================================================
void emitStatus() {
  StaticJsonDocument<200> doc;
  doc["t"]  = isnan(lastTemp) ? JsonVariant() : JsonVariant(round(lastTemp * 10.0) / 10.0);
  doc["h"]  = isnan(lastHum)  ? JsonVariant() : JsonVariant(round(lastHum  * 10.0) / 10.0);
  doc["p"]  = pumpState  ? 1 : 0;
  doc["f"]  = fanState   ? 1 : 0;
  doc["m"]  = (currentMode == AUTO_MODE) ? "AUTO" : "MANUAL";
  doc["w"]  = waterOK    ? 1 : 0;
  doc["tt"] = tempThreshold;
  doc["ht"] = humThreshold;

  // Send JSON over Bluetooth Serial
  serializeJson(doc, SerialBT);
  SerialBT.println();

  // Also print to USB Serial for debugging
  serializeJson(doc, Serial);
  Serial.println();
}

// =============================================================================
void processBTSerial() {
  // Read from Bluetooth
  while (SerialBT.available()) {
    String line = SerialBT.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    StaticJsonDocument<128> doc;
    DeserializationError err = deserializeJson(doc, line);
    if (err) continue;

    const char* cmd = doc["cmd"];
    if (!cmd) continue;

    if (strcmp(cmd, "mode") == 0) {
      const char* val = doc["val"];
      if (val && strcmp(val, "MANUAL") == 0) currentMode = MANUAL_MODE;
      else                                    currentMode = AUTO_MODE;

    } else if (strcmp(cmd, "pump") == 0) {
      if (currentMode == MANUAL_MODE) {
        pumpState = (doc["val"].as<int>() == 1);
      }

    } else if (strcmp(cmd, "fan") == 0) {
      if (currentMode == MANUAL_MODE) {
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
}
