#!/usr/bin/env python3
"""
SIDECAR V1 — BLE Demo / Test Script
=====================================
Connects to the watch via BLE from a laptop and runs through every feature
automatically. No Android phone needed — useful for desk testing.

Requirements:
    pip install bleak

Usage:
    python3 tools/demo_test.py                    # auto-scan for SIDECAR V1
    python3 tools/demo_test.py --addr AA:BB:CC:DD  # connect to specific MAC
    python3 tools/demo_test.py --dry-run           # print steps without BLE

Each step sends a BLE packet, waits for the watch to process it, then waits
a configurable pause so you can visually confirm the watch screen changed.

BLE packets are logged to stdout with timestamps. Incoming packets from the
watch (battery telemetry, state sync) are printed as they arrive.
"""

import asyncio
import argparse
import sys
import time
from datetime import datetime, timedelta

try:
    from bleak import BleakScanner, BleakClient
    from bleak.exc import BleakError
    HAS_BLEAK = True
except ImportError:
    HAS_BLEAK = False

# ── Bellafaire BLE UUIDs (same as firmware) ──────────────────────────────────
SVC_UUID = "5ac9bc5e-f8ba-48d4-8908-98b80b566e49"
CHR_UUID = "bcca872f-1a3e-4491-b8ec-bfc93c5dd91a"
DEVICE_NAME = "SIDECAR V1"

# ── ANSI colours ─────────────────────────────────────────────────────────────
GRN  = "\033[92m"
RED  = "\033[91m"
YLW  = "\033[93m"
CYN  = "\033[96m"
ORG  = "\033[38;5;208m"
DIM  = "\033[2m"
RST  = "\033[0m"
BOLD = "\033[1m"


def ts() -> str:
    return datetime.now().strftime("%H:%M:%S")


def log(msg: str, colour: str = RST) -> None:
    print(f"{DIM}{ts()}{RST} {colour}{msg}{RST}")


def ok(msg: str) -> None:
    log(f"[ OK ] {msg}", GRN)


def fail(msg: str) -> None:
    log(f"[FAIL] {msg}", RED)


def step(n: int, total: int, title: str) -> None:
    bar = "█" * n + "░" * (total - n)
    pct = int(n / total * 100)
    print(f"\n{ORG}{BOLD}── Step {n}/{total} [{bar}] {pct}% — {title}{RST}")


# ── Packet definitions ────────────────────────────────────────────────────────
def pkt_time() -> str:
    return f"T|{int(time.time())}"


def pkt_call(name: str = "Mum") -> str:
    return f"C|{name}"


def pkt_notif(app: str, title: str, body: str) -> str:
    return f"N|{app}|{title}|{body}"


def pkt_media(artist: str, song: str, playing: bool = True, vol: int = 72) -> str:
    return f"S|{artist}|{song}|{1 if playing else 0}|{vol}"


def pkt_weather(temp: int, hi: int, lo: int, cond: str) -> str:
    return f"W|{temp}|{hi}|{lo}|{cond}"


def pkt_config(key: str, value: int) -> str:
    return f"X|{key}|{value}"


def pkt_dnd(enabled: bool) -> str:
    return pkt_config("DND", 1 if enabled else 0)


def pkt_power(mode: int) -> str:
    return pkt_config("POWER", mode)


def pkt_bright(level: int) -> str:
    return pkt_config("BRIGHT", level)


def pkt_alarm(idx: int, hour: int, minute: int, enabled: bool = True,
              days: int = 0x7F, use_date: bool = False,
              day: int = 1, month: int = 1) -> str:
    return (f"X|ALARM|{idx}|{hour}|{minute}"
            f"|{1 if enabled else 0}|{days}"
            f"|{1 if use_date else 0}|{day}|{month}")


def pkt_pwrcfg(mode: int, brightness: int, timeout: int) -> str:
    return f"X|PWRCFG|{mode}|{brightness}|{timeout}"


# ── Demo sequence ─────────────────────────────────────────────────────────────
DEMO_STEPS = []   # filled below

