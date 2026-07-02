// ============================================================
//  oura_ble.h  --  NimBLE Central + AES-ECB auth + GATT polling
//  IRON BLOOM v7
//  Date: 2026-06-29
//
//  Connects to the Oura Ring 4 by MAC (ORING_MAC), performs the
//  Marchand auth handshake using ORING_AUTH_KEY (mbedtls AES-128-ECB),
//  subscribes to the notify characteristic, and pumps the polling
//  cadence on its loop() tick.
//
//  Frames flow:  notify -> OuraBLE::onNotify -> OuraDec::decode
//                       -> Sample -> sink (set externally to MQTT)
// ============================================================
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "oura_decoder.h"

namespace OuraBLE {

  // Set the downstream sink that receives post-auth Sample frames.
  // Must be called AFTER begin(); the auth handshake frames are
  // intercepted inside oura_ble.cpp and never forwarded.
  void setForwardSink(OuraDec::SampleSink sink);


  enum class State : uint8_t {
    IDLE     = 0,
    SCANNING,
    CONNECTING,
    PAIRING,
    AUTHING,
    READY,
    BACKOFF,
    FAIL
  };

  // Service + characteristic UUIDs (Marchand).
  extern const char* SVC_UUID;
  extern const char* CHR_NOTIFY_UUID;
  extern const char* CHR_WRITE_UUID;

  // Initialize NimBLE in Central role, install scan/connect callbacks,
  // wire the decoder sink. Call once from setup().
  bool begin();

  // Pump the state machine. Call frequently from loop().
  // Handles scan->connect, auth state machine, polling cadence,
  // reconnect with exponential backoff.
  void loop();

  // Snapshot diagnostics.
  State currentState();
  const char* stateName();
  uint32_t lastNotifyMs();
  uint32_t connectAttempts();
  uint32_t authFailures();
  bool     isReady();

  // Force-disconnect (used by recovery paths / OTA).
  void disconnect();

}  // namespace OuraBLE
