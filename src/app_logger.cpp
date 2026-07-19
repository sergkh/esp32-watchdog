#include "app_logger.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

namespace {

String gIngestHost;
String gSourceToken;
String gServiceName = "esp32-watchdog";

String escapeJson(const String& input) {
  String out;
  out.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
      continue;
    }
    if (c == '\n') {
      out += "\\n";
      continue;
    }
    if (c == '\r') {
      out += "\\r";
      continue;
    }
    if (c == '\t') {
      out += "\\t";
      continue;
    }
    out += c;
  }
  return out;
}

bool canSendToBetterStack() {
  return gIngestHost.length() > 0 && gSourceToken.length() > 0 && WiFi.status() == WL_CONNECTED;
}

String buildIngestUrl() {
  if (gIngestHost.startsWith("http://") || gIngestHost.startsWith("https://")) {
    return gIngestHost;
  }
  return String("https://") + gIngestHost;
}

void sendToBetterStack(const char* level, const String& message) {
  if (!canSendToBetterStack()) {
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  String ingestUrl = buildIngestUrl();
  if (!https.begin(client, ingestUrl)) {
    Serial.println("[WARN] Better Stack connection init failed.");
    return;
  }

  https.setTimeout(2000);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Authorization", String("Bearer ") + gSourceToken);

  String payload = "{";
  payload += "\"message\":\"";
  payload += escapeJson(message);
  payload += "\",";
  payload += "\"level\":\"";
  payload += level;
  payload += "\",";
  payload += "\"service\":\"";
  payload += escapeJson(gServiceName);
  payload += "\",";
  payload += "\"ip\":\"";
  payload += WiFi.localIP().toString();
  payload += "\",";
  payload += "\"uptimeMs\":";
  payload += String(millis());
  payload += "}";

  int code = https.POST(payload);
  https.end();

  if (code != 200 && code != 202) {
    Serial.print("[WARN] Better Stack rejected log, code=");
    Serial.println(code);
  }
}

void logWithLevel(const char* level, const String& message) {
  Serial.print("[");
  Serial.print(level);
  Serial.print("] ");
  Serial.println(message);
  sendToBetterStack(level, message);
}

}  // namespace

void initAppLogger(const char* ingestHost, const char* sourceToken, const char* serviceName) {
  if (ingestHost != nullptr) {
    gIngestHost = ingestHost;
  }
  if (sourceToken != nullptr) {
    gSourceToken = sourceToken;
  }
  if (serviceName != nullptr && serviceName[0] != '\0') {
    gServiceName = serviceName;
  }
}

void setAppLoggerConfig(const char* ingestHost, const char* sourceToken) {
  if (ingestHost != nullptr) {
    gIngestHost = ingestHost;
  }
  if (sourceToken != nullptr) {
    gSourceToken = sourceToken;
  }
}

void logInfo(const String& message) {
  logWithLevel("info", message);
}

void logWarn(const String& message) {
  logWithLevel("warn", message);
}

void logError(const String& message) {
  logWithLevel("error", message);
}
