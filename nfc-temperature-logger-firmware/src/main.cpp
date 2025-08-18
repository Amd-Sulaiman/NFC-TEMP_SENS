//how the code works

//TMP117 driver: minimal I²C read of the temperature register, converting the raw 16-bit value: °C = raw * 0.0078125.

//Session logic: collects READINGS_PER_SESSION samples at SAMPLE_INTERVAL_MS (default 12 × 5s ≈ 60s), computes average, classifies by thresholds in config.h.

//NFC write: builds a single NDEF Text record (ST25DV64KC::writeTextNDEF) and writes it starting at NDEF_START. A smartphone tap shows the compact string.

//Cloud: if Wi-Fi and CLOUD_URL are set, sends one JSON payload per session. Add retries/HTTPS cert pinning later if needed.

//Timestamps: uses SNTP (via configTime) to make an ISO-8601 UTC string.

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>

#include "config.h"
#include "secrets.h"     // create from secrets.h.example
#include "ST25DV64KC.h"

// ---------------- TMP117 minimal driver ----------------
static const uint8_t TMP117_REG_TEMP = 0x00;
static const uint8_t TMP117_REG_CONF = 0x01;

bool tmp117_begin() {
  Wire.beginTransmission(TMP117_ADDR);
  Wire.write(TMP117_REG_CONF);
  if (Wire.endTransmission() != 0) return false;
  // Optional: configure averaging etc. (keep defaults)
  return true;
}

// Return temperature in Celsius
bool tmp117_readC(float &outC) {
  Wire.beginTransmission(TMP117_ADDR);
  Wire.write(TMP117_REG_TEMP);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)TMP117_ADDR, 2) != 2) return false;
  uint16_t raw = ((uint16_t)Wire.read() << 8) | Wire.read();
  // TMP117: 16-bit two's complement, LSB = 7.8125e-3 °C
  int16_t s = (int16_t)raw;
  outC = s * 0.0078125f;
  return true;
}

// --------------- Globals ----------------
ST25DV64KC nfc(Wire);
float readings[READINGS_PER_SESSION];
uint8_t rIndex = 0;

// Simple status classifier
const char* classifyStatus(float c) {
  if (c < THRESH_NORMAL_MAX_C) return "Normal";
  if (c < THRESH_FEVER_MAX_C)  return "Fever";
  return "High Fever";
}

// ISO8601 UTC timestamp via SNTP
String iso8601_utc() {
  time_t now; struct tm tm; char buf[32];
  time(&now);
  gmtime_r(&now, &tm);
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return String(buf);
}

void connectWiFi() {
  if (strlen(WIFI_SSID) == 0) return; // allow offline
  WiFi.mode(WIFI_MODE_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  }
}

void sendToCloud(const String& patientId, float avgC, const String& status, const String& isoTs) {
  if (strlen(CLOUD_URL) == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  String payload = "{";
  payload += "\"patient_id\":\"" + patientId + "\",";
  payload += "\"timestamp\":\"" + isoTs + "\",";
  payload += "\"readings_c\":[";
  for (int i = 0; i < READINGS_PER_SESSION; ++i) {
    payload += String(readings[i], 3);
    if (i < READINGS_PER_SESSION - 1) payload += ",";
  }
  payload += "],";
  payload += "\"average_c\":" + String(avgC,3) + ",";
  payload += "\"status\":\"" + status + "\",";
  payload += "\"device\":\"" DEVICE_NAME "\",";
  payload += "\"session_count\":" + String(READINGS_PER_SESSION);
  payload += "}";

  HTTPClient http;
  http.begin(CLOUD_URL);
  http.addHeader("Content-Type", "application/json");
  if (strlen(CLOUD_AUTH_BEARER)) {
    http.addHeader("Authorization", String("Bearer ") + CLOUD_AUTH_BEARER);
  }
  int code = http.POST(payload);
  // Optional: check response
  http.end();
}

void writeNFC(float avgC, const String& status, const String& ts, const String& patientId) {
  // Compact text for phone display via NDEF Text record
  // Keep under NFC_TEXT_MAX chars for the demo
  char text[NFC_TEXT_MAX];
  snprintf(text, sizeof(text),
           "T=%.2fC; S=%s; N=%d; ts=%s; pid=%s",
           avgC, status.c_str(), READINGS_PER_SESSION, ts.c_str(), patientId.c_str());
  nfc.writeTextNDEF(text, NDEF_LANG_CODE);
}

void setup() {
  Serial.begin(115200);
  Wire.begin(); // SDA/SCL default pins for your ESP32-C6 board

  Serial.println("[boot] starting...");

  // Sensors
  if (!tmp117_begin()) {
    Serial.println("[tmp117] not found");
  }

  // NFC
  if (!nfc.begin()) {
    Serial.println("[nfc] ST25DV64KC not found or I2C issue");
  }

  // Wi-Fi (optional)
  connectWiFi();
}

void loop() {
  // Take 12 readings every ~5 seconds
  float c;
  if (tmp117_readC(c)) {
    readings[rIndex++] = c;
    Serial.printf("[reading] #%u: %.3f C\n", rIndex, c);
  } else {
    Serial.println("[reading] TMP117 read failed");
  }

  if (rIndex >= READINGS_PER_SESSION) {
    // Compute average
    float sum = 0.f;
    for (int i = 0; i < READINGS_PER_SESSION; ++i) sum += readings[i];
    float avgC = sum / READINGS_PER_SESSION;

    String status = classifyStatus(avgC);
    String ts = iso8601_utc();
    String patientId = DEFAULT_PATIENT_ID;

    Serial.printf("[session] avg=%.3f C, status=%s, ts=%s\n", avgC, status.c_str(), ts.c_str());

    // Write to NFC (phone tap will show text)
    writeNFC(avgC, status, ts, patientId);
    Serial.println("[nfc] NDEF text written");

    // Send to cloud (optional)
    sendToCloud(patientId, avgC, status, ts);
    Serial.println("[cloud] POST attempted");

    // Reset session
    rIndex = 0;
  }

  delay(SAMPLE_INTERVAL_MS);
}
