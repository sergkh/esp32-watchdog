#include <Arduino.h>
#include <WiFi.h>
#include <ESP32Ping.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <string.h>
#include "esp_sleep.h"
#include "secrets.h"
#include "web_page.h"
#include "json_utils.h"
#include "app_logger.h"

#ifndef BETTERSTACK_INGESTING_HOST
#define BETTERSTACK_INGESTING_HOST ""
#endif

#ifndef BETTERSTACK_SOURCE_TOKEN
#define BETTERSTACK_SOURCE_TOKEN ""
#endif

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
const char* configMonitoredHostKey = "monitoredHost";
const size_t maxMonitoredHostLength = 255;

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* defaultMonitoredHost = MONITORED_HOST;
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
  String monitoredHost = defaultMonitoredHost;
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

String sanitizeMonitoredHost(const String& value) {
  String sanitized = value;
  sanitized.trim();
  if (sanitized.length() == 0) {
    return String(defaultMonitoredHost);
  }
  if (sanitized.length() > maxMonitoredHostLength) {
    sanitized = sanitized.substring(0, maxMonitoredHostLength);
  }
  return sanitized;
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

String getMonitoredHost() {
  xSemaphoreTake(configMutex, portMAX_DELAY);
  String value = config.monitoredHost;
  xSemaphoreGive(configMutex);
  return value;
}

void loadConfig() {
  preferences.begin(configNamespace, true);
  uint32_t storedRestartDelayMs = preferences.getULong(configRestartDelayMsKey, defaultRestartDelayMs);
  uint32_t storedNoSuccessPingTimeMs = preferences.getULong(configNoSuccessPingTimeMsKey, defaultNoSuccessPingTimeMs);
  uint32_t storedMinFailedPings = preferences.getULong(configMinFailedPingsKey, defaultMinFailedPings);
  bool storedAutoRestartEnabled = preferences.getBool(configAutoRestartEnabledKey, defaultAutoRestartEnabled);
  String storedMonitoredHost = preferences.getString(configMonitoredHostKey, defaultMonitoredHost);
  preferences.end();

  xSemaphoreTake(configMutex, portMAX_DELAY);
  config.restartDelayMs = clampRestartDelayMs(storedRestartDelayMs);
  config.noSuccessPingTimeMs = clampNoSuccessPingTimeMs(storedNoSuccessPingTimeMs);
  config.minFailedPings = clampMinFailedPings(storedMinFailedPings);
  config.autoRestartEnabled = storedAutoRestartEnabled;
  config.monitoredHost = sanitizeMonitoredHost(storedMonitoredHost);
  xSemaphoreGive(configMutex);
}

void saveConfig() {
  xSemaphoreTake(configMutex, portMAX_DELAY);
  uint32_t restartDelayMs = config.restartDelayMs;
  uint32_t noSuccessPingTimeMs = config.noSuccessPingTimeMs;
  uint32_t minFailedPings = config.minFailedPings;
  bool autoRestartEnabled = config.autoRestartEnabled;
  String monitoredHost = config.monitoredHost;
  xSemaphoreGive(configMutex);

  preferences.begin(configNamespace, false);
  preferences.putULong(configRestartDelayMsKey, restartDelayMs);
  preferences.putULong(configNoSuccessPingTimeMsKey, noSuccessPingTimeMs);
  preferences.putULong(configMinFailedPingsKey, minFailedPings);
  preferences.putBool(configAutoRestartEnabledKey, autoRestartEnabled);
  preferences.putString(configMonitoredHostKey, monitoredHost);
  preferences.end();
}

void updateConfig(uint32_t restartDelayMs, uint32_t noSuccessPingTimeMs, uint32_t minFailedPings,
                  bool autoRestartEnabled, const String& monitoredHost) {
  xSemaphoreTake(configMutex, portMAX_DELAY);
  config.restartDelayMs = clampRestartDelayMs(restartDelayMs);
  config.noSuccessPingTimeMs = clampNoSuccessPingTimeMs(noSuccessPingTimeMs);
  config.minFailedPings = clampMinFailedPings(minFailedPings);
  config.autoRestartEnabled = autoRestartEnabled;
  config.monitoredHost = sanitizeMonitoredHost(monitoredHost);
  xSemaphoreGive(configMutex);
  saveConfig();
}

String configJson() {
  uint32_t restartDelayMs = getRestartDelayMs();
  uint32_t noSuccessPingTimeMs = getNoSuccessPingTimeMs();
  uint32_t minFailedPings = getMinFailedPings();
  bool autoRestartEnabled = getAutoRestartEnabled();
  String monitoredHost = getMonitoredHost();
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
  json += ",";
  json += "\"monitoredHost\":\"";
  json += escapeJsonString(monitoredHost);
  json += "\"";
  json += "}";
  return json;
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

  logWarn("WiFi disconnected, attempting reconnect...");

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
  }

  if (WiFi.status() == WL_CONNECTED) {
    logInfo("WiFi reconnected, IP address: " + WiFi.localIP().toString());
    return true;
  }

  logWarn("WiFi reconnect failed, skipping ping check.");
  markHistoryEvent(millis(), HISTORY_WIFI_FAILED);
  return false;
}

