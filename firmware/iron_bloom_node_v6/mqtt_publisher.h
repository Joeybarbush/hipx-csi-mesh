// ============================================================
//  mqtt_publisher.h  --  IRON BLOOM v6
//  Thin wrappers over PubSubClient used by all three FreeRTOS
//  tasks. Connection is established once in setup() and shared
//  across tasks; locking is done by serializing publishes
//  through a FreeRTOS mutex (see main .ino).
// ============================================================
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ESPmDNS.h>

#define MQ_PORT             1883
#define MQ_BUFFER           2048
#define MQ_BROKER_NAME      "hipjoy-pc"

inline void mqWifiConnect(const char* ssid, const char* pass) {
  Serial.printf("[wifi] connecting to %s ", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\n[wifi] timeout, rebooting"); ESP.restart();
    }
  }
  Serial.printf("\n[wifi] up · IP %s · RSSI %d\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

inline IPAddress mqResolveBroker(const char* clientId,
                                 const char* fallbackIp) {
  static bool mdnsStarted = false;
  IPAddress out;
  if (!mdnsStarted) {
    if (MDNS.begin(clientId)) {
      Serial.printf("[mdns] started as %s.local\n", clientId);
      mdnsStarted = true;
    }
  }
  IPAddress r = MDNS.queryHost(MQ_BROKER_NAME);
  if (r != IPAddress(0,0,0,0)) {
    out = r;
    Serial.printf("[mdns] %s.local -> %s\n",
                  MQ_BROKER_NAME, out.toString().c_str());
    return out;
  }
  out.fromString(fallbackIp);
  Serial.printf("[mdns] fallback -> %s\n", out.toString().c_str());
  return out;
}

inline void mqConnect(PubSubClient& mqtt,
                      const IPAddress& brokerIp,
                      const char* clientId) {
  while (!mqtt.connected()) {
    mqtt.setServer(brokerIp, MQ_PORT);
    Serial.printf("[mqtt] connect %s:%d as %s ... ",
                  brokerIp.toString().c_str(), MQ_PORT, clientId);
    if (mqtt.connect(clientId)) {
      Serial.println("ok");
    } else {
      Serial.printf("failed rc=%d, retry in 3s\n", mqtt.state());
      delay(3000);
    }
  }
}
