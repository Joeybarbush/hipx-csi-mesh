// ============================================================
//  oura_ble.cpp  --  NimBLE Central + Marchand auth + GATT
//  IRON BLOOM v7
//
//  REBEL/GUARDIAN floor:
//    - ORING_AUTH_KEY never appears in this file. We dereference
//      the externally-provided 16-byte array from secrets.h only
//      inside the auth handshake (mbedtls_aes_setkey_enc), and
//      we never log it.
//    - ORING_MAC is read from secrets.h via the #define; not echoed.
// ============================================================
#include "oura_ble.h"
#include "oura_decoder.h"
#include "secrets.h"

#include <NimBLEDevice.h>
#include <mbedtls/aes.h>

namespace OuraBLE {

  // ---- UUIDs (Marchand) ----
  const char* SVC_UUID         = "98ED0001-A541-11E4-B6A0-0002A5D5C51B";
  const char* CHR_NOTIFY_UUID  = "98ED0003-A541-11E4-B6A0-0002A5D5C51B";
  const char* CHR_WRITE_UUID   = "98ED0002-A541-11E4-B6A0-0002A5D5C51B";

  static const uint16_t TARGET_MTU = 203;

  // ---- State ----
  static State           g_state          = State::IDLE;
  static NimBLEClient*   g_client         = nullptr;
  static NimBLERemoteCharacteristic* g_chrNotify = nullptr;
  static NimBLERemoteCharacteristic* g_chrWrite  = nullptr;
  static NimBLEAddress   g_target         = NimBLEAddress(ORING_MAC, BLE_ADDR_PUBLIC);

  static uint32_t        g_lastNotifyMs   = 0;
  static uint32_t        g_lastPollMs     = 0;
  static uint32_t        g_lastBeatMs     = 0;
  static uint32_t        g_attempts       = 0;
  static uint32_t        g_authFails      = 0;
  static uint32_t        g_backoffMs      = 1000;
  static uint32_t        g_backoffUntil   = 0;

  // ---- Auth state ----
  static bool            g_haveNonce      = false;
  static uint8_t         g_nonce[15];

  // ---- Pre-built command frames (Marchand) ----
  static const uint8_t CMD_GET_NONCE[3]   = { 0x2F, 0x01, 0x2B };
  static const uint8_t CMD_BATTERY[2]     = { 0x0C, 0x00 };
  static const uint8_t CMD_HR_DAYTIME[4]  = { 0x2F, 0x02, 0x24, 0x02 };
  static const uint8_t CMD_SPO2[4]        = { 0x2F, 0x02, 0x24, 0x04 };
  static const uint8_t CMD_HR_RESTING[4]  = { 0x2F, 0x02, 0x24, 0x08 };
  static const uint8_t CMD_TEMP_SKIN[4]   = { 0x2F, 0x02, 0x24, 0x0A };
  static const uint8_t CMD_HRV_RMSSD[4]   = { 0x2F, 0x02, 0x24, 0x0C };
  static const uint8_t CMD_MOTION[4]      = { 0x2F, 0x02, 0x24, 0x12 };
  static const uint8_t CMD_SLEEP_MAD[4]   = { 0x2F, 0x02, 0x24, 0x14 };
  static const uint8_t CMD_NOTIFY_ALL[3]  = { 0x1C, 0x01, 0x3F };

