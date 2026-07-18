#include <Arduino.h>
#include <WiFi.h>
#include <ESP32Ping.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <ctype.h>
#include <string.h>
#include "esp_sleep.h"
#include "secrets.h"
#include "web_page.h"

const uint32_t sleepTime = 20000; // 20 seconds
const uint32_t historyBucketMs = 10UL * 60UL * 1000UL; // 10 minutes
const uint8_t historyBucketsCount = 144; // 24h / 10m
const uint32_t wifiReconnectTimeoutMs = 10000; // 10 seconds
const uint32_t wifiReconnectStepMs = 500;
const uint32_t defaultRestartDelayMs = 3UL * 60000UL; // 3 mins
const uint32_t maxRestartDelayMs = 24UL * 60UL * 60000UL; // 24h
const uint32_t defaultNoSuccessPingTimeMs = 5UL * 60000UL; // 5 mins without successful ping
const uint32_t maxNoSuccessPingTimeMs = 24UL * 60UL * 60000UL; // 24h
const uint32_t defaultMinFailedPings = 10;
const uint32_t maxMinFailedPings = 1000;
const bool defaultAutoRestartEnabled = true;

const char* configNamespace = "watchdog";
const char* configRestartDelayMsKey = "restartDelayMs";
const char* configNoSuccessPingTimeMsKey = "noSuccessPingTimeMs";
const char* configMinFailedPingsKey = "minFailedPings";
const char* configAutoRestartEnabledKey = "autoRestartEnabled";

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* watchdogHost = MONITORED_HOST; // TODO: add watchdog IP/host
const int PIN = 14;

struct DeviceStatus {
  String status = "WAIT";
  ulong lastUpdate = 0;
  ulong lastRestart = 0;
  ulong waitUntil = 0;
  uint failedPings = 0;
};

struct AppConfig {
  uint32_t restartDelayMs = defaultRestartDelayMs;
  uint32_t noSuccessPingTimeMs = defaultNoSuccessPingTimeMs;
  uint32_t minFailedPings = defaultMinFailedPings;
  bool autoRestartEnabled = defaultAutoRestartEnabled;
};

enum HistoryEventFlag : uint8_t {
  HISTORY_OK = 1,
  HISTORY_FAILED = 2,
  HISTORY_RESTART = 4,
  HISTORY_WIFI_FAILED = 8
};

DeviceStatus status;
AppConfig config;
SemaphoreHandle_t statusMutex;
SemaphoreHandle_t configMutex;
uint8_t historyBuckets[historyBucketsCount];
uint32_t historyCurrentBucketId = 0;
bool historyInitialized = false;
Preferences preferences;

// --- Web part

AsyncWebServer server(80);
TaskHandle_t pollingTaskHandle;

uint32_t clampRestartDelayMs(uint32_t value) {
  if (value > maxRestartDelayMs) {
    return maxRestartDelayMs;
  }
  return value;
}

uint32_t clampNoSuccessPingTimeMs(uint32_t value) {
  if (value > maxNoSuccessPingTimeMs) {
    return maxNoSuccessPingTimeMs;
  }
  return value;
}

uint32_t clampMinFailedPings(uint32_t value) {
  if (value < 1) {
    return 1;
  }
  if (value > maxMinFailedPings) {
    return maxMinFailedPings;
  }
  return value;
}

uint32_t minutesToMs(uint32_t minutes) {
  if (minutes > (maxRestartDelayMs / 60000UL)) {
    return maxRestartDelayMs;
  }
  return minutes * 60000UL;
}

uint32_t noSuccessMinutesToMs(uint32_t minutes) {
  if (minutes > (maxNoSuccessPingTimeMs / 60000UL)) {
    return maxNoSuccessPingTimeMs;
  }
  return minutes * 60000UL;
}

uint32_t getRestartDelayMs() {
  xSemaphoreTake(configMutex, portMAX_DELAY);
  uint32_t delay = config.restartDelayMs;
  xSemaphoreGive(configMutex);
  return delay;
}

