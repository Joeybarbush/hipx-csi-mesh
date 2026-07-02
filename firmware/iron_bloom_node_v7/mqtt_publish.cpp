// ============================================================
//  mqtt_publish.cpp  --  WiFi + MQTT publisher for IRON BLOOM v7
//
//  CRC32 + monotonic sequence stamped on every payload (matches
//  the v6 pattern in iron_bloom_node_v6/csi_publisher.h-style
//  pipelines).
// ============================================================
#include "mqtt_publish.h"
#include "oura_ble.h"
#include "secrets.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

namespace MqttPub {

  static WiFiClient    g_net;
  static PubSubClient  g_mqtt(g_net);
  static uint32_t      g_seq            = 0;
  static uint32_t      g_published      = 0;
  static uint32_t      g_dropped        = 0;
  static uint32_t      g_lastHeartbeat  = 0;
  static uint32_t      g_lastReconnect  = 0;
  static IPAddress     g_brokerIp;
  static bool          g_brokerResolved = false;

  // Topics
  static const char* T_ACCEL    = "hipjoy/field/oura/accel";
  static const char* T_HR_LIVE  = "hipjoy/field/oura/hr_live";
  static const char* T_HRV      = "hipjoy/field/oura/hrv";
  static const char* T_TEMP     = "hipjoy/field/oura/temp_skin";
  static const char* T_SPO2     = "hipjoy/field/oura/spo2";
  static const char* T_MOTION   = "hipjoy/field/oura/motion_orientation";
  static const char* T_SLEEPMAD = "hipjoy/field/oura/sleep_mad";
  static const char* T_BATTERY  = "hipjoy/field/oura/battery";
  static const char* T_RAW_PPG  = "hipjoy/field/oura/raw_ppg";
  static const char* T_STATUS   = "hipjoy/field/oura/status";

