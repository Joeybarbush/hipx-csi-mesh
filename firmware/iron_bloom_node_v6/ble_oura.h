// ============================================================
//  ble_oura.h  --  IRON BLOOM v6
//  BLE Central scanner + Oura Ring 4 GATT reader
// ============================================================
//
//  REBEL FLOOR -- this firmware is for personal use, sovereign
//  biometric capture, never push real MAC to public.
//
//  HONESTY: Oura proprietary GATT services are NOT reverse-engineered
//  here. We read ONLY the standard Bluetooth SIG services that the
//  ring advertises (if any):
//
//    READABLE (when ring advertises them as a standard peripheral):
//      0x180A  Device Information     -> manufacturer, model, fw rev
//      0x180F  Battery Service        -> battery %
//      0x180D  Heart Rate Service     -> HR notifications (BPM, RR)
//
//    NOT READABLE (proprietary, would require reverse engineering --
//    NOT done here):
//      - HRV / SpO2 / temperature / sleep stages
//      - Historic sample buffers
//      - Anything Oura exposes only to their own app via custom UUIDs
//
//  If Oura Ring 4 does not advertise standard 0x180D in peripheral
//  mode (it often only does so transiently or while charging), HR
//  will silently come up empty and we log that honestly to MQTT.
//
//  Pairing: ONE-TIME, manual, with Joey's explicit consent. See README.
// ============================================================
#pragma once

#include <Arduino.h>
#include <PubSubClient.h>

// Topic root for ring-derived data (LOCAL broker only)
#define T_RING_RSSI       "hipjoy/wellspring/ring/rssi"
#define T_RING_BPM        "hipjoy/wellspring/ring/bpm"
#define T_RING_BATT       "hipjoy/wellspring/ring/battery"
#define T_RING_INFO       "hipjoy/wellspring/ring/device_info"
#define T_RING_PRESENCE   "hipjoy/wellspring/ring/presence"
#define T_RING_STATUS     "hipjoy/wellspring/ring/status"

// Standard Bluetooth SIG service UUIDs (16-bit, expanded to full 128)
#define BLE_SVC_DEVICE_INFO   "0000180a-0000-1000-8000-00805f9b34fb"
#define BLE_CHR_MANUFACTURER  "00002a29-0000-1000-8000-00805f9b34fb"
#define BLE_CHR_MODEL         "00002a24-0000-1000-8000-00805f9b34fb"
#define BLE_CHR_FW_REV        "00002a26-0000-1000-8000-00805f9b34fb"

#define BLE_SVC_BATTERY       "0000180f-0000-1000-8000-00805f9b34fb"
#define BLE_CHR_BATT_LEVEL    "00002a19-0000-1000-8000-00805f9b34fb"

#define BLE_SVC_HEART_RATE    "0000180d-0000-1000-8000-00805f9b34fb"
#define BLE_CHR_HR_MEAS       "00002a37-0000-1000-8000-00805f9b34fb"

// Init BLE Central. Pass the live MQTT client so we can publish.
// targetMac may be "" -- in that case we scan by name hint only and
// log MACs of candidates to serial (one-time discovery flow).
void bleOuraInit(PubSubClient* mqttRef,
                 const char* targetMac,
                 const char* nameHint,
                 int rssiFloor);

// Pump from main loop -- non-blocking. Drives scan/connect/read cycle.
void bleOuraTick();

// Lightweight status snapshot for the status heartbeat task.
struct BleOuraStatus {
  bool      enabled;
  bool      scanning;
  bool      connected;
  int       lastRssi;
  int       lastBpm;
  int       lastBattery;
  uint32_t  hrNotifications;
  uint32_t  reconnects;
  uint32_t  lastSeenMs;     // millis() when last advertisement was seen
};
BleOuraStatus bleOuraSnapshot();
