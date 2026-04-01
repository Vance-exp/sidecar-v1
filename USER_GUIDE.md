# SIDECAR V1 — User Guide

**Firmware v1.0-stable · Hardware: M5StickC PLUS2**

---

## Boot Log (confirmed working on flash)

```
[SIDECAR V1] boot
[PWR] pre-BLE heap=205624
[BLE] modem sleep enabled
[BLE] GATT server started
[PWR] post-BLE heap=185772
[SD] init failed — no card or wrong pins   ← expected if no SD card wired
[SIDECAR V1] ready  mode=EFFICIENT  heap=120608
```

RAM free at boot: **120 KB** of 320 KB · Flash used: **37%** of 3.2 MB

---

## Hardware Overview

| Button | Location |
|--------|----------|
| **BtnA** | Front face (large) |
| **BtnB** | Right side (small) |
| **BtnPWR** | Top (power button) |

**Button legend used in this guide:**

| Symbol | Meaning |
|--------|---------|
| `A·` | BtnA short press |
| `A—` | BtnA long press (>700ms) |
| `B·` | BtnB short press |
| `B—` | BtnB long press |
| `P·` | BtnPWR short press |
| `P—` | BtnPWR long press |

---

## Global Controls (work on any screen)

| Action | Effect |
|--------|--------|
| Any button (screen off) | Wake display |
| `B·` | Next screen (cycle forward) |
| `B—` | Previous screen (cycle backward) |
| `P—` (most screens) | Jump directly to **Settings** |
| `A—` (most screens) | Go home to **Clock** |
| Any button (alarm ringing) | Dismiss alarm |

---

## Screen Tour

The watch cycles through **10 screens** in order. Use `B·` / `B—` to navigate.

```
CLOCK → STOPWATCH → TIMER → ALARM → NOTIFS → MEDIA → WEATHER → POWER → DIAG → SETTINGS
```

---

### 1. CLOCK (home)

**Displays:** Large time (HH:MM:SS), date, day-of-week, step count, battery %, BLE status.
**Status bar** (top of every screen): `HH:MM  BATT%  [BLE dot]  STEPS`

| Button | Action |
|--------|--------|
| `P·` | Cycle power mode (NORMAL → EFFICIENT → DEEPSLEEP → NORMAL) |

---

### 2. STOPWATCH

**Displays:** Elapsed time (MM:SS.cs), lap times (up to 8 laps), running indicator.

| Button | Action |
|--------|--------|
| `A·` | Start / Stop |
| `P·` | Lap (while running) |
| `A—` | Reset (clears laps) |

---

### 3. TIMER

Three timer modes — cycle with `A—` when idle:

#### Normal mode
Countdown timer with arc progress ring. Set 1–99 minutes.

| Button | Action |
|--------|--------|
| `P·` (idle) | +1 minute |
| `P—` (idle) | −1 minute |
| `A·` (idle) | Start |
| `A·` (running) | Pause |
| `A·` (paused) | Resume |
| `P·` (running/paused) | Cancel + reset |
| `A—` (idle) | Cycle to next mode |
| `A—` (running/paused) | Cancel + reset |

#### Pomodoro mode
25-minute work session → 5-minute break, **auto-cycles indefinitely**.
- Work done: beeps, auto-starts 5-min break
- Break done: beeps, auto-starts next 25-min work session
- Controls same as Normal except time is preset.

#### Breathing mode (4-4-4 box breathing)
12-second cycle: **4s inhale → 4s hold → 4s exhale**, animated arc.
Runs continuously until cancelled.

| Button | Action |
|--------|--------|
| `A·` (idle) | Start breathing session |
| `A—` (running) | Stop |

---

### 4. ALARM

Up to **3 independent alarms**. Each supports day-of-week repeat OR one-shot date.

| Button | Action |
|--------|--------|
| `P·` (not editing) | Cycle to next alarm slot (1/2/3) |
| `A·` (not editing) | Toggle alarm enabled/disabled |
| `A—` | Enter / exit edit mode |
| `A·` (editing) | Increment current field |
| `P·` (editing) | Advance to next field (saves on exit) |

**Edit fields (in order):** Hour → Minute → Sun → Mon → Tue → Wed → Thu → Fri → Sat → Use-date toggle → Day → Month

