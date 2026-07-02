// ============================================================
//  ble_oura.cpp  --  IRON BLOOM v6
//  BLE Central scanner + Oura Ring 4 GATT reader
//
//  REBEL FLOOR -- this firmware is for personal use, sovereign
//  biometric capture, never push real MAC to public.
//
//  HONESTY:
//    - We read standard Bluetooth SIG services ONLY.
//    - 0x180D (Heart Rate) may not be advertised by Oura Ring 4 in
//      typical operation; the ring is primarily a peripheral to
//      Oura's own app over their proprietary protocol. If 0x180D
//      is absent, we still publish RSSI + presence + battery (if
//      0x180F is exposed), and log the honest gap to MQTT.
//    - HRV, SpO2, temperature, sleep stages are NOT readable from
//      this firmware. They live behind Oura's proprietary UUIDs
//      which we explicitly do not reverse engineer here.
// ============================================================
#include "ble_oura.h"
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>

static PubSubClient*    s_mqtt          = nullptr;
static String           s_targetMac     = "";
static String           s_nameHint      = "Oura";
static int              s_rssiFloor     = -90;

static BLEScan*         s_scan          = nullptr;
static BLEClient*       s_client        = nullptr;
static BLEAddress*      s_ringAddr      = nullptr;
static bool             s_connecting    = false;
static bool             s_connected     = false;
static uint32_t         s_lastSeenMs    = 0;
static uint32_t         s_lastScanStart = 0;
static uint32_t         s_lastPresence  = 0;
static bool             s_presentNow    = false;

static BleOuraStatus    s_status        = {};

static void publishPresence(bool present) {
  if (!s_mqtt || !s_mqtt->connected()) return;
  char body[160];
  snprintf(body, sizeof(body),
    "{\"ts\":%lu,\"present\":%s,\"node\":\"iron-bloom-v6\",\"rssi\":%d}",
    (unsigned long)(millis()/1000),
    present ? "true" : "false",
    s_status.lastRssi);
  s_mqtt->publish(T_RING_PRESENCE, body, true);
}

static void publishRssi(int rssi) {
  if (!s_mqtt || !s_mqtt->connected()) return;
  char body[140];
  snprintf(body, sizeof(body),
    "{\"ts\":%lu,\"rssi\":%d}",
    (unsigned long)(millis()/1000), rssi);
  s_mqtt->publish(T_RING_RSSI, body, false);
}

static void publishBpm(int bpm, int rssi) {
  if (!s_mqtt || !s_mqtt->connected()) return;
  char body[160];
  snprintf(body, sizeof(body),
    "{\"ts\":%lu,\"bpm\":%d,\"rssi\":%d,\"source\":\"gatt_0x2A37\"}",
    (unsigned long)(millis()/1000), bpm, rssi);
  s_mqtt->publish(T_RING_BPM, body, false);
}

static void publishBattery(int pct) {
  if (!s_mqtt || !s_mqtt->connected()) return;
  char body[120];
  snprintf(body, sizeof(body),
    "{\"ts\":%lu,\"battery_pct\":%d}",
    (unsigned long)(millis()/1000), pct);
  s_mqtt->publish(T_RING_BATT, body, true);
}

static void publishDeviceInfo(const char* manu,
                              const char* model,
                              const char* fw) {
  if (!s_mqtt || !s_mqtt->connected()) return;
  char body[260];
  snprintf(body, sizeof(body),
    "{\"ts\":%lu,\"manufacturer\":\"%s\",\"model\":\"%s\",\"fw\":\"%s\"}",
    (unsigned long)(millis()/1000),
    manu ? manu : "",
    model ? model : "",
    fw ? fw : "");
  s_mqtt->publish(T_RING_INFO, body, true);
}

static void publishStatus(const char* what) {
  if (!s_mqtt || !s_mqtt->connected()) return;
  char body[200];
  snprintf(body, sizeof(body),
    "{\"ts\":%lu,\"state\":\"%s\",\"connected\":%s,\"scanning\":%s}",
    (unsigned long)(millis()/1000),
    what,
    s_status.connected ? "true" : "false",
    s_status.scanning ? "true" : "false");
  s_mqtt->publish(T_RING_STATUS, body, false);
}