void forceReboot() {
  uint32_t restartDelayMs = getRestartDelayMs();

  logWarn("Forcing reboot...");
  
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

  logWarn("Reboot forced.");
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

  String monitoredHost = getMonitoredHost();
  logInfo("Pinging " + monitoredHost);

  bool ok = Ping.ping(monitoredHost.c_str(), 5);
  
  if (ok) {
    logInfo("Ping OK");
    xSemaphoreTake(statusMutex, portMAX_DELAY);
    status.status = "OK";
    status.lastUpdate = millis();
    status.waitUntil = 0;
    status.failedPings = 0;
    xSemaphoreGive(statusMutex);
    markHistoryEvent(millis(), HISTORY_OK);
  } else {
    logWarn("Ping FAILED");
    
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
      logWarn("No successful ping for too long, forcing reboot.");
      forceReboot();
    } else if (noSuccessTooLong && restartDelayPassed && failedPings >= minFailedPings &&
               !autoRestartEnabled) {
      logWarn("Auto-restart disabled, skipping reboot.");
    }
  }
}

void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  logInfo("Version 0.1 booting");

  int timeout = 100;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    if (timeout-- <= 0) {
      logError("WiFi connect timeout, restarting ESP.");
      ESP.restart();
    }
  }

  logInfo("WiFi connected, IP address: " + WiFi.localIP().toString());
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
  initAppLogger(BETTERSTACK_INGESTING_HOST, BETTERSTACK_SOURCE_TOKEN, "esp32-watchdog");
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
        String monitoredHost;
        bool hasRestartDelayMs = parseJsonUint32Field(*body, "restartDelayMs", &restartDelayMs);
        bool hasRestartDelayMinutes = parseJsonUint32Field(*body, "restartDelayMinutes", &restartDelayMinutes);
        bool hasNoSuccessPingTimeMs = parseJsonUint32Field(*body, "noSuccessPingTimeMs", &noSuccessPingTimeMs);
        bool hasNoSuccessPingTimeMinutes =
            parseJsonUint32Field(*body, "noSuccessPingTimeMinutes", &noSuccessPingTimeMinutes);
        bool hasMinFailedPings = parseJsonUint32Field(*body, "minFailedPings", &minFailedPings);
        bool hasAutoRestartEnabled =
            parseJsonBoolField(*body, "autoRestartEnabled", &autoRestartEnabled);
        bool hasMonitoredHost = parseJsonStringField(*body, "monitoredHost", &monitoredHost);

        delete body;
        request->_tempObject = nullptr;

        if (!hasRestartDelayMs && !hasRestartDelayMinutes && !hasNoSuccessPingTimeMs &&
            !hasNoSuccessPingTimeMinutes && !hasMinFailedPings && !hasAutoRestartEnabled &&
            !hasMonitoredHost) {
          request->send(400, "application/json",
                        "{\"ok\":false,\"error\":\"Missing config fields\"}");
          return;
        }

        uint32_t nextRestartDelayMs = getRestartDelayMs();
        uint32_t nextNoSuccessPingTimeMs = getNoSuccessPingTimeMs();
        uint32_t nextMinFailedPings = getMinFailedPings();
        bool nextAutoRestartEnabled = getAutoRestartEnabled();
        String nextMonitoredHost = getMonitoredHost();

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

        if (hasMonitoredHost) {
          nextMonitoredHost = sanitizeMonitoredHost(monitoredHost);
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
                     nextAutoRestartEnabled, nextMonitoredHost);

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