When **Use-date** is on, the alarm fires once on the specified day/month instead of repeating on days of the week.

---

### 5. NOTIFICATIONS

Stores up to **10 notifications**. Colour-coded by type:
- 🔵 **CALL** (cyan bar) — incoming call
- 🟢 **MSG** (green bar) — WhatsApp, Telegram, Signal, SMS, etc.
- 🟠 **APP** (orange bar) — all other apps

Unread count shown as dot in status bar header.

| Button | Action |
|--------|--------|
| `P·` | Scroll to next notification |
| `A·` | Dismiss current notification |
| `A—` | **Find My Phone** — sends ring+vibrate command to phone |

**Notification overlay** (auto-pops on new notif):

| Button | Action |
|--------|--------|
| `A·` | Mark read + close overlay |
| `A—` | Delete notification |
| `B·` / `P·` | Close overlay (keep unread) |

---

### 6. MEDIA

Controls the currently playing app on your phone (Spotify, YouTube, etc.).

| Button | Action |
|--------|--------|
| `A·` | Play / Pause |
| `A—` | Next track |
| `P·` | Volume up |
| `P—` | Volume down |

**Displays:** Artist, song title, play/pause state, volume level.

---

### 7. WEATHER

Current conditions fetched automatically on BLE connect. Updates each time the companion app reconnects.

**Displays:** Current temperature (°C), condition icon, day high / low, data age.

Conditions: `SUNNY` `PCLOUD` `CLOUD` `FOG` `RAIN` `SNOW` `STORM`

> Weather is fetched from wttr.in — no account or API key required.
> Shows "No weather data / Connect phone to sync" until first fetch.

---

### 8. POWER

Configure and apply power modes. Three modes:

| Mode | CPU | BLE | Screen-off | Avg current |
|------|-----|-----|-----------|-------------|
| **NORMAL** | 160 MHz | On (100ms adv) | 80 MHz + light sleep | ~9.5 mA |
| **EFFICIENT** | 80 MHz | On (500ms adv) | 80 MHz + light sleep | ~4 mA |
| **DEEPSLEEP** | 80 MHz | Off | No IMU steps | ~2 mA |

**Projected battery life (200 mAh prototype / 1200 mAh final):**

| Mode | 200 mAh | 1200 mAh |
|------|---------|---------|
| NORMAL | ~21 h | ~5.3 days |
| EFFICIENT | ~50 h | ~12.5 days |
| DEEPSLEEP | ~100 h | ~25 days |

| Button | Action |
|--------|--------|
| `P·` | Cycle selected tab (N / E / D) |
| `A·` | Apply selected mode |
| `A—` | Enter / exit custom edit for selected mode |
| `A·` (custom) | Increment field (brightness or timeout) |
| `P·` (custom) | Advance to next field |

**Customisable per mode:** brightness level (0–5), screen timeout (0–120s in 5s steps; 0 = never off).

> **Predictive auto-switch:** if the watch has been idle for 2 hours (no steps, no BLE, no button press) while in NORMAL mode, it automatically drops to EFFICIENT.

---

### 9. DIAG (Diagnostics)

**Displays:** Power mode, CPU freq, free heap, battery %, voltage (mV), estimated current draw (mA), step count, BLE connected, RTC time, uptime.

| Button | Action |
|--------|--------|
| `A·` | Reset step counter to 0 |

---

### 10. SETTINGS

14 settings accessible in two columns. Use `P·` to advance field, `A·` to increment, `A—` to decrement.

| # | Setting | Values |
|---|---------|--------|
| 0 | **Hour** | 0–23 |
| 1 | **Minute** | 0–59 |
| 2 | **Date day** | 1–31 |
| 3 | **Month** | 1–12 |
| 4 | **Year** | 2024–2099 |
| 5 | **Power mode** | NORMAL / EFFICIENT / DEEPSLEEP |
| 6 | **Brightness** | 0–5 |
| 7 | **Auto-rotate** | ON / OFF |
| 8 | **AOD** | ON / OFF |
| 9 | **Sound** | SILENT / GENERAL |
| 10 | **Notif timeout** | 5s / 10s / 20s / 30s / OFF |
| 11 | **Wrist wake** | ON / OFF |
| 12 | **DND** | ON / OFF |
| 13 | **AOD face** | CLOCK / +STEPS / +ALARM |