// ----- HR notification callback ---------------------------------
// Heart Rate Measurement (0x2A37) format per BT spec:
//   byte 0: flags. bit0 = HR value format (0 = uint8, 1 = uint16)
//   byte 1..: HR value (1 or 2 bytes)
//   then optional energy expended, RR intervals, etc.
static void hrNotifyCb(BLERemoteCharacteristic* chr,
                       uint8_t* data, size_t len,
                       bool isNotify) {
  if (!data || len < 2) return;
  uint8_t flags = data[0];
  int bpm = 0;
  if (flags & 0x01) {
    if (len < 3) return;
    bpm = (int)data[1] | ((int)data[2] << 8);
  } else {
    bpm = (int)data[1];
  }
  s_status.lastBpm = bpm;
  s_status.hrNotifications++;
  publishBpm(bpm, s_status.lastRssi);
}

// ----- ADV callback -- scan match -------------------------------
class OuraAdvCallbacks : public BLEAdvertisedDeviceCallbacks {
public:
  void onResult(BLEAdvertisedDevice dev) override {
    String mac = String(dev.getAddress().toString().c_str());
    mac.toLowerCase();
    int rssi = dev.getRSSI();

    bool macMatch = false;
    if (s_targetMac.length() > 0) {
      String t = s_targetMac;
      t.toLowerCase();
      macMatch = (mac == t);
    }
    bool nameMatch = false;
    if (dev.haveName()) {
      String n = String(dev.getName().c_str());
      if (s_nameHint.length() > 0 && n.indexOf(s_nameHint) >= 0) {
        nameMatch = true;
      }
    }

    // Discovery mode: target MAC not set -- log any candidate to serial
    if (s_targetMac.length() == 0 && nameMatch) {
      Serial.printf("[ble][discover] candidate ring mac=%s rssi=%d name=%s\n",
                    mac.c_str(), rssi,
                    dev.haveName() ? dev.getName().c_str() : "?");
      return;
    }

    if ((macMatch || nameMatch) && rssi >= s_rssiFloor) {
      s_status.lastRssi = rssi;
      s_lastSeenMs = millis();
      publishRssi(rssi);
      if (!s_presentNow) {
        s_presentNow = true;
        publishPresence(true);
      }
      // Remember address for connect attempt (handled in tick)
      if (s_ringAddr == nullptr) {
        s_ringAddr = new BLEAddress(dev.getAddress());
        Serial.printf("[ble] ring locked  mac=%s rssi=%d\n", mac.c_str(), rssi);
      }
    }
  }
};

class OuraClientCb : public BLEClientCallbacks {
public:
  void onConnect(BLEClient* c) override {
    s_status.connected = true;
    s_connected        = true;
    s_connecting       = false;
    Serial.println("[ble] connected");
    publishStatus("connected");
  }
  void onDisconnect(BLEClient* c) override {
    s_status.connected = false;
    s_connected        = false;
    s_status.reconnects++;
    Serial.println("[ble] disconnected");
    publishStatus("disconnected");
    if (s_ringAddr) { delete s_ringAddr; s_ringAddr = nullptr; }
  }
};

// Try to read a single string characteristic (best effort).
static String readStringChr(BLERemoteService* svc, const char* uuid) {
  if (!svc) return String("");
  BLERemoteCharacteristic* c = svc->getCharacteristic(uuid);
  if (!c) return String("");
  if (!c->canRead()) return String("");
  return String(c->readValue().c_str());
}

// Read battery % from 0x180F / 0x2A19. -1 on failure.
static int readBatteryPct(BLEClient* cli) {
  BLERemoteService* svc = cli->getService(BLE_SVC_BATTERY);
  if (!svc) return -1;
  BLERemoteCharacteristic* c = svc->getCharacteristic(BLE_CHR_BATT_LEVEL);
  if (!c || !c->canRead()) return -1;
  // esp32 core 3.3.x: readValue() returns Arduino String, not std::string
  String v = c->readValue();
  if (v.length() < 1) return -1;
  return (int)(uint8_t)v[0];
}

// Subscribe to HR notifications if 0x180D is exposed.
// Returns true if subscription succeeded.
static bool subscribeHrIfAvailable(BLEClient* cli) {
  BLERemoteService* svc = cli->getService(BLE_SVC_HEART_RATE);
  if (!svc) {
    Serial.println("[ble][honesty] no 0x180D heart rate service "
                   "advertised -- HR not available from standard GATT");
    publishStatus("no_hr_service");
    return false;
  }
  BLERemoteCharacteristic* c = svc->getCharacteristic(BLE_CHR_HR_MEAS);
  if (!c) {
    Serial.println("[ble][honesty] 0x180D present but 0x2A37 absent");
    return false;
  }
  if (!c->canNotify()) {
    Serial.println("[ble][honesty] 0x2A37 not notifiable");
    return false;
  }
  c->registerForNotify(hrNotifyCb);
  Serial.println("[ble] subscribed to 0x2A37 HR notifications");
  publishStatus("hr_subscribed");
  return true;
}

