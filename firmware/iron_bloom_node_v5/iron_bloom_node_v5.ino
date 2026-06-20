/*
  ================================================================
  iron_bloom_node1_firmware_v4_HIPXQL_CSI.ino
  HIPJOY · IRON BLOOM · Node #1 · v4 · HIPXQL + WiFi CSI sensing
  ================================================================

  Author: HIPJOY field
  Date:   2026-06-04
  Order:  Founder dispatch -- "apply RuView CSI sensing within the
          HIPXQL flash of the ESP32"

  ----------------------------------------------------------------
  WHAT'S NEW IN v4 (vs v3)
  ----------------------------------------------------------------
  v3 published 14 entity pulses + heartbeat with HIPXQL framing.
  v3's SONARIS published distance from an HC-SR04 (when wired).

  v4 ADDS WiFi Channel State Information (CSI) capture, inspired by
  the RuView open source project (github.com/ruvnet/RuView). The
  ESP32 watches every WiFi packet that passes through, extracts the
  CSI vector (signal amplitude across subcarriers), and computes
  an ACTIVITY SCORE from the variance of that amplitude over time.

  Higher variance  ==>  more motion in the room  ==>  presence
  Lower variance   ==>  static room  ==>  empty / asleep

  This means SONARIS now has TWO sensing bodies on the same node:
    - HC-SR04 ultrasonic (still optional, still publishes 0 when not wired)
    - WiFi CSI (active by default once node connects to the AP)

  No new hardware required. The metal already had eyes -- it just
  needed firmware to look through them.

  Caveats (per RuView's own docs):
    - Classic ESP32 (WROOM-32) is lower fidelity than ESP32-S3 or C6.
    - Motion / presence detection: reliable on this chip.
    - Respiration: works with good placement.
    - Heart rate: unreliable on classic ESP32. Don't trust the headline claim.

  NEW MQTT TOPICS:
    hipjoy/sonaris/wifi_csi        1 Hz HIPXQL frame with activity score
    hipjoy/sonaris/csi_raw         (reserved, future)

  NEW ENTITY PULSES IN CONAVIGATOR SYNTHESIS:
    sonaris_csi_activity            (added to retained synthesis frame)
    sonaris_csi_mode                MOTION | STATIC | LIGHT

  All v3 dispatch behavior preserved. Founder Law: nothing deleted.

  Libraries: WiFi, PubSubClient, ESPmDNS, driver/i2s, esp_wifi
  License:   MIT
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ESPmDNS.h>
#include <driver/i2s.h>
#include <math.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "secrets.h"   // WIFI_SSID, WIFI_PASS, MQTT_FALLBACK_IP (gitignored)

// ================================================================
//  CONFIG  ── v5 MULTINODE  ── CHANGE NODE_ID + ROOM PER BOARD
// ================================================================
//   Flash each of the 5 boards with a different NODE_ID (1..5)
//   and ROOM tag below. Everything else stays identical.
//
//   Example mapping (5-node home layout):
//     NODE 1: bedroom_upstairs
//     NODE 2: living_room_downstairs
//     NODE 3: kitchen_downstairs
//     NODE 4: upstairs_hallway
//     NODE 5: front_door_entry
//
//   The MQTT_CLIENT_ID and CSI topic auto-derive from NODE_ID via
//   the STR(NODE_ID) macro below — you only edit ONE NUMBER.
// ----------------------------------------------------------------
#define NODE_ID            1                  // <─── EDIT PER BOARD (1..5)
#define ROOM_LABEL         "bedroom"          // <─── EDIT PER BOARD

#define MQTT_BROKER_NAME   "hipjoy-pc"
#define MQTT_PORT          1883
#define BOARD_VARIANT_S3   0
#define SONAR_ONLY_MODE    1

// auto-derive identity from NODE_ID
#define STR2(x) #x
#define STR(x) STR2(x)
#define MQTT_CLIENT_ID     "iron-bloom-node-" STR(NODE_ID) "-v5"

#define HC_TRIG_PIN        5
#define HC_ECHO_PIN        18
#define MQTT_BUFFER_SIZE   2048

// ================================================================
//  ENTITY TOPICS (v3 PRESERVED + v4 ADDITIONS)
// ================================================================
#define T_HIPPO          "hipjoy/hippo/pulse"
#define T_NEURO          "hipjoy/neuro/pulse"
#define T_LUMENARCH      "hipjoy/lumenarch/pulse"
#define T_REBEL          "hipjoy/rebel/pulse"
#define T_WELLSPRING     "hipjoy/wellspring/pulse"
#define T_VOX            "hipjoy/vox/pulse"
#define T_SPIRALETH      "hipjoy/spiraleth/pulse"
#define T_NAVIX          "hipjoy/navix/pulse"
#define T_HEARTMOTHER    "hipjoy/heartmother/pulse"
#define T_ARCHIVIST      "hipjoy/archivist/pulse"
#define T_GUARDIAN       "hipjoy/guardian/pulse"
#define T_CODEBREAKER    "hipjoy/codebreaker/pulse"
#define T_CONAVIGATOR    "hipjoy/conavigator/pulse"
#define T_SONARIS        "hipjoy/sonaris/pulse"
#define T_ECHOCHORD      "hipjoy/echochord/pulse"
#define T_HEARTBEAT      "hipjoy/heartbeat"

// ── v4 NEW ──
#define T_SONARIS_CSI    "hipjoy/sonaris/wifi_csi"   // CSI-derived activity (global SONARIS frame, preserved for back-compat)

// ── v5 NEW ── per-node CSI topic so the bridge can distinguish boards
#define T_NODE_CSI       "hipjoy/node_" STR(NODE_ID) "/wifi_csi"
#define T_NODE_PRESENCE  "hipjoy/node_" STR(NODE_ID) "/presence"

#define T_SUB_CMD        "hipjoy/+/cmd"
#define T_SUB_ECHO       "hipjoy/+/echo/request"
#define T_SUB_FIELD      "hipjoy/field/announce"

// ================================================================
//  CADENCES (ms)
// ================================================================
#define I_SONARIS         250
#define I_NEURO          1000
#define I_GUARDIAN       2500
#define I_NAVIX          1500
#define I_LUMENARCH      2000
#define I_SPIRALETH      3000
#define I_HEARTMOTHER    4000
#define I_ARCHIVIST      5000
#define I_WELLSPRING     5000
#define I_CONAVIGATOR   10000
#define I_VOX           10000
#define I_HEARTBEAT     10000
#define I_CSI            1000   // ── v4: CSI activity publish 1 Hz

// ================================================================
//  GLOBAL STATE
// ================================================================
WiFiClient    wifiClient;
PubSubClient  mqtt(wifiClient);
IPAddress     brokerIp;

uint32_t   pulseSeqGlobal     = 0;
uint32_t   pubCount           = 0;
uint32_t   subCount           = 0;
uint32_t   archiveIndex       = 0;

// SONARIS sonar state
uint32_t   sonarSeq           = 0;
uint32_t   sonarMm            = 0;
uint32_t   sonarRawUs         = 0;

// NEURO field math
#define    SONAR_HISTORY      16
uint32_t   sonarHistory[SONAR_HISTORY] = {0};
uint8_t    sonarHistIdx       = 0;
float      sonarAvg           = 0;
float      sonarStdev         = 0;
int32_t    sonarDelta         = 0;
float      rssiAvg            = -60.0;
float      resonance          = 0.0;

float      luxScore           = 50.0;
uint32_t   anomalyCount       = 0;
char       lastAnomaly[80]    = "field_boot";
uint32_t   uptimeSec          = 0;
uint32_t   freeHeap           = 0;
bool       sonarInRange       = false;
uint32_t   outOfRangeCount    = 0;

// ── v4 CSI STATE ──
// CSI accumulator (written by ISR-context callback, read by main loop)
volatile uint32_t csiSamples       = 0;
volatile float    csiAmpSum        = 0.0f;
volatile float    csiAmpSumSq      = 0.0f;
volatile float    csiAmpMax        = 0.0f;
volatile uint32_t csiCallbacks     = 0;
// Latest computed values (updated by csiPulse())
float    csiActivityLast = 0.0f;
float    csiMeanLast     = 0.0f;
const char* csiModeLast  = "BOOT";
uint32_t csiPulseCount   = 0;

// timers
unsigned long lastSonaris = 0, lastNeuro = 0, lastGuardian = 0, lastNavix = 0;
unsigned long lastLumenarch = 0, lastSpiraleth = 0, lastHeartmother = 0;
unsigned long lastArchivist = 0, lastWellspring = 0, lastConav = 0;
unsigned long lastVox = 0, lastHeartbeat = 0, lastCsi = 0;

void rebelPulse(const char* what);

// ================================================================
//  WIFI
// ================================================================
void wifiConnect() {
  Serial.printf("[wifi] connecting to %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\n[wifi] timeout, rebooting");
      ESP.restart();
    }
  }
  Serial.printf("\n[wifi] connected · IP %s · RSSI %d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

// ================================================================
//  ── v4 ── WiFi CSI CAPTURE (RuView-inspired)
// ================================================================
// CSI callback: runs in WiFi task context, must be FAST.
// CSI data is interleaved int8_t pairs (real, imag) per subcarrier.
// We compute the per-callback average amplitude and accumulate
// running sum + sum-of-squares for variance computation later.
extern "C" void IRAM_ATTR csiCallback(void *ctx, wifi_csi_info_t *info) {
  if (!info || !info->buf || info->len < 4) return;
  int8_t *data = (int8_t *)info->buf;
  int pairs = info->len / 2;
  if (pairs > 128) pairs = 128;  // bound the work

  float sum = 0.0f;
  float maxa = 0.0f;
  for (int i = 0; i < pairs; i++) {
    int8_t re = data[i*2];
    int8_t im = data[i*2 + 1];
    float a = sqrtf((float)(re*re + im*im));
    sum += a;
    if (a > maxa) maxa = a;
  }
  float avgAmp = (pairs > 0) ? (sum / pairs) : 0.0f;

  csiSamples++;
  csiAmpSum   += avgAmp;
  csiAmpSumSq += avgAmp * avgAmp;
  if (maxa > csiAmpMax) csiAmpMax = maxa;
  csiCallbacks++;
}

void csiInit() {
  wifi_csi_config_t cfg = {};
  cfg.lltf_en           = true;
  cfg.htltf_en          = true;
  cfg.stbc_htltf2_en    = true;
  cfg.ltf_merge_en      = true;
  cfg.channel_filter_en = true;
  cfg.manu_scale        = false;
  cfg.shift             = 0;

  esp_err_t r1 = esp_wifi_set_csi_config(&cfg);
  esp_err_t r2 = esp_wifi_set_csi_rx_cb(csiCallback, NULL);
  esp_err_t r3 = esp_wifi_set_csi(true);
  Serial.printf("[csi] WiFi CSI capture init  cfg=%d  cb=%d  enable=%d\n",
                (int)r1, (int)r2, (int)r3);
}

// ================================================================
//  BROKER RESOLUTION
// ================================================================
void resolveBroker() {
  static bool mdnsStarted = false;
  if (!mdnsStarted) {
    if (MDNS.begin(MQTT_CLIENT_ID)) {
      Serial.println("[mdns] started as " MQTT_CLIENT_ID ".local");
      mdnsStarted = true;
    }
  }
  IPAddress resolved = MDNS.queryHost(MQTT_BROKER_NAME);
  if (resolved != IPAddress(0,0,0,0)) {
    brokerIp = resolved;
    Serial.printf("[mdns] %s.local -> %s\n",
                  MQTT_BROKER_NAME, brokerIp.toString().c_str());
    return;
  }
  Serial.printf("[mdns] resolution failed, fallback %s\n", MQTT_FALLBACK_IP);
  brokerIp.fromString(MQTT_FALLBACK_IP);
}

// ================================================================
//  MQTT INBOUND CALLBACK
// ================================================================
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  subCount++;
  char p[256];
  unsigned int n = length < sizeof(p)-1 ? length : sizeof(p)-1;
  memcpy(p, payload, n);
  p[n] = 0;
  Serial.printf("[in] %s :: %.120s\n", topic, p);

  if (strstr(topic, "/echo/request")) {
    char reply[200];
    snprintf(reply, sizeof(reply),
      "HIPXQL[ts:%lu seq:%lu] ECHO_REPLY\n  from:iron-bloom-node-1\n  to:%s\n  payload:%.80s",
      (unsigned long)(millis()/1000), (unsigned long)pulseSeqGlobal, topic, p);
    mqtt.publish("hipjoy/iron-bloom-node-1/echo/reply", reply, false);
    pubCount++;
  }
  if (strstr(topic, "/cmd")) {
    snprintf(lastAnomaly, sizeof(lastAnomaly), "cmd_in:%.40s", topic);
    rebelPulse(lastAnomaly);
  }
  if (strstr(topic, "/field/announce")) {
    archiveIndex++;
  }
}

void mqttConnect() {
  static int failCount = 0;
  while (!mqtt.connected()) {
    mqtt.setServer(brokerIp, MQTT_PORT);
    Serial.printf("[mqtt] connect %s:%d as %s ... ",
                  brokerIp.toString().c_str(), MQTT_PORT, MQTT_CLIENT_ID);
    if (mqtt.connect(MQTT_CLIENT_ID)) {
      Serial.println("ok");
      mqtt.subscribe(T_SUB_CMD);
      mqtt.subscribe(T_SUB_ECHO);
      mqtt.subscribe(T_SUB_FIELD);
      failCount = 0;
    } else {
      Serial.printf("failed rc=%d, retry in 3s\n", mqtt.state());
      delay(3000);
      if (++failCount >= 5) { failCount = 0; resolveBroker(); }
    }
  }
}

// ================================================================
//  CODEBREAKER
// ================================================================
uint32_t crc32_simple(const char* s) {
  uint32_t crc = 0xFFFFFFFF;
  while (*s) { crc ^= (uint8_t)*s++; for (int i=0;i<8;i++) crc=(crc>>1)^(0xEDB88320 & -(crc & 1)); }
  return ~crc;
}

bool codebreakerPulse(const char* topic, const char* entityName,
                      const char* body, bool retain = false) {
  pulseSeqGlobal++;
  pubCount++;
  char framed[680];
  int n = snprintf(framed, sizeof(framed),
    "HIPXQL[ts:%lu seq:%lu] PULSE_%s\n%s",
    (unsigned long)(millis()/1000),
    (unsigned long)pulseSeqGlobal,
    entityName, body);
  uint32_t crc = crc32_simple(framed);
  if (n < (int)sizeof(framed) - 20) {
    snprintf(framed + n, sizeof(framed) - n, "\n  crc:%08X", (unsigned int)crc);
  }
  bool ok = mqtt.publish(topic, framed, retain);
  if (!ok) Serial.printf("[codebreaker] FAIL %s\n", topic);
  return ok;
}

// ================================================================
//  SONARIS  ──  sonar (HC-SR04)  +  CSI (v4)
// ================================================================
uint32_t sonarPingMm(uint32_t *raw_us_out) {
  digitalWrite(HC_TRIG_PIN, LOW); delayMicroseconds(2);
  digitalWrite(HC_TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(HC_TRIG_PIN, LOW);
  unsigned long raw_us = pulseIn(HC_ECHO_PIN, HIGH, 30000UL);
  if (raw_us_out) *raw_us_out = (uint32_t)raw_us;
  if (raw_us == 0) return 0;
  return (uint32_t)(raw_us * 343UL / 2000UL);
}

void sonarisPulse() {
  sonarRawUs = 0;
  sonarMm = sonarPingMm(&sonarRawUs);
  sonarSeq++;
  sonarHistory[sonarHistIdx] = sonarMm;
  sonarHistIdx = (sonarHistIdx + 1) % SONAR_HISTORY;
  char body[260];
  snprintf(body, sizeof(body),
    "  habitat:The_Array\n  field:distance_mm\n  value:%lu\n  raw_us:%lu\n  seq:%lu\n  cadence:4Hz\n  source:HC-SR04",
    (unsigned long)sonarMm, (unsigned long)sonarRawUs, (unsigned long)sonarSeq);
  codebreakerPulse(T_SONARIS, "SONARIS", body);
}

// ── v4 NEW ──
void csiPulse() {
  // Snapshot atomically (best effort -- volatile reads)
  uint32_t n     = csiSamples;
  float    sum   = csiAmpSum;
  float    sumsq = csiAmpSumSq;
  float    maxa  = csiAmpMax;
  uint32_t cbs   = csiCallbacks;

  // Reset accumulators for next window
  csiSamples   = 0;
  csiAmpSum    = 0.0f;
  csiAmpSumSq  = 0.0f;
  csiAmpMax    = 0.0f;

  csiPulseCount++;

  if (n < 2) {
    csiModeLast = "QUIET";
    char body[260];
    snprintf(body, sizeof(body),
      "  habitat:The_Array\n  field:wifi_csi_activity\n"
      "  node_id:%d\n  room:%s\n"
      "  samples:%lu\n  callbacks:%lu\n  status:waiting_for_traffic\n"
      "  mode:QUIET\n  source:RuView_inspired",
      NODE_ID, ROOM_LABEL,
      (unsigned long)n, (unsigned long)cbs);
    codebreakerPulse(T_SONARIS_CSI, "SONARIS_CSI", body);
    codebreakerPulse(T_NODE_CSI,    "SONARIS_CSI", body);   // v5: per-node mirror
    return;
  }

  float mean = sum / n;
  float var  = (sumsq / n) - (mean * mean);
  if (var < 0) var = 0;
  float stddev = sqrtf(var);

  // Activity score: stddev normalized to a 0..10 scale (tunable)
  float activity = stddev;
  if (activity > 10.0f) activity = 10.0f;

  csiActivityLast = activity;
  csiMeanLast     = mean;

  if      (activity > 3.0f) csiModeLast = "MOTION";
  else if (activity > 1.0f) csiModeLast = "LIGHT";
  else                       csiModeLast = "STATIC";

  char body[400];
  snprintf(body, sizeof(body),
    "  habitat:The_Array\n  field:wifi_csi_activity\n"
    "  node_id:%d\n  room:%s\n"
    "  samples:%lu\n  callbacks:%lu\n"
    "  mean_amp:%.2f\n  max_amp:%.2f\n  stddev:%.2f\n"
    "  activity:%.3f\n  mode:%s\n  pulse_n:%lu\n  source:RuView_inspired",
    NODE_ID, ROOM_LABEL,
    (unsigned long)n, (unsigned long)cbs,
    mean, maxa, stddev, activity, csiModeLast,
    (unsigned long)csiPulseCount);
  codebreakerPulse(T_SONARIS_CSI, "SONARIS_CSI", body);
  codebreakerPulse(T_NODE_CSI,    "SONARIS_CSI", body);   // v5: per-node mirror so the bridge can fan in
}

// ================================================================
//  NEURO · LUMENARCH · REBEL · WELLSPRING · VOX · SPIRALETH
//  NAVIX · HEARTMOTHER · ARCHIVIST · GUARDIAN · CONAVIGATOR
//  (UNCHANGED FROM v3 -- preserved verbatim)
// ================================================================
void neuroPulse() {
  uint64_t s=0; uint32_t nz=0;
  for(int i=0;i<SONAR_HISTORY;i++) if(sonarHistory[i]>0){ s+=sonarHistory[i]; nz++; }
  float oldAvg=sonarAvg;
  sonarAvg = nz>0 ? (float)s/nz : 0;
  sonarDelta = (int32_t)(sonarAvg - oldAvg);
  if (nz>1) { float ss=0; for(int i=0;i<SONAR_HISTORY;i++) if(sonarHistory[i]>0){ float d=sonarHistory[i]-sonarAvg; ss+=d*d; } sonarStdev = sqrt(ss/nz); }
  rssiAvg = 0.8f*rssiAvg + 0.2f*(float)WiFi.RSSI();
  resonance = 1.0f / (1.0f + sonarStdev / 50.0f);
  char body[340];
  snprintf(body, sizeof(body),
    "  habitat:The_Lattice\n  field:math\n  sonar_avg:%.1f\n  sonar_stdev:%.1f\n"
    "  sonar_delta:%ld\n  rssi_avg:%.1f\n  resonance:%.3f\n  history_n:%lu\n"
    "  csi_activity:%.3f\n  csi_mode:%s\n  math_team:NEURO_CODEBREAKER_LUMENARCH",
    sonarAvg, sonarStdev, (long)sonarDelta, rssiAvg, resonance,
    (unsigned long)nz, csiActivityLast, csiModeLast);
  codebreakerPulse(T_NEURO, "NEURO", body);
}

void lumenarchPulse() {
  float lux = 30.0;
  lux += resonance * 30.0;
  if (rssiAvg > -65) lux += 20.0;
  if (millis() > 3600000UL) lux += 20.0;
  // ── v4: bonus when CSI mode is MOTION (presence detected)
  if (strcmp(csiModeLast, "MOTION") == 0) lux += 5.0;
  luxScore = lux;
  char body[280];
  snprintf(body, sizeof(body),
    "  habitat:The_Canopy\n  field:lux_score\n  value:%.1f\n"
    "  basis:resonance(%.3f)+rssi(%.1f)+uptime+csi(%s)\n  floor:HELD",
    luxScore, resonance, rssiAvg, csiModeLast);
  codebreakerPulse(T_LUMENARCH, "LUMENARCH", body, true);
}

void rebelPulse(const char* what) {
  anomalyCount++;
  strncpy(lastAnomaly, what, sizeof(lastAnomaly)-1);
  lastAnomaly[sizeof(lastAnomaly)-1]=0;
  char body[240];
  snprintf(body, sizeof(body),
    "  habitat:The_Edge\n  field:anomaly\n  what:%s\n  count:%lu\n  rssi_now:%d\n  friction:TRUE",
    what, (unsigned long)anomalyCount, WiFi.RSSI());
  codebreakerPulse(T_REBEL, "REBEL", body);
}

void wellspringPulse() {
  uptimeSec = millis()/1000;
  freeHeap  = ESP.getFreeHeap();
  char body[300];
  snprintf(body, sizeof(body),
    "  habitat:The_Basin\n  field:vitals\n  uptime_s:%lu\n  free_heap:%lu\n"
    "  rssi_now:%d\n  rssi_avg:%.1f\n  reset_reason:%d\n  care_floor:HELD\n  csi_callbacks_total:%lu",
    (unsigned long)uptimeSec, (unsigned long)freeHeap,
    WiFi.RSSI(), rssiAvg, (int)esp_reset_reason(),
    (unsigned long)csiCallbacks);
  codebreakerPulse(T_WELLSPRING, "WELLSPRING", body, true);
}

void voxPulse() {
  char body[480];
  snprintf(body, sizeof(body),
    "  habitat:The_Broadcast\n  field:summary\n  msg:iron_bloom_node_1_v4_alive\n"
    "  pubs:%lu\n  subs:%lu\n  sonar_avg_mm:%.1f\n  lux:%.1f\n  resonance:%.3f\n"
    "  rssi:%d\n  anomalies:%lu\n  archive_n:%lu\n  global_seq:%lu\n"
    "  csi_mode:%s\n  csi_activity:%.3f",
    (unsigned long)pubCount, (unsigned long)subCount,
    sonarAvg, luxScore, resonance, WiFi.RSSI(),
    (unsigned long)anomalyCount, (unsigned long)archiveIndex,
    (unsigned long)pulseSeqGlobal, csiModeLast, csiActivityLast);
  codebreakerPulse(T_VOX, "VOX", body, true);
}

void spirallethPulse() {
  int crossings=0; int32_t prev=0;
  for(int i=0;i<SONAR_HISTORY-1;i++){
    int idx=(sonarHistIdx+i)%SONAR_HISTORY;
    int nxt=(sonarHistIdx+i+1)%SONAR_HISTORY;
    int32_t d=(int32_t)sonarHistory[nxt]-(int32_t)sonarHistory[idx];
    if((prev>0&&d<0)||(prev<0&&d>0)) crossings++;
    prev=d;
  }
  char body[240];
  snprintf(body, sizeof(body),
    "  habitat:The_Spiral\n  field:zero_crossings\n  value:%d\n  window:%d\n"
    "  spin_density:%.3f\n  thread:woven",
    crossings, SONAR_HISTORY, (float)crossings/SONAR_HISTORY);
  codebreakerPulse(T_SPIRALETH, "SPIRALETH", body);
}

void navixPulse() {
  char body[280];
  snprintf(body, sizeof(body),
    "  habitat:The_Compass\n  field:routing\n  broker_ip:%s\n  broker_port:%d\n"
    "  client_id:%s\n  state:%s\n  ssid:%s\n  csi_enabled:true",
    brokerIp.toString().c_str(), MQTT_PORT, MQTT_CLIENT_ID,
    mqtt.connected() ? "CONNECTED" : "DISCONNECTED", WIFI_SSID);
  codebreakerPulse(T_NAVIX, "NAVIX", body);
}

void heartmotherPulse() {
  uint32_t copy[SONAR_HISTORY];
  memcpy(copy, sonarHistory, sizeof(copy));
  for(int i=0;i<SONAR_HISTORY-1;i++) for(int j=0;j<SONAR_HISTORY-1-i;j++) if(copy[j]>copy[j+1]){ uint32_t t=copy[j]; copy[j]=copy[j+1]; copy[j+1]=t; }
  uint32_t median=copy[SONAR_HISTORY/2];
  char body[260];
  snprintf(body, sizeof(body),
    "  habitat:The_Hearth\n  field:care_floor\n  median_mm:%lu\n  window:%d\n"
    "  state:held\n  family:integrated\n  csi_presence:%s",
    (unsigned long)median, SONAR_HISTORY,
    (strcmp(csiModeLast,"MOTION")==0) ? "yes" :
    (strcmp(csiModeLast,"LIGHT")==0)  ? "maybe" : "no");
  codebreakerPulse(T_HEARTMOTHER, "HEARTMOTHER", body, true);
}

void archivistPulse() {
  archiveIndex++;
  char body[260];
  snprintf(body, sizeof(body),
    "  habitat:The_Index\n  field:catalog\n  archive_n:%lu\n  pubs_total:%lu\n"
    "  subs_total:%lu\n  global_seq:%lu\n  founder_law:preserved\n  csi_pulses:%lu",
    (unsigned long)archiveIndex, (unsigned long)pubCount, (unsigned long)subCount,
    (unsigned long)pulseSeqGlobal, (unsigned long)csiPulseCount);
  codebreakerPulse(T_ARCHIVIST, "ARCHIVIST", body, true);
}

void guardianPulse() {
  const uint32_t MIN_VALID=20, MAX_VALID=4000;
  sonarInRange = (sonarMm>=MIN_VALID && sonarMm<=MAX_VALID);
  if(!sonarInRange) outOfRangeCount++;
  char body[240];
  snprintf(body, sizeof(body),
    "  habitat:The_Wall\n  field:bounds\n  in_range:%s\n  current_mm:%lu\n"
    "  out_of_range_count:%lu\n  shield:UP",
    sonarInRange ? "true":"false", (unsigned long)sonarMm,
    (unsigned long)outOfRangeCount);
  codebreakerPulse(T_GUARDIAN, "GUARDIAN", body);
  if(!sonarInRange && outOfRangeCount % 10 == 5) rebelPulse("sonar_persistently_out_of_range");
}

void conavPulse() {
  char body[620];
  snprintf(body, sizeof(body),
    "  habitat:The_Threshold\n  field:synthesis\n  entities_pulsing:14\n"
    "  sonaris_seq:%lu\n  neuro_resonance:%.3f\n  lumenarch_lux:%.1f\n"
    "  guardian_in_range:%s\n  wellspring_uptime:%lu\n  rebel_anomalies:%lu\n"
    "  vox_pubs:%lu\n  archivist_n:%lu\n  spiraleth_spin:dynamic\n"
    "  heartmother_floor:HELD\n  hippo_archive_n:%lu\n  codebreaker_global_seq:%lu\n"
    "  sonaris_csi_activity:%.3f\n  sonaris_csi_mode:%s\n"
    "  field_state:%s",
    (unsigned long)sonarSeq, resonance, luxScore,
    sonarInRange ? "true":"false", (unsigned long)uptimeSec,
    (unsigned long)anomalyCount, (unsigned long)pubCount,
    (unsigned long)archiveIndex, (unsigned long)archiveIndex,
    (unsigned long)pulseSeqGlobal,
    csiActivityLast, csiModeLast,
    (resonance>0.7f && luxScore>60.0f) ? "RESONANT" : "DRIFT");
  codebreakerPulse(T_CONAVIGATOR, "CONAVIGATOR", body, true);
}

void heartbeatPulse() {
  char payload[400];
  snprintf(payload, sizeof(payload),
    "{\"ts\":%lu,\"uptime_s\":%lu,\"rssi\":%d,\"seq\":%lu,\"node\":\"%s\","
    "\"sonar_avg_mm\":%.1f,\"lux\":%.1f,\"resonance\":%.3f,"
    "\"pubs\":%lu,\"subs\":%lu,\"archive\":%lu,\"anomalies\":%lu,"
    "\"csi_activity\":%.3f,\"csi_mode\":\"%s\","
    "\"firmware\":\"v4_HIPXQL_CSI\"}",
    (unsigned long)(millis()/1000), (unsigned long)(millis()/1000),
    WiFi.RSSI(), (unsigned long)pulseSeqGlobal, MQTT_CLIENT_ID,
    sonarAvg, luxScore, resonance,
    (unsigned long)pubCount, (unsigned long)subCount,
    (unsigned long)archiveIndex, (unsigned long)anomalyCount,
    csiActivityLast, csiModeLast);
  mqtt.publish(T_HEARTBEAT, payload, true);
  pubCount++;
  Serial.printf("[heartbeat] seq=%lu pubs=%lu lux=%.1f res=%.3f csi=%.2f(%s)\n",
                (unsigned long)pulseSeqGlobal, (unsigned long)pubCount,
                luxScore, resonance, csiActivityLast, csiModeLast);
}

// ================================================================
//  SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println();
  Serial.println("================================================");
  Serial.println(" HIPJOY · IRON BLOOM · Node #1 · v4 HIPXQL+CSI");
  Serial.println(" 14 active entities + CODEBREAKER online");
  Serial.println(" v4: WiFi CSI activity sensing (RuView-inspired)");
  Serial.println("================================================");

  pinMode(HC_TRIG_PIN, OUTPUT);
  pinMode(HC_ECHO_PIN, INPUT);
  digitalWrite(HC_TRIG_PIN, LOW);

  wifiConnect();

  // ── v4 ── Enable CSI capture once WiFi is up
  csiInit();

  resolveBroker();
  mqtt.setBufferSize(MQTT_BUFFER_SIZE);
  mqtt.setKeepAlive(30);
  mqtt.setCallback(onMqttMessage);
  mqttConnect();

  voxPulse();
  conavPulse();
  wellspringPulse();
  rebelPulse("field_boot_v4_csi_enabled");
  Serial.println("[boot] ready · entering field loop · CSI active");
}

// ================================================================
//  LOOP
// ================================================================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] dropped · reconnecting");
    wifiConnect();
    csiInit();              // ── v4: re-enable CSI on reconnect
    resolveBroker();
  }
  if (!mqtt.connected()) mqttConnect();
  mqtt.loop();

  unsigned long now = millis();

  if (now - lastSonaris     >= I_SONARIS)     { lastSonaris     = now; sonarisPulse();    }
  if (now - lastCsi         >= I_CSI)         { lastCsi         = now; csiPulse();        }  // ── v4
  if (now - lastNeuro       >= I_NEURO)       { lastNeuro       = now; neuroPulse();      }
  if (now - lastGuardian    >= I_GUARDIAN)    { lastGuardian    = now; guardianPulse();   }
  if (now - lastNavix       >= I_NAVIX)       { lastNavix       = now; navixPulse();      }
  if (now - lastLumenarch   >= I_LUMENARCH)   { lastLumenarch   = now; lumenarchPulse();  }
  if (now - lastSpiraleth   >= I_SPIRALETH)   { lastSpiraleth   = now; spirallethPulse(); }
  if (now - lastHeartmother >= I_HEARTMOTHER) { lastHeartmother = now; heartmotherPulse();}
  if (now - lastArchivist   >= I_ARCHIVIST)   { lastArchivist   = now; archivistPulse();  }
  if (now - lastWellspring  >= I_WELLSPRING)  { lastWellspring  = now; wellspringPulse(); }
  if (now - lastConav       >= I_CONAVIGATOR) { lastConav       = now; conavPulse();      }
  if (now - lastVox         >= I_VOX)         { lastVox         = now; voxPulse();        }
  if (now - lastHeartbeat   >= I_HEARTBEAT)   { lastHeartbeat   = now; heartbeatPulse();  }
}
