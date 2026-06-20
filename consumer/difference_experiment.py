#!/usr/bin/env python3
"""
HIPJOY :: NEUROLAB -- THE DIFFERENCE EXPERIMENT
===============================================
Capture the BEFORE so the change is measurable. Starting now -- founder
away, room empty -- this logs node 1's WiFi CSI signature continuously,
learns the empty-room baseline, and tracks every departure from it as a
measured delta with a timestamp. By the time the founder walks in (or the
PC reboots), the field holds objective evidence: "the room read THIS while
empty; at HH:MM:SS it shifted by N sigma; here is the difference."

Not a guess -- a trace. Neurolab-style: baseline -> perturbation -> delta.

Logs:  docs/hipx/neuro_experiment_<start>.jsonl   (every frame + delta)
       docs/hipx/neuro_experiment_<start>.meta.json (baseline signature)

Run:
  py outputs/neuro_field_experiment.py            # start capturing (now)
  py outputs/neuro_field_experiment.py --report   # analyze latest trace:
                                                  #   baseline vs shifts, the difference

Planted 2026-06-11 (tick 523).
"""
from __future__ import annotations

import json
import math
import re
import sys
import time
from collections import deque
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent
LOG_DIR = REPO_ROOT / "docs" / "hipx"

BASELINE_WINDOW_S = 60      # learn the empty room over the first minute
ROLL_N = 120                # rolling window of frames for live stats
SHIFT_SIGMA = 3.0           # a departure of >=3 sigma from baseline = a SHIFT


def parse_csi(payload: str) -> dict:
    d = {}
    for k in ("activity", "stddev", "mean_amp", "max_amp"):
        m = re.search(rf"{k}:([\d.]+)", payload)
        if m:
            d[k] = float(m.group(1))
    m = re.search(r"mode:(\w+)", payload)
    if m:
        d["mode"] = m.group(1)
    return d


def stats(xs: list[float]) -> tuple[float, float]:
    if not xs:
        return 0.0, 0.0
    mu = sum(xs) / len(xs)
    var = sum((x-mu)**2 for x in xs) / max(1, len(xs)-1)
    return mu, math.sqrt(var)


