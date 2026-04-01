#!/usr/bin/env python3
"""
SIDECAR V1 — IMU Wrist-Wake Tuner
===================================
Captures raw IMU CSV (accel + gyro) from the watch serial port, labels
gestures, then finds optimal thresholds for the two-stage firmware detector.

Usage
-----
  # Capture live from watch (hold BtnA+BtnB 2s on watch to start):
  python3 tools/imu_tune.py --serial /dev/cu.usbserial-XXXX --capture imu_session.csv

  # Analyse an existing capture:
  python3 tools/imu_tune.py --analyse imu_session.csv

  # Both at once:
  python3 tools/imu_tune.py --serial /dev/cu.usbserial-XXXX --capture imu_session.csv --analyse imu_session.csv

What it produces
----------------
  1. PNG: raw ax/ay/az + gy with RAISE/REST event markers
  2. PNG: computed features (ayDelta, |gy|) — shows class separation
  3. Printed firmware threshold recommendations (paste into display_mgr.cpp)

Two-stage classifier (mirrors firmware logic)
---------------------------------------------
  Stage 1 — MOTION:   |gy| > GY_THRESH  (gyro rotation spike)
  Stage 2 — ORIENT:   ayDelta < AY_THRESH (accel baseline delta)
  Fire when BOTH true simultaneously.
"""

import argparse
import csv
import sys
from pathlib import Path

try:
    import serial
except ImportError:
    serial = None

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
    HAS_MPL = True
except ImportError:
    HAS_MPL = False


# ── Capture ───────────────────────────────────────────────────────────────────

def capture(port: str, out_path: str):
    if serial is None:
        print("ERROR: pyserial not installed. Run: pip3 install pyserial")
        sys.exit(1)

    print(f"Opening {port}…")
    ser = serial.Serial(port, 115200, timeout=1)
    print("Waiting for watch to enter IMU record mode (hold BtnA+BtnB 2s)…")

    rows = []
    started = False
    try:
        while True:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue
            if "IMU_RECORD_START" in line:
                print("Recording started.")
                print("  BtnA tap = label RAISE  |  BtnB tap = label REST")
                print("  Hold BtnA+BtnB 2s to stop.")
                started = True
                continue
            if "IMU_RECORD_STOP" in line:
                print("Recording stopped.")
                break
            if started and not line.startswith("#"):
                rows.append(line)
                if len(rows) % 250 == 0:
                    print(f"  {len(rows)} samples ({len(rows)/50:.0f}s)…")
    except KeyboardInterrupt:
        print("\nInterrupted.")
    finally:
        ser.close()

    if not rows:
        print("No data captured.")
        return

    out = Path(out_path)
    if out.exists():
        ans = input(f"File '{out_path}' already exists. Overwrite? [y/N] ").strip().lower()
        if ans != "y":
            print("Aborted — existing file kept.")
            return

    with open(out_path, "w") as f:
        f.write("ts_ms,ax,ay,az,gx,gy,gz,event\n")
        for r in rows:
            f.write(r + "\n")
    print(f"Saved {len(rows)} rows → {out_path}")


# ── Analyse ───────────────────────────────────────────────────────────────────

