#!/usr/bin/env python3
"""
SIDECAR V1 — Battery Benchmark Tool
=====================================
Reads serial output from the watch (115200 baud) and BLE battery packets
forwarded via the Android app's adb logcat, builds a timestamped dataset,
then produces:

  1. A CSV log  →  benchmark_YYYYMMDD_HHMMSS.csv
  2. A PNG chart →  benchmark_YYYYMMDD_HHMMSS.png
  3. A summary printed to stdout with actual vs. model mA drain.

Usage
-----
  # Serial-only mode (watch connected via USB):
  python3 battery_benchmark.py --serial /dev/cu.usbserial-XXXX --duration 3600

  # Logcat mode (phone connected via adb, app running):
  python3 battery_benchmark.py --logcat --duration 3600

  # Both simultaneously:
  python3 battery_benchmark.py --serial /dev/cu.usbserial-XXXX --logcat --duration 3600

  # Replay an existing CSV (re-chart without re-running):
  python3 battery_benchmark.py --replay benchmark_20260401_120000.csv

What it measures
----------------
  - Battery % over time  (from B| BLE packets or AXP2101 serial lines)
  - Voltage (mV)
  - Estimated mA         (from B| packet 3rd field)
  - Power mode / screen state (parsed from serial log lines)
  - Real drain rate      (Δpct / Δtime → mAh, compared to model)

The script does NOT require a current probe — it uses the on-chip
AXP2101 coulomb counter / battery level, which updates every ~30s.
For absolute mA readings you still want a µCurrent Gold in series,
but this script lets you compare modes and optimisations quantitatively.
"""

import argparse
import csv
import os
import re
import sys
import threading
import time
from datetime import datetime
from collections import deque

# ── Optional imports ──────────────────────────────────────────────────────────
try:
    import serial
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False

try:
    import subprocess
    HAS_ADB = True
except ImportError:
    HAS_ADB = False

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import matplotlib.dates as mdates
    HAS_MPL = True
except ImportError:
    HAS_MPL = False

# ── Battery constants ─────────────────────────────────────────────────────────
BATT_MAH = 200   # prototype single cell — change to 1200 for final build

# ── Regex patterns for serial log lines ──────────────────────────────────────
RE_POWER_MODE  = re.compile(r'\[PWR\] -> (\w+) cpu=(\d+)MHz')
RE_SCREEN_ON   = re.compile(r'\[DISP\] screen on')
RE_SCREEN_OFF  = re.compile(r'\[DISP\] screen off')
RE_BLE_CONN    = re.compile(r'\[BLE\] connected')
RE_BLE_DISC    = re.compile(r'\[BLE\] disconnected')
RE_BAT_PUSHED  = re.compile(r'\[BAT\] pushed B\|(\d+)\|(\d+)\|(\d+)')
# Logcat line from BleManager: "RX: B|pct|mvolt|maX10"
RE_LOGCAT_BLE  = re.compile(r'RX: B\|(\d+)\|(\d+)\|(\d+)')

# ── Data record ───────────────────────────────────────────────────────────────
class Record:
    __slots__ = ('ts', 'pct', 'mvolt', 'est_ma', 'power_mode', 'cpu_mhz',
                 'screen_on', 'ble_conn')
    def __init__(self, ts, pct, mvolt, est_ma, power_mode, cpu_mhz,
                 screen_on, ble_conn):
        self.ts         = ts
        self.pct        = pct
        self.mvolt      = mvolt
        self.est_ma     = est_ma
        self.power_mode = power_mode
        self.cpu_mhz    = cpu_mhz
        self.screen_on  = screen_on
        self.ble_conn   = ble_conn

