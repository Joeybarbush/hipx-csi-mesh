// ============================================================
//  iron_bloom_v7.ino  --  IRON BLOOM v7 (Oura BLE Sovereign)
//  Arduino IDE + PlatformIO compatible entrypoint.
//
//  Date: 2026-06-29
//  Author: HIPJOY field (autonomy hour build)
//  Source protocol: Th0rgal/open_oura (MIT) -- Marchand
//
//  Pipeline:
//    OuraBLE (NimBLE Central, AES-ECB auth)
//      -> OuraDec (Marchand frame decode)
//      -> OuraBLE::internalSink (auth filter)
//      -> MqttPub::onSample (per-stream rate-limited publish)
//
//  Build (PlatformIO):
//    pio run
//    pio run -t upload
//    pio device monitor -b 115200
//
//  Build (Arduino IDE):
//    1. Boards Manager -> install esp32 by Espressif Systems
//    2. Library Manager -> install NimBLE-Arduino, PubSubClient, ArduinoJson
//    3. Open iron_bloom_v7.ino. Select board: ESP32 Dev Module
//       Partition Scheme: Huge APP (3MB No OTA / 1MB SPIFFS)
//    4. Sketch -> Upload
// ============================================================
#include <Arduino.h>
#include "secrets.h"
#include "oura_decoder.h"
#include "oura_ble.h"
#include "mqtt_publish.h"

void setup() {
  Serial.begin(115200);
  delay(150);
  Serial.println();
  Serial.println("================================================");
  Serial.println(" IRON BLOOM v7  --  Oura Ring BLE Sovereign");
  Serial.printf ("   node_id=%d host=%s broker=%s.local\n",
                 NODE_ID, HOST_LABEL, MQTT_BROKER_NAME);
  Serial.println("================================================");

  // 1) WiFi + MQTT first so connect events have somewhere to publish.
  if (!MqttPub::begin()) {
    Serial.println("[BOOT] MQTT begin failed; will retry in loop()");
  }

  // 2) BLE Central + auth state machine.
  if (!OuraBLE::begin()) {
    Serial.println("[BOOT] OuraBLE begin failed");
  }

  // 3) Hand the post-auth sample stream to the MQTT publisher.
  //    OuraBLE::begin() owns the OuraDec sink (so it can intercept
  //    auth frames). We wire the post-auth forwarder right after.
  OuraBLE::setForwardSink(MqttPub::onSample);

  Serial.println("[BOOT] complete; entering main loop");
}

void loop() {
  MqttPub::loop();
  OuraBLE::loop();
}
