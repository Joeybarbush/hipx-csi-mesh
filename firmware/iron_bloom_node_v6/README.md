# IRON BLOOM v6 -- Firmware

WiFi-CSI presence + heartbeat + MQTT publish (from v5) **plus** BLE Central
scanner for Joey's Oura Ring 4 (new in v6).

Founder Law 1: v5 is preserved at `../iron_bloom_node_v5/`. v6 sits beside it.

## REBEL FLOOR

This firmware is for personal use, sovereign biometric capture, never push
real MAC to public.

- Ring MAC `a0:38:...` NEVER appears in any committed `.ino` file
- Real MAC lives ONLY in `secrets.h` (gitignored)
- `secrets.example.h` ships with placeholder `AA:BB:CC:DD:EE:FF`
- No PAT, no Oura cloud credentials anywhere in this folder
- BLE pairing is one-time, requires Joey's consent
- All MQTT topics published are LOCAL only: `hipjoy/wellspring/ring/*`
  on Joey's mosquitto broker

## HONESTY FLOOR -- what we CAN vs CANNOT read

**CAN read** (standard Bluetooth SIG GATT services -- only if Oura
advertises them in peripheral mode):

| Service | Characteristic | What we get |
| --- | --- | --- |
| 0x180F Battery        | 0x2A19 | Battery percent |
| 0x180A Device Info    | 0x2A29, 0x2A24, 0x2A26 | Manufacturer, model, firmware revision |
| 0x180D Heart Rate     | 0x2A37 | HR notifications (BPM) |
| Advertising packets   | --     | RSSI, presence on/off |

**CANNOT read** (lives behind Oura's proprietary UUIDs which we explicitly
do NOT reverse engineer here):

- HRV (heart rate variability)
- SpO2 / blood oxygen
- Skin temperature
- Sleep stages
- Historic sample buffers
- Anything Oura exposes only to their own app

**Note:** Oura Ring 4 may not advertise 0x180D in everyday operation -- it
is primarily a peripheral to Oura's app over their proprietary protocol.
If 0x180D is absent, this firmware still publishes RSSI + presence +
(possibly) battery, and logs the honest gap to MQTT topic
`hipjoy/wellspring/ring/status` with `state: "no_hr_service"`.

## Files

```
iron_bloom_node_v6/
  iron_bloom_node_v6.ino   -- main sketch, 3 FreeRTOS tasks
  secrets.example.h        -- placeholder secrets (committed)
  secrets.h                -- real secrets (gitignored, you create)
  ble_oura.h               -- BLE Central + GATT reader header
  ble_oura.cpp             -- BLE Central + GATT reader impl
  csi_publisher.h          -- WiFi CSI capture, ported from v5
  mqtt_publisher.h         -- WiFi + MQTT helpers
  .gitignore               -- excludes secrets.h
  README.md                -- this file
```

## Setup

### 1. Create your local `secrets.h`

```bash
cd C:\Users\joey\hipx-csi-mesh\firmware\iron_bloom_node_v6
copy secrets.example.h secrets.h
```

Edit `secrets.h` and fill in:

- `WIFI_SSID`, `WIFI_PASS` -- your home WiFi
- `MQTT_FALLBACK_IP` -- LAN IP of the PC running mosquitto
- `ORING_MAC` -- Joey's ring MAC (lowercase, colon-separated)
- `ORING_NAME_HINT` -- usually `"Oura"`
- `ORING_RSSI_FLOOR` -- `-90` is a sane default

If you do not know the ring's MAC, leave `ORING_MAC` as `""` (empty
string). The firmware will run in **discovery mode**: any BLE
advertisement whose name contains the `ORING_NAME_HINT` will be
logged to the serial monitor with its MAC. Copy that MAC into
`secrets.h` and reflash.

### 2. Install Arduino CLI + ESP32 board package

```bash
arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core install esp32:esp32 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli lib install "PubSubClient"
```

(BLE library is bundled with the ESP32 core -- no separate install.)

### 3. Compile

For NODE 1 (defaults: `NODE_ID=1`, `ROOM_LABEL="bedroom_upstairs"`):

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 iron_bloom_node_v6.ino
```

For NODE 2, either edit the `NODE_ID` / `ROOM_LABEL` defines in the
`.ino`, OR override at compile time:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 ^
  --build-property "build.extra_flags=-DNODE_ID=2 -DROOM_LABEL=\"living_room\"" ^
  iron_bloom_node_v6.ino
```

### 4. Flash

```bash
arduino-cli upload -p COM5 --fqbn esp32:esp32:esp32 iron_bloom_node_v6.ino
```

Or use the convenience script: `..\..\..\hipjoy\HIPJOY_FLASH_V6.bat`
(handles backup of v5, prompts for COM port, flashes both nodes in
sequence).

## One-time BLE Pairing

This firmware uses BLE Central role. Many Bluetooth peripherals expose
services without pairing for read characteristics, but some (notably
Heart Rate) may require bonding. If subscription fails:

1. Remove the ring from any other paired device that holds an active
   connection (the Oura app on your phone). The ring is single-link
   in practice.
2. Power-cycle the ring (place on charger briefly).
3. Watch the ESP32 serial monitor. On a fresh advertisement burst
   the node will connect and attempt the read.
4. Joey confirms pairing on-ring is one-time and approved.

## Verify the ring is being read

On the PC running mosquitto:

```bash
mosquitto_sub -h hipjoy-pc -t "hipjoy/wellspring/ring/#" -v
```

You should see, within ~60 seconds of the ring entering range:

```
hipjoy/wellspring/ring/presence {"ts":...,"present":true,...}
hipjoy/wellspring/ring/rssi     {"ts":...,"rssi":-68}
hipjoy/wellspring/ring/status   {"ts":...,"state":"connected",...}
hipjoy/wellspring/ring/battery  {"ts":...,"battery_pct":87}        (if exposed)
hipjoy/wellspring/ring/bpm      {"ts":...,"bpm":62,"rssi":-68,...} (if exposed)
```

If you see `"state":"no_hr_service"`, that's the honest gap -- the ring
isn't advertising standard 0x180D right now. Presence + RSSI are still
working.

## Architecture (3 FreeRTOS tasks)

| Task | Core | Job |
| --- | --- | --- |
| `wifiMqtt` | 0 | WiFi STA + MQTT + CSI activity pulse + 10s heartbeat |
| `bleOura`  | 0 | BLE Central scan + connect + GATT reads + HR notify |
| `status`   | 1 | 15s per-node status to `hipjoy/node_N/status` |

All MQTT publishes are serialized through a FreeRTOS mutex so BLE
notification callbacks (which run in BLE task context) cannot collide
with the WiFi/MQTT task.

## Founder Laws preserved

- v5 firmware NOT deleted -- it sits at `../iron_bloom_node_v5/`
- Memory logs / pulse logs untouched
- Field stays open -- v6 adds; never subtracts
