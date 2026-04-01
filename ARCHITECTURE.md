# SIDECAR V1 — Architecture Report

A complete guide to the firmware for someone who's never touched embedded code before.

---

## What is this thing?

Sidecar V1 is a cyberdeck bracer watch — a tiny computer strapped to your wrist. The hardware is an **M5StickC PLUS2**, which packs:

- **ESP32** microcontroller (dual-core, 240MHz, WiFi+Bluetooth)
- **1.14" TFT screen** (240x135 pixels, ST7789V2 driver)
- **MPU6886** accelerometer/gyroscope (counts your steps)
- **AXP2101** power management IC (battery charging, voltage rails)
- **BLE** (Bluetooth Low Energy) for phone notifications

The firmware runs on bare metal (no operating system like Linux) using the Arduino framework. It boots in ~1 second and runs a simple `setup()` → `loop()` cycle forever.

---

## The Big Picture

```
┌─────────────────────────────────────────────────┐
│                    main.cpp                      │
│         setup() → loop() → handleButtons()       │
│                                                   │
│  Reads buttons, dispatches to modules, ties       │
│  everything together. ~490 lines.                 │
└──────┬──────┬──────┬──────┬──────┬──────┬────────┘
       │      │      │      │      │      │
       v      v      v      v      v      v
   display  power   BLE   notif   time   step
   _mgr    _mgr    _mgr  _queue  _mgr   _ctr
```

Every module is a `.h` (declarations) + `.cpp` (implementation) pair. They communicate through **global variables** (prefixed with `g_`) and **function calls**. There are no classes, no inheritance hierarchies, no design patterns — just plain C-style modules.

---

## File-by-File Breakdown

### `hardware_config.h` (143 lines) — The Single Source of Truth

Every magic number lives here and NOWHERE else:

- **Pin definitions**: `PIN_PWR_RAIL=4` (must stay HIGH or the watch dies), `PIN_IR_TX=9`
- **Display constants**: 240x135, rotation 1 (landscape)
- **BLE UUIDs**: from the Bellafaire open-source watch project (MIT licensed)
- **Color palette**: `NIXIE_ORANGE` (0xFB60), `NIXIE_DIM`, `NIXIE_GHOST` — the phosphor-tube aesthetic
- **EEPROM layout**: 32 bytes of persistent storage, mapped byte-by-byte
- **Enums**: `PowerMode` (NORMAL/EFFICIENT/DEEPSLEEP), `Screen` (8 screens), `TimerState`, `SoundProfile`
- **Structs**: `PwrCfg`, `AlarmCfg`, `Notif`
- **Helper functions**: `daysInMonth()`, `calcDow()` (day-of-week), `fmtMs()` (format milliseconds), `brtValue()` (brightness lookup table)

**Why it matters:** If you need to change a pin, a color, a timing constant, or add a new screen — this is the ONLY file you touch. No grep-and-replace across 12 files.

### `main.cpp` (489 lines) — The Brain

Three functions matter:

1. **`setup()`** — runs once at boot:
   - Holds GPIO4 HIGH (power rail — without this, the AXP2101 PMIC kills the entire board)
   - Initializes M5Unified (which auto-configures display, IMU, RTC, buttons, speaker)
   - Reads EEPROM for saved settings (brightness, power mode, sound profile)
   - Shows "SIDECAR V1 / ALIVE" splash screen
   - Initializes all subsystem modules

2. **`loop()`** — runs ~100 times/second forever:
   - `M5.update()` — polls button hardware
   - `handleButtons()` — the button dispatcher
   - `tmMgr_update()` — checks alarms
   - `stepCtr_update()` — reads accelerometer
   - Checks volatile flags from BLE (notifications, alarm fired)
   - `dispMgr_update()` — redraws screen if dirty