  // ---- Polling cadence (ms) ----
  struct Poll {
    const uint8_t* frame;
    uint8_t        len;
    uint32_t       periodMs;
    uint32_t       lastMs;
  };
  static Poll g_polls[] = {
    { CMD_HR_DAYTIME, sizeof(CMD_HR_DAYTIME),     1000,    0 },  // 1 Hz
    { CMD_SPO2,       sizeof(CMD_SPO2),           5000,    0 },  // 0.2 Hz
    { CMD_TEMP_SKIN,  sizeof(CMD_TEMP_SKIN),     60000,    0 },  // 1/60 Hz
    { CMD_HRV_RMSSD,  sizeof(CMD_HRV_RMSSD),    300000,    0 },  // 1/300 Hz
    { CMD_MOTION,     sizeof(CMD_MOTION),          200,    0 },  // 5 Hz
    { CMD_SLEEP_MAD,  sizeof(CMD_SLEEP_MAD),    60000,    0 },  // 1/60 Hz
    { CMD_HR_RESTING, sizeof(CMD_HR_RESTING),   120000,    0 },  // 1/120 Hz
    { CMD_BATTERY,    sizeof(CMD_BATTERY),      120000,    0 },  // 1/120 Hz
  };
  static const size_t g_pollCount = sizeof(g_polls) / sizeof(g_polls[0]);

  // ---- Helpers ----
  static void setState(State s) {
    g_state = s;
  }

  static void scheduleBackoff() {
    g_backoffUntil = millis() + g_backoffMs;
    // Exponential backoff capped at 30s, per patch plan.
    g_backoffMs *= 2;
    if (g_backoffMs > 30000) g_backoffMs = 30000;
    setState(State::BACKOFF);
  }

  static void resetBackoff() {
    g_backoffMs = 1000;
    g_backoffUntil = 0;
  }

  static bool writeChar(const uint8_t* data, size_t len) {
    if (!g_chrWrite) return false;
    // Write-without-response (Marchand uses unack writes); NimBLE: writeValue(..., false)
    return g_chrWrite->writeValue(const_cast<uint8_t*>(data), len, false);
  }

  // ----------------------------------------------------------
  // Auth handshake (Marchand): AES-128-ECB(nonce15 || 0x01)
  // ----------------------------------------------------------
  static bool sendAuthResponse() {
    if (!g_haveNonce) return false;

    uint8_t padded[16];
    OuraDec::padNonce15(g_nonce, padded);

    uint8_t enc[16];
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    // ORING_AUTH_KEY comes from secrets.h. We pass the pointer
    // straight to mbedtls. The bytes are never read into a logged
    // buffer; they only live in stack-resident mbedtls state during
    // setkey/crypt/free.
    if (mbedtls_aes_setkey_enc(&ctx, ORING_AUTH_KEY, 128) != 0) {
      mbedtls_aes_free(&ctx);
      Serial.println("[OURA] AES setkey failed");
      return false;
    }
    int rc = mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, padded, enc);
    mbedtls_aes_free(&ctx);
    if (rc != 0) {
      Serial.println("[OURA] AES encrypt failed");
      return false;
    }

    uint8_t frame[3 + 16];
    frame[0] = 0x2F;
    frame[1] = 0x11;
    frame[2] = 0x2D;
    memcpy(frame + 3, enc, 16);
    bool ok = writeChar(frame, sizeof(frame));