static void doConnectAndRead() {
  if (!s_ringAddr) return;
  if (s_connected || s_connecting) return;
  s_connecting = true;
  publishStatus("connecting");

  if (!s_client) {
    s_client = BLEDevice::createClient();
    s_client->setClientCallbacks(new OuraClientCb());
  }
  Serial.printf("[ble] connect attempt mac=%s\n",
                s_ringAddr->toString().c_str());

  bool ok = s_client->connect(*s_ringAddr);
  if (!ok) {
    Serial.println("[ble] connect failed");
    s_connecting = false;
    publishStatus("connect_failed");
    if (s_ringAddr) { delete s_ringAddr; s_ringAddr = nullptr; }
    return;
  }

  // Device info (best effort)
  BLERemoteService* di = s_client->getService(BLE_SVC_DEVICE_INFO);
  if (di) {
    String manu  = readStringChr(di, BLE_CHR_MANUFACTURER);
    String model = readStringChr(di, BLE_CHR_MODEL);
    String fw    = readStringChr(di, BLE_CHR_FW_REV);
    publishDeviceInfo(manu.c_str(), model.c_str(), fw.c_str());
  } else {
    Serial.println("[ble][honesty] no 0x180A device info service");
  }

  // Battery (best effort)
  int batt = readBatteryPct(s_client);
  if (batt >= 0) {
    s_status.lastBattery = batt;
    publishBattery(batt);
  } else {
    Serial.println("[ble][honesty] no 0x180F battery service");
  }

  // Heart rate notifications (may not be available -- documented)
  subscribeHrIfAvailable(s_client);
}

// ============================================================
void bleOuraInit(PubSubClient* mqttRef,
                 const char* targetMac,
                 const char* nameHint,
                 int rssiFloor) {
  s_mqtt        = mqttRef;
  s_targetMac   = String(targetMac ? targetMac : "");
  s_nameHint    = String(nameHint ? nameHint : "Oura");
  s_rssiFloor   = rssiFloor;
  s_status      = {};
  s_status.enabled = true;

  BLEDevice::init("iron-bloom-v6");
  s_scan = BLEDevice::getScan();
  s_scan->setAdvertisedDeviceCallbacks(new OuraAdvCallbacks(), true /* wantDuplicates */);
  s_scan->setActiveScan(true);
  s_scan->setInterval(100);
  s_scan->setWindow(80);

  Serial.println("[ble] init complete -- scanning for Oura ring");
  Serial.printf("[ble] target_mac=%s  name_hint=%s  rssi_floor=%d\n",
                s_targetMac.length() ? "[redacted-from-log]" : "(discovery)",
                s_nameHint.c_str(),
                s_rssiFloor);

  // Kick off first scan
  s_scan->start(0, nullptr, false);
  s_status.scanning = true;
  s_lastScanStart   = millis();
}

void bleOuraTick() {
  if (!s_status.enabled) return;

  // If we've spotted the ring, try to connect.
  if (s_ringAddr && !s_connected && !s_connecting) {
    s_scan->stop();
    s_status.scanning = false;
    doConnectAndRead();
    if (!s_connected) {
      // resume scanning
      s_scan->start(0, nullptr, false);
      s_status.scanning = true;
      s_lastScanStart   = millis();
    }
  }

  // Presence timeout -- if no advertisement seen for 60 sec
  uint32_t now = millis();
  if (s_presentNow && (now - s_lastSeenMs) > 60000UL) {
    s_presentNow = false;
    publishPresence(false);
  }

  // If connected, do nothing -- HR notifications flow async.
  // If scan has run > 30 sec with no hit, log status occasionally.
  if (!s_connected && s_status.scanning &&
      (now - s_lastPresence) > 30000UL) {
    s_lastPresence = now;
    publishStatus("scanning");
  }
}

BleOuraStatus bleOuraSnapshot() {
  s_status.lastSeenMs = s_lastSeenMs;
  return s_status;
}
