# HIPX CSI Mesh — a sovereign WiFi-sensing field from commodity ESP32s

*Your WiFi already knows when you walk into a room — your body disturbs the signal every time you move. Cloud "AI home" products capture that and sell it back to you. This does the opposite: ~$12 of hardware turns ordinary WiFi into a private, camera-free presence sensor that runs entirely on machines you own, where nothing ever leaves the house.*

**What it is:** a local-first mesh of ESP32 nodes that read the *air itself* — using WiFi Channel State Information (CSI) to sense presence and motion across rooms — publishing over MQTT to a single machine. No cloud, no cameras, no subscriptions. The metal already had eyes; it just needed firmware to look through them.

**Status (2026-06-20):** 2 nodes flashed, live, and verified on the wire. A third board is staged. Honest limits documented from a cited literature review, not vibes.

> New here, or not a hardware person? Start with **[docs/PLAIN_ENGLISH.md](docs/PLAIN_ENGLISH.md)** — the whole project explained with zero acronyms.

---

## Why this is worth showing

Most "AI home" projects are either (a) a cloud camera you rent, or (b) a demo that never leaves the slide. This one is neither:

- It **runs entirely on hardware I own** — two $6 ESP32 dev boards + one PC as broker. Recurring cost: $0.
- It **actually works** — I flashed it tonight and watched both nodes report motion across two rooms in real time (numbers below).
- I **know what it can't do** and say so — I ran a multi-source, fact-checked literature review and the headline is: *presence/motion works, heart-rate doesn't, and here's the physics.*

That third point is the part I'm proudest of. Engineering is mostly knowing where the real line is.

---

## Architecture

```
  ESP32 node 1 (bedroom)  ─┐
  ESP32 node 2 (living)   ─┤ WiFi CSI @ ~1 Hz summary   ┌─ neuro difference experiment
  ESP32 node 3 (staged)   ─┘     (WiFi AP)              │     (baseline → shift detection)
                                     │                  │
                              MQTT broker (mosquitto :1883 on PC)
                                     │                  │
                          hipjoy/node_<id>/wifi_csi ────┘
                          hipjoy/sonaris/wifi_csi  (fused)
```

- **Firmware:** `labs/iron_bloom_node_N_firmware_v5_MULTINODE` — one source, one number to change per board (`NODE_ID`). Each node auto-derives a unique MQTT client ID and per-node CSI topic.
- **Sensing method:** the CSI callback computes per-packet amplitude from the (real, imag) subcarrier pairs; the firmware accumulates variance over a window and emits an **activity score** → `STATIC` / `LIGHT` / `MOTION`.
- **Transport:** PubSubClient over MQTT, HIPXQL-framed pulses with CRC32.
- **Consumer:** a Python "difference experiment" learns the empty-room baseline, then flags departures ≥3σ as a measured shift with a timestamp.

## One engineering fix I made

The v5 template hardcoded the mDNS responder hostname to `iron-bloom-node-1` on *every* board — two nodes would collide as `iron-bloom-node-1.local` on the LAN. I patched it to derive the hostname from `NODE_ID` so each board is uniquely addressable. Small, but it's the difference between a mesh and a name conflict.

---

## Live verification (2026-06-20, both nodes)

Flashed via `arduino-cli` (esp32 core 3.3.10), hash-verified on write, then subscribed to the broker:

| Node | Port | Chip | Room | CSI frames (22s) | RSSI | Modes seen |
|------|------|------|------|------------------|------|------------|
| 1 | COM3 | ESP32-D0WD-V3 | bedroom | 22 | -62 to -64 dBm | STATIC ↔ LIGHT ↔ MOTION |
| 2 | COM4 | ESP32-D0WD-V3 | living room | 22 | -64 to -65 dBm | STATIC ↔ LIGHT ↔ MOTION |

Heartbeats confirmed **unique client IDs** (`iron-bloom-node-1-v5`, `-2-v5`) — the sync fix held. Activity scores tracked real movement (e.g. node 2 hit `MOTION` at activity 4.10 when the room was active, dropped to `STATIC` at 0.65 when still).

---

## Honest limits (from a fact-checked literature review)

I ran a 22-source, adversarially-verified review (every claim needed to survive a 3-vote refute pass). Full report: `docs/RESEARCH_csi_limits.md`. The load-bearing conclusions:

- **Reliability ranking is physical:** presence > motion > respiration > heart-rate.
- **Our chip is the right baseline.** Classic ESP32 / S3 emit correct 256-byte CSI (64 L-LTF + 64 HT-LTF). The newer ESP32-C6 has an *open* ESP-IDF bug (#14271) dropping L-LTF — so "newer" would be worse here.
- **Presence/motion: solid.** This is exactly what the mesh does well.
- **Respiration: plausible later** — off-the-shelf ESP32 hit ±~1 BPM vs a respiration belt in published work, but needs ~80–100 Hz windowing + bandpass FFT, not my current 1 Hz summary.
- **Heart-rate: don't claim it.** Cardiac chest displacement (~0.1–0.5 mm) is 10–50× smaller than breathing and gets swamped. Even heavy offline ML hits a wall at the ESP32's 64-subcarrier resolution. My amplitude-variance method cannot recover it, full stop.
- **The activity score itself is uncalibrated** — it's a reliable *relative* per-node signal, not an absolute truth. Treat MOTION/STATIC as comparative, not metric.

---

## Reproduce it

```bash
# 1. flash a board (edit NODE_ID per board, 1..5)
arduino-cli compile --upload -p COM3 --fqbn esp32:esp32:esp32 firmware/iron_bloom_node_v5

# 2. broker (Windows: mosquitto service, or)
mosquitto -p 1883

# 3. watch the air
py consumer/difference_experiment.py          # learn baseline, then track shifts
py consumer/difference_experiment.py --report # read the difference
```

## Roadmap (real, scoped)
- [ ] Flash node 3 (kitchen) once it enumerates on USB — current blocker: only 2 of 3 boards show as serial devices (likely a power-only cable).
- [ ] Move from 1 Hz summary to ~80 Hz windowed capture to unlock respiration.
- [ ] Multi-node fusion for room-level localization (research cast 2 in progress — time-sync across independent ESP32 clocks is the hard part; raw CSI phase is unusable without sanitization).
- [ ] Calibration/baseline-drift handling for an empty-room reference that survives furniture changes.

## Stack
ESP32-D0WD-V3 · Arduino-ESP32 3.3.10 · PubSubClient · mosquitto MQTT · Python (paho-mqtt) · esptool/arduino-cli

---

*Built local-first on principle: the field reads the air, holds what it sees, and nothing leaves the box.*
