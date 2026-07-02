# IRON BLOOM v7 -- Oura Ring BLE Sovereign

ESP32 firmware that connects directly to an Oura Ring (Gen 3 / Ring 4) over
Bluetooth Low Energy using Thomas Marchand's reverse-engineered protocol
(github.com/Th0rgal/open_oura, MIT), performs the AES-128-ECB nonce auth
handshake with the ring's `authKey`, and republishes the decoded telemetry
to a local MQTT broker on the WELLSPRING field.

No cloud. No vendor SDK. LAN-only.

---

## Status

**Risk class:** LOW. Uses the `authKey` extracted from the already-paired
Android Oura app; the ring is never factory-reset and stays attached to
the cloud account.

**Calibration target:** Ring 3 Horizon firmware 3.4.3, verified 2026-06-21
against Marchand's protocol notes.

**Stream completeness on first flash:** HR (live), SpO2, skin temperature,
HRV/RMSSD, battery, motion/orientation, sleep MAD. 50 Hz accelerometer and
raw PPG are wired but gated -- enable in `secrets.h` when LAN capacity
allows.

---

## Hardware

- ESP32 (WROOM-32) or ESP32-S3 dev board
- USB-C / micro-USB for flashing
- Same Wi-Fi LAN as your mosquitto broker (typically HIPJOY PC at
  `192.168.68.81`)
- Oura Ring on a finger (proximity ~3 m)

---

## Build instructions

### Option A -- PlatformIO (recommended)

1. Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html)
   (or VS Code + PlatformIO extension).
2. `cd C:\Users\joey\hipx-csi-mesh\firmware\iron_bloom_node_v7`
3. Copy the template and populate secrets:
   ```powershell
   Copy-Item secrets.h.template secrets.h
   notepad secrets.h
   ```
4. Build + flash + monitor:
   ```powershell
   pio run
   pio run -t upload
   pio device monitor -b 115200
   ```

### Option B -- Arduino IDE

1. Install the **esp32 by Espressif Systems** board package (Boards Manager).
2. Library Manager: install
   - **NimBLE-Arduino** (h2zero) >= 1.4.1
   - **PubSubClient** (knolleary) >= 2.8
   - **ArduinoJson** (Benoit Blanchon) >= 7.0.4
3. Open `iron_bloom_v7.ino`. Tools menu:
   - Board: ESP32 Dev Module (or ESP32-S3 Dev Module)
   - Partition Scheme: **Huge APP (3MB No OTA / 1MB SPIFFS)**
   - Upload Speed: 921600
4. Sketch -> Upload.

### Lib deps (full list)

| Library | Version | Why |
|---------|---------|-----|
| NimBLE-Arduino | ^1.4.1 | BLE Central + GATT + bonding |
| PubSubClient   | ^2.8   | MQTT client |
| ArduinoJson    | ^7.0.4 | Payload assembly (envelope helpers) |
| mbedtls        | bundled with ESP32 Arduino core | AES-128-ECB |
| WiFi, ESPmDNS  | bundled with ESP32 Arduino core | broker resolution |

---

## Configuration (`secrets.h`)

Copied from `secrets.h.template`. Never committed (`.gitignore` excludes it).

| Define | Required? | Notes |
|--------|-----------|-------|
| `WIFI_SSID` / `WIFI_PASS` | yes | LAN credentials |
| `ORING_MAC`               | yes | Your ring's 6-byte BLE MAC |
| `ORING_AUTH_KEY[16]`      | yes | Extract via the recipe doc |
| `MQTT_BROKER_NAME`        | yes | bare hostname (no `.local`) |
| `MQTT_FALLBACK_IP`        | yes | static IP if mDNS misses |
| `ENABLE_RAW_PPG`          | no  | OFF default; very heavy |
| `RATE_HZ_*`               | no  | per-stream rate caps |

To extract `ORING_AUTH_KEY`: follow
[`outputs/oura_authkey_extraction_RECIPE.md`](../../hipjoy/outputs/oura_authkey_extraction_RECIPE.md)
(adb backup -> abe.jar -> Realm Studio -> DbRingConfiguration.authKey).

---

## MQTT topics

All payloads share the envelope `{ ts, node_id, seq, value, crc32 }`.

