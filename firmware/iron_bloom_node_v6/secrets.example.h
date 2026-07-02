// secrets.example.h  --  IRON BLOOM v6
// -----------------------------------------------------------------
// Copy this file to "secrets.h" (same folder) and fill in your own
// values. secrets.h is gitignored so your credentials never get
// committed or published.
//
// REBEL FLOOR -- this firmware is for personal use, sovereign
// biometric capture, never push real MAC to public.
// -----------------------------------------------------------------
#pragma once

// --- WiFi + MQTT (from v5) ---------------------------------------
#define WIFI_SSID          "YOUR_WIFI_SSID"
#define WIFI_PASS          "YOUR_WIFI_PASSWORD"

// LAN IP of the PC running the MQTT broker (used if mDNS lookup fails)
#define MQTT_FALLBACK_IP   "192.168.1.100"

// --- Oura Ring 4 BLE (new in v6) ---------------------------------
// PLACEHOLDER MAC. The real ring MAC NEVER appears in committed code.
// Replace this in your local secrets.h only. secrets.h is gitignored.
//
// To find your ring's MAC the first time:
//   1. Put ring near the node
//   2. Flash this firmware with ORING_MAC defined as "" (empty)
//   3. Watch the serial monitor. Any BLE device advertising
//      "Oura" in its name will be logged with its MAC.
//   4. Copy that MAC into your local secrets.h.
//
// Format must be lowercase, colon-separated:  aa:bb:cc:dd:ee:ff
#define ORING_MAC          "AA:BB:CC:DD:EE:FF"

// Optional name fragment fallback if MAC unknown / changing.
// Most Oura rings advertise as "Oura" or "Oura Ring".
#define ORING_NAME_HINT    "Oura"

// RSSI floor for "ring is close enough to read" (dBm, more negative = farther)
#define ORING_RSSI_FLOOR   -90