def build_demo():
    """Return list of (title, pause_s, [packets], note) tuples."""
    now = datetime.now()
    alarm_time = now + timedelta(minutes=3)

    return [
        (
            "Time sync",
            2.0,
            [pkt_time()],
            "Watch clock updates to current time"
        ),
        (
            "Weather push — SUNNY 24°C (hi 28 / lo 18)",
            3.0,
            [pkt_weather(24, 28, 18, "SUNNY")],
            "Switch watch to WEATHER screen — shows temp + condition"
        ),
        (
            "Weather push — RAIN 12°C",
            3.0,
            [pkt_weather(12, 15, 9, "RAIN")],
            "WEATHER screen updates to rainy condition"
        ),
        (
            "Weather push — STORM 8°C",
            2.5,
            [pkt_weather(8, 10, 5, "STORM")],
            "WEATHER screen shows STORM"
        ),
        (
            "Notification — CALL / cyan overlay",
            3.5,
            [pkt_call("Mum")],
            "Watch shows CYAN call overlay — dismiss with BtnA"
        ),
        (
            "Notification — MSG / WhatsApp (green)",
            3.5,
            [pkt_notif("WhatsApp", "Alice", "Hey are you coming tonight?")],
            "GREEN notification overlay"
        ),
        (
            "Notification — MSG / Telegram (green)",
            3.5,
            [pkt_notif("Telegram", "Bob", "Check this out!")],
            "GREEN overlay — Telegram classified as messaging"
        ),
        (
            "Notification — MSG / Signal (green)",
            3.5,
            [pkt_notif("Signal", "Carol", "Encrypted message")],
            "GREEN overlay — Signal classified as messaging"
        ),
        (
            "Notification — APP / Gmail (orange)",
            3.5,
            [pkt_notif("Gmail", "Order shipped", "Your package is on its way!")],
            "ORANGE overlay — generic app"
        ),
        (
            "Notification — APP / GitHub (orange)",
            3.0,
            [pkt_notif("GitHub", "New PR", "sidecar-v1 #42 opened")],
            "ORANGE overlay — GitHub notification"
        ),
        (
            "Media state — now playing (Daft Punk)",
            3.0,
            [pkt_media("Daft Punk", "Get Lucky", True, 72)],
            "Switch to MEDIA screen — shows artist + song + play icon"
        ),
        (
            "Media state — paused",
            2.0,
            [pkt_media("Daft Punk", "Get Lucky", False, 72)],
            "MEDIA screen shows paused icon"
        ),
        (
            "Media state — new track (Radiohead)",
            3.0,
            [pkt_media("Radiohead", "Exit Music", True, 65)],
            "MEDIA screen updates track info"
        ),
        (
            "DND — enable",
            2.5,
            [pkt_dnd(True)],
            "Watch header shows orange DND dot. Buzz suppressed."
        ),
        (
            "Notification while DND (silent)",
            3.5,
            [pkt_notif("YouTube", "New video", "Someone you follow uploaded")],
            "Notif arrives but NO beep/buzz — DND working"
        ),
        (
            "DND — disable",
            2.0,
            [pkt_dnd(False)],
            "Orange DND dot disappears from header"
        ),
        (
            "Alarm #1 — set to 3min from now, all days",
            3.0,
            [pkt_alarm(0, alarm_time.hour, alarm_time.minute, True, 0x7F)],
            f"Watch ALARM screen shows {alarm_time.strftime('%H:%M')} enabled"
        ),
        (
            "Alarm #2 — weekdays only (Mon-Fri)",
            3.0,
            [pkt_alarm(1, 7, 30, True, 0b0111110)],  # Mon=bit1..Fri=bit5
            "Watch ALARM screen: alarm 2 set Mon-Fri 07:30"
        ),
        (
            "Alarm #3 — disabled placeholder",
            2.0,
            [pkt_alarm(2, 9, 0, False, 0x7F)],
            "Watch ALARM screen: alarm 3 disabled"
        ),
        (
            "Power mode → EFFICIENT",
            3.5,
            [pkt_power(1)],
            "Watch drops to 80MHz, BLE adv slows — POWER screen shows EFFICIENT"
        ),
        (
            "Power mode → DEEPSLEEP",
            3.5,
            [pkt_power(2)],
            "Watch drops BLE, 80MHz only — BLE may disconnect briefly"
        ),
        (
            "Power mode → NORMAL",
            3.0,
            [pkt_power(0)],
            "Watch back to 160MHz full features"
        ),
        (
            "Brightness — dim (level 1)",
            2.0,
            [pkt_bright(1)],
            "Watch dims noticeably"
        ),
        (
            "Brightness — default (level 3)",
            1.5,
            [pkt_bright(3)],
            "Watch returns to default brightness"
        ),
        (
            "Brightness — max (level 5)",
            2.0,
            [pkt_bright(5)],
            "Watch at full brightness"
        ),
        (
            "Brightness — restore default",
            1.5,
            [pkt_bright(3)],
            "Restored to level 3"
        ),
        (
            "Power config — set all 3 mode configs",
            2.5,
            [
                pkt_pwrcfg(0, 3, 60),   # NORMAL: br=3, tmo=60s
                pkt_pwrcfg(1, 2, 30),   # EFFICIENT: br=2, tmo=30s
                pkt_pwrcfg(2, 1, 10),   # DEEPSLEEP: br=1, tmo=10s
            ],
            "Power profiles synced — visible in POWER screen custom edit"
        ),
        (
            "AOD face — clock+steps",
            2.5,
            [pkt_config("AOD_FACE", 1)],
            "Let screen time out — AOD shows clock + step count"
        ),
        (
            "AOD face — clock+alarm",
            2.5,
            [pkt_config("AOD_FACE", 2)],
            "Let screen time out — AOD shows clock + next alarm time"
        ),
        (
            "AOD face — clock only (restore)",
            1.5,
            [pkt_config("AOD_FACE", 0)],
            "AOD back to clock-only face"
        ),
        (
            "Find My Phone — ACTION REQUIRED",
            6.0,
            [],   # watch-initiated; we just wait
            (
                "On watch: go to NOTIFS screen, hold BtnA for 1s.\n"
                "          Phone (companion app) should ring + vibrate 3× burst.\n"
                "          Demo waits 6s for you to test this."
            )
        ),
        (
            "Restore clean state",
            2.0,
            [pkt_bright(3), pkt_dnd(False), pkt_power(0)],
            "Brightness=3, DND=off, mode=NORMAL"
        ),
    ]


