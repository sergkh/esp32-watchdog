#include <Arduino.h>
#include <WiFi.h>
#include <ESP32Ping.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <string.h>
#include "esp_sleep.h"
#include "secrets.h"
#include "web_page.h"
#include "json_utils.h"
#include "logger.h"
#include "config.h"

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
const uint32_t defaultRestartDelayMs = 5UL * 60000UL; // 5 mins
const uint32_t maxRestartDelayMs = 24UL * 60UL * 60000UL; // 24h
const uint32_t defaultNoSuccessPingTimeMs = 5UL * 60000UL; // 5 mins without successful ping
const uint32_t maxNoSuccessPingTimeMs = 24UL * 60UL * 60000UL; // 24h
const uint32_t defaultMinFailedPings = 10;
const uint32_t maxMinFailedPings = 1000;
const bool defaultAutoRestartEnabled = true;
const uint32_t restartSleepMs = 8000;

const char* configNamespace = "watchdog";
const char* configRestartDelayMsKey = "restartDelayMs";
const char* configNoSuccessPingTimeMsKey = "noSuccessPingMs";
const char* configMinFailedPingsKey = "minFailedPings";
const char* configAutoRestartEnabledKey = "autoRestart";

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const int PIN = 14;

struct DeviceStatus {
  String status = "WAIT";
  ulong lastUpdate = 0;
  ulong lastRestart = 0;
  ulong waitUntil = 0;
  uint failedPings = 0;
};

enum HistoryEventFlag : uint8_t {
  HISTORY_OK = 1,
  HISTORY_FAILED = 2,
  HISTORY_RESTART = 4,
  HISTORY_WIFI_FAILED = 8
};

DeviceStatus status;
SemaphoreHandle_t statusMutex;
uint8_t historyBuckets[historyBucketsCount];
uint32_t historyCurrentBucketId = 0;
bool historyInitialized = false;

// --- Web part

AsyncWebServer server(80);
TaskHandle_t pollingTaskHandle;

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

  logWarn("Forcing reboot (pin HIGH for " + String(restartSleepMs) + "ms)...");
  
  digitalWrite(PIN, HIGH);
  delay(restartSleepMs);
  digitalWrite(PIN, LOW);
  
  xSemaphoreTake(statusMutex, portMAX_DELAY);
  String prevStatus = status.status;
  status.lastRestart = millis();
  status.status = "WAIT";
  status.waitUntil = millis() + restartDelayMs;
  status.failedPings = 0;
  xSemaphoreGive(statusMutex);

  if (prevStatus != "WAIT") {
    logInfo("Status change: " + prevStatus + " -> WAIT (after forced reboot)");
  }
  logInfo("Next check deferred for " + String(restartDelayMs) + "ms after reboot.");

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
    ulong remaining = waitUntil - millis();
    logInfo("Decision: skip ping, still in WAIT for " + String(remaining) + "ms");
    xSemaphoreTake(statusMutex, portMAX_DELAY);
    String prevStatus = status.status;
    status.status = "WAIT";
    xSemaphoreGive(statusMutex);
    if (prevStatus != "WAIT") {
      logInfo("Status change: " + prevStatus + " -> WAIT (cooldown active)");
    }
    return;
  }

  if (!ensureWifiConnected()) {
    xSemaphoreTake(statusMutex, portMAX_DELAY);
    String prevStatus = status.status;
    status.status = "WAIT";
    status.waitUntil = 0;
    xSemaphoreGive(statusMutex);
    logWarn("Decision: skip ping because WiFi is not connected.");
    if (prevStatus != "WAIT") {
      logInfo("Status change: " + prevStatus + " -> WAIT (wifi unavailable)");
    }
    return;
  }

  String monitoredHost = getMonitoredHost();

  bool ok = Ping.ping(monitoredHost.c_str(), 5);
  
  if (ok) {
    logInfo("Ping of " + monitoredHost + " OK");
    xSemaphoreTake(statusMutex, portMAX_DELAY);
    String prevStatus = status.status;
    status.status = "OK";
    status.lastUpdate = millis();
    status.waitUntil = 0;
    status.failedPings = 0;
    xSemaphoreGive(statusMutex);
    if (prevStatus != "OK") {
      logInfo("Status change: " + prevStatus + " -> OK");
    }
    markHistoryEvent(millis(), HISTORY_OK);
  } else {
    logWarn("Ping of " + monitoredHost + " FAILED");

    xSemaphoreTake(statusMutex, portMAX_DELAY);
    String prevStatus = status.status;
    status.status = "FAILED";
    status.waitUntil = millis() + restartDelayMs;
    status.failedPings++;
    uint failedPings = status.failedPings;
    ulong lastSuccess = status.lastUpdate;
    ulong lastRestart = status.lastRestart;
    xSemaphoreGive(statusMutex);

    if (prevStatus != "FAILED") {
      logInfo("Status change: " + prevStatus + " -> FAILED");
    }

    logInfo("Failure counters: failedPings=" + String(failedPings) +
            ", minFailedPings=" + String(minFailedPings) +
            ", noSuccessWindowMs=" + String(noSuccessPingTimeMs));

    markHistoryEvent(millis(), HISTORY_FAILED);

    ulong rebootGraceUntil = lastRestart == 0 ? 0 : (lastRestart + restartDelayMs);
    ulong noSuccessReference = lastSuccess > rebootGraceUntil ? lastSuccess : rebootGraceUntil;
    bool noSuccessTooLong =
        (noSuccessReference == 0) || (millis() - noSuccessReference >= noSuccessPingTimeMs);
    bool restartDelayPassed = millis() - lastRestart > restartDelayMs;

    logInfo("Decision factors: noSuccessTooLong=" + String(noSuccessTooLong ? "true" : "false") +
            ", restartDelayPassed=" + String(restartDelayPassed ? "true" : "false") +
            ", autoRestartEnabled=" + String(autoRestartEnabled ? "true" : "false"));

    if (noSuccessTooLong && restartDelayPassed && failedPings >= minFailedPings && autoRestartEnabled) {
      logWarn("No successful ping for too long, forcing reboot.");
      forceReboot();
    } else if (noSuccessTooLong && restartDelayPassed && failedPings >= minFailedPings &&
               !autoRestartEnabled) {
      logWarn("Auto-restart disabled, skipping reboot.");
    } else {
      logInfo("Decision: keep waiting, reboot conditions are not met yet.");
    }
  }
}