def analyse(path: str):
    if not HAS_MPL:
        print("ERROR: matplotlib/numpy not installed.")
        sys.exit(1)

    rows = []
    with open(path) as f:
        reader = csv.DictReader(f)
        for r in reader:
            try:
                rows.append({
                    "ts":  int(r["ts_ms"]),
                    "ax":  float(r["ax"]),
                    "ay":  float(r["ay"]),
                    "az":  float(r["az"]),
                    "gx":  float(r.get("gx", 0)),
                    "gy":  float(r.get("gy", 0)),
                    "gz":  float(r.get("gz", 0)),
                    "event": r["event"].strip(),
                })
            except (ValueError, KeyError):
                continue

    if not rows:
        print("Empty or malformed file.")
        return

    ts  = np.array([r["ts"]  for r in rows]) / 1000.0
    ax  = np.array([r["ax"]  for r in rows])
    ay  = np.array([r["ay"]  for r in rows])
    az  = np.array([r["az"]  for r in rows])
    gx  = np.array([r["gx"]  for r in rows])
    gy  = np.array([r["gy"]  for r in rows])
    gz  = np.array([r["gz"]  for r in rows])
    ev  = np.array([r["event"] for r in rows])

    print(f"Loaded {len(rows)} samples, {ts[-1]-ts[0]:.1f}s duration")
    raise_idx = np.where(ev == "RAISE")[0]
    rest_idx  = np.where(ev == "REST")[0]
    print(f"Labelled: {len(raise_idx)} RAISE, {len(rest_idx)} REST events")

    # ── Compute features matching firmware ────────────────────────────────────
    alpha = 0.05   # matches firmware: 0.95 * prev + 0.05 * new
    ay_base = np.zeros_like(ay)
    ay_base[0] = ay[0]
    for i in range(1, len(ay)):
        ay_base[i] = ay_base[i-1] * (1-alpha) + ay[i] * alpha

    ay_delta = ay - ay_base
    abs_gy   = np.abs(gy)

    # ── Plot 1: raw signals with event markers ────────────────────────────────
    BG, DARK = "#111111", "#1a1a1a"
    fig, axes = plt.subplots(4, 1, figsize=(15, 10), facecolor=BG)
    fig.suptitle("IMU Raw  —  RAISE=green  |  REST=red", color="white", fontsize=13)

    signals = [
        (ay,      "ay (g)",    "#00BFFF"),
        (az,      "az (g)",    "#7FFF00"),
        (gy,      "gy (°/s)",  "#FF6B35"),
        (abs_gy,  "|gy| (°/s)","#FFD700"),
    ]
    for ax_plot, (data, label, col) in zip(axes, signals):
        ax_plot.set_facecolor(DARK)
        ax_plot.plot(ts, data, color=col, linewidth=0.8)
        ax_plot.axhline(0, color="#444", linewidth=0.5)
        for i in raise_idx:
            ax_plot.axvline(ts[i], color="lime",  alpha=0.8, linewidth=1.5, label="_raise")
        for i in rest_idx:
            ax_plot.axvline(ts[i], color="red",   alpha=0.8, linewidth=1.5, label="_rest")
        ax_plot.set_ylabel(label, color="white", fontsize=9)
        ax_plot.tick_params(colors="white", labelsize=8)
        for spine in ax_plot.spines.values():
            spine.set_edgecolor("#444")
    axes[-1].set_xlabel("time (s)", color="white")
    plt.tight_layout()
    out1 = path.replace(".csv", "_raw.png")
    plt.savefig(out1, dpi=120, facecolor=BG)
    print(f"Raw plot → {out1}")
    plt.close()

    # ── Plot 2: features (what the classifier sees) ───────────────────────────
    fig, (f1, f2, f3) = plt.subplots(3, 1, figsize=(15, 8), facecolor=BG)
    fig.suptitle("Classifier features  —  RAISE=green  |  REST=red", color="white", fontsize=13)

    for ax_plot, data, label, col, thresh, tdir in [
        (f1, ay_delta, "ayDelta (g) [orient]",  "#00BFFF", -0.4, "below"),
        (f2, abs_gy,   "|gy| (°/s) [motion]",   "#FF6B35", 120,  "above"),
        (f3, ay_delta * abs_gy, "ayDelta × |gy| [combined]", "#FFD700", None, None),
    ]:
        ax_plot.set_facecolor(DARK)
        ax_plot.plot(ts, data, color=col, linewidth=0.8)
        ax_plot.axhline(0, color="#444", linewidth=0.5)
        if thresh is not None:
            ax_plot.axhline(thresh, color=col, linewidth=1, linestyle="--", alpha=0.6,
                            label=f"thresh={thresh}")
            ax_plot.legend(facecolor=DARK, labelcolor="white", fontsize=8)
        for i in raise_idx:
            ax_plot.axvline(ts[i], color="lime", alpha=0.8, linewidth=1.5)
        for i in rest_idx:
            ax_plot.axvline(ts[i], color="red",  alpha=0.8, linewidth=1.5)
        ax_plot.set_ylabel(label, color="white", fontsize=9)
        ax_plot.tick_params(colors="white", labelsize=8)
        for spine in ax_plot.spines.values():
            spine.set_edgecolor("#444")
    axes[-1].set_xlabel("time (s)", color="white") if 'axes' in dir() else None
    f3.set_xlabel("time (s)", color="white")
    plt.tight_layout()
    out2 = path.replace(".csv", "_features.png")
    plt.savefig(out2, dpi=120, facecolor=BG)
    print(f"Feature plot → {out2}")
    plt.close()

    # ── Threshold search ──────────────────────────────────────────────────────
    if len(raise_idx) == 0:
        print("\nNo RAISE events labelled — can't compute thresholds.")
        print("Collect more data: tap BtnA after each wrist raise.")
        return

    # For each event, find peak feature value in ±500ms window (±25 samples at 50Hz)
    W = 25
    def peak_window(arr, idx_list, fn):
        return [fn(arr[max(0,i-W):min(len(arr),i+W)]) for i in idx_list]

    raise_ay = np.array(peak_window(ay_delta, raise_idx, np.min))  # most negative
    raise_gy = np.array(peak_window(abs_gy,   raise_idx, np.max))  # highest rotation

    rest_ay  = np.array(peak_window(ay_delta, rest_idx, np.min)) if len(rest_idx) else np.array([0.0])
    rest_gy  = np.array(peak_window(abs_gy,   rest_idx, np.max)) if len(rest_idx) else np.array([0.0])

    # Target: p25 of RAISE peaks → catches 75% of raises
    # Validate: threshold must be stricter than p75 of REST peaks (low false positive rate)
    rec_ay = np.percentile(raise_ay, 25)
    rec_gy = np.percentile(raise_gy, 25)

    # Scatter plot: separation between classes
    fig, (s1, s2) = plt.subplots(1, 2, figsize=(12, 5), facecolor=BG)
    fig.suptitle("Class separation — RAISE=green  |  REST=red", color="white")
    for ax_plot, raise_vals, rest_vals, xlabel, thresh in [
        (s1, raise_ay, rest_ay, "ayDelta peak (g)", rec_ay),
        (s2, raise_gy, rest_gy, "|gy| peak (°/s)",  rec_gy),
    ]:
        ax_plot.set_facecolor(DARK)
        ax_plot.scatter(range(len(raise_vals)), raise_vals, color="lime",  s=40, label="RAISE", zorder=3)
        ax_plot.scatter(range(len(rest_vals)),  rest_vals,  color="red",   s=40, label="REST",  zorder=3)
        ax_plot.axhline(thresh, color="white", linewidth=1.2, linestyle="--", label=f"rec={thresh:.2f}")
        ax_plot.set_xlabel(xlabel, color="white")
        ax_plot.tick_params(colors="white")
        ax_plot.legend(facecolor=DARK, labelcolor="white")
        for spine in ax_plot.spines.values():
            spine.set_edgecolor("#444")
    plt.tight_layout()
    out3 = path.replace(".csv", "_scatter.png")
    plt.savefig(out3, dpi=120, facecolor=BG)
    print(f"Scatter plot → {out3}")
    plt.close()

    # ── Simulate detector on recording ────────────────────────────────────────
    # Check how many RAISE events the recommended thresholds would catch
    tp = sum(1 for i in raise_idx
             if np.min(ay_delta[max(0,i-W):i+W]) < rec_ay
             and np.max(abs_gy[max(0,i-W):i+W]) > rec_gy)
    fp = sum(1 for i in rest_idx
             if np.min(ay_delta[max(0,i-W):i+W]) < rec_ay
             and np.max(abs_gy[max(0,i-W):i+W]) > rec_gy)

    print()
    print("═" * 62)
    print("  WRIST WAKE THRESHOLD RECOMMENDATIONS")
    print("═" * 62)
    print(f"\n  RAISE peaks  ayDelta: p25={np.percentile(raise_ay,25):.3f}  "
          f"median={np.median(raise_ay):.3f}  p75={np.percentile(raise_ay,75):.3f}")
    print(f"  RAISE peaks  |gy|:    p25={np.percentile(raise_gy,25):.1f}  "
          f"median={np.median(raise_gy):.1f}  p75={np.percentile(raise_gy,75):.1f}")
    if len(rest_idx):
        print(f"\n  REST  peaks  ayDelta: p25={np.percentile(rest_ay,25):.3f}  "
              f"median={np.median(rest_ay):.3f}")
        print(f"  REST  peaks  |gy|:    p25={np.percentile(rest_gy,25):.1f}  "
              f"median={np.median(rest_gy):.1f}")
    print(f"\n  Simulated accuracy (on this session):")
    print(f"    True positives : {tp}/{len(raise_idx)} RAISE caught")
    print(f"    False positives: {fp}/{len(rest_idx)} REST triggered")
    print(f"\n  Recommended thresholds:")
    print(f"    Stage 1 (gyro):   fabsf(gy) > {rec_gy:.0f}.0f")
    print(f"    Stage 2 (orient): ayDelta   < {rec_ay:.2f}f")
    print(f"\n  Paste into display_mgr.cpp dispMgr_checkWristWake():")
    print(f"    bool motionDetected = fabsf(gy) > {rec_gy:.0f}.0f;")
    print(f"    bool orientationOk  = (ayDelta < {rec_ay:.2f}f);")
    print("═" * 62)


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="SIDECAR V1 IMU Wrist-Wake Tuner")
    parser.add_argument("--serial",  metavar="PORT", help="Serial port to capture from")
    parser.add_argument("--capture", metavar="CSV",  help="Output CSV for capture")
    parser.add_argument("--analyse", metavar="CSV",  help="CSV file to analyse")
    args = parser.parse_args()

    if not args.capture and not args.analyse:
        parser.print_help()
        sys.exit(1)

    if args.capture:
        if not args.serial:
            print("ERROR: --capture requires --serial PORT")
            sys.exit(1)
        capture(args.serial, args.capture)

    if args.analyse:
        analyse(args.analyse)


if __name__ == "__main__":
    main()
