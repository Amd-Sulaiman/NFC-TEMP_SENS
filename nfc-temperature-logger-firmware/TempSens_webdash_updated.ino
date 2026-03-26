/*
 * TempSens v5 — Cold Chain & Patient Monitor
 * ============================================
 * Hardware: ESP32, TMP117, ST25DV64KC (64Kbit = 8192 bytes), SSD1306 OLED
 *
 * ST25DV64KC MEMORY MAP:
 *   Total EEPROM : 8192 bytes
 *   Target usage : 70-80% = ~5734-6553 bytes
 *   NFC_LOG_MAX  : 5800 bytes (70.8%) — safe headroom
 *   CC File      : 8 bytes (managed by library)
 *   NDEF overhead: ~12 bytes per record
 *
 * KEY FEATURES:
 *   - NFC: meaningful alerts only (fever/high/sudden change/low) with \r\n
 *   - WiFi: continuous reading until power-off (no session limit)
 *   - WiFi: STA mode preferred; AP fallback; resets to STA-first on reboot
 *   - AP/STA SSID+Password configurable from web dashboard
 *   - OLED + WiFi update simultaneously (non-blocking millis() architecture)
 *   - Timestamps from browser JS injected via /api/sync
 *   - Anomaly detection: sustained high, sudden spike, sudden drop, sustained low
 *   - NFC writes top meaningful events only (not every reading)
 *   - Configurable: reading interval, NFC entry count, thresholds
 *
 * SETUP:
 *   1. Flash — open Serial at 115200
 *   2. On first boot with no saved WiFi: connects as AP "TempSens-Setup"
 *   3. Open 192.168.4.1 → Settings tab → enter your WiFi credentials → Save
 *   4. Device reboots into STA mode
 *   5. On subsequent boots: always tries STA first, AP only if STA fails after timeout
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeMono12pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <SparkFun_ST25DV64KC_Arduino_Library.h>
#include <SparkFun_TMP117.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>      // NVS — persist WiFi creds + config

// =====================================================
// HARDWARE
// =====================================================
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define BATTERY_PIN      1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
SFE_ST25DV64KC_NDEF tag;
TMP117 tempSensor;
WebServer server(80);
Preferences prefs;

// =====================================================
// NFC MEMORY BUDGET
// ST25DV64KC = 8192 bytes total EEPROM
// We target 70-80% = ~5734-6553 bytes
// NFC_LOG_MAX = 5800 bytes (70.8% of 8192)
// Each meaningful event line ≈ 60 bytes with \r\n
// So we can store ~90 meaningful events comfortably
// =====================================================
#define NFC_TOTAL_BYTES    8192
#define NFC_LOG_MAX        5800   // 70.8% of 8192 — target 70-80%
#define NFC_MAX_EVENTS      90    // max meaningful events stored on NFC

// =====================================================
// DEFAULT ADJUSTABLE PARAMETERS (saved to NVS, editable via web)
// =====================================================
#define DEFAULT_READING_INTERVAL_SEC  10
#define DEFAULT_NFC_STORE_EVERY_N      4   // store 1 in N normal readings on NFC
#define DEFAULT_TEMP_LOW              95.0  // F — below this = cold alert
#define DEFAULT_TEMP_ELEVATED         99.0  // F
#define DEFAULT_TEMP_FEVER           100.4  // F
#define DEFAULT_TEMP_HIGH_FEVER      102.4  // F
#define DEFAULT_SPIKE_THRESHOLD        1.5  // F change in one reading = spike
#define DEFAULT_NUM_NFC_READINGS       60   // max readings stored on NFC

// WiFi
#define AP_SSID_DEFAULT   "TempSens-Setup"
#define AP_PASSWORD       "tempsens123"
#define STA_CONNECT_TIMEOUT_MS  15000
#define WIFI_RETRY_INTERVAL_SEC    60

// OLED
#define OLED_TRANSITION_MS  400

// =====================================================
// RUNTIME CONFIG (loaded from NVS on boot)
// =====================================================
struct Config {
  char    staSSID[64];
  char    staPass[64];
  char    apSSID[32];
  char    patientName[32];
  char    patientID[16];
  char    monitorMode[16];  // "patient" or "coldchain"
  int     readingIntervalSec;
  int     nfcStoreEveryN;
  int     numNfcReadings;
  float   tempLow;
  float   tempElevated;
  float   tempFever;
  float   tempHighFever;
  float   spikeThreshold;
} cfg;

void loadConfig() {
  prefs.begin("tempsens", true);
  strlcpy(cfg.staSSID,          prefs.getString("ssid",    "").c_str(),          sizeof(cfg.staSSID));
  strlcpy(cfg.staPass,          prefs.getString("pass",    "").c_str(),          sizeof(cfg.staPass));
  strlcpy(cfg.apSSID,           prefs.getString("apssid",  AP_SSID_DEFAULT).c_str(), sizeof(cfg.apSSID));
  strlcpy(cfg.patientName,      prefs.getString("pname",   "Unknown").c_str(),   sizeof(cfg.patientName));
  strlcpy(cfg.patientID,        prefs.getString("pid",     "000").c_str(),       sizeof(cfg.patientID));
  strlcpy(cfg.monitorMode,      prefs.getString("mode",    "patient").c_str(),   sizeof(cfg.monitorMode));
  cfg.readingIntervalSec = prefs.getInt("interval",  DEFAULT_READING_INTERVAL_SEC);
  cfg.nfcStoreEveryN     = prefs.getInt("nfcevery",  DEFAULT_NFC_STORE_EVERY_N);
  cfg.numNfcReadings     = prefs.getInt("nfccount",  DEFAULT_NUM_NFC_READINGS);
  cfg.tempLow            = prefs.getFloat("tlow",    DEFAULT_TEMP_LOW);
  cfg.tempElevated       = prefs.getFloat("televated", DEFAULT_TEMP_ELEVATED);
  cfg.tempFever          = prefs.getFloat("tfever",  DEFAULT_TEMP_FEVER);
  cfg.tempHighFever      = prefs.getFloat("thigh",   DEFAULT_TEMP_HIGH_FEVER);
  cfg.spikeThreshold     = prefs.getFloat("tspike",  DEFAULT_SPIKE_THRESHOLD);
  prefs.end();
}

void saveConfig() {
  prefs.begin("tempsens", false);
  prefs.putString("ssid",      cfg.staSSID);
  prefs.putString("pass",      cfg.staPass);
  prefs.putString("apssid",    cfg.apSSID);
  prefs.putString("pname",     cfg.patientName);
  prefs.putString("pid",       cfg.patientID);
  prefs.putString("mode",      cfg.monitorMode);
  prefs.putInt("interval",     cfg.readingIntervalSec);
  prefs.putInt("nfcevery",     cfg.nfcStoreEveryN);
  prefs.putInt("nfccount",     cfg.numNfcReadings);
  prefs.putFloat("tlow",       cfg.tempLow);
  prefs.putFloat("televated",  cfg.tempElevated);
  prefs.putFloat("tfever",     cfg.tempFever);
  prefs.putFloat("thigh",      cfg.tempHighFever);
  prefs.putFloat("tspike",     cfg.spikeThreshold);
  prefs.end();
}

// =====================================================
// READING STORAGE (continuous — WiFi logs everything)
// Dynamic allocation capped at 2048 readings ~= plenty
// =====================================================
#define MAX_WIFI_READINGS  2048

struct Reading {
  float   tempF;
  uint32_t localMs;       // millis() at time of reading
  char    browserTime[24]; // "HH:MM:SS" from browser sync
  uint8_t flags;          // bitmask: 0x01=spike_up 0x02=spike_down 0x04=low 0x08=elevated 0x10=fever 0x20=high_fever
};

Reading  readings[MAX_WIFI_READINGS];
int      readingCount    = 0;
float    maxTemp         = -999.0;
float    minTemp         =  999.0;
float    latestTemp      =  98.6;
bool     sessionStarted  = false;

// NFC event log (only meaningful events)
struct NfcEvent {
  int     index;
  float   tempF;
  uint8_t flags;
  char    browserTime[24];
  float   change;
};
NfcEvent nfcEvents[NFC_MAX_EVENTS];
int      nfcEventCount   = 0;

// =====================================================
// WIFI / NETWORK STATE
// =====================================================
bool         wifiConnected    = false;
bool         apModeActive     = false;
unsigned long lastWifiRetryMs = 0;
unsigned long sessionStartMs  = 0;
unsigned long lastReadingMs   = 0;

// Browser time sync — offset between browser epoch and millis()
// browserEpochMs = millis() at moment of sync + browserTimeAtSync
bool          timeSynced        = false;
unsigned long syncMillis        = 0;
unsigned long long browserEpochAtSync = 0; // ms

// Current session status string
const char*  sessionStatus = "Waiting";

// =====================================================
// UTILITIES
// =====================================================

void printCentered(const char* text, int y, const GFXfont* font = NULL) {
  int16_t x1, y1; uint16_t w, h;
  if (font) display.setFont(font); else display.setFont();
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  int x = (SCREEN_WIDTH - w) / 2;
  if (font) display.setCursor(x, y + h); else display.setCursor(x, y);
  display.print(text);
}

float readBatteryVoltage() {
  long sum = 0;
  for (int i = 0; i < 16; i++) { sum += analogReadMilliVolts(BATTERY_PIN); delay(2); }
  return (sum / 16.0 * 2.0) / 1000.0;
}

int batteryPercentage(float v) {
  if (v >= 4.20) return 100;
  if (v <= 3.30) return 0;
  return constrain((int)(((v - 3.30) / (4.20 - 3.30)) * 100.0), 0, 100);
}

const char* classifyTemp(float f) {
  if (f < cfg.tempLow)         return "Low";
  if (f >= cfg.tempHighFever)  return "High Fever";
  if (f >= cfg.tempFever)      return "Fever";
  if (f >= cfg.tempElevated)   return "Elevated";
  return "Normal";
}

// Build flag bitmask for a reading
uint8_t buildFlags(float f, float prevF, bool isFirst) {
  uint8_t fl = 0;
  if (f < cfg.tempLow)                                  fl |= 0x04; // low
  if (f >= cfg.tempElevated && f < cfg.tempFever)       fl |= 0x08; // elevated
  if (f >= cfg.tempFever    && f < cfg.tempHighFever)   fl |= 0x10; // fever
  if (f >= cfg.tempHighFever)                           fl |= 0x20; // high fever
  if (!isFirst) {
    float diff = f - prevF;
    if (diff  >=  cfg.spikeThreshold) fl |= 0x01; // spike up
    if (diff  <= -cfg.spikeThreshold) fl |= 0x02; // spike down
  }
  return fl;
}

bool isMeaningful(uint8_t flags) {
  return flags != 0; // any flag = meaningful
}

// Get browser time string for a reading (uses sync offset)
void getBrowserTime(char* buf, size_t len, uint32_t readingMs) {
  if (!timeSynced) {
    snprintf(buf, len, "--:--:--");
    return;
  }
  unsigned long long epochMs = browserEpochAtSync + (readingMs - syncMillis);
  unsigned long epochSec = (unsigned long)(epochMs / 1000);
  int hh = (epochSec / 3600) % 24;
  int mm = (epochSec /   60) % 60;
  int ss =  epochSec         % 60;
  snprintf(buf, len, "%02d:%02d:%02d", hh, mm, ss);
}

// =====================================================
// OLED SCREENS
// =====================================================

void oledClear() {
  display.clearDisplay();
  display.display();
  delay(OLED_TRANSITION_MS);
}

void showWelcomeScreen() {
  oledClear();
  display.setTextColor(SSD1306_WHITE);
  printCentered("TempSens", 14, &FreeMono12pt7b);
  printCentered("v5", 36, NULL);
  display.drawRect(10, 48, 108, 6, SSD1306_WHITE);
  display.fillRect(12, 50, 104, 2, SSD1306_WHITE);
  display.display();
  delay(1500);
}

void showWiFiStatus(bool connected, const char* ip, bool isSTA) {
  oledClear();
  display.setFont(); display.setTextSize(1);
  if (connected && isSTA) {
    printCentered("WiFi Connected", 2, NULL);
    printCentered(ip, 20, NULL);
    printCentered("Open browser:", 36, NULL);
    printCentered(ip, 48, NULL);
  } else {
    printCentered("AP Mode", 2, NULL);
    printCentered(cfg.apSSID, 16, NULL);
    printCentered("pw:tempsens123", 30, NULL);
    printCentered("192.168.4.1", 46, NULL);
  }
  display.display();
  delay(3000);
}

void showReadingScreen(float fval, int idx, uint8_t flags) {
  // Temp value — top region
  display.fillRect(0, 0, 128, 30, SSD1306_BLACK);
  display.setFont(&FreeMono12pt7b);
  display.setTextColor(SSD1306_WHITE);
  char ts[14]; snprintf(ts, sizeof(ts), "%.1fF", fval);
  printCentered(ts, 2, &FreeMono12pt7b);

  // Status + flags
  display.setFont();
  display.fillRect(0, 32, 128, 9, SSD1306_BLACK);
  const char* cls = classifyTemp(fval);
  char statusLine[32];
  if      (flags & 0x01) snprintf(statusLine, sizeof(statusLine), "%s ^SPIKE", cls);
  else if (flags & 0x02) snprintf(statusLine, sizeof(statusLine), "%s vDROP",  cls);
  else                   snprintf(statusLine, sizeof(statusLine), "%s",         cls);
  printCentered(statusLine, 32, NULL);

  // Counter + elapsed
  display.fillRect(0, 44, 128, 8, SSD1306_BLACK);
  char lbl[28];
  int elapsed = (int)((millis() - sessionStartMs) / 1000);
  int em = elapsed / 60, es = elapsed % 60;
  snprintf(lbl, sizeof(lbl), "R%d | %02d:%02d", idx+1, em, es);
  printCentered(lbl, 44, NULL);

  // Alert bar if anomalous
  display.fillRect(0, 56, 128, 8, SSD1306_BLACK);
  if (flags) {
    display.fillRect(0, 56, 128, 8, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    if      (flags & 0x20) printCentered("!! HIGH FEVER !!", 57, NULL);
    else if (flags & 0x10) printCentered("! FEVER ALERT !", 57, NULL);
    else if (flags & 0x04) printCentered("! LOW TEMP !", 57, NULL);
    else if (flags & 0x01) printCentered("^ SUDDEN RISE ^", 57, NULL);
    else if (flags & 0x02) printCentered("v SUDDEN DROP v", 57, NULL);
    else                   printCentered("~ ELEVATED ~", 57, NULL);
    display.setTextColor(SSD1306_WHITE);
  } else {
    printCentered("Normal", 57, NULL);
  }

  display.display();
  // No delay — non-blocking
}

void showFinalScreen(float mxTemp) {
  oledClear();
  display.setFont(&FreeMonoBold9pt7b);
  printCentered(sessionStatus, 5, &FreeMonoBold9pt7b);
  display.drawLine(0, 26, 128, 26, SSD1306_WHITE);
  display.setFont(&FreeMono12pt7b);
  char buf[16]; snprintf(buf, sizeof(buf), "%.1fF", mxTemp);
  printCentered(buf, 32, &FreeMono12pt7b);
  display.setFont();
  char aline[28];
  snprintf(aline, sizeof(aline), "Events: %d R:%d", nfcEventCount, readingCount);
  printCentered(aline, 54, NULL);
  display.display();
}

// =====================================================
// NFC LOG BUILDER
// Only writes meaningful events — fills up to NFC_LOG_MAX
// Uses \r\n for universal phone reader compatibility
// =====================================================
char nfcLogBuf[NFC_LOG_MAX + 64]; // slight oversize for safety check
uint16_t nfcMemLoc = 0;

void buildAndWriteNFC() {
  memset(nfcLogBuf, 0, sizeof(nfcLogBuf));
  size_t used = 0;
  size_t budget = NFC_LOG_MAX;

  // === HEADER ===
  char hdr[160];
  float avg = 0;
  for (int i = 0; i < readingCount; i++) avg += readings[i].tempF;
  if (readingCount > 0) avg /= readingCount;

  int elapsed = (int)((millis() - sessionStartMs) / 1000);
  int em = elapsed/60, es = elapsed%60;

  snprintf(hdr, sizeof(hdr),
    "TempSens v5\r\n"
    "Mode:%s\r\n"
    "Pt:%s ID:%s\r\n"
    "Max:%.2fF Min:%.2fF Avg:%.2fF\r\n"
    "Status:%s Events:%d\r\n"
    "Duration:%02d:%02d Readings:%d\r\n"
    "-- EVENTS (Meaningful Only) --\r\n",
    cfg.monitorMode,
    cfg.patientName, cfg.patientID,
    maxTemp, minTemp, avg,
    sessionStatus, nfcEventCount,
    em, es, readingCount);

  size_t hlen = strlen(hdr);
  if (hlen < budget) {
    memcpy(nfcLogBuf, hdr, hlen);
    used = hlen;
  }

  // === MEANINGFUL EVENTS ===
  int written = 0;
  for (int i = 0; i < nfcEventCount && used < budget - 80; i++) {
    NfcEvent& e = nfcEvents[i];
    char tag_str[48] = "";

    // Build human-readable flag tags
    if      (e.flags & 0x20) strlcat(tag_str, "[HIGH-FEVER]",   sizeof(tag_str));
    else if (e.flags & 0x10) strlcat(tag_str, "[FEVER]",        sizeof(tag_str));
    else if (e.flags & 0x08) strlcat(tag_str, "[ELEVATED]",     sizeof(tag_str));
    else if (e.flags & 0x04) strlcat(tag_str, "[LOW]",          sizeof(tag_str));
    if      (e.flags & 0x01) strlcat(tag_str, "[SPIKE+]",       sizeof(tag_str));
    if      (e.flags & 0x02) strlcat(tag_str, "[DROP-]",        sizeof(tag_str));

    char chgStr[12];
    if (e.change >= 0) snprintf(chgStr, sizeof(chgStr), "+%.2fF", e.change);
    else               snprintf(chgStr, sizeof(chgStr), "%.2fF",  e.change);

    char line[96];
    snprintf(line, sizeof(line),
      "R%03d %s %.2fF %s chg:%s\r\n",
      e.index, e.browserTime, e.tempF, tag_str, chgStr);

    size_t llen = strlen(line);
    if (used + llen < budget) {
      memcpy(nfcLogBuf + used, line, llen);
      used += llen;
      written++;
    } else break;
  }

  // === FOOTER ===
  char foot[80];
  snprintf(foot, sizeof(foot),
    "-- END %d/%d events written --\r\n"
    "NFC used:%d/%d bytes (%.0f%%)\r\n",
    written, nfcEventCount,
    (int)used, NFC_TOTAL_BYTES,
    (float)used / NFC_TOTAL_BYTES * 100.0);

  size_t flen = strlen(foot);
  if (used + flen < budget) {
    memcpy(nfcLogBuf + used, foot, flen);
    used += flen;
  }

  Serial.printf("NFC log: %d bytes / %d budget (%.1f%% of tag)\n",
    (int)used, NFC_LOG_MAX,
    (float)used / NFC_TOTAL_BYTES * 100.0);

  // Write to tag
  nfcMemLoc = tag.getCCFileLen();
  tag.writeNDEFText(nfcLogBuf, &nfcMemLoc, true, true);
  Serial.printf("NFC written: %d events, %d bytes\n", written, (int)used);
}

// =====================================================
// WEB SERVER — API HANDLERS
// =====================================================

void handleApiSync() {
  // Browser posts its current epoch ms so we can timestamp readings
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_POST && server.hasArg("plain")) {
    StaticJsonDocument<128> doc;
    if (!deserializeJson(doc, server.arg("plain"))) {
      unsigned long long epochMs = doc["epochMs"].as<unsigned long long>();
      syncMillis          = millis();
      browserEpochAtSync  = epochMs;
      timeSynced          = true;
      // Backfill timestamps for readings taken before sync
      for (int i = 0; i < readingCount; i++) {
        if (readings[i].browserTime[0] == '-') {
          getBrowserTime(readings[i].browserTime,
            sizeof(readings[i].browserTime),
            readings[i].localMs);
        }
      }
      server.send(200, "application/json", "{\"ok\":true}");
      return;
    }
  }
  server.send(400, "application/json", "{\"ok\":false}");
}

void handleApiData() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  float voltage = readBatteryVoltage();
  int   pct     = batteryPercentage(voltage);
  float avg = 0;
  for (int i = 0; i < readingCount; i++) avg += readings[i].tempF;
  if (readingCount > 0) avg /= readingCount;

  unsigned long elapsedMs = sessionStarted ? (millis() - sessionStartMs) : 0;

  // Use DynamicJsonDocument for variable reading count
  DynamicJsonDocument doc(32768);

  JsonObject patient = doc.createNestedObject("patient");
  patient["name"] = cfg.patientName;
  patient["id"]   = cfg.patientID;
  patient["mode"] = cfg.monitorMode;

  JsonObject battery = doc.createNestedObject("battery");
  battery["voltage"] = round(voltage * 100) / 100.0;
  battery["percent"] = pct;

  JsonObject sess = doc.createNestedObject("session");
  sess["started"]     = sessionStarted;
  sess["count"]       = readingCount;
  sess["max"]         = readingCount > 0 ? round(maxTemp * 100) / 100.0 : 0;
  sess["min"]         = readingCount > 0 ? round(minTemp * 100) / 100.0 : 0;
  sess["avg"]         = readingCount > 0 ? round(avg     * 100) / 100.0 : 0;
  sess["status"]      = sessionStatus;
  sess["intervalSec"] = cfg.readingIntervalSec;
  sess["events"]      = nfcEventCount;
  sess["elapsedMs"]   = elapsedMs;
  sess["timeSynced"]  = timeSynced;
  sess["nfcBytes"]    = strlen(nfcLogBuf);
  sess["nfcBudget"]   = NFC_LOG_MAX;

  // Config block so dashboard can show live config
  JsonObject cfgObj = doc.createNestedObject("config");
  cfgObj["tempLow"]      = cfg.tempLow;
  cfgObj["tempElevated"] = cfg.tempElevated;
  cfgObj["tempFever"]    = cfg.tempFever;
  cfgObj["tempHighFever"]= cfg.tempHighFever;
  cfgObj["spikeThresh"]  = cfg.spikeThreshold;
  cfgObj["mode"]         = cfg.monitorMode;

  // Last 200 readings sent to browser (chart/table); full log in /api/full
  int startIdx = max(0, readingCount - 200);
  JsonArray arr = doc.createNestedArray("readings");
  for (int i = startIdx; i < readingCount; i++) {
    JsonObject r = arr.createNestedObject();
    r["index"]  = i + 1;
    r["tempF"]  = round(readings[i].tempF * 100) / 100.0;
    r["status"] = classifyTemp(readings[i].tempF);
    r["flags"]  = readings[i].flags;
    r["time"]   = readings[i].browserTime;
  }

  // NFC events (all)
  JsonArray evArr = doc.createNestedArray("events");
  for (int i = 0; i < nfcEventCount; i++) {
    JsonObject ev = evArr.createNestedObject();
    ev["index"] = nfcEvents[i].index;
    ev["tempF"] = nfcEvents[i].tempF;
    ev["flags"] = nfcEvents[i].flags;
    ev["time"]  = nfcEvents[i].browserTime;
    ev["change"]= round(nfcEvents[i].change * 100) / 100.0;
  }

  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleApiLive() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  unsigned long elapsedMs = sessionStarted ? (millis() - sessionStartMs) : 0;
  StaticJsonDocument<256> doc;
  doc["tempF"]     = round(latestTemp * 100) / 100.0;
  doc["status"]    = classifyTemp(latestTemp);
  doc["index"]     = readingCount;
  doc["events"]    = nfcEventCount;
  doc["elapsedMs"] = elapsedMs;
  doc["started"]   = sessionStarted;
  doc["flags"]     = readingCount > 0 ? readings[readingCount-1].flags : 0;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleApiSettings() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.method() == HTTP_POST && server.hasArg("plain")) {
    StaticJsonDocument<512> doc;
    if (!deserializeJson(doc, server.arg("plain"))) {
      if (doc.containsKey("ssid"))      strlcpy(cfg.staSSID,     doc["ssid"],     sizeof(cfg.staSSID));
      if (doc.containsKey("pass"))      strlcpy(cfg.staPass,     doc["pass"],     sizeof(cfg.staPass));
      if (doc.containsKey("apssid"))    strlcpy(cfg.apSSID,      doc["apssid"],   sizeof(cfg.apSSID));
      if (doc.containsKey("pname"))     strlcpy(cfg.patientName, doc["pname"],    sizeof(cfg.patientName));
      if (doc.containsKey("pid"))       strlcpy(cfg.patientID,   doc["pid"],      sizeof(cfg.patientID));
      if (doc.containsKey("mode"))      strlcpy(cfg.monitorMode, doc["mode"],     sizeof(cfg.monitorMode));
      if (doc.containsKey("interval"))  cfg.readingIntervalSec = doc["interval"];
      if (doc.containsKey("nfcevery"))  cfg.nfcStoreEveryN     = doc["nfcevery"];
      if (doc.containsKey("nfccount"))  cfg.numNfcReadings     = doc["nfccount"];
      if (doc.containsKey("tlow"))      cfg.tempLow            = doc["tlow"];
      if (doc.containsKey("televated")) cfg.tempElevated       = doc["televated"];
      if (doc.containsKey("tfever"))    cfg.tempFever          = doc["tfever"];
      if (doc.containsKey("thigh"))     cfg.tempHighFever      = doc["thigh"];
      if (doc.containsKey("tspike"))    cfg.spikeThreshold     = doc["tspike"];
      saveConfig();
      server.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
      delay(500);
      ESP.restart();
      return;
    }
  }
  // GET: return current settings
  StaticJsonDocument<512> out;
  out["ssid"]      = cfg.staSSID;
  out["apssid"]    = cfg.apSSID;
  out["pname"]     = cfg.patientName;
  out["pid"]       = cfg.patientID;
  out["mode"]      = cfg.monitorMode;
  out["interval"]  = cfg.readingIntervalSec;
  out["nfcevery"]  = cfg.nfcStoreEveryN;
  out["nfccount"]  = cfg.numNfcReadings;
  out["tlow"]      = cfg.tempLow;
  out["televated"] = cfg.tempElevated;
  out["tfever"]    = cfg.tempFever;
  out["thigh"]     = cfg.tempHighFever;
  out["tspike"]    = cfg.spikeThreshold;
  String s; serializeJson(out, s);
  server.send(200, "application/json", s);
}

// =====================================================
// WEB DASHBOARD HTML
// Served from handleRoot() — full single-page app
// =====================================================
void handleRoot() {
  // HTML is too large for a single string — send in chunks
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  // Part 1: HTML head + CSS
  server.sendContent(
"<!DOCTYPE html><html lang='en'><head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>TempSens v5</title>"
"<link href='https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@300;400;500;600;700&family=JetBrains+Mono:wght@400;600;700&display=swap' rel='stylesheet'>"
"<style>"
":root{"
"--bg:#050a0e;--surf:#0a1520;--surf2:#0f1e2e;--border:#162030;--border2:#1e3045;"
"--green:#00e676;--yellow:#ffb300;--red:#ff3d3d;--blue:#29b6f6;--purple:#ce93d8;"
"--orange:#ff7043;--text:#e8f4fd;--muted:#4a7090;--dim:#0f2030;"
"--grad1:linear-gradient(135deg,#00e67620,#29b6f610);"
"}"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:var(--bg);color:var(--text);font-family:'Space Grotesk',sans-serif;min-height:100vh;overflow-x:hidden}"
"body::before{content:'';position:fixed;inset:0;"
"background:radial-gradient(ellipse 80% 50% at 20% 10%,#00e67608,transparent),"
"radial-gradient(ellipse 60% 40% at 80% 80%,#29b6f606,transparent);"
"pointer-events:none;z-index:0}"
".wrap{max-width:1100px;margin:0 auto;padding:1.2rem 1rem 5rem;position:relative;z-index:1}"
/* Header */
"header{display:flex;align-items:center;justify-content:space-between;"
"padding:.8rem 1.4rem;background:var(--surf);border:1px solid var(--border);"
"border-radius:12px;margin-bottom:1rem;gap:1rem;flex-wrap:wrap}"
".logo{font-family:'JetBrains Mono',monospace;font-size:1.4rem;font-weight:700;letter-spacing:-1px}"
".logo span{color:var(--green)}"
".logo small{font-size:.65rem;color:var(--muted);font-weight:400;display:block;letter-spacing:.15em;margin-top:1px}"
".hright{display:flex;align-items:center;gap:.8rem;flex-wrap:wrap}"
".conn-dot{display:flex;align-items:center;gap:5px;font-size:.58rem;letter-spacing:.15em;text-transform:uppercase;color:var(--muted)}"
".dot{width:7px;height:7px;border-radius:50%;background:var(--dim);transition:background .4s}"
".dot.live{background:var(--green);animation:blink 1.8s infinite}"
".dot.warn{background:var(--yellow);animation:blink .8s infinite}"
".dot.err{background:var(--red)}"
"@keyframes blink{0%,100%{opacity:1}50%{opacity:.2}}"
".badge{font-size:.58rem;letter-spacing:.1em;text-transform:uppercase;padding:3px 10px;"
"border-radius:20px;border:1px solid;cursor:default}"
".badge-green{background:rgba(0,230,118,.1);color:var(--green);border-color:rgba(0,230,118,.25)}"
".badge-blue{background:rgba(41,182,246,.1);color:var(--blue);border-color:rgba(41,182,246,.25)}"
".badge-yellow{background:rgba(255,179,0,.1);color:var(--yellow);border-color:rgba(255,179,0,.25)}"
".badge-red{background:rgba(255,61,61,.1);color:var(--red);border-color:rgba(255,61,61,.25)}"
/* Tabs */
".tabs{display:flex;gap:.4rem;margin-bottom:1rem;border-bottom:1px solid var(--border);padding-bottom:0}"
".tab{padding:.55rem 1.2rem;font-size:.7rem;letter-spacing:.1em;text-transform:uppercase;"
"cursor:pointer;border-radius:6px 6px 0 0;color:var(--muted);border:1px solid transparent;"
"border-bottom:none;transition:all .2s;font-family:'JetBrains Mono',monospace}"
".tab.active{color:var(--green);border-color:var(--border);border-bottom:1px solid var(--bg);"
"background:var(--surf);margin-bottom:-1px}"
".tab:hover:not(.active){color:var(--text)}"
".panel{display:none}.panel.active{display:block}"
/* Metric cards */
".metrics{display:grid;grid-template-columns:repeat(4,1fr);gap:.7rem;margin-bottom:1rem}"
".mc{background:var(--surf);border:1px solid var(--border);border-radius:10px;"
"padding:.9rem 1rem;position:relative;overflow:hidden;transition:border-color .3s}"
".mc:hover{border-color:var(--border2)}"
".mc::after{content:'';position:absolute;top:0;left:0;right:0;height:2px;"
"background:linear-gradient(90deg,var(--ac,var(--green)),transparent)}"
".mc-label{font-size:.52rem;letter-spacing:.2em;color:var(--muted);text-transform:uppercase;margin-bottom:5px}"
".mc-val{font-family:'JetBrains Mono',monospace;font-size:1.4rem;font-weight:700;line-height:1;color:var(--text);transition:color .3s}"
".mc-sub{font-size:.55rem;color:var(--muted);margin-top:3px}"
/* Alert panel */
".alert-panel{background:var(--surf);border:1px solid var(--border);border-radius:10px;"
"padding:1rem 1.1rem;margin-bottom:1rem;position:relative;overflow:hidden}"
".alert-panel::before{content:'';position:absolute;top:0;left:0;right:0;height:2px;"
"background:linear-gradient(90deg,var(--red),var(--yellow),var(--orange),transparent)}"
".ap-header{display:flex;align-items:center;justify-content:space-between;margin-bottom:.7rem}"
".ap-title{font-size:.58rem;letter-spacing:.18em;text-transform:uppercase;color:var(--muted)}"
".alert-scroll{display:flex;flex-direction:column;gap:.4rem;max-height:140px;overflow-y:auto}"
".ae{display:flex;align-items:center;gap:.6rem;padding:5px 9px;border-radius:6px;"
"font-family:'JetBrains Mono',monospace;font-size:.62rem;border:1px solid;animation:slideIn .25s ease}"
"@keyframes slideIn{from{opacity:0;transform:translateX(-6px)}to{opacity:1;transform:none}}"
".ae-hf{background:rgba(255,61,61,.08);border-color:rgba(255,61,61,.25);color:var(--red)}"
".ae-fe{background:rgba(255,112,67,.08);border-color:rgba(255,112,67,.25);color:var(--orange)}"
".ae-el{background:rgba(255,179,0,.08);border-color:rgba(255,179,0,.25);color:var(--yellow)}"
".ae-lo{background:rgba(41,182,246,.08);border-color:rgba(41,182,246,.25);color:var(--blue)}"
".ae-sp{background:rgba(206,147,216,.08);border-color:rgba(206,147,216,.25);color:var(--purple)}"
".ae-dr{background:rgba(41,182,246,.08);border-color:rgba(41,182,246,.25);color:var(--blue)}"
".ae-empty{font-size:.6rem;color:var(--muted);padding:.4rem 0}"
/* Middle grid */
".mid{display:grid;grid-template-columns:1fr 260px;gap:.7rem;margin-bottom:1rem}"
".chart-card{background:var(--surf);border:1px solid var(--border);border-radius:10px;"
"padding:1rem;position:relative;overflow:hidden}"
".chart-card::before{content:'';position:absolute;top:0;left:0;right:0;height:2px;"
"background:linear-gradient(90deg,var(--green),var(--blue),transparent)}"
".card-hdr{display:flex;align-items:center;justify-content:space-between;margin-bottom:.8rem}"
".card-title{font-size:.55rem;letter-spacing:.18em;text-transform:uppercase;color:var(--muted)}"
".chart-area{position:relative;height:200px}"
"canvas{width:100%!important}"
/* Patient card */
".pt-card{background:var(--surf);border:1px solid var(--border);border-radius:10px;"
"padding:1rem;display:flex;flex-direction:column;gap:.9rem;position:relative;overflow:hidden}"
".pt-card::before{content:'';position:absolute;top:0;left:0;right:0;height:2px;"
"background:linear-gradient(90deg,var(--purple),var(--blue),transparent)}"
".pt-avatar{width:44px;height:44px;border-radius:50%;"
"background:linear-gradient(135deg,rgba(206,147,216,.2),rgba(41,182,246,.2));"
"border:1px solid var(--purple);display:flex;align-items:center;justify-content:center;font-size:1.3rem}"
".pt-name{font-family:'JetBrains Mono',monospace;font-size:1rem;font-weight:700}"
".pt-id{font-size:.58rem;color:var(--muted);margin-top:1px}"
".ig{display:grid;grid-template-columns:1fr 1fr;gap:.6rem}"
".ik{font-size:.48rem;text-transform:uppercase;letter-spacing:.15em;color:var(--muted);margin-bottom:1px}"
".iv{font-family:'JetBrains Mono',monospace;font-size:.72rem;color:var(--text)}"
/* Battery */
".bat-wrap{display:flex;align-items:center;gap:7px}"
".bat-body{display:flex;align-items:center;width:38px;height:13px;"
"border:1.5px solid var(--border2);border-radius:3px;padding:2px;position:relative}"
".bat-body::after{content:'';position:absolute;right:-5px;top:50%;transform:translateY(-50%);"
"width:3px;height:6px;background:var(--border2);border-radius:0 2px 2px 0}"
".bat-fill{height:100%;border-radius:1px;transition:width .6s,background .4s}"
/* NFC gauge */
".nfc-gauge{margin-top:.3rem}"
".nfc-track{height:5px;background:var(--dim);border-radius:3px;overflow:hidden;margin-top:3px}"
".nfc-fill{height:100%;background:var(--green);border-radius:3px;transition:width .5s}"
/* Log table */
".log-card{background:var(--surf);border:1px solid var(--border);border-radius:10px;"
"padding:1rem;position:relative;overflow:hidden}"
".log-card::before{content:'';position:absolute;top:0;left:0;right:0;height:2px;"
"background:linear-gradient(90deg,transparent,var(--blue),transparent)}"
"table{width:100%;border-collapse:collapse;font-family:'JetBrains Mono',monospace;font-size:.65rem}"
"thead th{text-align:left;font-size:.5rem;letter-spacing:.15em;text-transform:uppercase;"
"color:var(--muted);padding:0 7px 7px;border-bottom:1px solid var(--border)}"
"tbody td{padding:6px 7px;border-bottom:1px solid rgba(22,32,48,.7);vertical-align:middle}"
"tbody tr:last-child td{border-bottom:none}"
"tbody tr.r-hf td{background:rgba(255,61,61,.05)}"
"tbody tr.r-fe td{background:rgba(255,112,67,.04)}"
"tbody tr.r-el td{background:rgba(255,179,0,.04)}"
"tbody tr.r-lo td{background:rgba(41,182,246,.04)}"
"tbody tr:hover td{background:rgba(255,255,255,.02)}"
".flag-chip{display:inline-block;font-size:.5rem;padding:1px 5px;border-radius:3px;"
"margin-left:3px;vertical-align:middle;font-weight:700}"
".fc-hf{background:rgba(255,61,61,.2);color:var(--red)}"
".fc-fe{background:rgba(255,112,67,.2);color:var(--orange)}"
".fc-el{background:rgba(255,179,0,.2);color:var(--yellow)}"
".fc-lo{background:rgba(41,182,246,.2);color:var(--blue)}"
".fc-sp{background:rgba(206,147,216,.2);color:var(--purple)}"
".fc-dr{background:rgba(41,182,246,.15);color:var(--blue)}"
/* Settings form */
".settings-grid{display:grid;grid-template-columns:1fr 1fr;gap:.8rem}"
".form-group{display:flex;flex-direction:column;gap:.3rem}"
".form-group label{font-size:.55rem;letter-spacing:.15em;text-transform:uppercase;color:var(--muted)}"
".form-group input,.form-group select{background:var(--dim);border:1px solid var(--border2);"
"border-radius:6px;color:var(--text);padding:.45rem .7rem;font-family:'JetBrains Mono',monospace;"
"font-size:.75rem;outline:none;transition:border-color .2s}"
".form-group input:focus,.form-group select:focus{border-color:var(--green)}"
".form-section{background:var(--surf);border:1px solid var(--border);border-radius:10px;"
"padding:1rem 1.1rem;margin-bottom:.8rem}"
".form-section-title{font-size:.55rem;letter-spacing:.2em;text-transform:uppercase;"
"color:var(--muted);margin-bottom:.8rem;padding-bottom:.5rem;border-bottom:1px solid var(--border)}"
"button{background:var(--green);color:#050a0e;border:none;border-radius:7px;"
"padding:.55rem 1.4rem;font-family:'Space Grotesk',sans-serif;font-size:.8rem;font-weight:700;"
"cursor:pointer;transition:opacity .2s}"
"button:hover{opacity:.85}"
"button.danger{background:var(--red);color:var(--text)}"
"button.sec{background:var(--surf2);color:var(--text);border:1px solid var(--border2)}"
/* Ticker */
".ticker{position:fixed;bottom:0;left:0;right:0;background:#0a1520ee;"
"border-top:1px solid var(--border);padding:7px 1.5rem;"
"display:flex;align-items:center;gap:.8rem;font-size:.62rem;color:var(--muted);"
"backdrop-filter:blur(12px);z-index:100}"
".tk-temp{font-family:'JetBrains Mono',monospace;font-size:.9rem;font-weight:700;color:var(--text)}"
".tk-flag{font-size:.58rem;padding:1px 6px;border-radius:3px;font-weight:700}"
/* Misc */
".empty-row td{text-align:center;color:var(--muted);padding:1.5rem!important;font-size:.62rem}"
"@media(max-width:700px){.metrics{grid-template-columns:1fr 1fr}.mid{grid-template-columns:1fr}.settings-grid{grid-template-columns:1fr}}"
"@media(max-width:460px){.metrics{grid-template-columns:1fr}}"
"</style></head><body><div class='wrap'>"
  );

  // Part 2: Header + Tabs
  server.sendContent(
"<header>"
"<div class='logo'>Temp<span>Sens</span><small>v5 — Cold Chain / Patient Monitor</small></div>"
"<div class='hright'>"
"<div class='conn-dot'><div class='dot' id='cDot'></div><span id='cLbl'>Connecting</span></div>"
"<div class='badge badge-blue' id='durBadge'>0s</div>"
"<div class='badge badge-green' id='modeBadge'>-</div>"
"<div class='badge' id='syncBadge' style='background:rgba(74,112,144,.1);color:var(--muted);border-color:var(--border2)'>Time: Syncing</div>"
"</div></header>"

"<div class='tabs'>"
"<div class='tab active' onclick='showTab(\"dash\")'>Dashboard</div>"
"<div class='tab' onclick='showTab(\"events\")'>Events</div>"
"<div class='tab' onclick='showTab(\"log\")'>Full Log</div>"
"<div class='tab' onclick='showTab(\"nfc\")'>NFC Info</div>"
"<div class='tab' onclick='showTab(\"settings\")'>Settings</div>"
"</div>"
  );

  // Part 3: Dashboard panel
  server.sendContent(
"<div class='panel active' id='p-dash'>"
"<div class='metrics'>"
"<div class='mc' style='--ac:var(--green)'><div class='mc-label'>Live Temp</div>"
"<div class='mc-val' id='mLive'>-</div><div class='mc-sub' id='mLiveSt'>Waiting</div></div>"
"<div class='mc' style='--ac:var(--red)'><div class='mc-label'>Peak</div>"
"<div class='mc-val' id='mPeak'>-</div><div class='mc-sub'>Session max</div></div>"
"<div class='mc' style='--ac:var(--yellow)'><div class='mc-label'>Average</div>"
"<div class='mc-val' id='mAvg'>-</div><div class='mc-sub'>Session mean</div></div>"
"<div class='mc' style='--ac:var(--blue)'><div class='mc-label'>Battery</div>"
"<div class='mc-val' id='mBat'>-</div><div class='mc-sub' id='mBatV'>-</div></div>"
"</div>"
"<div class='alert-panel'>"
"<div class='ap-header'>"
"<div class='ap-title'>Anomaly Alerts</div>"
"<span style='font-size:.58rem;color:var(--muted)' id='evCount'>0 events</span>"
"</div>"
"<div class='alert-scroll' id='alertList'><div class='ae-empty'>Monitoring — no anomalies yet</div></div>"
"</div>"
"<div class='mid'>"
"<div class='chart-card'>"
"<div class='card-hdr'><div class='card-title'>Temperature Trend</div>"
"<div style='font-size:.58rem;color:var(--muted)' id='readCtr'>0 readings</div></div>"
"<div class='chart-area'><canvas id='tChart'></canvas></div>"
"</div>"
"<div class='pt-card'>"
"<div style='display:flex;align-items:center;gap:9px'>"
"<div class='pt-avatar' id='ptEmoji'>&#x1FA7A;</div>"
"<div><div class='pt-name' id='ptName'>-</div><div class='pt-id' id='ptId'>ID: -</div></div>"
"</div>"
"<div class='ig'>"
"<div><div class='ik'>Mode</div><div class='iv' id='ptMode'>-</div></div>"
"<div><div class='ik'>Min Temp</div><div class='iv' id='ptMin'>-</div></div>"
"<div><div class='ik'>Readings</div><div class='iv' id='ptReads'>0</div></div>"
"<div><div class='ik'>Events</div><div class='iv' id='ptEvents' style='color:var(--yellow)'>0</div></div>"
"<div><div class='ik'>Interval</div><div class='iv' id='ptInterval'>-</div></div>"
"<div><div class='ik'>Status</div><div class='iv' id='ptStatus'>-</div></div>"
"</div>"
"<div><div class='ik' style='margin-bottom:5px'>Battery</div>"
"<div class='bat-wrap'>"
"<div class='bat-body'><div class='bat-fill' id='batFill' style='width:0%;background:var(--green)'></div></div>"
"<span style='font-size:.65rem' id='batPct'>-%</span></div></div>"
"<div class='nfc-gauge'>"
"<div class='ik'>NFC Memory Used</div>"
"<div class='nfc-track'><div class='nfc-fill' id='nfcFill' style='width:0%'></div></div>"
"<div style='font-size:.55rem;color:var(--muted);margin-top:2px' id='nfcBytes'>0 / 5800 bytes</div>"
"</div>"
"</div></div></div>"
  );

  // Part 4: Events, Log, NFC, Settings panels
  server.sendContent(
"<div class='panel' id='p-events'>"
"<div class='log-card'>"
"<div class='card-hdr'><div class='card-title'>Anomaly Events Log</div>"
"<span style='font-size:.58rem;color:var(--muted)'>All meaningful deviations</span></div>"
"<table><thead><tr><th>#</th><th>Time</th><th>Temp</th><th>Flags</th><th>Change</th></tr></thead>"
"<tbody id='evBody'><tr class='empty-row'><td colspan='5'>No events yet</td></tr></tbody>"
"</table></div></div>"

"<div class='panel' id='p-log'>"
"<div class='log-card'>"
"<div class='card-hdr'><div class='card-title'>Full Reading Log</div>"
"<span style='font-size:.58rem;color:var(--muted)' id='logNote'>Last 200 readings shown</span></div>"
"<table><thead><tr><th>#</th><th>Time</th><th>Temp (F)</th><th>Status</th><th>Flags</th><th>Change</th><th>Bar</th></tr></thead>"
"<tbody id='logBody'><tr class='empty-row'><td colspan='7'>Waiting for readings...</td></tr></tbody>"
"</table></div></div>"

"<div class='panel' id='p-nfc'>"
"<div class='form-section'>"
"<div class='form-section-title'>NFC Tag Memory Info</div>"
"<div class='ig' style='gap:.7rem'>"
"<div><div class='ik'>Tag Model</div><div class='iv'>ST25DV64KC</div></div>"
"<div><div class='ik'>Total Memory</div><div class='iv'>8192 bytes (64Kbit)</div></div>"
"<div><div class='ik'>Target Budget</div><div class='iv'>5800 bytes (70.8%)</div></div>"
"<div><div class='ik'>Currently Used</div><div class='iv' id='nfcUsedBytes'>-</div></div>"
"<div><div class='ik'>Events on NFC</div><div class='iv' id='nfcEvCount'>-</div></div>"
"<div><div class='ik'>% of Tag Used</div><div class='iv' id='nfcPct'>-</div></div>"
"</div>"
"<div style='margin-top:1rem;padding-top:.8rem;border-top:1px solid var(--border)'>"
"<div class='ik' style='margin-bottom:.4rem'>What gets written to NFC tag:</div>"
"<div style='font-size:.65rem;color:var(--muted);line-height:1.7'>"
"Only meaningful events are written to NFC to maximise storage.<br>"
"This includes: Fever, High Fever, Low Temp, Sudden Spike (+), Sudden Drop (-).<br>"
"Each event line includes: reading #, time, temperature, flag tags, and change delta.<br>"
"Normal readings are NOT stored on NFC — use WiFi dashboard for full log.<br>"
"Tap the tag with any NFC-capable phone to read the log directly."
"</div></div></div></div>"

"<div class='panel' id='p-settings'>"
"<div class='form-section'>"
"<div class='form-section-title'>WiFi Credentials</div>"
"<div class='settings-grid'>"
"<div class='form-group'><label>Station SSID (Home/Office WiFi)</label>"
"<input type='text' id='sSSID' placeholder='YourWiFiName'></div>"
"<div class='form-group'><label>Station Password</label>"
"<input type='password' id='sPass' placeholder='WiFiPassword'></div>"
"<div class='form-group'><label>AP Hotspot Name (fallback)</label>"
"<input type='text' id='sAPSSID' placeholder='TempSens-Setup'></div>"
"</div></div>"
"<div class='form-section'>"
"<div class='form-section-title'>Patient / Asset Info</div>"
"<div class='settings-grid'>"
"<div class='form-group'><label>Name / Asset Label</label><input type='text' id='sPName'></div>"
"<div class='form-group'><label>ID / Batch Number</label><input type='text' id='sPID'></div>"
"<div class='form-group'><label>Monitor Mode</label>"
"<select id='sMode'><option value='patient'>Patient Monitoring</option>"
"<option value='coldchain'>Cold Chain</option></select></div>"
"</div></div>"
"<div class='form-section'>"
"<div class='form-section-title'>Reading Parameters</div>"
"<div class='settings-grid'>"
"<div class='form-group'><label>Reading Interval (seconds)</label><input type='number' id='sInterval' min='1' max='3600'></div>"
"<div class='form-group'><label>NFC Store Every N Normal Readings</label><input type='number' id='sNFCEvery' min='1' max='50'></div>"
"<div class='form-group'><label>Max NFC Readings Stored</label><input type='number' id='sNFCCount' min='5' max='90'></div>"
"</div></div>"
"<div class='form-section'>"
"<div class='form-section-title'>Temperature Thresholds (°F)</div>"
"<div class='settings-grid'>"
"<div class='form-group'><label>Low Temp Alert Below</label><input type='number' step='0.1' id='sTLow'></div>"
"<div class='form-group'><label>Elevated Above</label><input type='number' step='0.1' id='sTElev'></div>"
"<div class='form-group'><label>Fever Above</label><input type='number' step='0.1' id='sTFever'></div>"
"<div class='form-group'><label>High Fever Above</label><input type='number' step='0.1' id='sTHigh'></div>"
"<div class='form-group'><label>Spike Threshold (°F change)</label><input type='number' step='0.1' id='sTSpike'></div>"
"</div></div>"
"<div style='display:flex;gap:.7rem;flex-wrap:wrap'>"
"<button onclick='saveSettings()'>Save &amp; Reboot Device</button>"
"<button class='sec' onclick='loadSettings()'>Reload from Device</button>"
"</div></div>"
  );

  // Part 5: JavaScript
  server.sendContent(
"<div class='ticker'>"
"<div class='dot' id='tkDot'></div>"
"<span style='font-size:.6rem;text-transform:uppercase;letter-spacing:.1em'>Live</span>"
"<span class='tk-temp' id='tkTemp'>-.--F</span>"
"<span id='tkSt' style='color:var(--muted);font-size:.6rem'>-</span>"
"<span id='tkFlag' style='margin-left:4px'></span>"
"<span id='tkTime' style='margin-left:auto;font-size:.6rem'>-</span>"
"<span id='tkElapsed' style='color:var(--blue);font-size:.6rem;margin-left:.6rem'>-</span>"
"</div>"

"<script src='https://cdnjs.cloudflare.com/ajax/libs/Chart.js/4.4.1/chart.umd.min.js'></script>"
"<script>"
"const AB=window.location.origin;"
"let done=false,synced=false,pLiveId,pDataId,elSec=0,sesStart=null,seenAlerts=new Set();"
// Send browser time to device
"async function syncTime(){"
"try{"
"const r=await fetch(AB+'/api/sync',{method:'POST',"
"headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({epochMs:Date.now()})});"
"if(r.ok){synced=true;document.getElementById('syncBadge').textContent='Time: Synced';"
"document.getElementById('syncBadge').style.color='var(--green)';}}"
"catch(e){}}"
// Classify
"function cls(f){"
"const c=window._cfg||{};"
"const tL=c.tempLow||95,tE=c.tempElevated||99,tF=c.tempFever||100.4,tH=c.tempHighFever||102.4;"
"if(f<tL)       return{l:'Low',      col:'var(--blue)',   ac:'ae-lo',fc:'fc-lo',rc:'r-lo'};"
"if(f>=tH)      return{l:'High Fever',col:'var(--red)',   ac:'ae-hf',fc:'fc-hf',rc:'r-hf'};"
"if(f>=tF)      return{l:'Fever',    col:'var(--orange)', ac:'ae-fe',fc:'fc-fe',rc:'r-fe'};"
"if(f>=tE)      return{l:'Elevated', col:'var(--yellow)', ac:'ae-el',fc:'fc-el',rc:'r-el'};"
"return{l:'Normal',col:'var(--green)',ac:'',fc:'',rc:''};"
"}"
"function fmtDur(s){if(s<60)return s+'s';return Math.floor(s/60)+'m '+(s%60)+'s';}"
"function setDot(id,st){const d=document.getElementById(id);d.className='dot '+(st==='live'?'live':st==='done'?'':st);}"
"function flagChips(fl){"
"let h='';"
"if(fl&0x20)h+=\"<span class='flag-chip fc-hf'>HIGH-FVR</span>\";"
"else if(fl&0x10)h+=\"<span class='flag-chip fc-fe'>FEVER</span>\";"
"else if(fl&0x08)h+=\"<span class='flag-chip fc-el'>ELEVATED</span>\";"
"else if(fl&0x04)h+=\"<span class='flag-chip fc-lo'>LOW</span>\";"
"if(fl&0x01)h+=\"<span class='flag-chip fc-sp'>SPIKE+</span>\";"
"if(fl&0x02)h+=\"<span class='flag-chip fc-dr'>DROP-</span>\";"
"return h||'<span style=\\'color:var(--muted)\\'>—</span>';"
"}"
"function alertClass(fl){"
"if(fl&0x20)return'ae-hf';"
"if(fl&0x10)return'ae-fe';"
"if(fl&0x08)return'ae-el';"
"if(fl&0x04)return'ae-lo';"
"if(fl&0x01||fl&0x02)return'ae-sp';"
"return'ae-sp';"
"}"
"function pushAlert(idx,tF,fl,chg,time){"
"const k='r'+idx; if(seenAlerts.has(k))return; seenAlerts.add(k);"
"const list=document.getElementById('alertList');"
"const em=list.querySelector('.ae-empty'); if(em)em.remove();"
"const ac=alertClass(fl);"
"const chgS=chg>=0?'+'+chg.toFixed(2)+'F':chg.toFixed(2)+'F';"
"const d=document.createElement('div');"
"d.className='ae '+ac;"
"d.innerHTML='<b>R'+idx+'</b> <span style=\\'opacity:.7\\'>'+time+'</span> '+tF.toFixed(2)+'F '+flagChips(fl)+' <span style=\\'opacity:.6;margin-left:auto\\'>'+chgS+'</span>';"
"list.appendChild(d); list.scrollTop=list.scrollHeight;"
"if((fl&0x30)&&Notification&&Notification.permission==='granted')"
"new Notification('TempSens Alert',{body:'R'+idx+' '+tF.toFixed(2)+'F '+((fl&0x20)?'HIGH FEVER':(fl&0x10)?'Fever':'Alert')});}"
// Chart
"const ctx=document.getElementById('tChart').getContext('2d');"
"const chart=new Chart(ctx,{type:'line',data:{labels:[],datasets:[{"
"data:[],borderColor:'#00e676',borderWidth:1.5,"
"pointBackgroundColor:[],pointBorderColor:'#050a0e',pointBorderWidth:1.5,"
"pointRadius:4,pointHoverRadius:6,fill:true,"
"backgroundColor:(c)=>{const g=c.chart.ctx.createLinearGradient(0,0,0,200);"
"g.addColorStop(0,'rgba(0,230,118,0.12)');g.addColorStop(1,'rgba(0,230,118,0)');return g;},"
"tension:0.3}]},"
"options:{responsive:true,maintainAspectRatio:false,"
"animation:{duration:300,easing:'easeOutQuart'},"
"plugins:{legend:{display:false},tooltip:{"
"backgroundColor:'#0a1520',borderColor:'#162030',borderWidth:1,"
"titleColor:'#4a7090',bodyColor:'#e8f4fd',"
"callbacks:{label:c=>' '+c.parsed.y.toFixed(2)+'F — '+cls(c.parsed.y).l}}}"
",scales:{"
"x:{grid:{color:'#0f2030'},ticks:{color:'#4a7090',font:{family:'JetBrains Mono',size:9},maxTicksLimit:10}},"
"y:{grid:{color:'#0f2030'},ticks:{color:'#4a7090',font:{family:'JetBrains Mono',size:9},callback:v=>v.toFixed(1)+'F'}}"
"}}});"
"function updateChart(rds){"
"chart.data.labels=rds.map(r=>r.time&&r.time!='--:--:--'?r.time:'R'+r.index);"
"chart.data.datasets[0].data=rds.map(r=>r.tempF);"
"chart.data.datasets[0].pointBackgroundColor=rds.map(r=>cls(r.tempF).col);"
"chart.data.datasets[0].pointRadius=rds.map(r=>r.flags?6:3);"
"if(rds.length>1){"
"const vs=rds.map(r=>r.tempF),mn=Math.min(...vs),mx=Math.max(...vs);"
"chart.options.scales.y.min=+(mn-0.5).toFixed(1);"
"chart.options.scales.y.max=+(mx+0.5).toFixed(1);}"
"chart.update();}"
"function renderLog(rds){"
"const tb=document.getElementById('logBody');"
"if(!rds||!rds.length){tb.innerHTML=\"<tr class='empty-row'><td colspan='7'>Waiting...</td></tr>\";return;}"
"tb.innerHTML=rds.map((r,i)=>{"
"const c=cls(r.tempF);"
"const prev=i>0?rds[i-1].tempF:r.tempF,diff=r.tempF-prev;"
"const diffS=i===0?'-':(diff>=0?'+'+diff.toFixed(2):diff.toFixed(2));"
"const diffC=diff>0.5?'var(--red)':diff<-0.5?'var(--blue)':'var(--muted)';"
"const pct=Math.round(((r.tempF-92)/(108-92))*100);"
"return \"<tr class='\"+c.rc+\"'>\"+"
"\"<td style='color:var(--muted)'>\"+String(r.index).padStart(3,'0')+\"</td>\"+"
"\"<td style='color:var(--muted);font-size:.58rem'>\"+r.time+\"</td>\"+"
"\"<td style='color:\"+c.col+\";font-weight:600'>\"+r.tempF.toFixed(2)+\"F</td>\"+"
"\"<td style='font-size:.6rem'>\"+c.l+\"</td>\"+"
"\"<td>\"+flagChips(r.flags)+\"</td>\"+"
"\"<td style='color:\"+diffC+\"'>\"+diffS+\"</td>\"+"
"\"<td><div style='width:70px;height:3px;background:var(--dim);border-radius:2px;overflow:hidden'>\"+"
"\"<div style='width:\"+pct+\"%;height:100%;background:\"+c.col+\";border-radius:2px'></div></div></td>\"+"
"\"</tr>\";"
"}).join('');}"
"function renderEvents(evs){"
"const tb=document.getElementById('evBody');"
"if(!evs||!evs.length){tb.innerHTML=\"<tr class='empty-row'><td colspan='5'>No events recorded</td></tr>\";return;}"
"tb.innerHTML=evs.map(e=>{"
"const c=cls(e.tempF);"
"const chgS=e.change>=0?'+'+e.change.toFixed(2)+'F':e.change.toFixed(2)+'F';"
"return \"<tr class='\"+c.rc+\"'>\"+"
"\"<td style='color:var(--muted)'>\"+String(e.index).padStart(3,'0')+\"</td>\"+"
"\"<td style='color:var(--muted);font-size:.58rem'>\"+e.time+\"</td>\"+"
"\"<td style='color:\"+c.col+\";font-weight:600'>\"+e.tempF.toFixed(2)+\"F</td>\"+"
"\"<td>\"+flagChips(e.flags)+\"</td>\"+"
"\"<td style='color:\"+((e.change>0)?'var(--red)':'var(--blue)')+\"'>\"+chgS+\"</td>\"+"
"\"</tr>\";"
"}).join('');}"
// Apply full data response
"function applyData(d){"
"window._cfg=d.config;"
"document.getElementById('ptName').textContent=d.patient.name;"
"document.getElementById('ptId').textContent='ID: '+d.patient.id;"
"document.getElementById('ptMode').textContent=d.patient.mode;"
"document.getElementById('modeBadge').textContent=d.patient.mode;"
"const bp=d.battery.percent,bc=bp>50?'var(--green)':bp>20?'var(--yellow)':'var(--red)';"
"document.getElementById('mBat').textContent=bp+'%';"
"document.getElementById('mBatV').textContent=d.battery.voltage.toFixed(2)+'V';"
"document.getElementById('batFill').style.width=bp+'%';"
"document.getElementById('batFill').style.background=bc;"
"document.getElementById('batPct').textContent=bp+'%';"
"const s=d.session;"
"if(s.count>0){"
"document.getElementById('mPeak').textContent=s.max.toFixed(1)+'F';"
"document.getElementById('mAvg').textContent=s.avg.toFixed(1)+'F';"
"document.getElementById('ptMin').textContent=s.min.toFixed(1)+'F';}"
"document.getElementById('ptReads').textContent=s.count;"
"document.getElementById('ptEvents').textContent=s.events;"
"document.getElementById('ptInterval').textContent=s.intervalSec+'s';"
"document.getElementById('ptStatus').textContent=s.status;"
"document.getElementById('readCtr').textContent=s.count+' readings';"
"document.getElementById('evCount').textContent=s.events+' events';"
// NFC gauge
"const nb=s.nfcBytes,nbudget=s.nfcBudget||5800;"
"const npct=Math.round((nb/nbudget)*100);"
"document.getElementById('nfcFill').style.width=Math.min(npct,100)+'%';"
"document.getElementById('nfcFill').style.background=npct>90?'var(--red)':npct>70?'var(--yellow)':'var(--green)';"
"document.getElementById('nfcBytes').textContent=nb+' / '+nbudget+' bytes ('+npct+'%)';"
"document.getElementById('nfcUsedBytes').textContent=nb+' bytes';"
"document.getElementById('nfcEvCount').textContent=s.events;"
"document.getElementById('nfcPct').textContent=((nb/8192)*100).toFixed(1)+'% of tag';"
"if(d.readings&&d.readings.length>0){"
"updateChart(d.readings);renderLog(d.readings);"
"d.readings.forEach((r,i)=>{"
"if(r.flags){const prev=i>0?d.readings[i-1].tempF:r.tempF;pushAlert(r.index,r.tempF,r.flags,r.tempF-prev,r.time);}});}"
"if(d.events)renderEvents(d.events);"
"if(s.done){done=true;clearInterval(pLiveId);clearInterval(pDataId);"
"setDot('cDot','');setDot('tkDot','');"
"document.getElementById('cLbl').textContent='Complete';}}"
// Live poll
"let elTimer;"
"async function pollLive(){"
"if(done)return;"
"try{"
"const r=await fetch(AB+'/api/live');"
"const d=await r.json();"
"if(!sesStart&&d.started){sesStart=Date.now();"
"elTimer=setInterval(()=>{"
"if(done)return;"
"elSec=Math.floor((Date.now()-sesStart)/1000);"
"document.getElementById('durBadge').textContent=fmtDur(elSec);"
"document.getElementById('tkElapsed').textContent=fmtDur(elSec);"
"},1000);}"
"const c=cls(d.tempF);"
"document.getElementById('mLive').textContent=d.tempF.toFixed(1)+'F';"
"document.getElementById('mLive').style.color=c.col;"
"document.getElementById('mLiveSt').textContent=c.l;"
"document.getElementById('tkTemp').textContent=d.tempF.toFixed(2)+'F';"
"document.getElementById('tkTemp').style.color=c.col;"
"document.getElementById('tkSt').textContent=c.l;"
"document.getElementById('tkFlag').innerHTML=d.flags?flagChips(d.flags):'';"
"document.getElementById('tkTime').textContent=new Date().toLocaleTimeString();"
"setDot('cDot',d.done?'':'live');setDot('tkDot',d.done?'':'live');"
"document.getElementById('cLbl').textContent=d.done?'Complete':'Live';"
"if(d.done)done=true;"
"}catch(e){setDot('cDot','err');setDot('tkDot','err');"
"document.getElementById('cLbl').textContent='Offline';}}"
"async function pollData(){try{const r=await fetch(AB+'/api/data');const d=await r.json();applyData(d);}catch(e){}}"
// Settings
"async function loadSettings(){"
"try{const r=await fetch(AB+'/api/settings');const d=await r.json();"
"document.getElementById('sSSID').value=d.ssid||'';"
"document.getElementById('sPass').value='';"
"document.getElementById('sAPSSID').value=d.apssid||'';"
"document.getElementById('sPName').value=d.pname||'';"
"document.getElementById('sPID').value=d.pid||'';"
"document.getElementById('sMode').value=d.mode||'patient';"
"document.getElementById('sInterval').value=d.interval||10;"
"document.getElementById('sNFCEvery').value=d.nfcevery||4;"
"document.getElementById('sNFCCount').value=d.nfccount||60;"
"document.getElementById('sTLow').value=d.tlow||95;"
"document.getElementById('sTElev').value=d.televated||99;"
"document.getElementById('sTFever').value=d.tfever||100.4;"
"document.getElementById('sTHigh').value=d.thigh||102.4;"
"document.getElementById('sTSpike').value=d.tspike||1.5;"
"}catch(e){alert('Could not load settings from device');}}"
"async function saveSettings(){"
"const body={ssid:document.getElementById('sSSID').value,"
"pass:document.getElementById('sPass').value,"
"apssid:document.getElementById('sAPSSID').value,"
"pname:document.getElementById('sPName').value,"
"pid:document.getElementById('sPID').value,"
"mode:document.getElementById('sMode').value,"
"interval:+document.getElementById('sInterval').value,"
"nfcevery:+document.getElementById('sNFCEvery').value,"
"nfccount:+document.getElementById('sNFCCount').value,"
"tlow:+document.getElementById('sTLow').value,"
"televated:+document.getElementById('sTElev').value,"
"tfever:+document.getElementById('sTFever').value,"
"thigh:+document.getElementById('sTHigh').value,"
"tspike:+document.getElementById('sTSpike').value};"
"if(!body.pass)delete body.pass;"
"try{const r=await fetch(AB+'/api/settings',{method:'POST',"
"headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});"
"const d=await r.json();"
"if(d.ok)alert('Saved! Device rebooting...');"
"else alert('Save failed');}catch(e){alert('Error saving settings: '+e.message);}}"
// Tab switching
"function showTab(id){"
"document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active'));"
"document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));"
"document.getElementById('p-'+id).classList.add('active');"
"event.target.classList.add('active');"
"if(id==='settings')loadSettings();}"
// Notification permission
"if(Notification&&Notification.permission==='default')Notification.requestPermission();"
// Init
"syncTime();"
"pollData();pollLive();"
"pLiveId=setInterval(pollLive,800);"
"pDataId=setInterval(pollData,2500);"
"setInterval(syncTime,300000);" // re-sync time every 5 min
"</script></div></body></html>"
  );
}