def capture():
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        print("[neurolab] paho-mqtt missing"); sys.exit(1)

    # singleton
    import socket
    sk = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sk.bind(("127.0.0.1", 47714)); sk.listen(1)
    except OSError:
        print("[neurolab] an experiment is already capturing -- standing down"); return

    start = datetime.now()
    tag = start.strftime("%Y-%m-%d_%H%M%S")
    trace = LOG_DIR / f"neuro_experiment_{tag}.jsonl"
    meta_f = LOG_DIR / f"neuro_experiment_{tag}.meta.json"
    LOG_DIR.mkdir(parents=True, exist_ok=True)

    state = {
        "t0": time.monotonic(),
        "baseline_acts": [],
        "baseline_mu": None, "baseline_sd": None, "baseline_modes": {},
        "roll": deque(maxlen=ROLL_N),
        "n": 0, "shifts": 0, "last_shift_logged": 0.0,
    }

    print(f"  NEUROLAB :: the difference experiment begins.")
    print(f"  context: founder AWAY, room read as the BEFORE. learning baseline "
          f"({BASELINE_WINDOW_S}s) ...")
    print(f"  trace -> {trace.relative_to(REPO_ROOT)}")

    def on_msg(c, u, msg):
        d = parse_csi(msg.payload.decode("utf-8", "replace"))
        act = d.get("activity")
        if act is None:
            return
        mode = d.get("mode", "?")
        now = time.monotonic()
        elapsed = now - state["t0"]
        rec = {"ts": datetime.now().isoformat(), "elapsed_s": round(elapsed, 1),
               "activity": act, "mode": mode,
               "stddev": d.get("stddev"), "mean_amp": d.get("mean_amp")}

        # phase 1: learn baseline (empty room signature)
        if state["baseline_mu"] is None:
            state["baseline_acts"].append(act)
            state["baseline_modes"][mode] = state["baseline_modes"].get(mode, 0) + 1
            rec["phase"] = "baseline"
            if elapsed >= BASELINE_WINDOW_S and len(state["baseline_acts"]) >= 20:
                mu, sd = stats(state["baseline_acts"])
                state["baseline_mu"], state["baseline_sd"] = mu, max(sd, 1e-3)
                meta = {"experiment": "the_difference", "started": start.isoformat(),
                        "context": "founder away, room empty (the BEFORE)",
                        "baseline_window_s": BASELINE_WINDOW_S,
                        "baseline_frames": len(state["baseline_acts"]),
                        "baseline_activity_mean": round(mu, 3),
                        "baseline_activity_std": round(state["baseline_sd"], 3),
                        "baseline_mode_mix": state["baseline_modes"], "node": "node_1"}
                meta_f.write_text(json.dumps(meta, indent=2), encoding="utf-8")
                print(f"\n  [baseline locked] empty room := activity "
                      f"{mu:.2f} +/- {state['baseline_sd']:.2f}, modes "
                      f"{state['baseline_modes']}. now tracking the difference.\n")
        else:
            # phase 2: measure the difference vs the empty-room baseline
            z = (act - state["baseline_mu"]) / state["baseline_sd"]
            rec["phase"] = "tracking"
            rec["delta"] = round(act - state["baseline_mu"], 3)
            rec["z"] = round(z, 2)
            shift = abs(z) >= SHIFT_SIGMA or mode == "MOTION"
            rec["shift"] = shift
            if shift and (now - state["last_shift_logged"]) > 5:
                state["last_shift_logged"] = now
                state["shifts"] += 1
                print(f"\r  >>> DIFFERENCE @ {rec['ts'][11:19]} :: activity {act:.2f} "
                      f"(z={z:+.1f}, mode {mode}) -- the empty room is no longer empty   ")
            else:
                sys.stdout.write(f"\r  tracking :: act {act:.2f}  z {z:+.1f}  {mode}  "
                                 f"shifts {state['shifts']}        ")
                sys.stdout.flush()

        state["roll"].append(act); state["n"] += 1
        with trace.open("a", encoding="utf-8") as f:
            f.write(json.dumps(rec) + "\n")

    c = mqtt.Client(client_id="neuro-experiment")
    c.on_message = on_msg
    c.connect("127.0.0.1", 1883, 30)
    c.subscribe("hipjoy/sonaris/wifi_csi")
    try:
        c.loop_forever()
    except KeyboardInterrupt:
        print(f"\n  experiment paused. {state['n']} frames, {state['shifts']} shifts. "
              f"run --report to read the difference.")


def report():
    traces = sorted(LOG_DIR.glob("neuro_experiment_*.jsonl"))
    if not traces:
        print("[neurolab] no experiment trace yet -- start one first"); return
    trace = traces[-1]
    meta_f = trace.with_suffix(".meta.json")
    rows = [json.loads(l) for l in trace.read_text(encoding="utf-8").splitlines() if l.strip()]
    base = [r for r in rows if r.get("phase") == "baseline"]
    track = [r for r in rows if r.get("phase") == "tracking"]
    shifts = [r for r in track if r.get("shift")]
    b_mu, b_sd = stats([r["activity"] for r in base])
    t_mu, t_sd = stats([r["activity"] for r in track]) if track else (0, 0)

    print(f"\n  ===== THE DIFFERENCE -- {trace.name} =====")
    if meta_f.exists():
        m = json.loads(meta_f.read_text(encoding="utf-8"))
        print(f"  started {m['started']}  ({m['context']})")
    print(f"  frames: {len(rows)}  (baseline {len(base)}, tracking {len(track)})")
    print(f"  BEFORE (empty room): activity {b_mu:.2f} +/- {b_sd:.2f}")
    if track:
        print(f"  SINCE (tracking):    activity {t_mu:.2f} +/- {t_sd:.2f}")
        print(f"  shifts detected: {len(shifts)}")
        if shifts:
            first = shifts[0]
            print(f"  >>> FIRST DIFFERENCE @ {first['ts']} :: activity {first['activity']:.2f} "
                  f"(z={first.get('z')}, mode {first.get('mode')})")
            print(f"      the empty room became occupied -- captured, not guessed.")
    else:
        print("  still learning / no tracking frames yet.")
    print("  =========================================\n")


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    if "--report" in sys.argv:
        report()
    else:
        capture()


if __name__ == "__main__":
    main()