# ── Collector thread ──────────────────────────────────────────────────────────
class Collector:
    def __init__(self):
        self.records   = []
        self.lock      = threading.Lock()
        self._stop     = threading.Event()

        # Mutable state updated by log lines
        self.power_mode = "NORMAL"
        self.cpu_mhz    = 240
        self.screen_on  = True
        self.ble_conn   = False

    def stop(self):
        self._stop.set()

    def _parse_line(self, line: str, source: str):
        """Parse one log line, updating state and appending records."""
        now = time.time()

        # Power mode change
        m = RE_POWER_MODE.search(line)
        if m:
            self.power_mode = m.group(1)
            self.cpu_mhz    = int(m.group(2))

        if RE_SCREEN_ON.search(line):  self.screen_on = True
        if RE_SCREEN_OFF.search(line): self.screen_on = False
        if RE_BLE_CONN.search(line):   self.ble_conn  = True
        if RE_BLE_DISC.search(line):   self.ble_conn  = False

        # Battery reading from serial
        m = RE_BAT_PUSHED.search(line)
        if not m and source == "logcat":
            m_lc = RE_LOGCAT_BLE.search(line)
            if m_lc:
                m = m_lc  # same group layout

        if m:
            pct    = int(m.group(1))
            mvolt  = int(m.group(2))
            est_ma = int(m.group(3)) / 10.0
            r = Record(now, pct, mvolt, est_ma,
                       self.power_mode, self.cpu_mhz,
                       self.screen_on, self.ble_conn)
            with self.lock:
                self.records.append(r)
            print(f"  [{source}] {datetime.fromtimestamp(now).strftime('%H:%M:%S')} "
                  f"pct={pct}% {mvolt}mV est={est_ma:.1f}mA "
                  f"mode={self.power_mode} scr={'ON' if self.screen_on else 'OFF'}")

    def run_serial(self, port: str, baud: int = 115200):
        if not HAS_SERIAL:
            print("ERROR: pyserial not installed. Run: pip install pyserial")
            return
        try:
            ser = serial.Serial(port, baud, timeout=1)
            print(f"[serial] opened {port} @ {baud}")
            while not self._stop.is_set():
                try:
                    line = ser.readline().decode('utf-8', errors='replace').strip()
                    if line:
                        self._parse_line(line, "serial")
                except Exception:
                    pass
            ser.close()
        except Exception as e:
            print(f"[serial] ERROR: {e}")

    def run_logcat(self):
        if not HAS_ADB:
            print("ERROR: subprocess unavailable")
            return
        try:
            # Clear logcat first so we don't replay old messages
            subprocess.run(['adb', 'logcat', '-c'], check=True,
                           capture_output=True)
            proc = subprocess.Popen(
                ['adb', 'logcat', '-s', 'BleManager:D'],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                text=True, bufsize=1
            )
            print("[logcat] started — filtering BleManager")
            while not self._stop.is_set():
                line = proc.stdout.readline()
                if line:
                    self._parse_line(line, "logcat")
            proc.terminate()
        except FileNotFoundError:
            print("ERROR: adb not found. Add platform-tools to PATH.")
        except Exception as e:
            print(f"[logcat] ERROR: {e}")

# ── Analysis ──────────────────────────────────────────────────────────────────
def analyse(records: list) -> dict:
    if len(records) < 2:
        return {}

    duration_s  = records[-1].ts - records[0].ts
    duration_h  = duration_s / 3600.0

    pcts   = [r.pct   for r in records]
    mvolts = [r.mvolt for r in records]
    est_mas = [r.est_ma for r in records if r.est_ma > 0]

    pct_drop  = pcts[0] - pcts[-1]
    real_mah  = BATT_MAH * pct_drop / 100.0
    real_ma   = real_mah / duration_h if duration_h > 0 else 0.0

    # Per-mode breakdown
    mode_seconds = {}
    for i in range(len(records) - 1):
        mode = records[i].power_mode
        dt   = records[i+1].ts - records[i].ts
        mode_seconds[mode] = mode_seconds.get(mode, 0) + dt

    return {
        'duration_s':    duration_s,
        'pct_start':     pcts[0],
        'pct_end':       pcts[-1],
        'pct_drop':      pct_drop,
        'volt_start':    mvolts[0] / 1000,
        'volt_end':      mvolts[-1] / 1000,
        'real_mah':      real_mah,
        'real_ma_avg':   real_ma,
        'model_ma_avg':  sum(est_mas) / len(est_mas) if est_mas else 0,
        'mode_seconds':  mode_seconds,
    }

def print_summary(stats: dict):
    if not stats:
        print("Not enough data for analysis.")
        return

    h, rem = divmod(int(stats['duration_s']), 3600)
    m, s   = divmod(rem, 60)
    print("\n" + "═" * 56)
    print("  SIDECAR V1 — BATTERY BENCHMARK SUMMARY")
    print("═" * 56)
    print(f"  Duration     : {h}h {m:02d}m {s:02d}s")
    print(f"  Battery      : {stats['pct_start']}% → {stats['pct_end']}%  "
          f"(−{stats['pct_drop']}%)")
    print(f"  Voltage      : {stats['volt_start']:.3f}V → {stats['volt_end']:.3f}V")
    print(f"  Real drain   : {stats['real_mah']:.1f} mAh  "
          f"→  {stats['real_ma_avg']:.2f} mA avg")
    print(f"  Model est.   : {stats['model_ma_avg']:.2f} mA avg")
    if stats['model_ma_avg'] > 0:
        err = (stats['real_ma_avg'] - stats['model_ma_avg']) / stats['model_ma_avg'] * 100
        print(f"  Model error  : {err:+.1f}%")
    if stats['real_ma_avg'] > 0:
        life_h = BATT_MAH / stats['real_ma_avg']
        life_d = life_h / 24
        print(f"  Projected life ({BATT_MAH}mAh): {life_h:.0f}h = {life_d:.1f} days")
    print("\n  Time by mode:")
    for mode, secs in sorted(stats['mode_seconds'].items()):
        pct = secs / stats['duration_s'] * 100
        print(f"    {mode:<12} {secs/60:5.1f} min  ({pct:.0f}%)")
    print("═" * 56)

