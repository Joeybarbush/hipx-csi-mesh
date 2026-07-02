// ============================================================
//  oura_decoder.h  --  Marchand frame decoder for Oura Ring 4
//  IRON BLOOM v7
//  Date: 2026-06-29
//
//  Source: Th0rgal/open_oura (MIT) - protocol notes verified
//  against Ring 3 Horizon FW 3.4.3 on 2026-06-21.
//
//  Framing: <tag> <length> <payload...>  (little-endian)
//  Extended ops: tag = 0x2F, first payload byte is sub-op.
//
//  This module is pure parsing -- no BLE, no MQTT. The BLE
//  module pushes raw notify buffers in; the decoder pushes
//  normalized samples out via a callback.
// ============================================================
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

namespace OuraDec {

  // ---- Stream identifiers (1:1 with MQTT topic list) ----
  enum class StreamKind : uint8_t {
    UNKNOWN = 0,
    AUTH_NONCE,        // 0x2F10 2C <15 bytes>
    AUTH_RESULT,       // 0x2F02 2E <00|01|0F>
    HR_LIVE,           // i16 bpm
    HRV_RMSSD,         // i16 ms (5-min window)
    TEMP_SKIN,         // i16 (c x 100)
    SPO2,              // i16 pct + perfusion
    MOTION_ORIENT,     // u8 bitfield + magnitude
    SLEEP_MAD,         // u16 movement activity deviation
    BATTERY,           // u8 pct, charging, recommended
    ACCEL,             // i16 x3 (~50Hz)
    RAW_PPG,           // optional, heavy
    EVENT_TAG,         // historical/event marker
    FIRMWARE_INFO      // API/FW/BT/MAC
  };

  // ---- Normalized sample produced by decode() ----
  struct Sample {
    StreamKind kind;
    uint32_t   ts_ms;          // local millis() at receipt
    // numeric channels (use the ones relevant to `kind`)
    int32_t    i0, i1, i2;
    uint16_t   u0, u1;
    uint8_t    b0, b1, b2;
    // raw payload pointer for downstream (e.g. raw PPG, event)
    const uint8_t* raw;
    size_t         raw_len;
  };

  // ---- Auth result codes (from 0x2F02 2E XX) ----
  enum class AuthResult : uint8_t {
    OK              = 0x00,
    BAD_KEY         = 0x01,
    NEEDS_ENCRYPTION= 0x0F,
    UNKNOWN         = 0xFF
  };

  // ---- Sub-op tags relevant to v7 (subset) ----
  // (See oura_v7_PATCH_PLAN.md section 1 for full table.)
  static constexpr uint8_t TAG_EXTENDED      = 0x2F;
  static constexpr uint8_t SUBOP_NONCE       = 0x10;   // payload byte after length
  static constexpr uint8_t SUBOP_AUTH_RESULT = 0x02;
  static constexpr uint8_t SUBOP_FEAT_STATUS = 0x06;
  static constexpr uint8_t SUBOP_LATEST      = 0x10;   // 0x2F10 25 ... (HR/SpO2 latest)

  static constexpr uint8_t FEAT_HR_DAYTIME   = 0x02;
  static constexpr uint8_t FEAT_SPO2         = 0x04;
  static constexpr uint8_t FEAT_HR_RESTING   = 0x08;
  static constexpr uint8_t FEAT_TEMP_SKIN    = 0x0A;
  static constexpr uint8_t FEAT_HRV_RMSSD    = 0x0C;
  static constexpr uint8_t FEAT_MOTION       = 0x12;
  static constexpr uint8_t FEAT_SLEEP_MAD    = 0x14;
  static constexpr uint8_t FEAT_BATTERY      = 0x20;
  static constexpr uint8_t FEAT_ACCEL_50HZ   = 0x30;   // optional stream
  static constexpr uint8_t FEAT_RAW_PPG      = 0x40;   // optional stream
  static constexpr uint8_t FEAT_EVENT        = 0x50;

  static constexpr uint8_t TAG_BATTERY_RESP  = 0x0D;
  static constexpr uint8_t TAG_FW_RESP       = 0x09;

  // ---- Callback delivered by BLE module on every notify chunk ----
  typedef void (*SampleSink)(const Sample& s);

  // Set the sink. Called once at startup from setup().
  void setSink(SampleSink sink);

  // Parse one notify packet. May yield 0..N samples via the sink.
  // Returns number of samples emitted.
  size_t decode(const uint8_t* buf, size_t len);

  // ---- Helpers ----
  // PKCS#5/7 pad a 15-byte nonce to 16 bytes (pad = 0x01 single byte).
  void  padNonce15(const uint8_t* in15, uint8_t out16[16]);
  // Translate an auth result byte to enum.
  AuthResult mapAuthResult(uint8_t v);
  // Convert raw skin-temp word (units of 0.01 deg C) to float.
  float tempC100ToC(int16_t v);
  // Convert raw accel i16 to gravity units (Marchand: 1g == 16384).
  float accelI16ToG(int16_t v);

}  // namespace OuraDec
