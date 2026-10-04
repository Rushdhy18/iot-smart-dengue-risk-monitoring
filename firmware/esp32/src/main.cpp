/*
 * IoT-Based Smart Dengue Risk Monitoring and Early Warning System
 * Board : ESP32 (Wokwi)
 *
 * Sensors (simulated in Wokwi):
 *   DHT22          -> temperature + humidity       (GPIO15)
 *   Potentiometer  -> rainfall (0-100 mm/day)      (GPIO34)
 *   HC-SR04        -> stagnant water depth (cm)    (TRIG 5 / ECHO 18)
 * Outputs:
 *   LCD 16x2 I2C (0x27), Green/Yellow/Red LEDs (25/26/27), Buzzer (14)
 * Cloud:
 *   WiFi (Wokwi-GUEST) + MQTT -> broker.hivemq.com
 *   Topic: dengue/zone1/telemetry  (JSON for GIS dashboard / ML model)
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h> 

// ---------- Pins ----------
#define DHT_PIN     15
#define DHT_TYPE    DHT22
#define RAIN_PIN    34
#define TRIG_PIN    5
#define ECHO_PIN    18
#define LED_GREEN   25
#define LED_YELLOW  26
#define LED_RED     27
#define BUZZER_PIN  14

// ---------- Config ----------
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";
const char* MQTT_HOST = "broker.hivemq.com";
const int   MQTT_PORT = 1883;
const char* ZONE_ID   = "zone1";
const char* TOPIC     = "dengue/zone1/telemetry";

// Ultrasonic sensor mounted above a container; distance to bottom = 30 cm
const float CONTAINER_HEIGHT_CM = 30.0;

// Historical dengue cases in this area (last 4 weeks). Replace with real data
// from your dataset / ML backend (0 = none, 100 = very high).
float historicalIndex = 45.0;

const unsigned long SAMPLE_MS = 5000;

DHT dht(DHT_PIN, DHT_TYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

unsigned long lastSample = 0;

// ---------- Helpers ----------
float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

float readWaterDepthCm() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long dur = pulseIn(ECHO_PIN, HIGH, 30000);
  if (dur == 0) return 0;
  float dist = dur * 0.0343 / 2.0;             // cm from sensor to water surface
  float depth = CONTAINER_HEIGHT_CM - dist;    // water depth
  if (depth < 0) depth = 0;
  if (depth > CONTAINER_HEIGHT_CM) depth = CONTAINER_HEIGHT_CM;
  return depth;
}

float readRainfallMm() {
  int raw = analogRead(RAIN_PIN);              // 0..4095
  return raw * 100.0 / 4095.0;                 // 0..100 mm/day
}

/*
 * Risk model (0-100). Weights mirror a trained logistic/linear model;
 * swap these coefficients with the ones from your ML training later.
 * Aedes aegypti breeds best at ~25-32 C, humidity > 60 %, after rain,
 * where stagnant water collects.
 */
float computeRisk(float t, float h, float rain, float depth) {
  float tScore;
  if (t >= 25 && t <= 32)      tScore = 1.0;
  else if (t < 25)             tScore = clamp01((t - 15) / 10.0);
  else                         tScore = clamp01(1.0 - (t - 32) / 8.0);

  float hScore = clamp01((h - 40.0) / 40.0);          // 40% -> 0, 80% -> 1
  float rScore = clamp01(rain / 60.0);                // 60 mm -> 1
  float wScore = clamp01(depth / 10.0);               // 10 cm standing water -> 1
  float hiScore = clamp01(historicalIndex / 100.0);

  float risk = 0.25 * tScore + 0.20 * hScore + 0.20 * rScore
             + 0.25 * wScore + 0.10 * hiScore;
  return risk * 100.0;
}

const char* levelName(float r) {
  if (r < 40) return "LOW";
  if (r < 70) return "MEDIUM";
  return "HIGH";
}

void setAlert(float r) {
  digitalWrite(LED_GREEN,  r < 40);
  digitalWrite(LED_YELLOW, r >= 40 && r < 70);
  digitalWrite(LED_RED,    r >= 70);
  if (r >= 70) tone(BUZZER_PIN, 1500, 400);
  else noTone(BUZZER_PIN);
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) delay(250);
}

void connectMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  String id = String("dengue-") + ZONE_ID + "-" + String((uint32_t)esp_random(), HEX);
  mqtt.connect(id.c_str());
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  dht.begin();
  lcd.init();
  lcd.backlight();
  lcd.print("Dengue EWS");
  lcd.setCursor(0, 1);
  lcd.print("Starting...");

  connectWifi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  connectMqtt();
  Serial.println(WiFi.status() == WL_CONNECTED ? "WiFi OK" : "WiFi FAILED (offline mode)");
}

void loop() {
  if (millis() - lastSample < SAMPLE_MS) { mqtt.loop(); return; }
  lastSample = millis();

  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h)) { Serial.println("DHT read error"); return; }

  float rain  = readRainfallMm();
  float depth = readWaterDepthCm();
  float risk  = computeRisk(t, h, rain, depth);
  const char* level = levelName(risk);

  setAlert(risk);

  // LCD
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.printf("T%.0fC H%.0f%% R%.0f", t, h, rain);
  lcd.setCursor(0, 1);
  lcd.printf("W%.0f Risk:%s", depth, level);

  // Serial
  Serial.printf("T=%.1f H=%.1f Rain=%.1f Water=%.1f Risk=%.1f (%s)\n",
                t, h, rain, depth, risk, level);

  // MQTT (JSON). Lat/Lon is a placeholder for the GIS map - set your zone coords.
  char payload[256];
  snprintf(payload, sizeof(payload),
    "{\"zone\":\"%s\",\"lat\":6.9271,\"lon\":79.8612,\"temp\":%.1f,\"hum\":%.1f,"
    "\"rain\":%.1f,\"water\":%.1f,\"risk\":%.1f,\"level\":\"%s\"}",
    ZONE_ID, t, h, rain, depth, risk, level);

  connectWifi();
  connectMqtt();
  if (mqtt.connected()) {
    mqtt.publish(TOPIC, payload);
    Serial.println("MQTT published");
  }
}
