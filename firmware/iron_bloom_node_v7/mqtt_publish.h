// ============================================================
//  mqtt_publish.h  --  WiFi + mDNS broker resolution + per-stream
//                      MQTT publish for IRON BLOOM v7 Oura bridge
//  Date: 2026-06-29
//
//  Topic plan (mirror of oura_v7_PATCH_PLAN.md section 4) but
//  the dispatch lives at hipjoy/field/oura/* per dispatch order:
//
//    hipjoy/field/oura/accel              (heavy, gated)
//    hipjoy/field/oura/hr_live
//    hipjoy/field/oura/hrv
//    hipjoy/field/oura/temp_skin
//    hipjoy/field/oura/spo2
//    hipjoy/field/oura/motion_orientation
//    hipjoy/field/oura/sleep_mad
//    hipjoy/field/oura/battery
//    hipjoy/field/oura/raw_ppg            (optional, OFF default)
//
//  Payload envelope (v6-compatible):
//    { ts, value | {...fields}, node_id, seq, crc32 }
// ============================================================
#pragma once
#include <Arduino.h>
#include "oura_decoder.h"

namespace MqttPub {

  // WiFi + MQTT bring-up. Non-blocking after first attempt.
  bool begin();

  // Pump WiFi/MQTT keepalive. Call frequently from loop().
  void loop();

  // Sample sink for OuraDec / OuraBLE. Walks rate-limited dispatch
  // and emits one MQTT publish per relevant topic.
  void onSample(const OuraDec::Sample& s);

  // Heartbeat: 1 Hz status frame on hipjoy/field/oura/status with
  // BLE state, MQTT state, uptime, seq, last_notify_age_ms.
  void heartbeat();

  // Diagnostics.
  bool isConnected();
  uint32_t publishedCount();
  uint32_t droppedCount();
  uint32_t lastSeq();
}