# ── BLE helpers ───────────────────────────────────────────────────────────────
async def send(client: BleakClient, char_uuid: str, packet: str) -> None:
    data = (packet + "\n").encode("utf-8")
    await client.write_gatt_char(char_uuid, data, response=False)
    log(f"  TX  {packet!r}", CYN)


def on_notify(sender, data: bytearray) -> None:
    try:
        pkt = data.decode("utf-8").strip()
        parts = pkt.split("|")
        if parts[0] == "B" and len(parts) >= 3:
            pct = parts[1]
            mv  = parts[2]
            ma_x10 = int(parts[3]) if len(parts) >= 4 else 0
            log(f"  RX  BATTERY  {pct}% · {mv}mV · {ma_x10/10:.1f}mA", YLW)
        elif parts[0] == "ST":
            log(f"  RX  STATE_SYNC  {pkt}", YLW)
        elif parts[0] == "MC":
            log(f"  RX  MEDIA_CMD  {parts[1] if len(parts)>1 else '?'}", YLW)
        elif parts[0] == "FP":
            ok("  RX  FIND_MY_PHONE received from watch!")
        else:
            log(f"  RX  {pkt!r}", YLW)
    except Exception:
        pass


async def run_demo(addr: str | None, dry_run: bool, pause_multiplier: float) -> int:
    demo = build_demo()
    total = len(demo)

    print(f"\n{ORG}{BOLD}{'='*60}{RST}")
    print(f"{ORG}{BOLD}  SIDECAR V1 — DEMO / TEST  ({total} steps){RST}")
    print(f"{ORG}{BOLD}{'='*60}{RST}\n")

    if dry_run:
        log("DRY RUN — no BLE connection", YLW)
        for i, (title, pause, packets, note) in enumerate(demo, 1):
            step(i, total, title)
            for p in packets:
                log(f"  TX  {p!r}", CYN)
            log(f"  NOTE: {note}", DIM)
            log(f"  pause {pause * pause_multiplier:.1f}s", DIM)
        ok(f"\nDry run complete — {total} steps")
        return 0

    if not HAS_BLEAK:
        fail("bleak not installed — run:  pip install bleak")
        return 1

    # ── Scan ──────────────────────────────────────────────────────────────────
    if not addr:
        log(f"Scanning for '{DEVICE_NAME}' …", YLW)
        devices = await BleakScanner.discover(timeout=8.0)
        for d in devices:
            if d.name == DEVICE_NAME:
                addr = d.address
                ok(f"Found {DEVICE_NAME} at {addr}")
                break
        if not addr:
            fail(f"'{DEVICE_NAME}' not found. Is the watch on and BLE advertising?")
            return 1

    log(f"Connecting to {addr} …", YLW)
    errors = 0

    async with BleakClient(addr, timeout=15.0) as client:
        ok(f"Connected (MTU={client.mtu_size})")

        # Subscribe to notifications
        await client.start_notify(CHR_UUID, on_notify)
        log("Subscribed to GATT notifications")

        t_start = time.time()

        for i, (title, pause, packets, note) in enumerate(demo, 1):
            step(i, total, title)
            log(f"  {note}", DIM)

            for p in packets:
                try:
                    await send(client, CHR_UUID, p)
                    await asyncio.sleep(0.12)   # give GATT queue time to drain
                except BleakError as e:
                    fail(f"  TX failed: {e}")
                    errors += 1

            await asyncio.sleep(pause * pause_multiplier)

        elapsed = time.time() - t_start
        await client.stop_notify(CHR_UUID)

    print(f"\n{ORG}{BOLD}{'='*60}{RST}")
    status = GRN if errors == 0 else RED
    result = "PASS" if errors == 0 else f"PARTIAL ({errors} errors)"
    print(f"{status}{BOLD}  DEMO {result} — {total} steps in {elapsed:.0f}s{RST}")
    print(f"{ORG}{BOLD}{'='*60}{RST}\n")
    return 0 if errors == 0 else 1


# ── Entry point ───────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(
        description="Sidecar V1 BLE demo/test script",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    ap.add_argument("--addr", metavar="MAC", help="BLE address (skip scan)")
    ap.add_argument("--dry-run", action="store_true",
                    help="Print steps without connecting")
    ap.add_argument("--fast", action="store_true",
                    help="Halve all pauses (quicker run-through)")
    ap.add_argument("--slow", action="store_true",
                    help="Double all pauses (more time to read watch screen)")
    args = ap.parse_args()

    multiplier = 0.5 if args.fast else (2.0 if args.slow else 1.0)

    if not HAS_BLEAK and not args.dry_run:
        print("ERROR: bleak not installed.  pip install bleak")
        print("       or use --dry-run to preview without BLE")
        sys.exit(1)

    sys.exit(asyncio.run(run_demo(args.addr, args.dry_run, multiplier)))


if __name__ == "__main__":
    main()
