#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>

void initConfigManager(const char* defaultMonitoredHost);

uint32_t clampRestartDelayMs(uint32_t value);
uint32_t clampNoSuccessPingTimeMs(uint32_t value);
uint32_t clampMinFailedPings(uint32_t value);
uint32_t minutesToMs(uint32_t minutes);
uint32_t noSuccessMinutesToMs(uint32_t minutes);
String sanitizeMonitoredHost(const String& value);

uint32_t getRestartDelayMs();
uint32_t getNoSuccessPingTimeMs();
uint32_t getMinFailedPings();
bool getAutoRestartEnabled();
String getMonitoredHost();

void updateConfig(uint32_t restartDelayMs, uint32_t noSuccessPingTimeMs, uint32_t minFailedPings,
                  bool autoRestartEnabled, const String& monitoredHost);
String configJson();

#endif