    // Wipe transient material from stack.
    memset(padded, 0, sizeof(padded));
    memset(enc,    0, sizeof(enc));
    return ok;
  }

  // ----------------------------------------------------------
  // Decoder sink hook -- intercepts auth frames before forwarding
  // to the external MQTT sink.
  // ----------------------------------------------------------
  static OuraDec::SampleSink g_externalSink = nullptr;

  static void internalSink(const OuraDec::Sample& s) {
    switch (s.kind) {
      case OuraDec::StreamKind::AUTH_NONCE: {
        if (s.raw_len == 15) {
          memcpy(g_nonce, s.raw, 15);
          g_haveNonce = true;
          Serial.println("[OURA] nonce received, sending auth response");
          if (!sendAuthResponse()) {
            Serial.println("[OURA] auth response write failed");
            scheduleBackoff();
          }
        }
        return;  // do not forward auth frames downstream
      }
      case OuraDec::StreamKind::AUTH_RESULT: {
        OuraDec::AuthResult r = OuraDec::mapAuthResult(s.b0);
        if (r == OuraDec::AuthResult::OK) {
          Serial.println("[OURA] AUTH OK");
          setState(State::READY);
          resetBackoff();
        } else if (r == OuraDec::AuthResult::BAD_KEY) {
          Serial.println("[OURA] AUTH FAIL 01 -- bad key, will not retry until reflashed");
          g_authFails++;
          setState(State::FAIL);
        } else if (r == OuraDec::AuthResult::NEEDS_ENCRYPTION) {
          Serial.println("[OURA] AUTH FAIL 0F -- needs BLE encryption, attempting pair");
          if (g_client) g_client->secureConnection();
        } else {
          Serial.printf("[OURA] AUTH UNKNOWN 0x%02X\n", s.b0);
          g_authFails++;
          scheduleBackoff();
        }
        return;
      }
      default:
        break;
    }
    // Forward sample data to MQTT layer.
    if (g_externalSink) g_externalSink(s);
  }

  // ----------------------------------------------------------
  // NimBLE callbacks
  // ----------------------------------------------------------
  class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* c) override {
      Serial.println("[OURA] BLE connected");
      c->setMTU(TARGET_MTU);
    }
    void onDisconnect(NimBLEClient* c, int reason) override {
      Serial.printf("[OURA] BLE disconnected reason=%d\n", reason);
      g_haveNonce  = false;
      g_chrNotify  = nullptr;
      g_chrWrite   = nullptr;
      scheduleBackoff();
    }
    uint32_t onPassKeyRequest() override { return 000000; }
    bool     onConfirmPIN(uint32_t)      { return true; }
    void     onAuthenticationComplete(NimBLEConnInfo& info) override {
      Serial.printf("[OURA] BLE pairing complete encrypted=%d bonded=%d\n",
                    info.isEncrypted(), info.isBonded());
    }
  };
  static ClientCallbacks g_clientCbs;

  static void notifyCb(NimBLERemoteCharacteristic* /*chr*/,
                       uint8_t* data,
                       size_t   len,
                       bool     /*isNotify*/) {
    g_lastNotifyMs = millis();
    OuraDec::decode(data, len);
  }

  class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
      if (!dev) return;
      if (dev->getAddress() == g_target) {
        Serial.println("[OURA] target found, stopping scan");
        NimBLEDevice::getScan()->stop();
      }
    }
  };
  static ScanCallbacks g_scanCbs;

  // ----------------------------------------------------------
  // Connect + GATT discover + auth start
  // ----------------------------------------------------------
  static bool connectAndAuth() {
    if (!g_client) {
      g_client = NimBLEDevice::createClient();
      g_client->setClientCallbacks(&g_clientCbs, false);
      g_client->setConnectionParams(12, 12, 0, 200);
      g_client->setConnectTimeout(8);
    }

    setState(State::CONNECTING);
    g_attempts++;
    Serial.println("[OURA] connecting to target");
    if (!g_client->connect(g_target, true)) {
      Serial.println("[OURA] connect failed");
      return false;
    }

    // Discover Oura service
    NimBLERemoteService* svc = g_client->getService(SVC_UUID);
    if (!svc) {
      Serial.println("[OURA] Oura service not found");
      g_client->disconnect();
      return false;
    }
    g_chrWrite  = svc->getCharacteristic(CHR_WRITE_UUID);
    g_chrNotify = svc->getCharacteristic(CHR_NOTIFY_UUID);
    if (!g_chrWrite || !g_chrNotify) {
      Serial.println("[OURA] required characteristics missing");
      g_client->disconnect();
      return false;
    }
    if (!g_chrNotify->canNotify()) {
      Serial.println("[OURA] notify char does not support notify");
      g_client->disconnect();
      return false;
    }
    if (!g_chrNotify->subscribe(true, notifyCb)) {
      Serial.println("[OURA] subscribe failed");
      g_client->disconnect();
      return false;
    }

    setState(State::AUTHING);
    g_haveNonce = false;
    Serial.println("[OURA] subscribed; requesting auth nonce");
    if (!writeChar(CMD_GET_NONCE, sizeof(CMD_GET_NONCE))) {
      Serial.println("[OURA] nonce request write failed");
      g_client->disconnect();
      return false;
    }
    // READY transition happens asynchronously inside internalSink()
    // once 0x2F 02 2E 00 arrives.
    return true;
  }

  // ----------------------------------------------------------
  // Public API
  // ----------------------------------------------------------
  bool begin() {
    NimBLEDevice::init("iron-bloom-v7-oura");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setMTU(TARGET_MTU);

    // Capture the external sink that main has already registered with the
    // decoder, then re-route through internalSink() so auth frames are
    // intercepted here.
    g_externalSink = nullptr;   // main registers via setSink BEFORE begin()
    OuraDec::setSink(internalSink);

    setState(State::IDLE);
    Serial.println("[OURA] BLE module begin OK");
    return true;
  }

  // Lets main re-route the post-auth sample stream out to MQTT.
  // Must be called AFTER OuraBLE::begin() (which swallows the prior sink).
  // We expose it via a small forwarder defined below.
  void setForwardSink(OuraDec::SampleSink sink) {
    g_externalSink = sink;
  }

  void loop() {
    uint32_t now = millis();

    // 1) If FAIL: park here. Joey reflashes with corrected key.
    if (g_state == State::FAIL) return;

    // 2) If BACKOFF: wait until window passes, then return to IDLE.
    if (g_state == State::BACKOFF) {
      if (now >= g_backoffUntil) setState(State::IDLE);
      return;
    }

    // 3) If IDLE: kick a connect attempt (no scan required when MAC known).
    if (g_state == State::IDLE) {
      if (!connectAndAuth()) {
        if (g_client && g_client->isConnected()) g_client->disconnect();
        scheduleBackoff();
      }
      return;
    }

    // 4) If READY: drive polling cadence.
    if (g_state == State::READY) {
      // Watchdog: if no notify for 15s, disconnect and reconnect.
      if (g_lastNotifyMs && (now - g_lastNotifyMs) > 15000) {
        Serial.println("[OURA] notify watchdog tripped, disconnecting");
        disconnect();
        scheduleBackoff();
        return;
      }
      for (size_t i = 0; i < g_pollCount; i++) {
        Poll& p = g_polls[i];
        if (now - p.lastMs >= p.periodMs) {
          p.lastMs = now;
          if (!writeChar(p.frame, p.len)) {
            Serial.printf("[OURA] poll write %u failed\n", (unsigned)i);
          }
          // Stagger writes so we never queue more than one per loop tick.
          break;
        }
      }
    }
  }

  State currentState() { return g_state; }

  const char* stateName() {
    switch (g_state) {
      case State::IDLE:       return "IDLE";
      case State::SCANNING:   return "SCANNING";
      case State::CONNECTING: return "CONNECTING";
      case State::PAIRING:    return "PAIRING";
      case State::AUTHING:    return "AUTHING";
      case State::READY:      return "READY";
      case State::BACKOFF:    return "BACKOFF";
      case State::FAIL:       return "FAIL";
    }
    return "?";
  }

  uint32_t lastNotifyMs()   { return g_lastNotifyMs; }
  uint32_t connectAttempts(){ return g_attempts; }
  uint32_t authFailures()   { return g_authFails; }
  bool     isReady()        { return g_state == State::READY; }

  void disconnect() {
    if (g_client && g_client->isConnected()) {
      g_client->disconnect();
    }
    g_chrNotify = nullptr;
    g_chrWrite  = nullptr;
    g_haveNonce = false;
  }

}  // namespace OuraBLE

// ---- Forwarder symbol (declared in oura_ble.h as setForwardSink) ----
// Kept out of the namespace header to avoid making setSink callable
// before begin(). main calls OuraBLE::setForwardSink(...) after begin().
namespace OuraBLE { void setForwardSink(OuraDec::SampleSink sink); }
