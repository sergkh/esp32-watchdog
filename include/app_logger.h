#ifndef APP_LOGGER_H
#define APP_LOGGER_H

#include <Arduino.h>

void initAppLogger(const char* ingestHost, const char* sourceToken, const char* serviceName);
void setAppLoggerConfig(const char* ingestHost, const char* sourceToken);

void logInfo(const String& message);
void logWarn(const String& message);
void logError(const String& message);

#endif
