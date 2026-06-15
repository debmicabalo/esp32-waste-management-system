#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "secrets.h"

// ===== ENVIRONMENT CONFIGURATION =====
#define USE_PRODUCTION 1  // Set to 1 for HTTPS, 0 for HTTP (local testing)

// ===== SENSOR PINS =====
#define TRIG_PIN 5    // GPIO5  - Ultrasonic trigger
#define ECHO_PIN 18   // GPIO18 - Ultrasonic echo

// ===== DEVICE FINGERPRINTING =====
#define DEVICE_TYPE      "ESP32-AJ-SR04M-WasteSensor"
#define FIRMWARE_VERSION "1.0.0"

// ===== BIN CONFIGURATION =====
// Sensor is mounted at the top of the bin, facing downward.
// Distance = short → bin is FULL (trash is close to sensor)
// Distance = long  → bin is EMPTY (trash far away / at bottom)
//
// Bin height        : 100 cm  (sensor to bin floor when empty)
// Sensor blind zone : 20 cm   (AJ-SR04M minimum reliable range)
// Usable range      : 20–100 cm
//
// Fill % formula    : ((BIN_HEIGHT - distance) / BIN_HEIGHT) * 100
//
//   distance = 100 cm →   0% full (empty)
//   distance =  25 cm →  75% full (WARNING threshold)
//   distance =  20 cm → 100% full (FULL — at blind zone edge)
//   distance < 20 cm  → overflowing / sensor error

#define BIN_HEIGHT_CM      100.0   // Physical height of the bin
#define SENSOR_MIN_CM       20.0   // AJ-SR04M minimum reliable distance
#define WARN_THRESHOLD_CM   25.0   // 75% full  → (100 - 25) / 100 = 75%
#define FULL_THRESHOLD_CM   20.0   // 100% full → at blind zone, treat as full

float distance = 0;
float fillPercent = 0;
unsigned long lastTime = 0;

// ===== HELPER FUNCTIONS =====

String getDeviceMAC() {
  return WiFi.macAddress();
}

String generateRequestID() {
  return String(millis()) + "-" + String(random(1000, 9999));
}

void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected! IP: ");
  Serial.println(WiFi.localIP());

  // Sync time via NTP (UTC+8 for Philippines = 28800 seconds offset)
  configTime(28800, 0, "pool.ntp.org");
  Serial.print("Syncing time");
  time_t now = time(nullptr);
  while (now < 24 * 3600) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("\nTime synced!");
}

String getISO8601Timestamp() {
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);
  char timestamp[30];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", timeinfo);
  return String(timestamp) + "+08:00";
}

// ===== SENSOR =====

float measureDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);  // 30ms timeout
  float dist = (duration / 2.0) / 29.1;
  return dist;
}

// ===== BIN LOGIC =====

float computeFillPercent(float dist) {
  // Clamp to usable range before computing
  dist = constrain(dist, SENSOR_MIN_CM, BIN_HEIGHT_CM);
  return ((BIN_HEIGHT_CM - dist) / BIN_HEIGHT_CM) * 100.0;
}

String getBinStatus(float dist, float fill) {
  if (dist <= FULL_THRESHOLD_CM) {
    return "FULL";
  } else if (dist <= WARN_THRESHOLD_CM) {
    return "WARNING";
  } else {
    return "NORMAL";
  }
}

// ===== NETWORK =====

void sendData(float dist, float fill, String status) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERROR: WiFi not connected!");
    return;
  }

#if USE_PRODUCTION
  WiFiClientSecure client;
  client.setInsecure();  // Replace with cert pinning for production security
  HTTPClient http;
  if (!http.begin(client, BACKEND_URL)) {
    Serial.println("ERROR: Failed to initialize HTTPS connection");
    return;
  }
#else
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, BACKEND_URL)) {
    Serial.println("ERROR: Failed to initialize HTTP connection");
    return;
  }
#endif

  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-API-Key",       API_KEY);
  http.addHeader("X-Device-Type",          DEVICE_TYPE);
  http.addHeader("X-Firmware-Version",     FIRMWARE_VERSION);
  http.addHeader("X-Device-MAC",           getDeviceMAC());
  http.addHeader("X-Request-ID",           generateRequestID());
  http.addHeader("User-Agent",             "ESP32-WasteSensor/1.0");

  String jsonData = "{\"distance_cm\":"     + String(dist, 2)
                  + ",\"fill_percent\":"    + String(fill, 1)
                  + ",\"bin_status\":\""   + status + "\""
                  + ",\"device_timestamp\":\"" + getISO8601Timestamp() + "\"}";

  Serial.print("Sending JSON: ");
  Serial.println(jsonData);

  int httpCode = http.POST(jsonData);

  if (httpCode > 0) {
    Serial.print("HTTP Status: ");
    Serial.println(httpCode);

    String response = http.getString();
    if (response.length() > 0) {
      Serial.print("Response: ");
      Serial.println(response);
    }

    if (httpCode == 200 || httpCode == 201) {
      Serial.println("[OK] Data sent successfully.");
    } else if (httpCode == 401) {
      Serial.println("[ERROR 401] Unauthorized — check your API key.");
    } else if (httpCode == 429) {
      Serial.println("[ERROR 429] Rate Limited — slow down requests.");
    } else {
      Serial.println("[WARNING] Unexpected HTTP status.");
    }
  } else {
    Serial.print("[ERROR] HTTP Error: ");
    Serial.println(http.errorToString(httpCode));
  }

  http.end();
}

// ===== SETUP & LOOP =====

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  Serial.println("\n\n=== WASTE SENSOR STARTING ===");
  Serial.println("Firmware: " FIRMWARE_VERSION);
  Serial.print("Mode: ");
  Serial.println(USE_PRODUCTION ? "PRODUCTION (HTTPS)" : "LOCAL (HTTP)");
  Serial.println("==============================\n");

  connectWiFi();
  randomSeed(micros());  // Seed RNG after WiFi/NTP for better entropy
}

void loop() {
  if (millis() - lastTime >= 10000) {  // Send every 10 seconds
    lastTime = millis();

    Serial.print("[");
    Serial.print(getISO8601Timestamp());
    Serial.println("]");

    distance = measureDistance();
    fillPercent = computeFillPercent(distance);
    String binStatus = getBinStatus(distance, fillPercent);

    Serial.print("Distance:   ");
    Serial.print(distance, 1);
    Serial.println(" cm");

    Serial.print("Fill Level: ");
    Serial.print(fillPercent, 1);
    Serial.println("%");

    Serial.print("Bin Status: ");
    Serial.println(binStatus);

    if (binStatus == "FULL") {
      Serial.println("[!!!] BIN IS FULL — needs immediate collection!");
    } else if (binStatus == "WARNING") {
      Serial.println("[!]   BIN AT 75% — collection recommended soon.");
    }

    Serial.print("WiFi: ");
    Serial.println(WiFi.status() == WL_CONNECTED ? "OK" : "DISCONNECTED");

    sendData(distance, fillPercent, binStatus);
    Serial.println("---\n");
    Serial.flush();
  }
}