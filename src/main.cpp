#include <Arduino.h>
#include <WiFi.h>
#include <ESP32Ping.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <string.h>
#include "esp_sleep.h"
#include "secrets.h"
#include "web_page.h"

const uint32_t sleepTime = 20000; // 20 seconds
const uint32_t historyBucketMs = 10UL * 60UL * 1000UL; // 10 minutes
const uint8_t historyBucketsCount = 144; // 24h / 10m
const uint32_t wifiReconnectTimeoutMs = 10000; // 10 seconds
const uint32_t wifiReconnectStepMs = 500;
const uint minFailedPings = 10;
const uint minRestartDelay = 3*60000; // 3 mins
const uint minNoSuccessPingTime = 5*60000; // 5 mins without successful ping

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
  Serial.println("Forcing reboot...");
  
  digitalWrite(PIN, HIGH);
  delay(4000);
  digitalWrite(PIN, LOW);
  
  xSemaphoreTake(statusMutex, portMAX_DELAY);
  status.lastRestart = millis();
  status.status = "WAIT";
  status.waitUntil = millis() + minRestartDelay;
  status.failedPings = 0;
  xSemaphoreGive(statusMutex);

  markHistoryEvent(millis(), HISTORY_RESTART);

  Serial.println("Reboot forced.");
}

void runCheck() {
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
    status.waitUntil = millis() + minRestartDelay;
    status.failedPings++;
    ulong lastSuccess = status.lastUpdate;
    ulong lastRestart = status.lastRestart;
    xSemaphoreGive(statusMutex);

    markHistoryEvent(millis(), HISTORY_FAILED);

    bool noSuccessTooLong = (lastSuccess == 0) || (millis() - lastSuccess >= minNoSuccessPingTime);
    bool restartDelayPassed = millis() - lastRestart > minRestartDelay;

    if (noSuccessTooLong && restartDelayPassed) {
      Serial.println("No successful ping for too long, forcing reboot.");
      forceReboot();
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

  wifiConnect();

  xSemaphoreTake(statusMutex, portMAX_DELAY);
  status.status = "WAIT";
  status.waitUntil = millis() + minRestartDelay;
  advanceHistoryToNowLocked(millis());
  xSemaphoreGive(statusMutex);

  // Root page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) { 
    request->send(200,"text/html", htmlPage); 
  });

  server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest *request) {
    forceReboot();
    request->send(200, "text/plain", "Reboot forced.");
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