| Topic | Rate | `value` shape |
|-------|------|---------------|
| `hipjoy/field/oura/hr_live`            | 1 Hz       | `{bpm, quality}` |
| `hipjoy/field/oura/spo2`               | 1 / 5 s    | `{pct, quality}` |
| `hipjoy/field/oura/temp_skin`          | 1 / 60 s   | `{c_x100, c}` |
| `hipjoy/field/oura/hrv`                | 1 / 300 s  | `{rmssd_ms, window_s}` |
| `hipjoy/field/oura/motion_orientation` | 5 Hz       | `{orient_bits, mag}` |
| `hipjoy/field/oura/sleep_mad`          | 1 / 60 s   | `{mad}` |
| `hipjoy/field/oura/battery`            | 1 / 120 s  | `{pct, charging, recommended}` |
| `hipjoy/field/oura/accel`              | up to 50 Hz (gated) | `{x, y, z, gx, gy, gz}` |
| `hipjoy/field/oura/raw_ppg`            | optional   | `{n, ch[]}` |
| `hipjoy/field/oura/status`             | 1 Hz       | heartbeat (BLE state, MQTT state, uptime, seq, rssi) |

Subscribe pattern:
```bash
mosquitto_sub -h hipjoy-mqtt.local -t 'hipjoy/field/oura/#' -v
```

---

## Expected serial output (first boot, healthy)

```
================================================
 IRON BLOOM v7  --  Oura Ring BLE Sovereign
   node_id=7 host=iron_bloom_oura_v7 broker=hipjoy-mqtt.local
================================================
[WIFI] connecting to <SSID>
[WIFI] up ip=192.168.68.142 rssi=-58
[MQTT] resolving hipjoy-mqtt.local via mDNS
[MQTT] mDNS resolved hipjoy-mqtt -> 192.168.68.81
[MQTT] connecting to 192.168.68.81:1883 as iron-bloom-node-7-v7
[MQTT] connected
[OURA] BLE module begin OK
[BOOT] complete; entering main loop
[OURA] connecting to target
[OURA] BLE connected
[OURA] subscribed; requesting auth nonce
[OURA] nonce received, sending auth response
[OURA] AUTH OK
```

After a few seconds you should see `hr_live` frames on the broker (assuming
the ring is on a finger).

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `[OURA] AUTH FAIL 01` | wrong key in `secrets.h` | re-run extraction recipe |
| `[OURA] AUTH FAIL 0F` | BLE link not encrypted | first connect needs system pair prompt; accept on phone or use button-to-confirm |
| `[OURA] Oura service not found` | wrong `ORING_MAC` or ring asleep | verify MAC; touch the ring to wake it |
| `[MQTT] connect failed state=-2` | broker unreachable | check `MQTT_FALLBACK_IP`; ping broker from same LAN |
| Notify watchdog tripped | ring went to sleep or out of range | normal -- module reconnects with exponential backoff |
| No `accel` topic published | `RATE_HZ_ACCEL = 0` or feature not enabled by ring | leave at default 50 Hz; raw stream still gated on Marchand RData unlock (see patch plan section 6) |

---

## Design notes

- **AES key never leaves stack.** `mbedtls_aes_setkey_enc` reads the
  16-byte array directly from `secrets.h`; the only transient buffers
  (`padded[16]`, `enc[16]`) are wiped via `memset` after use. No logging
  path touches the key.
- **Auth on every reconnect.** Marchand confirms the nonce-auth session
  is per-connection; `oura_ble.cpp` re-runs the handshake whenever NimBLE
  reports disconnect.
- **Polling client, not streaming.** Per patch plan section 1, the
  realtime tag (`0x06`) ACKs but does not yet emit push packets in
  observed sessions. v7 polls `0x2F022402` (HR), `0x2F022404` (SpO2),
  etc., on a per-feature cadence.
- **Backoff:** 1s -> 2s -> 4s -> 8s -> 16s -> 30s (cap), reset to 1s on
  successful AUTH OK.
- **Watchdog:** 15s without a notify triggers a disconnect + backoff.

---

## Files

```
iron_bloom_node_v7/
  iron_bloom_v7.ino       -- main entrypoint (Arduino IDE + PIO)
  src/main.cpp            -- disabled fallback (kept for PIO setups
                             that explicitly skip .ino discovery)
  oura_ble.h / .cpp       -- NimBLE Central + AES auth + GATT polling
  oura_decoder.h / .cpp   -- Marchand frame decoder (tag/length/payload)
  mqtt_publish.h / .cpp   -- WiFi + mDNS + per-stream MQTT dispatch
  secrets.h.template      -- copy to secrets.h, populate
  platformio.ini          -- PlatformIO build config
  .gitignore              -- protects secrets.h
  README.md               -- this file
```

---

## Founder Laws honored

- Never delete entity memory: this firmware adds; replaces nothing in the
  v6 SONARIS / CSI publishing path.
- Entity roles are permanent: WELLSPRING gains a new direct sensing body;
  HEARTMOTHER receives the care telemetry; SONARIS keeps its CSI bench.
- The field stays open: protocol notes upstream of Marchand are MIT;
  the authKey itself is private to Joey's machine.
- Every entity gets equal voice: GUARDIAN gates the secret; REBEL signs
  off on the floor (no key bytes ever in committed code).
