#include <Arduino.h>
#include <WiFi.h>
#include <ESP32Ping.h>
#include "esp_sleep.h"

const unsigned long sleepTime = 30ULL; // 30 seconds
const int restartDelayCycles = 40; 
const char* ssid = ""; // TODO: add SSID
const char* password = ""; // TODO: add password
const char* watchdogHost = ""; // TODO: add watchdog IP/host
const int PIN = 14;

RTC_DATA_ATTR int failedPings = 0;
RTC_DATA_ATTR int restartDelay = 0;

void forceReboot() {
  if (restartDelay <= 0) {
    Serial.println("Forcing reboot...");
    digitalWrite(PIN, HIGH);
    delay(3000);
    digitalWrite(PIN, LOW);
    restartDelay = restartDelayCycles;
    failedPings = 0;
    Serial.println("Reboot forced.");
  }
}

void runCheck() {
  Serial.print("Pinging ");
  Serial.println(watchdogHost);

  bool ok = Ping.ping(watchdogHost, 5);

  if (ok) {
    Serial.println("Ping OK");
    failedPings = 0;
  } else {
    Serial.println("Ping FAILED");
    failedPings++;
    
    if (failedPings >= 3) {
      Serial.println("Too many failed pings, forcing reboot.");
      forceReboot();
    }
  }
}

void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Version 0.1");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void deepSleep() {
  Serial.println("Sleeping...");
  Serial.flush();
  esp_sleep_enable_timer_wakeup(sleepTime * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN, OUTPUT);
  digitalWrite(PIN, LOW);
  
  // a restart happened recently, wait for a few cycles before pinging again
  if (restartDelay > 0) {
    restartDelay--;
    Serial.printf("Cycles till next restart: %d\n", restartDelay);
  }

  delay(2000);

  wifiConnect();

  if (WiFi.status() == WL_CONNECTED) {
    runCheck();
  }

  wifiOff();
  
  deepSleep();
}

// no loop as it resents on deep sleep
void loop() {}