void handleNotFound() { server.send(404, "text/plain", "Not found"); }

// =====================================================
// SETUP
// =====================================================
void setup() {
  delay(500);
  Serial.begin(115200);
  Wire.begin();
  analogReadResolution(12);

  // Load config from NVS
  loadConfig();

  // OLED init
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { while (1); }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  showWelcomeScreen();

  // === WIFI — STA first, AP fallback ===
  // On every boot: always attempt STA. No persistent fallback state.
  WiFi.mode(WIFI_OFF);
  delay(200);

  if (strlen(cfg.staSSID) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.begin(cfg.staSSID, cfg.staPass);
    Serial.print("STA connecting to: "); Serial.println(cfg.staSSID);

    oledClear();
    display.setFont(); display.setTextSize(1);
    printCentered("Connecting WiFi", 20, NULL);
    printCentered(cfg.staSSID, 36, NULL);
    display.display();

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < STA_CONNECT_TIMEOUT_MS) {
      delay(300);
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    apModeActive  = false;
    Serial.println("STA connected: " + WiFi.localIP().toString());
    showWiFiStatus(true, WiFi.localIP().toString().c_str(), true);
  } else {
    // STA failed — start AP. Retry STA in loop() every WIFI_RETRY_INTERVAL_SEC.
    apModeActive  = true;
    wifiConnected = false;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(cfg.apSSID, AP_PASSWORD);
    delay(200);
    Serial.print("AP fallback: "); Serial.println(cfg.apSSID);
    showWiFiStatus(false, "192.168.4.1", false);
    lastWifiRetryMs = millis();
  }

  // Web server routes
  server.on("/",             handleRoot);
  server.on("/api/data",     handleApiData);
  server.on("/api/live",     handleApiLive);
  server.on("/api/sync",     handleApiSync);
  server.on("/api/settings", handleApiSettings);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");

  // Sensor init
  if (!tempSensor.begin()) {
    display.clearDisplay(); display.setCursor(0, 0);
    display.print("TMP117 ERR"); display.display();
    Serial.println("FATAL: TMP117 not found"); while (1);
  }
  Serial.println("TMP117 OK");

  // NFC init
  if (!tag.begin(Wire)) {
    display.clearDisplay(); display.setCursor(0, 0);
    display.print("NFC TAG ERR"); display.display();
    Serial.println("FATAL: ST25DV64KC not found"); while (1);
  }
  Serial.println("ST25DV64KC OK — 8192 bytes total");

  // Clear NFC and write patient header
  uint8_t tagMem[256]; memset(tagMem, 0, sizeof(tagMem));
  tag.writeEEPROM(0x0, tagMem, sizeof(tagMem));
  tag.writeCCFile8Byte();
  nfcMemLoc = tag.getCCFileLen();
  char patNFC[64];
  snprintf(patNFC, sizeof(patNFC), "TempSens v5 — %s (%s) — %s",
    cfg.patientName, cfg.patientID, cfg.monitorMode);
  tag.writeNDEFText(patNFC, &nfcMemLoc, true, false);
  Serial.printf("NFC init: budget=%d bytes (%.1f%% of 8192)\n",
    NFC_LOG_MAX, (float)NFC_LOG_MAX / NFC_TOTAL_BYTES * 100.0);

  // Ready — start session
  oledClear();
  display.setFont(); display.setTextSize(1);
  printCentered("Ready", 18, NULL);
  printCentered("Monitoring active", 32, NULL);
  printCentered("WiFi updating live", 46, NULL);
  display.display();
  delay(1500);

  sessionStarted = true;
  sessionStatus  = "Reading";
  sessionStartMs = millis();
  lastReadingMs  = millis() - (cfg.readingIntervalSec * 1000UL);
  Serial.printf("Session started — interval %ds — continuous until power-off\n",
    cfg.readingIntervalSec);
}