uint32_t getNoSuccessPingTimeMs() {
  xSemaphoreTake(configMutex, portMAX_DELAY);
  uint32_t value = config.noSuccessPingTimeMs;
  xSemaphoreGive(configMutex);
  return value;
}

uint32_t getMinFailedPings() {
  xSemaphoreTake(configMutex, portMAX_DELAY);
  uint32_t value = config.minFailedPings;
  xSemaphoreGive(configMutex);
  return value;
}

bool getAutoRestartEnabled() {
  xSemaphoreTake(configMutex, portMAX_DELAY);
  bool value = config.autoRestartEnabled;
  xSemaphoreGive(configMutex);
  return value;
}

void loadConfig() {
  preferences.begin(configNamespace, true);
  uint32_t storedRestartDelayMs = preferences.getULong(configRestartDelayMsKey, defaultRestartDelayMs);
  uint32_t storedNoSuccessPingTimeMs = preferences.getULong(configNoSuccessPingTimeMsKey, defaultNoSuccessPingTimeMs);
  uint32_t storedMinFailedPings = preferences.getULong(configMinFailedPingsKey, defaultMinFailedPings);
  bool storedAutoRestartEnabled = preferences.getBool(configAutoRestartEnabledKey, defaultAutoRestartEnabled);
  preferences.end();

  xSemaphoreTake(configMutex, portMAX_DELAY);
  config.restartDelayMs = clampRestartDelayMs(storedRestartDelayMs);
  config.noSuccessPingTimeMs = clampNoSuccessPingTimeMs(storedNoSuccessPingTimeMs);
  config.minFailedPings = clampMinFailedPings(storedMinFailedPings);
  config.autoRestartEnabled = storedAutoRestartEnabled;
  xSemaphoreGive(configMutex);
}

void saveConfig() {
  xSemaphoreTake(configMutex, portMAX_DELAY);
  uint32_t restartDelayMs = config.restartDelayMs;
  uint32_t noSuccessPingTimeMs = config.noSuccessPingTimeMs;
  uint32_t minFailedPings = config.minFailedPings;
  bool autoRestartEnabled = config.autoRestartEnabled;
  xSemaphoreGive(configMutex);

  preferences.begin(configNamespace, false);
  preferences.putULong(configRestartDelayMsKey, restartDelayMs);
  preferences.putULong(configNoSuccessPingTimeMsKey, noSuccessPingTimeMs);
  preferences.putULong(configMinFailedPingsKey, minFailedPings);
  preferences.putBool(configAutoRestartEnabledKey, autoRestartEnabled);
  preferences.end();
}

void updateConfig(uint32_t restartDelayMs, uint32_t noSuccessPingTimeMs, uint32_t minFailedPings,
                  bool autoRestartEnabled) {
  xSemaphoreTake(configMutex, portMAX_DELAY);
  config.restartDelayMs = clampRestartDelayMs(restartDelayMs);
  config.noSuccessPingTimeMs = clampNoSuccessPingTimeMs(noSuccessPingTimeMs);
  config.minFailedPings = clampMinFailedPings(minFailedPings);
  config.autoRestartEnabled = autoRestartEnabled;
  xSemaphoreGive(configMutex);
  saveConfig();
}

String configJson() {
  uint32_t restartDelayMs = getRestartDelayMs();
  uint32_t noSuccessPingTimeMs = getNoSuccessPingTimeMs();
  uint32_t minFailedPings = getMinFailedPings();
  bool autoRestartEnabled = getAutoRestartEnabled();
  String json = "{";
  json += "\"restartDelayMs\":";
  json += String(restartDelayMs);
  json += ",";
  json += "\"restartDelayMinutes\":";
  json += String(restartDelayMs / 60000UL);
  json += ",";
  json += "\"noSuccessPingTimeMs\":";
  json += String(noSuccessPingTimeMs);
  json += ",";
  json += "\"noSuccessPingTimeMinutes\":";
  json += String(noSuccessPingTimeMs / 60000UL);
  json += ",";
  json += "\"minFailedPings\":";
  json += String(minFailedPings);
  json += ",";
  json += "\"autoRestartEnabled\":";
  json += autoRestartEnabled ? "true" : "false";
  json += "}";
  return json;
}

