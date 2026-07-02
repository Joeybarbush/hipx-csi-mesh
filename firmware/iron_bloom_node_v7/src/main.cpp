// ============================================================
//  src/main.cpp  --  superseded by ../iron_bloom_v7.ino
//
//  This file ships only as a fallback for PlatformIO configurations
//  that explicitly disable .ino discovery. The real entrypoint is
//  iron_bloom_v7.ino at the project root; PlatformIO's
//  build_src_filter excludes this file in the default config.
//
//  If you delete iron_bloom_v7.ino and want to use main.cpp as the
//  sole entrypoint, remove the "-<src/main.cpp.disabled>" filter
//  in platformio.ini and rename this file to src/main.cpp.
// ============================================================
#if 0
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
  //    (OuraBLE::begin() takes ownership of the OuraDec sink during
  //    setup so it can intercept auth frames; we wire the downstream
  //    forwarder right after.)
  OuraBLE::setForwardSink(MqttPub::onSample);

  Serial.println("[BOOT] complete; entering main loop");
}

void loop() {
  MqttPub::loop();
  OuraBLE::loop();
}
#endif  // disabled fallback