All settings are **EEPROM-persisted** — survive reboot and power-off.

---

## Always-On Display (AOD)

When AOD is enabled (Settings #8) and the screen would otherwise turn off, the display dims to minimum brightness showing one of three faces:

| Face (Settings #13) | Shows |
|--------------------|-------|
| **CLOCK** | HH:MM centred |
| **+STEPS** | HH:MM + step count below |
| **+ALARM** | HH:MM + next enabled alarm time |

---

## Do Not Disturb (DND)

Enable via **Settings #12** or the companion app. While on:
- No notification buzz or beep
- No wrist-raise wake
- Timer done is silent
- Orange dot appears in status bar header

DND is synced bidirectionally with the companion app.

---

## Wrist-Raise Wake

When enabled (Settings #11), tilting the watch face toward you wakes the screen. Detected via MPU6886 accelerometer. Disabled automatically when DND is on.

---

## Find My Phone

From the **Notifications screen**, hold `A—` to send a Find My Phone command.
The companion app will:
1. Vibrate in a 500ms buzz pattern (×3)
2. Play the default ringtone for 3 seconds

Works only when BLE is connected. Watch plays a confirmation beep.

---

## SD Card Logging

Insert a MicroSD card wired to the SPI hat connector (G25/G26/G0/G36).
The watch logs one CSV row per minute to `/SIDECAR.CSV`:

```
unix_ts,batt_pct,batt_mv,steps
20260401143022,82,4050,1247
```

`unix_ts` is encoded as `YYYYMMDDHHMMSS` (from RTC — accurate after BLE time sync).
Gracefully no-ops if no card is present.

**Analyse with:**
```bash
python3 tools/analyse_log.py SIDECAR.CSV
```

---

## IMU Recording Mode (hidden / developer)

Hold **BtnA + BtnB together for 2 seconds** to enter IMU recording mode.
Streams accel + gyro CSV to serial at 50 Hz:
```
ts_ms,ax,ay,az,gx,gy,gz,event
```
- `A·` stamps `RAISE` event · `B·` stamps `REST` event
- Hold A+B again for 2s to stop
- Normal watch functions are suspended while recording

---

## Companion App

**Android only.** Install the APK from the companion app build.

### Features
- Auto-reconnects in background via foreground service
- Sends notifications (categorised by app)
- **Weather**: fetches wttr.in on every BLE connect, pushes to watch automatically
- **Media**: mirrors active MediaSession, volume controls
- **Alarms**: edit and sync all 3 alarms from phone
- **Battery chart**: real-time and historical current draw graph
- **Find My Phone**: triggered by watch → phone rings
- **DND sync**: toggle on phone mirrors to watch
- **App filter**: choose which apps send notifications to the watch

### BLE Packet Reference (Bellafaire protocol)

| Direction | Packet | Meaning |
|-----------|--------|---------|
| Phone → Watch | `T|unix_ts` | Time sync |
| Phone → Watch | `N|APP|TITLE|BODY` | Notification |
| Phone → Watch | `C|name` | Incoming call |
| Phone → Watch | `S|artist|song|play|vol` | Media state |
| Phone → Watch | `W|tempC|hi|lo|cond` | Weather update |
| Phone → Watch | `X|KEY|val` | Config (power, alarms, DND…) |
| Watch → Phone | `MC|cmd` | Media command (PLAY/NEXT/VOLU…) |
| Watch → Phone | `FP|` | Find My Phone |
| Watch → Phone | `B|pct|mv|maX10` | Battery telemetry |
| Watch → Phone | `ST|KEY|val` | State sync (power mode, DND, alarms…) |

---

## Power Tips

- **Daily use:** start in EFFICIENT mode — BLE stays on for notifications, ~50h battery (200mAh prototype)
- **Preserve battery overnight:** the predictive auto-switch handles this — idle for 2h with no phone → drops to EFFICIENT automatically
- **Off-grid / expedition:** switch to DEEPSLEEP — alarm and timer still work, no BLE, ~100h
- **Charging:** magnetic pogo-pin connector (no USB port on the bracer)

---

*Sidecar V1 — "Never leave beauty for function and function for beauty."*
