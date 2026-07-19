#include "config.h"

#include <Preferences.h>

#include "json_utils.h"

namespace {

const uint32_t kDefaultRestartDelayMs = 3UL * 60000UL;  // 3 mins
const uint32_t kMaxRestartDelayMs = 24UL * 60UL * 60000UL;  // 24h
const uint32_t kDefaultNoSuccessPingTimeMs = 5UL * 60000UL;  // 5 mins without successful ping
const uint32_t kMaxNoSuccessPingTimeMs = 24UL * 60UL * 60000UL;  // 24h
const uint32_t kDefaultMinFailedPings = 10;
const uint32_t kMaxMinFailedPings = 1000;
const bool kDefaultAutoRestartEnabled = true;

const char* kConfigNamespace = "watchdog";
const char* kConfigRestartDelayMsKey = "restartDelayMs";
const char* kConfigNoSuccessPingTimeMsKey = "noSuccessPingTimeMs";
const char* kConfigMinFailedPingsKey = "minFailedPings";
const char* kConfigAutoRestartEnabledKey = "autoRestartEnabled";
const char* kConfigMonitoredHostKey = "monitoredHost";
const size_t kMaxMonitoredHostLength = 255;

struct AppConfig {
  uint32_t restartDelayMs = kDefaultRestartDelayMs;
  uint32_t noSuccessPingTimeMs = kDefaultNoSuccessPingTimeMs;
  uint32_t minFailedPings = kDefaultMinFailedPings;
  bool autoRestartEnabled = kDefaultAutoRestartEnabled;
  String monitoredHost;
};

Preferences gPreferences;
SemaphoreHandle_t gConfigMutex;
AppConfig gConfig;
String gDefaultMonitoredHost;

void loadConfig() {
  gPreferences.begin(kConfigNamespace, true);
  uint32_t storedRestartDelayMs = gPreferences.getULong(kConfigRestartDelayMsKey, kDefaultRestartDelayMs);
  uint32_t storedNoSuccessPingTimeMs =
      gPreferences.getULong(kConfigNoSuccessPingTimeMsKey, kDefaultNoSuccessPingTimeMs);
  uint32_t storedMinFailedPings = gPreferences.getULong(kConfigMinFailedPingsKey, kDefaultMinFailedPings);
  bool storedAutoRestartEnabled =
      gPreferences.getBool(kConfigAutoRestartEnabledKey, kDefaultAutoRestartEnabled);
  String storedMonitoredHost = gPreferences.getString(kConfigMonitoredHostKey, gDefaultMonitoredHost);
  gPreferences.end();

  xSemaphoreTake(gConfigMutex, portMAX_DELAY);
  gConfig.restartDelayMs = clampRestartDelayMs(storedRestartDelayMs);
  gConfig.noSuccessPingTimeMs = clampNoSuccessPingTimeMs(storedNoSuccessPingTimeMs);
  gConfig.minFailedPings = clampMinFailedPings(storedMinFailedPings);
  gConfig.autoRestartEnabled = storedAutoRestartEnabled;
  gConfig.monitoredHost = sanitizeMonitoredHost(storedMonitoredHost);
  xSemaphoreGive(gConfigMutex);
}

void saveConfig() {
  xSemaphoreTake(gConfigMutex, portMAX_DELAY);
  uint32_t restartDelayMs = gConfig.restartDelayMs;
  uint32_t noSuccessPingTimeMs = gConfig.noSuccessPingTimeMs;
  uint32_t minFailedPings = gConfig.minFailedPings;
  bool autoRestartEnabled = gConfig.autoRestartEnabled;
  String monitoredHost = gConfig.monitoredHost;
  xSemaphoreGive(gConfigMutex);

  gPreferences.begin(kConfigNamespace, false);
  gPreferences.putULong(kConfigRestartDelayMsKey, restartDelayMs);
  gPreferences.putULong(kConfigNoSuccessPingTimeMsKey, noSuccessPingTimeMs);
  gPreferences.putULong(kConfigMinFailedPingsKey, minFailedPings);
  gPreferences.putBool(kConfigAutoRestartEnabledKey, autoRestartEnabled);
  gPreferences.putString(kConfigMonitoredHostKey, monitoredHost);
  gPreferences.end();
}

}  // namespace