bool parseJsonUint32Field(const String& body, const char* key, uint32_t* outValue) {
  String needle = "\"" + String(key) + "\"";
  int keyPos = body.indexOf(needle);
  if (keyPos < 0) {
    return false;
  }

  int colonPos = body.indexOf(':', keyPos + needle.length());
  if (colonPos < 0) {
    return false;
  }

  int i = colonPos + 1;
  while (i < body.length() && isspace(static_cast<unsigned char>(body[i]))) {
    i++;
  }

  bool quoted = false;
  if (i < body.length() && body[i] == '"') {
    quoted = true;
    i++;
  }

  if (i >= body.length() || !isdigit(static_cast<unsigned char>(body[i]))) {
    return false;
  }

  uint64_t value = 0;
  while (i < body.length() && isdigit(static_cast<unsigned char>(body[i]))) {
    value = value * 10 + static_cast<uint64_t>(body[i] - '0');
    if (value > UINT32_MAX) {
      return false;
    }
    i++;
  }

  if (quoted) {
    if (i >= body.length() || body[i] != '"') {
      return false;
    }
  }

  *outValue = static_cast<uint32_t>(value);
  return true;
}

bool parseJsonBoolField(const String& body, const char* key, bool* outValue) {
  String needle = "\"" + String(key) + "\"";
  int keyPos = body.indexOf(needle);
  if (keyPos < 0) {
    return false;
  }

  int colonPos = body.indexOf(':', keyPos + needle.length());
  if (colonPos < 0) {
    return false;
  }

  int i = colonPos + 1;
  while (i < body.length() && isspace(static_cast<unsigned char>(body[i]))) {
    i++;
  }

  bool quoted = false;
  if (i < body.length() && body[i] == '"') {
    quoted = true;
    i++;
  }

  if (i + 4 <= body.length() && body.substring(i, i + 4) == "true") {
    i += 4;
    if (quoted) {
      if (i >= body.length() || body[i] != '"') {
        return false;
      }
    } else if (i < body.length() && body[i] != ',' && body[i] != '}' &&
               !isspace(static_cast<unsigned char>(body[i]))) {
      return false;
    }
    *outValue = true;
    return true;
  }

  if (i + 5 <= body.length() && body.substring(i, i + 5) == "false") {
    i += 5;
    if (quoted) {
      if (i >= body.length() || body[i] != '"') {
        return false;
      }
    } else if (i < body.length() && body[i] != ',' && body[i] != '}' &&
               !isspace(static_cast<unsigned char>(body[i]))) {
      return false;
    }
    *outValue = false;
    return true;
  }

  return false;
}

void advanceHistoryToNowLocked(ulong now) {
  uint32_t newBucketId = now / historyBucketMs;

  if (!historyInitialized) {
    memset(historyBuckets, 0, sizeof(historyBuckets));
    historyCurrentBucketId = newBucketId;
    historyInitialized = true;
    return;
  }

  if (newBucketId == historyCurrentBucketId) {
    return;
  }

  uint32_t delta = newBucketId - historyCurrentBucketId;
  if (delta >= historyBucketsCount) {
    memset(historyBuckets, 0, sizeof(historyBuckets));
  } else {
    for (uint32_t i = 1; i <= delta; i++) {
      historyBuckets[(historyCurrentBucketId + i) % historyBucketsCount] = 0;
    }
  }

  historyCurrentBucketId = newBucketId;
}

void markHistoryEvent(ulong now, uint8_t eventFlag) {
  xSemaphoreTake(statusMutex, portMAX_DELAY);
  advanceHistoryToNowLocked(now);
  historyBuckets[historyCurrentBucketId % historyBucketsCount] |= eventFlag;
  xSemaphoreGive(statusMutex);
}