  // ---- CRC32 (IEEE 802.3) ----
  static uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
      crc ^= data[i];
      for (int k = 0; k < 8; k++) {
        crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1u)));
      }
    }
    return ~crc;
  }

  static bool resolveBroker() {
    Serial.printf("[MQTT] resolving %s.local via mDNS\n", MQTT_BROKER_NAME);
    IPAddress ip = MDNS.queryHost(MQTT_BROKER_NAME);
    if (ip != IPAddress(0, 0, 0, 0)) {
      g_brokerIp = ip;
      Serial.printf("[MQTT] mDNS resolved %s -> %s\n",
                    MQTT_BROKER_NAME, ip.toString().c_str());
      g_brokerResolved = true;
      return true;
    }
    Serial.printf("[MQTT] mDNS miss, falling back to %s\n", MQTT_FALLBACK_IP);
    if (!g_brokerIp.fromString(MQTT_FALLBACK_IP)) {
      Serial.println("[MQTT] fallback IP parse failed");
      return false;
    }
    g_brokerResolved = true;
    return true;
  }

  static bool wifiUp() {
    if (WiFi.status() == WL_CONNECTED) return true;
    Serial.printf("[WIFI] connecting to %s\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOST_LABEL);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < 20000) {
      delay(250);
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] connect timeout");
      return false;
    }
    Serial.printf("[WIFI] up ip=%s rssi=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    if (!MDNS.begin(HOST_LABEL)) {
      Serial.println("[MDNS] start failed (continuing)");
    }
    return true;
  }

  static bool mqttUp() {
    if (g_mqtt.connected()) return true;
    uint32_t now = millis();
    if (now - g_lastReconnect < 2000) return false;
    g_lastReconnect = now;

    if (!g_brokerResolved) {
      if (!resolveBroker()) return false;
    }
    g_mqtt.setServer(g_brokerIp, MQTT_PORT);
    g_mqtt.setBufferSize(1024);
    g_mqtt.setKeepAlive(30);

    Serial.printf("[MQTT] connecting to %s:%d as %s\n",
                  g_brokerIp.toString().c_str(), MQTT_PORT, MQTT_CLIENT_ID);
    if (g_mqtt.connect(MQTT_CLIENT_ID)) {
      Serial.println("[MQTT] connected");
      g_mqtt.publish(T_STATUS, "{\"event\":\"connect\",\"node_id\":7}", true);
      return true;
    }
    Serial.printf("[MQTT] connect failed state=%d\n", g_mqtt.state());
    return false;
  }

  // Build envelope: { ts, node_id, seq, crc32, ...kv }
  // kv is a JSON object string fragment WITHOUT enclosing braces.
  // Result is what we publish.
  static size_t buildEnvelope(char* out, size_t cap,
                              const char* kv) {
    g_seq++;
    // Provisional pre-CRC pass without crc field, to compute CRC.
    int n0 = snprintf(out, cap,
      "{\"ts\":%lu,\"node_id\":%d,\"seq\":%lu,%s,\"crc32\":0}",
      (unsigned long)(millis() / 1000), NODE_ID,
      (unsigned long)g_seq, kv);
    if (n0 < 0 || (size_t)n0 >= cap) return 0;
    uint32_t crc = crc32((const uint8_t*)out, (size_t)n0);
    // Final pass with CRC in place.
    int n1 = snprintf(out, cap,
      "{\"ts\":%lu,\"node_id\":%d,\"seq\":%lu,%s,\"crc32\":%lu}",
      (unsigned long)(millis() / 1000), NODE_ID,
      (unsigned long)g_seq, kv, (unsigned long)crc);
    if (n1 < 0 || (size_t)n1 >= cap) return 0;
    return (size_t)n1;
  }

  static bool publish(const char* topic, const char* payload, size_t len) {
    if (!g_mqtt.connected()) { g_dropped++; return false; }
    bool ok = g_mqtt.publish(topic, (const uint8_t*)payload, (unsigned int)len, false);
    if (ok) g_published++; else g_dropped++;
    return ok;
  }

  // ---- Per-stream emitters ----
  // Rate-limiting note: the BLE poll cadence (oura_ble.cpp) already
  // throttles HR_LIVE/SPO2/etc to their target rates. ACCEL is the
  // only firehose -- we additionally drop here to RATE_HZ_ACCEL.

  static uint32_t g_lastAccelMs = 0;

  static void emitHRLive(const OuraDec::Sample& s) {
    char kv[96];
    snprintf(kv, sizeof(kv), "\"value\":{\"bpm\":%ld,\"quality\":%u}",
             (long)s.i0, (unsigned)s.b0);
    char buf[256];
    size_t n = buildEnvelope(buf, sizeof(buf), kv);
    if (n) publish(T_HR_LIVE, buf, n);
  }

  static void emitHRV(const OuraDec::Sample& s) {
    char kv[96];
    snprintf(kv, sizeof(kv), "\"value\":{\"rmssd_ms\":%ld,\"window_s\":%u}",
             (long)s.i0, (unsigned)s.u0);
    char buf[256];
    size_t n = buildEnvelope(buf, sizeof(buf), kv);
    if (n) publish(T_HRV, buf, n);
  }

  static void emitTempSkin(const OuraDec::Sample& s) {
    float c = OuraDec::tempC100ToC((int16_t)s.i0);
    char kv[96];
    snprintf(kv, sizeof(kv), "\"value\":{\"c_x100\":%ld,\"c\":%.2f}",
             (long)s.i0, c);
    char buf[256];
    size_t n = buildEnvelope(buf, sizeof(buf), kv);
    if (n) publish(T_TEMP, buf, n);
  }

  static void emitSpO2(const OuraDec::Sample& s) {
    char kv[96];
    snprintf(kv, sizeof(kv), "\"value\":{\"pct\":%ld,\"quality\":%u}",
             (long)s.i0, (unsigned)s.b0);
    char buf[256];
    size_t n = buildEnvelope(buf, sizeof(buf), kv);
    if (n) publish(T_SPO2, buf, n);
  }

  static void emitMotion(const OuraDec::Sample& s) {
    char kv[128];
    snprintf(kv, sizeof(kv), "\"value\":{\"orient_bits\":%u,\"mag\":%u}",
             (unsigned)s.b0, (unsigned)s.u0);
    char buf[256];
    size_t n = buildEnvelope(buf, sizeof(buf), kv);
    if (n) publish(T_MOTION, buf, n);
  }

  static void emitSleepMad(const OuraDec::Sample& s) {
    char kv[96];
    snprintf(kv, sizeof(kv), "\"value\":{\"mad\":%u}", (unsigned)s.u0);
    char buf[256];
    size_t n = buildEnvelope(buf, sizeof(buf), kv);
    if (n) publish(T_SLEEPMAD, buf, n);
  }

  static void emitBattery(const OuraDec::Sample& s) {
    char kv[128];
    snprintf(kv, sizeof(kv),
             "\"value\":{\"pct\":%u,\"charging\":%u,\"recommended\":%u}",
             (unsigned)s.b0, (unsigned)s.b1, (unsigned)s.b2);
    char buf[256];
    size_t n = buildEnvelope(buf, sizeof(buf), kv);
    if (n) publish(T_BATTERY, buf, n);
  }

  static void emitAccel(const OuraDec::Sample& s) {
    uint32_t now = millis();
    uint32_t periodMs = (RATE_HZ_ACCEL > 0) ? (1000U / RATE_HZ_ACCEL) : 20U;
    if (now - g_lastAccelMs < periodMs) return;
    g_lastAccelMs = now;
    float gx = OuraDec::accelI16ToG((int16_t)s.i0);
    float gy = OuraDec::accelI16ToG((int16_t)s.i1);
    float gz = OuraDec::accelI16ToG((int16_t)s.i2);
    char kv[160];
    snprintf(kv, sizeof(kv),
             "\"value\":{\"x\":%ld,\"y\":%ld,\"z\":%ld,"
             "\"gx\":%.4f,\"gy\":%.4f,\"gz\":%.4f}",
             (long)s.i0, (long)s.i1, (long)s.i2, gx, gy, gz);
    char buf[320];
    size_t n = buildEnvelope(buf, sizeof(buf), kv);
    if (n) publish(T_ACCEL, buf, n);
  }

  static void emitRawPpg(const OuraDec::Sample& s) {
    if (!ENABLE_RAW_PPG) return;
    // Build a compact JSON array of i16 channel samples.
    char ch[768];
    int  off = snprintf(ch, sizeof(ch), "\"value\":{\"n\":%u,\"ch\":[", (unsigned)s.u0);
    for (size_t i = 0; i < s.u0 && (size_t)off < sizeof(ch) - 12; i++) {
      int16_t v = (int16_t)((uint16_t)s.raw[2*i] | ((uint16_t)s.raw[2*i + 1] << 8));
      off += snprintf(ch + off, sizeof(ch) - off, "%s%d", (i ? "," : ""), (int)v);
    }
    snprintf(ch + off, sizeof(ch) - off, "]}");
    char buf[900];
    size_t n = buildEnvelope(buf, sizeof(buf), ch);
    if (n) publish(T_RAW_PPG, buf, n);
  }

  // ---- Public API ----
  bool begin() {
    if (!wifiUp())   return false;
    if (!resolveBroker()) return false;
    g_mqtt.setServer(g_brokerIp, MQTT_PORT);
    g_mqtt.setBufferSize(1024);
    g_mqtt.setKeepAlive(30);
    return mqttUp();
  }

  void loop() {
    if (WiFi.status() != WL_CONNECTED) {
      // Non-blocking re-attach; wifiUp() blocks up to 20s -- gate by interval.
      static uint32_t lastTry = 0;
      uint32_t now = millis();
      if (now - lastTry > 5000) {
        lastTry = now;
        wifiUp();
      }
      return;
    }
    if (!g_mqtt.connected()) {
      mqttUp();
      return;
    }
    g_mqtt.loop();

    uint32_t now = millis();
    if (now - g_lastHeartbeat > 1000) {
      g_lastHeartbeat = now;
      heartbeat();
    }
  }

  void onSample(const OuraDec::Sample& s) {
    switch (s.kind) {
      case OuraDec::StreamKind::HR_LIVE:        emitHRLive(s);   break;
      case OuraDec::StreamKind::HRV_RMSSD:      emitHRV(s);      break;
      case OuraDec::StreamKind::TEMP_SKIN:      emitTempSkin(s); break;
      case OuraDec::StreamKind::SPO2:           emitSpO2(s);     break;
      case OuraDec::StreamKind::MOTION_ORIENT:  emitMotion(s);   break;
      case OuraDec::StreamKind::SLEEP_MAD:      emitSleepMad(s); break;
      case OuraDec::StreamKind::BATTERY:        emitBattery(s);  break;
      case OuraDec::StreamKind::ACCEL:          emitAccel(s);    break;
      case OuraDec::StreamKind::RAW_PPG:        emitRawPpg(s);   break;
      case OuraDec::StreamKind::EVENT_TAG:      /* future */     break;
      case OuraDec::StreamKind::FIRMWARE_INFO:  /* logged only */ break;
      default: break;
    }
  }

  void heartbeat() {
    if (!g_mqtt.connected()) return;
    char kv[256];
    snprintf(kv, sizeof(kv),
      "\"value\":{\"ble\":\"%s\",\"mqtt\":\"up\",\"uptime_s\":%lu,"
      "\"last_notify_ms\":%lu,\"published\":%lu,\"dropped\":%lu,"
      "\"rssi\":%d,\"auth_fail\":%lu,\"connects\":%lu}",
      OuraBLE::stateName(),
      (unsigned long)(millis() / 1000),
      (unsigned long)OuraBLE::lastNotifyMs(),
      (unsigned long)g_published, (unsigned long)g_dropped,
      WiFi.RSSI(),
      (unsigned long)OuraBLE::authFailures(),
      (unsigned long)OuraBLE::connectAttempts());
    char buf[400];
    size_t n = buildEnvelope(buf, sizeof(buf), kv);
    if (n) g_mqtt.publish(T_STATUS, (const uint8_t*)buf, (unsigned int)n, false);
  }

  bool     isConnected()    { return g_mqtt.connected(); }
  uint32_t publishedCount() { return g_published; }
  uint32_t droppedCount()   { return g_dropped; }
  uint32_t lastSeq()        { return g_seq; }
}