// =====================================================
// LOOP — non-blocking, continuous
// Reads until power-off (no session limit on WiFi)
// NFC updated after every meaningful event
// =====================================================
void loop() {
  server.handleClient();

  // === STA retry (if in AP fallback mode) ===
  if (apModeActive && !wifiConnected) {
    unsigned long now2 = millis();
    if (now2 - lastWifiRetryMs > (unsigned long)(WIFI_RETRY_INTERVAL_SEC * 1000UL)) {
      lastWifiRetryMs = now2;
      Serial.println("Retrying STA...");
      server.stop();
      WiFi.mode(WIFI_OFF); delay(100);
      WiFi.mode(WIFI_STA);
      WiFi.begin(cfg.staSSID, cfg.staPass);
      unsigned long rs = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - rs < 10000) {
        server.handleClient(); // keep AP clients served during retry window
        delay(100);
      }
      if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true; apModeActive = false;
        server.begin();
        Serial.println("STA reconnected: " + WiFi.localIP().toString());
        // Show on OLED briefly but don't block readings
        display.fillRect(0, 56, 128, 8, SSD1306_BLACK);
        display.setFont(); display.setTextColor(SSD1306_WHITE);
        char ipBuf[20]; WiFi.localIP().toString().toCharArray(ipBuf, sizeof(ipBuf));
        printCentered(ipBuf, 57, NULL);
        display.display();
      } else {
        // Restore AP — server still works for connected clients
        WiFi.mode(WIFI_AP);
        WiFi.softAP(cfg.apSSID, AP_PASSWORD);
        delay(100);
        server.begin();
        Serial.println("STA retry failed, AP restored");
      }
    }
  }

  if (!sessionStarted) return;

  // === Non-blocking reading timer ===
  unsigned long now = millis();
  if (now - lastReadingMs < (unsigned long)(cfg.readingIntervalSec * 1000UL)) return;
  lastReadingMs = now;

  // Sensor sanity check
  float c = tempSensor.readTempC();
  if (c < 20.0 || c > 50.0) {
    Serial.printf("Sensor read suspect (%.2fC) — skipped\n", c);
    return;
  }
  float f = c * 9.0 / 5.0 + 32.0;

  if (readingCount >= MAX_WIFI_READINGS) {
    // Rotate: drop oldest, shift down
    memmove(&readings[0], &readings[1], (MAX_WIFI_READINGS - 1) * sizeof(Reading));
    readingCount = MAX_WIFI_READINGS - 1;
  }

  float prevF = readingCount > 0 ? readings[readingCount-1].tempF : f;
  uint8_t flags = buildFlags(f, prevF, readingCount == 0);

  readings[readingCount].tempF   = f;
  readings[readingCount].localMs = now;
  readings[readingCount].flags   = flags;
  getBrowserTime(readings[readingCount].browserTime,
    sizeof(readings[readingCount].browserTime), now);

  latestTemp = f;
  if (f > maxTemp) maxTemp = f;
  if (f < minTemp) minTemp = f;

  // Track NFC-worthy events
  if (isMeaningful(flags) && nfcEventCount < NFC_MAX_EVENTS) {
    nfcEvents[nfcEventCount].index  = readingCount + 1;
    nfcEvents[nfcEventCount].tempF  = f;
    nfcEvents[nfcEventCount].flags  = flags;
    nfcEvents[nfcEventCount].change = f - prevF;
    strlcpy(nfcEvents[nfcEventCount].browserTime,
      readings[readingCount].browserTime, 24);
    nfcEventCount++;
    Serial.printf("EVENT R%d: %.2fF flags=0x%02X\n", readingCount+1, f, flags);
    // Update NFC tag with latest events
    buildAndWriteNFC();
  }

  // Update session status
  if      (maxTemp >= cfg.tempHighFever) sessionStatus = "High Fever";
  else if (maxTemp >= cfg.tempFever)     sessionStatus = "Fever";
  else if (maxTemp >= cfg.tempElevated)  sessionStatus = "Elevated";
  else if (minTemp <  cfg.tempLow)       sessionStatus = "Low Temp";
  else                                   sessionStatus = "Normal";

  readingCount++;

  Serial.printf("R%d: %.2fF [%s] flags=0x%02X t=%s\n",
    readingCount, f, classifyTemp(f), flags, readings[readingCount-1].browserTime);

  // OLED update — non-blocking, no delay
  showReadingScreen(f, readingCount-1, flags);
}
