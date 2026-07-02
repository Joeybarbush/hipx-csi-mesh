// ============================================================
//  oura_decoder.cpp  --  Marchand frame decoder implementation
//  IRON BLOOM v7
// ============================================================
#include "oura_decoder.h"

namespace OuraDec {

  static SampleSink g_sink = nullptr;

  void setSink(SampleSink sink) {
    g_sink = sink;
  }

  void padNonce15(const uint8_t* in15, uint8_t out16[16]) {
    // PKCS#7 padding for an AES-128 block when input is 15 bytes:
    // pad with a single byte 0x01 (16 - 15 = 1).
    for (int i = 0; i < 15; i++) out16[i] = in15[i];
    out16[15] = 0x01;
  }

  AuthResult mapAuthResult(uint8_t v) {
    switch (v) {
      case 0x00: return AuthResult::OK;
      case 0x01: return AuthResult::BAD_KEY;
      case 0x0F: return AuthResult::NEEDS_ENCRYPTION;
      default:   return AuthResult::UNKNOWN;
    }
  }

  float tempC100ToC(int16_t v)   { return ((float)v) / 100.0f; }
  float accelI16ToG(int16_t v)   { return ((float)v) / 16384.0f; }

  // Little-endian readers.
  static inline int16_t  rd_i16(const uint8_t* p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
  }
  static inline uint16_t rd_u16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
  }
  static inline uint32_t rd_u32(const uint8_t* p) {
    return ((uint32_t)p[0])       |
           ((uint32_t)p[1] <<  8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
  }

  static void emit(const Sample& s) {
    if (g_sink) g_sink(s);
  }

  // Build a blank sample with metadata, ready to be filled and emitted.
  static Sample make(StreamKind k, const uint8_t* raw, size_t n) {
    Sample s{};
    s.kind    = k;
    s.ts_ms   = (uint32_t)millis();
    s.raw     = raw;
    s.raw_len = n;
    return s;
  }

  // ----------------------------------------------------------
  // Extended-op decoder for tag 0x2F frames.
  // Layout:  2F <len> <subop> <data...>
  // (Some responses also use 2F <len> <subop_hi> <subop_lo> <data...>;
  //  for the v7 subset we treat the byte after `len` as the sub-op.)
  // ----------------------------------------------------------
  static size_t decode_extended(const uint8_t* buf, size_t len) {
    if (len < 3) return 0;
    uint8_t subop = buf[2];
    // The byte at buf[3] is usually a 2-byte sub-tag pair on responses
    // (e.g. 2C for nonce, 2D for auth-req, 2E for auth-result, 25 for latest).
    uint8_t sub2  = (len >= 4) ? buf[3] : 0x00;
    const uint8_t* data = (len >= 4) ? &buf[4] : nullptr;
    size_t          dlen = (len >= 4) ? (len - 4) : 0;
    size_t emitted = 0;

    // --- AUTH NONCE -----------------------------------------
    // 2F 10 2C <15 bytes>
    if (subop == SUBOP_LATEST && sub2 == 0x2C && dlen >= 15) {
      Sample s = make(StreamKind::AUTH_NONCE, data, 15);
      emit(s); return 1;
    }

    // --- AUTH RESULT ----------------------------------------
    // 2F 02 2E <00|01|0F>
    if (subop == SUBOP_AUTH_RESULT && sub2 == 0x2E && dlen >= 1) {
      Sample s = make(StreamKind::AUTH_RESULT, data, 1);
      s.b0 = data[0];
      emit(s); return 1;
    }

    // --- FEATURE STATUS -------------------------------------
    // 2F 06 21 <feat> <mode> <status> <state> <sub>
    if (subop == SUBOP_FEAT_STATUS && sub2 == 0x21 && dlen >= 5) {
      // Not directly published. Cached by BLE module for diagnostics.
      return 0;
    }

    // --- LATEST <FEAT> response -----------------------------
    // 2F 10 25 <feat> <state> <payload...>
    if (subop == SUBOP_LATEST && sub2 == 0x25 && dlen >= 2) {
      uint8_t feat  = data[0];
      uint8_t state = data[1];
      (void)state;
      const uint8_t* p = data + 2;
      size_t plen = dlen - 2;

      switch (feat) {
        case FEAT_HR_DAYTIME:
        case FEAT_HR_RESTING: {
          if (plen >= 2) {
            Sample s = make(StreamKind::HR_LIVE, p, plen);
            s.i0 = rd_i16(p);                  // bpm
            s.b0 = (plen >= 3) ? p[2] : 0;     // quality
            emit(s); emitted++;
          }
        } break;

        case FEAT_SPO2: {
          if (plen >= 2) {
            Sample s = make(StreamKind::SPO2, p, plen);
            s.i0 = rd_i16(p);                  // pct (x1)
            s.b0 = (plen >= 3) ? p[2] : 0;     // perfusion / quality
            emit(s); emitted++;
          }
        } break;

        case FEAT_TEMP_SKIN: {
          if (plen >= 2) {
            Sample s = make(StreamKind::TEMP_SKIN, p, plen);
            s.i0 = rd_i16(p);                  // c x 100
            emit(s); emitted++;
          }
        } break;

        case FEAT_HRV_RMSSD: {
          if (plen >= 2) {
            Sample s = make(StreamKind::HRV_RMSSD, p, plen);
            s.i0 = rd_i16(p);                  // ms
            s.u0 = (plen >= 4) ? rd_u16(p + 2) : 300;  // window_s
            emit(s); emitted++;
          }
        } break;

        case FEAT_MOTION: {
          if (plen >= 3) {
            Sample s = make(StreamKind::MOTION_ORIENT, p, plen);
            s.b0 = p[0];                       // orient bitfield
            s.u0 = rd_u16(p + 1);              // magnitude
            emit(s); emitted++;
          }
        } break;

        case FEAT_SLEEP_MAD: {
          if (plen >= 2) {
            Sample s = make(StreamKind::SLEEP_MAD, p, plen);
            s.u0 = rd_u16(p);                  // movement-activity-deviation
            emit(s); emitted++;
          }
        } break;

        case FEAT_ACCEL_50HZ: {
          // payload: <i16 x><i16 y><i16 z> per sample, possibly batched.
          // Emit one sample per triplet.
          size_t n = plen / 6;
          for (size_t i = 0; i < n; i++) {
            const uint8_t* q = p + (i * 6);
            Sample s = make(StreamKind::ACCEL, q, 6);
            s.i0 = rd_i16(q);
            s.i1 = rd_i16(q + 2);
            s.i2 = rd_i16(q + 4);
            emit(s); emitted++;
          }
        } break;

        case FEAT_RAW_PPG: {
          // payload: array of i16 channel samples. Pass through opaque.
          Sample s = make(StreamKind::RAW_PPG, p, plen);
          s.u0 = (uint16_t)(plen / 2);         // channel count
          emit(s); emitted++;
        } break;

        case FEAT_EVENT: {
          Sample s = make(StreamKind::EVENT_TAG, p, plen);
          emit(s); emitted++;
        } break;

        default:
          // Unknown feature -- ignored. Diagnostics could log here.
          break;
      }
      return emitted;
    }

    return 0;
  }

  // ----------------------------------------------------------
  // Top-level decode: walks all frames in a notify chunk.
  // ----------------------------------------------------------
  size_t decode(const uint8_t* buf, size_t len) {
    if (!buf || len < 2) return 0;
    size_t off = 0;
    size_t emitted = 0;
    while (off + 2 <= len) {
      uint8_t tag = buf[off];
      uint8_t flen = buf[off + 1];
      if ((size_t)(off + 2 + flen) > len) break;        // malformed; drop
      const uint8_t* frame = &buf[off];
      size_t framelen = (size_t)(2 + flen);

      if (tag == TAG_EXTENDED) {
        emitted += decode_extended(frame, framelen);
      } else if (tag == TAG_BATTERY_RESP) {
        // 0D 06 <pct> <chg> <reco> <3 unk>
        if (flen >= 3) {
          Sample s = make(StreamKind::BATTERY, &buf[off + 2], flen);
          s.b0 = buf[off + 2];                          // pct
          s.b1 = (flen >= 2) ? buf[off + 3] : 0;        // charging
          s.b2 = (flen >= 3) ? buf[off + 4] : 0;        // recommended
          emit(s); emitted++;
        }
      } else if (tag == TAG_FW_RESP) {
        Sample s = make(StreamKind::FIRMWARE_INFO, &buf[off + 2], flen);
        emit(s); emitted++;
      } else {
        // Unknown short-form tag -- skip silently.
      }

      off += framelen;
    }
    return emitted;
  }

}  // namespace OuraDec