bool ensureWifiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  Serial.println("WiFi disconnected, attempting reconnect...");

  WiFi.mode(WIFI_STA);

  // Reuse stored STA credentials/session first.
  bool reconnectStarted = WiFi.reconnect();
  if (!reconnectStarted) {
    // Fallback for cases where reconnect is not available yet.
    WiFi.begin(ssid, password);
  }

  ulong reconnectStart = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - reconnectStart) < wifiReconnectTimeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(wifiReconnectStepMs));
    Serial.print("~");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi reconnected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("\nWiFi reconnect failed, skipping ping check.");
  markHistoryEvent(millis(), HISTORY_WIFI_FAILED);
  return false;
}

void forceReboot() {
  uint32_t restartDelayMs = getRestartDelayMs();

  Serial.println("Forcing reboot...");
  
  digitalWrite(PIN, HIGH);
  delay(4000);
  digitalWrite(PIN, LOW);
  
  xSemaphoreTake(statusMutex, portMAX_DELAY);
  status.lastRestart = millis();
  status.status = "WAIT";
  status.waitUntil = millis() + restartDelayMs;
  status.failedPings = 0;
  xSemaphoreGive(statusMutex);

  markHistoryEvent(millis(), HISTORY_RESTART);

  Serial.println("Reboot forced.");
}

void runCheck() {
  uint32_t restartDelayMs = getRestartDelayMs();
  uint32_t noSuccessPingTimeMs = getNoSuccessPingTimeMs();
  uint32_t minFailedPings = getMinFailedPings();
  bool autoRestartEnabled = getAutoRestartEnabled();

  xSemaphoreTake(statusMutex, portMAX_DELAY);
  ulong waitUntil = status.waitUntil;
  xSemaphoreGive(statusMutex);

  if (waitUntil != 0 && millis() < waitUntil) {
    xSemaphoreTake(statusMutex, portMAX_DELAY);
    status.status = "WAIT";
    xSemaphoreGive(statusMutex);
    return;
  }

  if (!ensureWifiConnected()) {
    xSemaphoreTake(statusMutex, portMAX_DELAY);
    status.status = "WAIT";
    status.waitUntil = 0;
    xSemaphoreGive(statusMutex);
    return;
  }

  Serial.print("Pinging ");
  Serial.println(watchdogHost);

  bool ok = Ping.ping(watchdogHost, 5);
  
  if (ok) {
    Serial.println("Ping OK");
    xSemaphoreTake(statusMutex, portMAX_DELAY);
    status.status = "OK";
    status.lastUpdate = millis();
    status.waitUntil = 0;
    status.failedPings = 0;
    xSemaphoreGive(statusMutex);
    markHistoryEvent(millis(), HISTORY_OK);
  } else {
    Serial.println("Ping FAILED");
    
    xSemaphoreTake(statusMutex, portMAX_DELAY);
    status.status = "FAILED";
    status.waitUntil = millis() + restartDelayMs;
    status.failedPings++;
    uint failedPings = status.failedPings;
    ulong lastSuccess = status.lastUpdate;
    ulong lastRestart = status.lastRestart;
    xSemaphoreGive(statusMutex);

    markHistoryEvent(millis(), HISTORY_FAILED);

    ulong rebootGraceUntil = lastRestart == 0 ? 0 : (lastRestart + restartDelayMs);
    ulong noSuccessReference = lastSuccess > rebootGraceUntil ? lastSuccess : rebootGraceUntil;
    bool noSuccessTooLong =
        (noSuccessReference == 0) || (millis() - noSuccessReference >= noSuccessPingTimeMs);
    bool restartDelayPassed = millis() - lastRestart > restartDelayMs;

    if (noSuccessTooLong && restartDelayPassed && failedPings >= minFailedPings && autoRestartEnabled) {
      Serial.println("No successful ping for too long, forcing reboot.");
      forceReboot();
    } else if (noSuccessTooLong && restartDelayPassed && failedPings >= minFailedPings &&
               !autoRestartEnabled) {
      Serial.println("Auto-restart disabled, skipping reboot.");
    }
  }
}

