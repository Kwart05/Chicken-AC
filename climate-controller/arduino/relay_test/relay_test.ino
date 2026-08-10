// =============================================================================
// relay_test.ino — ESP8266 Relay Hardware Diagnostic Test
// =============================================================================

#include <Arduino.h>

#define FAN_PIN   D1 // GPIO 5
#define PUMP_PIN  D2 // GPIO 4
#define DHT_PIN   D5 // GPIO 14
#define FLOAT_PIN D6 // GPIO 12

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nStarting ESP8266 Relay Diagnostic Test...");

  pinMode(FAN_PIN, OUTPUT);
  pinMode(PUMP_PIN, OUTPUT);

  digitalWrite(FAN_PIN, HIGH);
  digitalWrite(PUMP_PIN, HIGH);
}

void loop() {
  Serial.println(">>> TESTING FAN (D1) -> Relay ON (LOW)");
  digitalWrite(FAN_PIN, LOW);
  delay(1500);

  Serial.println(">>> TESTING FAN (D1) -> Relay OFF (HIGH)");
  digitalWrite(FAN_PIN, HIGH);
  delay(1500);

  Serial.println(">>> TESTING PUMP (D2) -> Relay ON (LOW)");
  digitalWrite(PUMP_PIN, LOW);
  delay(1500);

  Serial.println(">>> TESTING PUMP (D2) -> Relay OFF (HIGH)");
  digitalWrite(PUMP_PIN, HIGH);
  delay(1500);
}