void initConfigManager(const char* defaultMonitoredHost) {
  gConfigMutex = xSemaphoreCreateMutex();
  gDefaultMonitoredHost =
      (defaultMonitoredHost != nullptr && defaultMonitoredHost[0] != '\0') ? defaultMonitoredHost : "8.8.8.8";
  gConfig.monitoredHost = gDefaultMonitoredHost;
  loadConfig();
}

uint32_t clampRestartDelayMs(uint32_t value) {
  if (value > kMaxRestartDelayMs) {
    return kMaxRestartDelayMs;
  }
  return value;
}

uint32_t clampNoSuccessPingTimeMs(uint32_t value) {
  if (value > kMaxNoSuccessPingTimeMs) {
    return kMaxNoSuccessPingTimeMs;
  }
  return value;
}

uint32_t clampMinFailedPings(uint32_t value) {
  if (value < 1) {
    return 1;
  }
  if (value > kMaxMinFailedPings) {
    return kMaxMinFailedPings;
  }
  return value;
}

uint32_t minutesToMs(uint32_t minutes) {
  if (minutes > (kMaxRestartDelayMs / 60000UL)) {
    return kMaxRestartDelayMs;
  }
  return minutes * 60000UL;
}

uint32_t noSuccessMinutesToMs(uint32_t minutes) {
  if (minutes > (kMaxNoSuccessPingTimeMs / 60000UL)) {
    return kMaxNoSuccessPingTimeMs;
  }
  return minutes * 60000UL;
}

String sanitizeMonitoredHost(const String& value) {
  String sanitized = value;
  sanitized.trim();
  if (sanitized.length() == 0) {
    return String(gDefaultMonitoredHost);
  }
  if (sanitized.length() > kMaxMonitoredHostLength) {
    sanitized = sanitized.substring(0, kMaxMonitoredHostLength);
  }
  return sanitized;
}

uint32_t getRestartDelayMs() {
  xSemaphoreTake(gConfigMutex, portMAX_DELAY);
  uint32_t delay = gConfig.restartDelayMs;
  xSemaphoreGive(gConfigMutex);
  return delay;
}

uint32_t getNoSuccessPingTimeMs() {
  xSemaphoreTake(gConfigMutex, portMAX_DELAY);
  uint32_t value = gConfig.noSuccessPingTimeMs;
  xSemaphoreGive(gConfigMutex);
  return value;
}

uint32_t getMinFailedPings() {
  xSemaphoreTake(gConfigMutex, portMAX_DELAY);
  uint32_t value = gConfig.minFailedPings;
  xSemaphoreGive(gConfigMutex);
  return value;
}

bool getAutoRestartEnabled() {
  xSemaphoreTake(gConfigMutex, portMAX_DELAY);
  bool value = gConfig.autoRestartEnabled;
  xSemaphoreGive(gConfigMutex);
  return value;
}

String getMonitoredHost() {
  xSemaphoreTake(gConfigMutex, portMAX_DELAY);
  String value = gConfig.monitoredHost;
  xSemaphoreGive(gConfigMutex);
  return value;
}

void updateConfig(uint32_t restartDelayMs, uint32_t noSuccessPingTimeMs, uint32_t minFailedPings,
                  bool autoRestartEnabled, const String& monitoredHost) {
  xSemaphoreTake(gConfigMutex, portMAX_DELAY);
  gConfig.restartDelayMs = clampRestartDelayMs(restartDelayMs);
  gConfig.noSuccessPingTimeMs = clampNoSuccessPingTimeMs(noSuccessPingTimeMs);
  gConfig.minFailedPings = clampMinFailedPings(minFailedPings);
  gConfig.autoRestartEnabled = autoRestartEnabled;
  gConfig.monitoredHost = sanitizeMonitoredHost(monitoredHost);
  xSemaphoreGive(gConfigMutex);
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