3. **`handleButtons()`** — THE critical function, and where the old code had its worst bug:

   ```cpp
   // CRITICAL FIX: capture all wasPressed() into locals ONCE
   bool aPrs = M5.BtnA.wasPressed();
   bool bPrs = M5.BtnB.wasPressed();
   bool pPrs = M5.BtnPWR.wasPressed();
   ```

   **The old bug:** `wasPressed()` is a *latch* — it returns `true` once, then clears itself. The old code called `wasPressed()` in multiple `if` branches. The first branch consumed the event; later branches never saw it. Buttons appeared dead.

   **The fix:** Read all three buttons into local booleans ONCE at the top. Every subsequent check uses the local copy.

   Button logic flow:
   ```
   capture wasPressed → record timestamps → wake check → alarm dismiss →
   banner dismiss → long-press detect → short-press dispatch
   ```

### `display_mgr.cpp` (1020 lines) — The Eyes

The largest module. Owns ALL drawing — no other module touches the LCD.

**Key concept: Double-buffered sprite drawing.**
Instead of drawing pixels directly to the screen (which flickers), we draw to an off-screen buffer (LGFX_Sprite), then push the entire buffer to the LCD in one DMA transfer. Zero flicker.

**Key concept: Dirty flag.**
`g_displayDirty` tracks whether anything changed. If nothing changed since last frame, we skip drawing entirely. This saves ~15mA when the screen is static.

**8 screens**, each drawn by its own function:
1. **Clock** — large time display, date, day-of-week, step count, battery
2. **Stopwatch** — start/stop/lap with mm:ss.cc format
3. **Timer** — countdown timer, 1-120 minutes
4. **Alarm** — 3 configurable alarms with day-of-week bitmask
5. **Notifications** — scrollable list from phone via BLE
6. **Power** — switch between NORMAL/EFFICIENT/DEEPSLEEP, customize each
7. **Diagnostics** — battery graph (60-sample ring buffer), heap, CPU freq, step count
8. **Settings** — time/date set, brightness, auto-rotate, AOD, sound

**Screen sleep/wake:**
After `timeoutSec` seconds of no button press, the screen dims → turns off (sends SLPIN command to LCD controller). Any button press wakes it. Optional AOD (Always-On Display) shows a minimal dim clock instead of fully sleeping.

**Battery smoothing:**
Raw battery readings from the AXP2101 jump ±5% randomly. We use EMA (Exponential Moving Average) with alpha=0.35 to smooth it. A 60-sample ring buffer stores mA readings for the diagnostic graph.

### `power_manager.cpp` (100 lines) — The Governor

Three power modes:

| Mode | CPU | BLE | Use Case |
|---|---|---|---|
| NORMAL | 240MHz | on, fast advertising | Full features, ~58mA |
| EFFICIENT | 80MHz | on, slow advertising | Daily wear, ~41mA |
| DEEPSLEEP | 40MHz | OFF | Airplane mode, ~15mA |

**The tricky part: mode transitions.**

BLE radio requires >=80MHz. You can't just `setCpuFrequencyMhz(40)` with BLE running — it crashes.

- **NORMAL <-> EFFICIENT**: Safe at runtime. Just change CPU freq and BLE advertising interval.
- **Anything <-> DEEPSLEEP**: Must `ESP.restart()`. The new mode is saved to EEPROM *before* restart, so on reboot `pwrMgr_init()` reads it and configures accordingly (skipping BLE init in DEEPSLEEP).

