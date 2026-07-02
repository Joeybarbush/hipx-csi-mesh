/*
  ================================================================
  iron_bloom_node_v6.ino
  HIPJOY · IRON BLOOM · Node · v6
  WiFi-CSI presence + heartbeat + MQTT + BLE Central (Oura Ring 4)
  ================================================================

  Order:    Founder dispatch -- "v6 upgrades NODE 1 + NODE 2 ESP32s
            with BLE Central scanner for Joey's Oura Ring 4 alongside
            existing v5 capabilities."

  WHAT'S NEW IN v6 (vs v5)
  ----------------------------------------------------------------
  v5 published 14 entity pulses + heartbeat with HIPXQL framing,
  plus WiFi CSI activity scoring (RuView-inspired).

  v6 ADDS BLE Central role on the same ESP32. The chip now:
    - keeps doing WiFi STA + MQTT + CSI capture (Task A)
    - runs a BLE scanner that locks onto Joey's Oura Ring 4 (Task B)
    - emits a unified status heartbeat (Task C)

  HONESTY FLOOR
    Oura's proprietary GATT services are NOT reverse-engineered here.
    We read only standard Bluetooth SIG services:
      0x180A Device Info, 0x180F Battery, 0x180D Heart Rate.
    If the ring does not advertise 0x180D in peripheral mode (its
    typical posture), we still get RSSI + presence + (possibly)
    battery -- and we publish the honest gap. HRV / SpO2 / sleep
    stages live behind Oura's proprietary UUIDs and are NOT readable.

  REBEL FLOOR
    This firmware is for personal use, sovereign biometric capture,
    never push real MAC to public. The ring MAC lives ONLY in
    secrets.h (gitignored). secrets.example.h ships with a placeholder
    AA:BB:CC:DD:EE:FF. No PAT, no Oura cloud credentials anywhere.
    BLE pairing is one-time and requires Joey's consent (see README).
    All published topics are LOCAL only (hipjoy/wellspring/ring/*
    on Joey's mosquitto).

  Libraries: WiFi, PubSubClient, ESPmDNS, esp_wifi, BLEDevice
  License:   MIT
  ================================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "secrets.h"          // WIFI_SSID, WIFI_PASS, MQTT_FALLBACK_IP,
                              // ORING_MAC, ORING_NAME_HINT, ORING_RSSI_FLOOR
#include "mqtt_publisher.h"
#include "csi_publisher.h"
#include "ble_oura.h"

// ================================================================
//  CONFIG -- edit per board
// ================================================================
#ifndef NODE_ID
#define NODE_ID            1                      // <-- 1 or 2
#endif
#ifndef ROOM_LABEL
#define ROOM_LABEL         "bedroom_upstairs"     // <-- per board
#endif

#define STR2(x) #x
#define STR(x) STR2(x)
#define MQTT_CLIENT_ID     "iron-bloom-node-" STR(NODE_ID) "-v6"

#define T_HEARTBEAT        "hipjoy/heartbeat"
#define T_NODE_STATUS      "hipjoy/node_" STR(NODE_ID) "/status"

// ================================================================
//  GLOBAL STATE
// ================================================================
WiFiClient        wifiClient;
PubSubClient      mqtt(wifiClient);
IPAddress         brokerIp;
SemaphoreHandle_t mqttMutex = nullptr;

uint32_t          uptimeSec = 0;

// Helper: safe-publish guarded by mutex (BLE callbacks may also
// publish from another task context).
static void mqttPublishGuarded(const char* topic,
                               const char* body,
                               bool retain) {
  if (!mqttMutex) return;
  if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (mqtt.connected()) mqtt.publish(topic, body, retain);
    xSemaphoreGive(mqttMutex);
  }
}

// ----------------------------------------------------------------
//  TASK A -- WiFi + MQTT publisher + CSI pulse
// ----------------------------------------------------------------
//   - keeps WiFi + MQTT connection alive
//   - publishes heartbeat every 10 sec
//   - publishes CSI activity every 1 sec
// ----------------------------------------------------------------
void taskWifiMqtt(void* arg) {
  unsigned long lastHeartbeat = 0, lastCsi = 0;
  const unsigned long I_HEARTBEAT = 10000;
  const unsigned long I_CSI       = 1000;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      mqWifiConnect(WIFI_SSID, WIFI_PASS);
      csiInit();
      brokerIp = mqResolveBroker(MQTT_CLIENT_ID, MQTT_FALLBACK_IP);
    }
    if (!mqtt.connected()) {
      if (xSemaphoreTake(mqttMutex, portMAX_DELAY) == pdTRUE) {
        mqConnect(mqtt, brokerIp, MQTT_CLIENT_ID);
        xSemaphoreGive(mqttMutex);
      }
    }
    if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      mqtt.loop();
      xSemaphoreGive(mqttMutex);
    }

    unsigned long now = millis();

    if (now - lastCsi >= I_CSI) {
      lastCsi = now;
      if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(30)) == pdTRUE) {
        csiPulse(&mqtt, NODE_ID, ROOM_LABEL);
        xSemaphoreGive(mqttMutex);
      }
    }

    if (now - lastHeartbeat >= I_HEARTBEAT) {
      lastHeartbeat = now;
      uptimeSec = millis()/1000;
      BleOuraStatus b = bleOuraSnapshot();
      char payload[420];
      snprintf(payload, sizeof(payload),
        "{\"ts\":%lu,\"uptime_s\":%lu,\"node\":\"%s\","
        "\"rssi\":%d,\"csi_activity\":%.3f,\"csi_mode\":\"%s\","
        "\"ble_connected\":%s,\"ble_scanning\":%s,"
        "\"ring_rssi\":%d,\"ring_bpm\":%d,\"ring_batt\":%d,"
        "\"firmware\":\"v6_csi_ble\"}",
        (unsigned long)(millis()/1000), (unsigned long)uptimeSec,
        MQTT_CLIENT_ID, WiFi.RSSI(),
        csi::activityLast, csi::modeLast,
        b.connected ? "true" : "false",
        b.scanning  ? "true" : "false",
        b.lastRssi, b.lastBpm, b.lastBattery);
      mqttPublishGuarded(T_HEARTBEAT, payload, true);
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ----------------------------------------------------------------
//  TASK B -- BLE Central scanner (Oura Ring)
// ----------------------------------------------------------------
void taskBleOura(void* arg) {
  // Init BLE only after WiFi is up so radio coexistence is settled.
  while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(200));
  vTaskDelay(pdMS_TO_TICKS(1500));
  bleOuraInit(&mqtt, ORING_MAC, ORING_NAME_HINT, ORING_RSSI_FLOOR);
  for (;;) {
    bleOuraTick();
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

// ----------------------------------------------------------------
//  TASK C -- status reporter (lighter heartbeat)
// ----------------------------------------------------------------
void taskStatus(void* arg) {
  const unsigned long I_STATUS = 15000;
  unsigned long last = 0;
  for (;;) {
    unsigned long now = millis();
    if (now - last >= I_STATUS) {
      last = now;
      BleOuraStatus b = bleOuraSnapshot();
      char body[360];
      snprintf(body, sizeof(body),
        "{\"ts\":%lu,\"node_id\":%d,\"room\":\"%s\","
        "\"wifi_rssi\":%d,\"ble\":{\"enabled\":%s,\"scanning\":%s,"
        "\"connected\":%s,\"ring_rssi\":%d,\"ring_bpm\":%d,"
        "\"hr_notifications\":%lu,\"reconnects\":%lu}}",
        (unsigned long)(millis()/1000), NODE_ID, ROOM_LABEL,
        WiFi.RSSI(),
        b.enabled   ? "true" : "false",
        b.scanning  ? "true" : "false",
        b.connected ? "true" : "false",
        b.lastRssi, b.lastBpm,
        (unsigned long)b.hrNotifications,
        (unsigned long)b.reconnects);
      mqttPublishGuarded(T_NODE_STATUS, body, false);
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ================================================================
//  SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println();
  Serial.println("================================================");
  Serial.println(" HIPJOY · IRON BLOOM · v6  CSI + BLE Oura");
  Serial.printf (" node_id=%d  room=%s\n", NODE_ID, ROOM_LABEL);
  Serial.println(" REBEL FLOOR -- ring MAC NEVER in committed code");
  Serial.println("================================================");

  mqttMutex = xSemaphoreCreateMutex();

  mqWifiConnect(WIFI_SSID, WIFI_PASS);
  csiInit();
  brokerIp = mqResolveBroker(MQTT_CLIENT_ID, MQTT_FALLBACK_IP);
  mqtt.setBufferSize(MQ_BUFFER);
  mqtt.setKeepAlive(30);
  mqConnect(mqtt, brokerIp, MQTT_CLIENT_ID);

  // Spin up the three tasks. Stack sizes tuned for ESP32 WROOM.
  xTaskCreatePinnedToCore(taskWifiMqtt, "wifiMqtt", 6144, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(taskBleOura,  "bleOura",  6144, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(taskStatus,   "status",   3072, nullptr, 1, nullptr, 1);

  Serial.println("[boot] ready -- 3 tasks running");
}

void loop() {
  // All work happens in FreeRTOS tasks. Keep loop() idle.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