# ── CSV I/O ───────────────────────────────────────────────────────────────────
CSV_FIELDS = ['timestamp', 'pct', 'mvolt', 'est_ma',
              'power_mode', 'cpu_mhz', 'screen_on', 'ble_conn']

def save_csv(records: list, path: str):
    with open(path, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        w.writeheader()
        for r in records:
            w.writerow({
                'timestamp':  datetime.fromtimestamp(r.ts).isoformat(),
                'pct':        r.pct,
                'mvolt':      r.mvolt,
                'est_ma':     f"{r.est_ma:.1f}",
                'power_mode': r.power_mode,
                'cpu_mhz':    r.cpu_mhz,
                'screen_on':  int(r.screen_on),
                'ble_conn':   int(r.ble_conn),
            })
    print(f"CSV saved → {path}")

def load_csv(path: str) -> list:
    records = []
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            r = Record(
                ts         = datetime.fromisoformat(row['timestamp']).timestamp(),
                pct        = int(row['pct']),
                mvolt      = int(row['mvolt']),
                est_ma     = float(row['est_ma']),
                power_mode = row['power_mode'],
                cpu_mhz    = int(row['cpu_mhz']),
                screen_on  = row['screen_on'] == '1',
                ble_conn   = row['ble_conn'] == '1',
            )
            records.append(r)
    return records

# ── Chart ─────────────────────────────────────────────────────────────────────
MODE_COLORS = {
    'NORMAL':    '#FB6000',
    'EFFICIENT': '#FFD700',
    'DEEPSLEEP': '#00BFFF',
}

def save_chart(records: list, stats: dict, path: str):
    if not HAS_MPL:
        print("matplotlib not installed — skipping chart. pip install matplotlib")
        return

    times  = [datetime.fromtimestamp(r.ts) for r in records]
    pcts   = [r.pct                         for r in records]
    volts  = [r.mvolt / 1000.0              for r in records]
    est_ma = [r.est_ma                       for r in records]

    fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(14, 10), sharex=True)
    fig.patch.set_facecolor('#0A0A0A')
    for ax in (ax1, ax2, ax3):
        ax.set_facecolor('#0F0F0F')
        ax.tick_params(colors='#7A7A7A')
        ax.spines[:].set_color('#2A2A2A')
        ax.yaxis.label.set_color('#7A7A7A')

    # Shade background by power mode
    for i in range(len(records) - 1):
        col = MODE_COLORS.get(records[i].power_mode, '#333333')
        for ax in (ax1, ax2, ax3):
            ax.axvspan(times[i], times[i+1], alpha=0.08, color=col, linewidth=0)

    # Panel 1: Battery %
    ax1.plot(times, pcts, color='#FB6000', linewidth=1.5, label='Battery %')
    ax1.set_ylabel('Battery %', color='#7A7A7A')
    ax1.set_ylim(0, 105)
    ax1.yaxis.grid(True, color='#1E1E1E', linewidth=0.5)
    if stats:
        ax1.set_title(
            f"SIDECAR V1 Battery Benchmark — "
            f"real drain {stats['real_ma_avg']:.2f} mA avg  |  "
            f"model {stats['model_ma_avg']:.2f} mA avg  |  "
            f"projected {BATT_MAH / stats['real_ma_avg']:.0f}h "
            f"({BATT_MAH / stats['real_ma_avg'] / 24:.1f}d)" if stats['real_ma_avg'] > 0 else "",
            color='#FB6000', fontsize=10, pad=8
        )

    # Panel 2: Voltage
    ax2.plot(times, volts, color='#FFD700', linewidth=1.5, label='Voltage V')
    ax2.set_ylabel('Voltage (V)', color='#7A7A7A')
    ax2.yaxis.grid(True, color='#1E1E1E', linewidth=0.5)
    ax2.axhline(3.2, color='#FF3333', linewidth=0.8, linestyle='--', alpha=0.5,
                label='3.2V cutoff')
    ax2.legend(facecolor='#1A1A1A', edgecolor='#333333',
               labelcolor='#7A7A7A', fontsize=8)

    # Panel 3: Estimated mA
    ax3.plot(times, est_ma, color='#00BFFF', linewidth=1.2, label='Est. mA (model)')
    ax3.set_ylabel('Est. mA', color='#7A7A7A')
    ax3.yaxis.grid(True, color='#1E1E1E', linewidth=0.5)
    ax3.xaxis.set_major_formatter(mdates.DateFormatter('%H:%M'))
    ax3.xaxis.set_major_locator(mdates.AutoDateLocator())
    ax3.set_xlabel('Time', color='#7A7A7A')

    # Screen-off shading on mA panel
    in_off = False
    off_start = None
    for i, r in enumerate(records):
        if not r.screen_on and not in_off:
            in_off = True; off_start = times[i]
        elif r.screen_on and in_off:
            in_off = False
            ax3.axvspan(off_start, times[i], alpha=0.15, color='#555555', linewidth=0,
                        label='Screen off' if off_start == [t for t in [times[j]
                            for j, rr in enumerate(records) if not rr.screen_on]][0]
                        else '')
    # Mode legend
    for mode, col in MODE_COLORS.items():
        ax1.axhline(-99, color=col, linewidth=4, alpha=0.5, label=mode)
    ax1.legend(facecolor='#1A1A1A', edgecolor='#333333',
               labelcolor='#7A7A7A', fontsize=8, loc='lower left')

    plt.tight_layout(pad=1.5)
    plt.savefig(path, dpi=150, facecolor='#0A0A0A')
    plt.close()
    print(f"Chart saved → {path}")

