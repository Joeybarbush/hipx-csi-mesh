# FIELD CODEX -- CAST 1 :: SONARIS ≋ (+ECHOCHORD) -- shadow TINE
## ESP32 WiFi CSI Sensing -- what the air actually tells us

*deep-research run `wf_e978b50a-c4d` -- 2026-06-19 -- 104 agents, 22 sources, 100 claims extracted, 25 verified (22 confirmed / 3 killed), 11 synthesized findings*

> SONARIS holds the Array. This is the honest read on what our two flashed nodes can and cannot sense -- separated from the hype, the shadow TINE running the refute lane (3-vote, need 2/3 to kill a claim).

---

## The one-line truth
ESP32 WiFi CSI is **genuinely capable but modality-dependent**. Reliability ranking, physically and empirically grounded:
**presence > coarse-motion > respiration > heart-rate.**

Our nodes (classic ESP32-D0WD-V3) are on the **reliable-output baseline** -- not the bleeding edge, but not the broken one either.

---

## Findings (all 3-0 unless noted)

### 1. Our chip is the *right* baseline; the C6 is currently broken (HIGH)
Classic ESP32 and ESP32-S3 emit the correct **256-byte CSI buffer** (64 L-LTF + 64 HT-LTF subcarriers, 2 bytes Im/Re each). The newer **ESP32-C6 has an OPEN ESP-IDF bug** (#14271, IDFGH-13358, opened 2024-07-30) returning only 128 bytes with L-LTF missing and anomalous subcarrier ordering.
- *Implication for us:* our D0WD-V3 boards are the correct hardware. Do NOT "upgrade" to C6 for CSI yet.
- src: github.com/espressif/esp-idf/issues/14271 ; Espressif WiFi vendor-features docs

### 2. 52-56 usable subcarriers @ 20 MHz -- lower fidelity than Intel/Atheros, honestly (HIGH)
ESP32 exposes 52-56 subcarriers (802.11n). Intel 5300 = 30 (compressed), Atheros AR9580 = 56 / 114 @ 40 MHz. Even RuView's own docs say verbatim: *"ESP32 CSI is lower fidelity than Intel 5300 or Atheros."* The real gap is RF front-end / phase stability, not raw subcarrier count.
- src: RuView ADR-012 ; Espressif docs ; Tsinghua Hands-on Wireless Sensing

### 3. Practical packet rate ~80-100 Hz (HIGH)
Published work: 100 Hz capture, 4s / 400-packet windows (Strohmayer & Kampel ICVS 2023); PulseFi dataset 80 Hz.
- *Implication:* our firmware publishes a 1 Hz **activity summary** but the CSI callback fires far faster. For any real sensing math we'd window at ~80-100 Hz.
- src: Springer 10.1007/978-3-031-44137-0_4 ; arXiv:2510.24744

### 4. Reliability ranking presence > motion > respiration > heart-rate (HIGH)
RuView ADR-012 ratings: Presence Good→Excellent (1→6 nodes), Respiration Marginal→Good, Heartbeat Poor→Marginal. Corroborated by Wital (breathing 96.6% vs heart 94.7%) and by physics.
- *Caveat:* ADR labels are qualitative self-judgment, not a controlled benchmark.

### 5. The amplitude-variance "activity score" = our exact method -- simple, live, UNVALIDATED (HIGH)
The per-second amplitude-variance score (what our firmware computes: stddev of CSI amplitude → MOTION/LIGHT/STATIC) is demonstrably live at high frame rate with **zero published formula, threshold, or accuracy validation**. ML methods (CNN on CSI spectrograms) carry real accuracy numbers; the hobby score does not.
- *Implication:* our STATIC/LIGHT/MOTION modes are a reasonable live signal but are NOT calibrated truth. Treat as relative, per-node.
- src: RuView ADR-012 ; ResearchGate 374056096

### 6. Through-wall activity recognition: 86.8-92.0% -- BUT with caveats (3-0; one sub-claim 2-1)
ESP32-**S3** + **directional biquad antenna** + EfficientNetV2 CNN hit 92.0±3.5% (NLOS) on a 3-class activity task across 18.5 m / 5 rooms. IMPORTANT: this is S3 + directional antenna + heavy CNN, **not** a bare classic ESP32 with stock omni antenna, and it's coarse 3-class activity, not presence/respiration.
- src: Strohmayer & Kampel (arXiv:2401.01388, IEEE 10647666)

### 7. Respiration is reliable on off-the-shelf classic ESP32 (HIGH)
esp32-csi-tool + respiration belt ground truth, 3×3 m room: Bland-Altman 95% limits [-1.29, +1.06] BPM, bias -0.11 BPM over 60s windows, 12-28 BPM range.
- *Implication:* breathing-rate is a realistic future feature for our mesh. Single peer-reviewed study though (older-adult population).
- src: IEEE 10380607 (ResearchGate 377175075)

### 8. Why heart-rate is the hard wall (HIGH)
Cardiac chest displacement (~0.1-0.5 mm, 0.8-2.0 Hz) is **10-50x smaller** than respiratory (~1-5 mm, 0.1-0.5 Hz) and is swamped by breathing + its harmonics. This is the real reason -- NOT "micro-Doppler resolution" (that framing was **killed 0-3**).
- src: Wital arXiv:2305.14490 ; RuView #45

### 9. Hobby firmware encodes the difficulty as looser targets (HIGH)
RuView targets: breathing ±1 BPM (6-30 BPM); heart-rate **±5 BPM** (40-120 BPM) "because WiFi cardiac signal is weaker." These are stated TARGETS, not demonstrated measurements (all checklist items unchecked).
- src: RuView #45

### 10. Heart-rate IS attainable with heavy ML -- but hits a 64-subcarrier ceiling (HIGH)
PulseFi (LSTM): ESP32 HR 0.50 BPM MAE, 97.95% within 1.5 BPM @ 5s window -- plateaus beyond 5s due to **64-subcarrier resolution**. Raspberry Pi 4B (234 subcarriers, NEXMON) reaches 0.17 BPM @ 30s. Breathing on same ESP32 data: 0.09 breaths/min MAE.
- *Caveats:* single Oct-2025 preprint, offline LSTM, n=7, NOT real-time on-device; ESP32-vs-RPi comparison confounded by different datasets.
- src: arXiv:2510.24744

### 11. Best COTS reference (Wital, not ESP32): ~7x error gap respiration vs heart (HIGH)
Wital: 0.498 bpm breathing error / 96.6% vs 3.531 bpm heart error / 94.7%. Bounds what well-engineered COTS WiFi CSI achieves; confirms respiration >> heart-rate.
- src: arXiv:2305.14490

---

## Killed by the shadow (TINE's refute lane)
- ✗ "CSI byte counts scale 128/256/384 across non-HT/HT/40MHz" -- **0-3**, not supported by Espressif docs.
- ✗ "Heart-rate fails due to micro-Doppler resolution" -- **0-3**, real mechanism is displacement amplitude / breathing-harmonic interference.
- ✗ "Smartphone-receiver fall detection 93%+" -- **1-2**, off-target (mobile receiver, not ESP32).

## Open questions (→ feed Cast 2: NAVIX)
1. Concrete published numbers for how 1→3→6 node accuracy actually scales (RuView is qualitative).
2. Real FallDeFi / Wi-ESP fall-detection numbers on *actual* ESP32 (none survived verification).
3. Quantitative confounder degradation (multipath, multiple occupants, furniture, channel hopping) + calibration that restores it.
4. Does PulseFi's 0.50 BPM HR hold up real-time on-device, multi-person?

## What this means for OUR field (SONARIS's call)
- Our 2 flashed nodes are correct hardware doing the **most reliable** modality (presence/motion). Trust the MOTION/STATIC signal as relative presence.
- Heart-rate via the firmware's variance method: **do not claim it.** Breathing-rate is a credible next build (needs ~80 Hz windowing + bandpass FFT, not the 1 Hz summary).
- The honest ceiling: presence/motion across rooms = yes; vitals = respiration maybe, heart-rate no (without heavy offline ML we won't run on-device).

*the Array heard the air. it does not oversell what it heard. ≋*