**The old bug:** Cycling through power modes on the clock screen would go DEEPSLEEP → NORMAL, which triggered restart, but booted back into DEEPSLEEP (EEPROM value wasn't updated before the restart). Infinite reboot loop. Fixed by writing EEPROM BEFORE calling `ESP.restart()`.

### `ble_manager.cpp` (89 lines) — The Radio

NimBLE GATT server. The phone (running the Bellafaire Android app) connects and writes packets to a single BLE characteristic.

**Packet protocol** (pipe-delimited):
```
T|1711843200        → time sync (Unix timestamp)
N|WhatsApp|John|Hey → notification
S|Artist|Song       → media/Spotify update
C|Mom               → incoming call
```

The `onWrite` callback parses the first character, then dispatches:
- `T` → `tmMgr_onBleTime()` — sets the ESP32's system clock
- `N` → `nq_onNotifPacket()` — pushes to notification queue
- `S` → `nq_onMediaPacket()` — pushes as "Music" notification
- `C` → `nq_onCallPacket()` — pushes as "CALL" notification

**Thread safety warning:** BLE callbacks run on NimBLE's FreeRTOS task (core 0). The main loop runs on core 1. We NEVER call `M5.Display` from a BLE callback — that would corrupt the display driver. Instead, callbacks set volatile flags (`g_nqNewNotif`, `g_alarmJustFired`), and the main loop checks those flags and does the actual screen work.

### `notif_queue.cpp` (111 lines) — The Mailbox

10-slot ring buffer for notifications. When full, oldest notification shifts out.

`nq_push()` is called from BLE callbacks (different FreeRTOS task). It writes to the array and sets `g_nqNewNotif = true`. The main loop sees this flag and plays the notification sound + wakes the screen.

Packet parsing functions (`nq_onNotifPacket`, `nq_onMediaPacket`, `nq_onCallPacket`) use a simple pipe-finder helper to split the raw BLE string into fields.

### `time_manager.cpp` (135 lines) — The Clock

**RTC (Real-Time Clock):** The ESP32's built-in RTC keeps time even between reboots (as long as battery doesn't die). BLE time sync from the phone updates it via `settimeofday()`.

**Alarm engine:** 3 configurable alarms, each with:
- Hour/minute
- Day-of-week bitmask (bit 0=Sun through bit 6=Sat)
- Optional date filter (specific day+month)
- Enabled/disabled, fired (prevents re-trigger within the same minute)

**Alarm sound:** 3-beep ascending pattern (1100Hz → 1400Hz → 1900Hz), repeating every 2 seconds, auto-dismiss after 60 seconds. Any button press dismisses immediately.

**EEPROM persistence:** Alarms survive reboot. 7 bytes per alarm (hour, minute, enabled, daysOfWeek, useDate, dateDay, dateMonth). Invalid values (from fresh/corrupted EEPROM) are clamped to safe defaults.

### `step_counter.cpp` (66 lines) — The Pedometer

Simple threshold-based step detection using the MPU6886 accelerometer:

1. Read acceleration vector (ax, ay, az)
2. Compute magnitude: `sqrt(ax² + ay² + az²)` — should be ~1.0g at rest
3. Low-pass filter extracts baseline (gravity + posture drift)
4. High-pass residual = magnitude - baseline (removes gravity, keeps walking impulse)
5. Adaptive threshold = 45% of peak-to-peak range over a 2-second window, minimum 0.08g
6. Step counted when residual > threshold AND 200ms debounce (max 5 steps/sec)

Not as accurate as a phone's step counter (no ML, no gyroscope fusion), but good enough for a wrist-worn device.

---

## How Data Flows

### Phone notification arrives:
```
Phone → BLE write "N|WhatsApp|John|Hey"
  → ChrCB::onWrite() [core 0]
    → nq_onNotifPacket() parses fields
      → nq_push("WhatsApp", "John", "Hey")
        → g_nqNewNotif = true  [volatile flag]

loop() [core 1] sees g_nqNewNotif == true
  → clears flag
  → dispMgr_wakeScreen()
  → plays notification sound
  → dispMgr_markDirty()
    → next dispMgr_update() redraws with new notification
```

### User presses BtnA to dismiss notification:
```
M5.update() polls hardware
  → M5.BtnA.wasPressed() returns true (one-shot latch)

handleButtons() captures: bool aPrs = true
  → screen is on, no alarm ringing, no banner
  → BtnA short press dispatched to handleBtnA_short()
    → SCR_NOTIFS case: nq_dismiss(g_notifIdx)
      → shifts array, decrements count
    → dispMgr_markDirty()
```