void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Version 0.1");

  int timeout = 100;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");    
    if (timeout-- <= 0) ESP.restart();
  }

  Serial.println("\nWiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}


void pollingTask(void *parameter)
{
  while (true) {
    runCheck();    
    vTaskDelay(pdMS_TO_TICKS(sleepTime));
  }
}

/// ---- Main

void setup() {
  Serial.begin(115200);
  pinMode(PIN, OUTPUT);
  digitalWrite(PIN, LOW);
  
  randomSeed(analogRead(0));
  statusMutex = xSemaphoreCreateMutex();
  configMutex = xSemaphoreCreateMutex();

  loadConfig();

  wifiConnect();

  xSemaphoreTake(statusMutex, portMAX_DELAY);
  status.status = "WAIT";
  status.waitUntil = millis() + getRestartDelayMs();
  advanceHistoryToNowLocked(millis());
  xSemaphoreGive(statusMutex);

  // Root page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) { 
    request->send(200,"text/html", htmlPage); 
  });

  server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest *request) {
    forceReboot();
    request->send(200, "application/json", "{\"ok\":true,\"message\":\"Reboot forced.\"}");
  });

  server.on(
      "/config",
      HTTP_POST,
      [](AsyncWebServerRequest *request) {},
      NULL,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (index == 0) {
          request->_tempObject = new String();
          static_cast<String*>(request->_tempObject)->reserve(total);
        }

        String* body = static_cast<String*>(request->_tempObject);
        body->concat(reinterpret_cast<const char*>(data), len);

        if ((index + len) != total) {
          return;
        }

        uint32_t restartDelayMs = 0;
        uint32_t restartDelayMinutes = 0;
        uint32_t noSuccessPingTimeMs = 0;
        uint32_t noSuccessPingTimeMinutes = 0;
        uint32_t minFailedPings = 0;
        bool autoRestartEnabled = false;
        bool hasRestartDelayMs = parseJsonUint32Field(*body, "restartDelayMs", &restartDelayMs);
        bool hasRestartDelayMinutes = parseJsonUint32Field(*body, "restartDelayMinutes", &restartDelayMinutes);
        bool hasNoSuccessPingTimeMs = parseJsonUint32Field(*body, "noSuccessPingTimeMs", &noSuccessPingTimeMs);
        bool hasNoSuccessPingTimeMinutes =
            parseJsonUint32Field(*body, "noSuccessPingTimeMinutes", &noSuccessPingTimeMinutes);
        bool hasMinFailedPings = parseJsonUint32Field(*body, "minFailedPings", &minFailedPings);
        bool hasAutoRestartEnabled =
            parseJsonBoolField(*body, "autoRestartEnabled", &autoRestartEnabled);

        delete body;
        request->_tempObject = nullptr;

        if (!hasRestartDelayMs && !hasRestartDelayMinutes && !hasNoSuccessPingTimeMs &&
            !hasNoSuccessPingTimeMinutes && !hasMinFailedPings && !hasAutoRestartEnabled) {
          request->send(400, "application/json",
                        "{\"ok\":false,\"error\":\"Missing config fields\"}");
          return;
        }

        uint32_t nextRestartDelayMs = getRestartDelayMs();
        uint32_t nextNoSuccessPingTimeMs = getNoSuccessPingTimeMs();
        uint32_t nextMinFailedPings = getMinFailedPings();
        bool nextAutoRestartEnabled = getAutoRestartEnabled();

        if (hasRestartDelayMinutes || hasRestartDelayMs) {
          nextRestartDelayMs = hasRestartDelayMinutes
                                   ? minutesToMs(restartDelayMinutes)
                                   : clampRestartDelayMs(restartDelayMs);
        }

        if (hasNoSuccessPingTimeMinutes || hasNoSuccessPingTimeMs) {
          nextNoSuccessPingTimeMs = hasNoSuccessPingTimeMinutes
                                        ? noSuccessMinutesToMs(noSuccessPingTimeMinutes)
                                        : clampNoSuccessPingTimeMs(noSuccessPingTimeMs);
        }

        if (hasMinFailedPings) {
          nextMinFailedPings = clampMinFailedPings(minFailedPings);
        }

        if (hasAutoRestartEnabled) {
          nextAutoRestartEnabled = autoRestartEnabled;
        }

        if (hasRestartDelayMs && hasRestartDelayMinutes) {
          uint32_t fromMinutes = minutesToMs(restartDelayMinutes);
          if (fromMinutes != clampRestartDelayMs(restartDelayMs)) {
            request->send(400, "application/json",
                          "{\"ok\":false,\"error\":\"restartDelayMs and restartDelayMinutes mismatch\"}");
            return;
          }
        }

        if (hasNoSuccessPingTimeMs && hasNoSuccessPingTimeMinutes) {
          uint32_t fromMinutes = noSuccessMinutesToMs(noSuccessPingTimeMinutes);
          if (fromMinutes != clampNoSuccessPingTimeMs(noSuccessPingTimeMs)) {
            request->send(400, "application/json",
                          "{\"ok\":false,\"error\":\"noSuccessPingTimeMs and noSuccessPingTimeMinutes mismatch\"}");
            return;
          }
        }

        updateConfig(nextRestartDelayMs, nextNoSuccessPingTimeMs, nextMinFailedPings,
                     nextAutoRestartEnabled);

        String response = "{";
        response += "\"ok\":true,";
        response += "\"config\":";
        response += configJson();
        response += "}";
        request->send(200, "application/json", response);
      });

  // JSON status endpoint
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    DeviceStatus copy;
    uint8_t historyCopy[historyBucketsCount];
    uint32_t historyCurrentIdCopy;
    ulong now = millis();

    xSemaphoreTake(statusMutex, portMAX_DELAY);
    copy = status;
    advanceHistoryToNowLocked(now);
    memcpy(historyCopy, historyBuckets, sizeof(historyBuckets));
    historyCurrentIdCopy = historyCurrentBucketId;
    xSemaphoreGive(statusMutex);

    ulong waitRemaining = 0;
    if (copy.waitUntil != 0 && copy.waitUntil > now) {
      waitRemaining = copy.waitUntil - now;
    }

    String json = "{";
    json += "\"status\":\"";
    json += copy.status;
    json += "\"";
    json += ",";
    json += "\"waitRemaining\":";
    json += String(waitRemaining);
    json += ",";
    json += "\"failedPings\":";
    json += String(copy.failedPings);
    json += ",";
    json += "\"lastUpdate\":";
    json += String(now - copy.lastUpdate);
    json += ",";
    json += "\"lastRestart\":";
    json += String(copy.lastRestart == 0 ? 0 : (now - copy.lastRestart));
    json += ",";
    json += "\"uptime\":";
    json += String(now);
    json += ",";
    json += "\"bucketMs\":";
    json += String(historyBucketMs);
    json += ",";
    json += "\"config\":";
    json += configJson();
    json += ",";
    json += "\"history\":[";

    uint32_t oldestBucketId = historyCurrentIdCopy - (historyBucketsCount - 1);
    uint32_t bucketsSinceBoot = now / historyBucketMs;
    for (uint8_t i = 0; i < historyBucketsCount; i++) {
      uint32_t ageBuckets = (historyBucketsCount - 1) - i;
      uint8_t rawFlags = 0;
      if (ageBuckets <= bucketsSinceBoot) {
        rawFlags = historyCopy[(oldestBucketId + i) % historyBucketsCount];
      }
      json += String(rawFlags);
      if (i + 1 < historyBucketsCount) {
        json += ",";
      }
    }

    json += "]";
    json += "}";

    request->send(200, "application/json", json);
  });

  server.begin();

  // Start polling task
  xTaskCreate(pollingTask, "Polling", 4096, NULL, 1, &pollingTaskHandle);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