void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int timeout = 100;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    if (timeout-- <= 0) {
      logError("WiFi connect timeout, restarting ESP.");
      ESP.restart();
    }
  }

  logInfo("Version 0.4. WiFi connected, IP address: " + WiFi.localIP().toString());
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
  initAppLogger(BETTERSTACK_INGESTING_HOST, BETTERSTACK_SOURCE_TOKEN, "watchdog");
  pinMode(PIN, OUTPUT);
  digitalWrite(PIN, LOW);
  
  randomSeed(analogRead(0));
  statusMutex = xSemaphoreCreateMutex();
  initConfigManager(MONITORED_HOST);
  logInfo("Config loaded: " + configJson());

  wifiConnect();

  xSemaphoreTake(statusMutex, portMAX_DELAY);
  status.status = "WAIT";
  status.waitUntil = millis() + getRestartDelayMs();
  advanceHistoryToNowLocked(millis());
  xSemaphoreGive(statusMutex);
  logInfo("Initial status set to WAIT, first check in " + String(getRestartDelayMs()) + "ms");

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
          logWarn("Config update rejected: no supported fields in payload.");
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
            logWarn("Config update rejected: restartDelayMs and restartDelayMinutes mismatch.");
            request->send(400, "application/json",
                          "{\"ok\":false,\"error\":\"restartDelayMs and restartDelayMinutes mismatch\"}");
            return;
          }
        }

        if (hasNoSuccessPingTimeMs && hasNoSuccessPingTimeMinutes) {
          uint32_t fromMinutes = noSuccessMinutesToMs(noSuccessPingTimeMinutes);
          if (fromMinutes != clampNoSuccessPingTimeMs(noSuccessPingTimeMs)) {
            logWarn("Config update rejected: noSuccessPingTimeMs and noSuccessPingTimeMinutes mismatch.");
            request->send(400, "application/json",
                          "{\"ok\":false,\"error\":\"noSuccessPingTimeMs and noSuccessPingTimeMinutes mismatch\"}");
            return;
          }
        }

        updateConfig(nextRestartDelayMs, nextNoSuccessPingTimeMs, nextMinFailedPings,
                     nextAutoRestartEnabled, nextMonitoredHost);
        logInfo("Config updated: " + configJson());

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