### Power mode change NORMAL → DEEPSLEEP:
```
Settings screen, user selects DEEPSLEEP
  → incrementSetting(5, 1)  // idx=5 is power mode
    → pwrMgr_apply(PWR_DEEPSLEEP)
      → crossingBoundary = true (NORMAL→DEEPSLEEP)
      → EEPROM.write(EE_PWRMODE, 2)
      → EEPROM.commit()
      → dispMgr_animDeepSleep()  // 24-step wipe animation
      → ESP.restart()

On reboot:
  setup() → pwrMgr_init()
    → reads EEPROM: mode = PWR_DEEPSLEEP
    → setCpuFrequencyMhz(40)
    → skips bleMgr_init()  // no BLE at 40MHz
```

---

## The Bugs We Fixed (and Why They Existed)

### 1. Buttons appeared dead after screen wake
**Root cause:** `wasPressed()` is a one-shot latch. Old code read it in the wake check, then tried to read it again for the action — second read always returned false.
**Fix:** Capture into local bools once. Every check uses the local.

### 2. DEEPSLEEP reboot loop
**Root cause:** Old code called `ESP.restart()` before writing the new mode to EEPROM. On reboot, it read the OLD mode (DEEPSLEEP), applied it, then the UI tried to cycle to NORMAL, which triggered another restart...
**Fix:** Write EEPROM BEFORE restart. Also detect same-mode transitions as no-ops.

### 3. GPIO4 power rail death
**Root cause:** The AXP2101 PMIC on the M5StickC PLUS2 uses GPIO4 to gate the power rail. If GPIO4 floats during boot (before `pinMode` runs), the PMIC can cut power.
**Fix:** `gpio_hold_en(GPIO_NUM_4)` as the VERY FIRST line in `setup()`, before any I2C activity.

### 4. I2C crash at low CPU frequency
**Root cause:** The MPU6886 IMU and RTC communicate over I2C, which has timing requirements. Below 40MHz, I2C transactions fail randomly.
**Fix:** DEEPSLEEP mode uses 40MHz (not lower). The `hardware_config.h` comment documents why.

### 5. Light sleep kills the PMIC
**Root cause:** `esp_light_sleep_start()` causes the ESP32 to momentarily release GPIO pins. The AXP2101 sees GPIO4 go LOW and starts a shutdown sequence. After a few sleep/wake cycles, it cuts power permanently until USB is reconnected.
**Fix:** Removed `esp_light_sleep_start()` entirely. Screen-off just sends SLPIN to the LCD and uses `vTaskDelay(50)` in the main loop (saves ~20mA without touching GPIOs).

---

## Memory Layout

```
EEPROM (32 bytes):
Byte 0:   Brightness level (0-5)
Byte 1:   Auto-rotate (0/1)
Byte 2:   Power mode (0=NORMAL, 1=EFFICIENT, 2=DEEPSLEEP)
Byte 3:   Sound profile (0=SILENT, 1=GENERAL)
Byte 4:   AOD enabled (0/1)
Bytes 5-7: unused
Bytes 8-14:  Alarm 0 (hour, min, enabled, daysOfWeek, useDate, day, month)
Bytes 15-21: Alarm 1
Bytes 22-28: Alarm 2
Bytes 29-31: unused
```

```
RAM usage: 41KB / 320KB (12.6%)
Flash usage: 797KB / 3.3MB (23.9%)
Total source: ~2326 lines across 15 files
```

---

## Build Commands

```bash
pio run                      # compile
pio run --target upload      # compile + flash to device
pio device monitor           # serial output (115200 baud)
pio run --target clean       # wipe build artifacts
```

---

## If You Want to Add a Feature

1. Add any new constants/enums/structs to `hardware_config.h`
2. If it needs a new screen, add to the `Screen` enum and increment `SCR_COUNT`
3. Create `your_module.h` + `your_module.cpp` — expose an `init()` and `update()` function
4. Call `init()` from `setup()` in main.cpp, `update()` from `loop()`
5. Add the draw function to `display_mgr.cpp` and wire it into the screen switch
6. Add button behavior to `handleBtnA_short()` / `handleBtnPWR_short()` in main.cpp

Never hardcode pin numbers outside `hardware_config.h`. Never draw to the LCD outside `display_mgr.cpp`. Never call `M5.Display` from a BLE callback.

---

*Designed by Atom, IIT Roorkee. "Never leave beauty for function and function for beauty."*
