// secrets.example.h
// -----------------------------------------------------------------
// Copy this file to "secrets.h" (same folder) and fill in your own
// values. secrets.h is gitignored so your credentials never get
// committed or published.
// -----------------------------------------------------------------
#pragma once

#define WIFI_SSID          "YOUR_WIFI_SSID"
#define WIFI_PASS          "YOUR_WIFI_PASSWORD"

// LAN IP of the PC running the MQTT broker (used if mDNS lookup fails)
#define MQTT_FALLBACK_IP   "192.168.1.100"
