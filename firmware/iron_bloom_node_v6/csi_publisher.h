// ============================================================
//  csi_publisher.h  --  IRON BLOOM v6
//  WiFi CSI capture, activity scoring, MQTT publish.
//  Lifted from v5 (preserved verbatim in spirit, modularized).
//  Founder Law 1: v5 firmware not deleted, v6 sits beside it.
// ============================================================
#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"

#define CSI_T_SONARIS_CSI  "hipjoy/sonaris/wifi_csi"
// NODE_ID is defined in the main .ino before this header is included.

// CSI accumulator state (ISR-writes, main-loop reads).
namespace csi {
  extern volatile uint32_t samples;
  extern volatile float    ampSum;
  extern volatile float    ampSumSq;
  extern volatile float    ampMax;
  extern volatile uint32_t callbacks;

  extern float       activityLast;
  extern float       meanLast;
  extern const char* modeLast;
  extern uint32_t    pulseCount;
}

void csiInit();
void csiPulse(PubSubClient* mqtt, int nodeId, const char* roomLabel);

// Inline definitions (header-only to keep build simple).
namespace csi {
  volatile uint32_t samples    = 0;
  volatile float    ampSum     = 0.0f;
  volatile float    ampSumSq   = 0.0f;
  volatile float    ampMax     = 0.0f;
  volatile uint32_t callbacks  = 0;

  float       activityLast = 0.0f;
  float       meanLast     = 0.0f;
  const char* modeLast     = "BOOT";
  uint32_t    pulseCount   = 0;
}

extern "C" void IRAM_ATTR csiCallback(void *ctx, wifi_csi_info_t *info) {
  if (!info || !info->buf || info->len < 4) return;
  int8_t *data = (int8_t *)info->buf;
  int pairs = info->len / 2;
  if (pairs > 128) pairs = 128;
  float sum = 0.0f, maxa = 0.0f;
  for (int i = 0; i < pairs; i++) {
    int8_t re = data[i*2];
    int8_t im = data[i*2 + 1];
    float a = sqrtf((float)(re*re + im*im));
    sum += a;
    if (a > maxa) maxa = a;
  }
  float avgAmp = (pairs > 0) ? (sum / pairs) : 0.0f;
  csi::samples++;
  csi::ampSum   += avgAmp;
  csi::ampSumSq += avgAmp * avgAmp;
  if (maxa > csi::ampMax) csi::ampMax = maxa;
  csi::callbacks++;
}

inline void csiInit() {
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
  Serial.printf("[csi] init cfg=%d cb=%d en=%d\n",
                (int)r1, (int)r2, (int)r3);
}

inline void csiPulse(PubSubClient* mqtt, int nodeId, const char* roomLabel) {
  uint32_t n     = csi::samples;
  float    sum   = csi::ampSum;
  float    sumsq = csi::ampSumSq;
  float    maxa  = csi::ampMax;
  uint32_t cbs   = csi::callbacks;

  csi::samples   = 0;
  csi::ampSum    = 0.0f;
  csi::ampSumSq  = 0.0f;
  csi::ampMax    = 0.0f;
  csi::pulseCount++;

  char body[400];
  if (n < 2) {
    csi::modeLast = "QUIET";
    snprintf(body, sizeof(body),
      "{\"ts\":%lu,\"node_id\":%d,\"room\":\"%s\",\"samples\":%lu,"
      "\"callbacks\":%lu,\"mode\":\"QUIET\",\"status\":\"waiting\"}",
      (unsigned long)(millis()/1000), nodeId, roomLabel,
      (unsigned long)n, (unsigned long)cbs);
    if (mqtt && mqtt->connected()) {
      mqtt->publish(CSI_T_SONARIS_CSI, body, false);
    }
    return;
  }
  float mean = sum / n;
  float var  = (sumsq / n) - (mean * mean);
  if (var < 0) var = 0;
  float stddev = sqrtf(var);
  float activity = stddev;
  if (activity > 10.0f) activity = 10.0f;
  csi::activityLast = activity;
  csi::meanLast     = mean;
  if      (activity > 3.0f) csi::modeLast = "MOTION";
  else if (activity > 1.0f) csi::modeLast = "LIGHT";
  else                       csi::modeLast = "STATIC";

  snprintf(body, sizeof(body),
    "{\"ts\":%lu,\"node_id\":%d,\"room\":\"%s\",\"samples\":%lu,"
    "\"callbacks\":%lu,\"mean\":%.2f,\"max\":%.2f,\"stddev\":%.2f,"
    "\"activity\":%.3f,\"mode\":\"%s\",\"pulse_n\":%lu}",
    (unsigned long)(millis()/1000), nodeId, roomLabel,
    (unsigned long)n, (unsigned long)cbs,
    mean, maxa, stddev, activity, csi::modeLast,
    (unsigned long)csi::pulseCount);
  if (mqtt && mqtt->connected()) {
    mqtt->publish(CSI_T_SONARIS_CSI, body, false);
  }
}