# ── Main ──────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description='SIDECAR V1 battery benchmark tool',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument('--serial',   metavar='PORT',
                        help='Serial port (e.g. /dev/cu.usbserial-XXXX)')
    parser.add_argument('--baud',     type=int, default=115200)
    parser.add_argument('--logcat',   action='store_true',
                        help='Also capture adb logcat BleManager output')
    parser.add_argument('--duration', type=int, default=0,
                        help='Stop after N seconds (0 = run until Ctrl+C)')
    parser.add_argument('--replay',   metavar='CSV',
                        help='Re-chart an existing CSV without collecting new data')
    global BATT_MAH
    parser.add_argument('--batt-mah', type=int, default=None,
                        help=f'Battery capacity in mAh (default {BATT_MAH})')
    args = parser.parse_args()

    if args.batt_mah is not None:
        BATT_MAH = args.batt_mah

    stamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    csv_path   = f"benchmark_{stamp}.csv"
    chart_path = f"benchmark_{stamp}.png"

    # ── Replay mode ──────────────────────────────────────────────────────────
    if args.replay:
        print(f"Replaying {args.replay}…")
        records = load_csv(args.replay)
        stats   = analyse(records)
        print_summary(stats)
        out = args.replay.replace('.csv', '_replay.png')
        save_chart(records, stats, out)
        return

    # ── Collect mode ─────────────────────────────────────────────────────────
    if not args.serial and not args.logcat:
        parser.print_help()
        print("\nERROR: specify --serial PORT and/or --logcat")
        sys.exit(1)

    collector = Collector()
    threads   = []

    if args.serial:
        t = threading.Thread(target=collector.run_serial,
                             args=(args.serial, args.baud), daemon=True)
        t.start(); threads.append(t)

    if args.logcat:
        t = threading.Thread(target=collector.run_logcat, daemon=True)
        t.start(); threads.append(t)

    print(f"\nCollecting… press Ctrl+C to stop"
          f"{f' (or wait {args.duration}s)' if args.duration else ''}\n")

    try:
        if args.duration > 0:
            time.sleep(args.duration)
        else:
            while True:
                time.sleep(1)
    except KeyboardInterrupt:
        print("\nStopping…")

    collector.stop()
    time.sleep(0.5)

    with collector.lock:
        records = list(collector.records)

    print(f"\nCollected {len(records)} battery samples.")

    if not records:
        print("No data recorded. Check serial port / adb connection.")
        sys.exit(1)

    save_csv(records, csv_path)
    stats = analyse(records)
    print_summary(stats)
    save_chart(records, stats, chart_path)


if __name__ == '__main__':
    main()
