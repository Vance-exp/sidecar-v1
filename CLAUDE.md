# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

---

## Build & Upload Commands

```bash
pio run                                          # build
pio run --target upload                          # build + upload
pio device monitor                               # serial monitor (115200 baud)
pio test --filter test_<name>                    # run single test
pio run --target clean                           # clean build artifacts
```

## platformio.ini (canonical)

```ini
[env:m5stick-c-plus2]
platform = espressif32
board = m5stick-c-plus2
framework = arduino
monitor_speed = 115200
lib_deps =
    m5stack/M5StickCPlus2 @ ^1.0.0
    h2zero/NimBLE-Arduino @ ^1.4.0
```

---

## Project: Sidecar V1

Ultra-slim cyberdeck bracer watch. Flat industrial form factor, all components in one plane. Nixie-tube phosphor orange UI. No USB port — pogo pin magnetic charging only.

**Hardware:** M5StickC PLUS2 (ESP32-PICO-V3-02 + AXP2101 + MPU6886 + 1.14" ST7789V2 TFT), 2× WLY503040 600mAh LiPo in parallel (target 1200mAh — currently only 1 cell wired = 200mAh for prototyping), SPI MicroSD for CSV logging.

### Firmware module map

```
src/
├── main.cpp               ← setup / loop / button handling
├── hardware_config.h      ← ALL pin defs + constants (single source of truth)
├── power_manager.h/.cpp   ← 4-state machine
├── nixie_display.h/.cpp   ← phosphor decay animation, 5 screens
├── ble_manager.h/.cpp     ← NimBLE GATT + Android Bellafaire + iOS ANCS
├── notif_queue.h/.cpp     ← notification storage + parsing
├── time_manager.h/.cpp    ← RTC + BLE time sync
├── step_counter.h/.cpp    ← MPU6886 FIFO adaptive drain
└── sd_logger.h/.cpp       ← MicroSD CSV logging
tools/
└── analyse_log.py         ← matplotlib battery/step analysis
```

### Power states

| Mode | Screen-on | Screen-off | CPU (on/off) | BLE interval | Avg (~5% on-time) |
|---|---|---|---|---|---|
| Normal | ~54mA | ~7mA | 160MHz / 80MHz+LS | 100–200ms / 2–4s lat=4 | ~9.5mA |
| Efficient | ~35mA | ~3mA | 80MHz+LS / 80MHz+LS | 500ms / 2–4s lat=4 | ~4mA |
| Extreme | ~10mA | ~2mA | 80MHz+LS / 80MHz+LS | off / off | ~2mA |

**Projected battery life (200mAh prototype / 1200mAh final):**

| Mode | 200mAh | 1200mAh |
|---|---|---|
| Normal | ~21h | ~5.3 days |
| Efficient | ~50h | ~12.5 days |
| Extreme | ~100h | ~25 days |

**Screen-off behaviour:**
- NORMAL: drops to 80MHz + auto light sleep on screen-off; restores to 160MHz on wake
- BLE connection interval relaxes to 2–4s with latency=4 on screen-off (max ~4s notif lag)
- BLE modem sleep always active (radio sleeps between connection events)
- Loop rate drops from 100ms → configured per-mode delay when screen is off

**Light sleep (NOT deep sleep)** — keeps BLE stack alive in all modes except Extreme.

### Key firmware rules

- `hardware_config.h` is the ONLY place pin numbers live — never hardcode elsewhere
- Use `NimBLE-Arduino`, NOT `ArduinoBLE` — 50% less RAM, better sleep compatibility
- GPIO4 must stay HIGH during sleep to maintain power rail (PLUS2-specific)
- BLE packet formats (Bellafaire protocol):
  - Time sync: `"T|unix_timestamp"`
  - Notification: `"N|APP|TITLE|BODY"`
  - Spotify: `"S|artist|song"`
  - Call: `"C|name"`

### Library sources

| Library | Licence | What to take |
|---|---|---|
| Bellafaire/ESP32-Smart-Watch | MIT | BLE GATT UUIDs, notif queue, time sync. Keep 3-line copyright comment. Strip ADXL337 step code. |
| m5stack/M5Unified | MIT | AXP2101 PMIC, MPU6886 IMU FIFO, M5GFX display |
| Xinyuan-LilyGo/TTGO_TWatch_Library | MIT | `esp_pm` config, GPIO hold, wake cause detection. Strip AXP202 code. |
| sqfmi/Watchy | MIT | Face registration pattern, activity storage pattern. Strip all display code. |
| Gadgetbridge | **AGPL** | Read protocol DOCS ONLY — copy NO code into this project |

### Build sequence (current phase: step 1–2)

1. **[IN PROGRESS]** Install PlatformIO + VS Code
2. Flash test firmware — `SIDECAR V1 / ALIVE` on screen + IMU serial output
3. Implement `hardware_config.h` + module skeleton
4. Implement `nixie_display` — phosphor decay animation
5. Implement `power_manager` — 4-state machine
6. Implement `ble_manager` — NimBLE, Bellafaire protocol, ANCS
7. Implement `notif_queue` + `time_manager`
8. Implement `step_counter` (MPU6886 FIFO)
9. Implement `sd_logger` + Python analysis
10. Breadboard peripherals (MOSFET switches, PCF8563 RTC, pogo charging)
11. Validate power budget (multimeter in series per mode)
12. Design Sidecar V2 KiCad PCB

---

## Project: Ghost Dial (next project, not started)

Mechanical skeleton watch (Seagull ST36) with a 1.51" transparent OLED (SSD1309) floating above the movement. OLED is dark by default — the digital layer serves the movement, never competes.

**Dual-chip:** nRF52840 always-on (3μA sleep) handles display + BLE + sensors. ESP32-C6 wakes only for WiFi bursts (NTP/weather/OTA), ~30s/day total.

### The 5 design laws — never break these in firmware

1. Movement always wins — battery death = watch still tells time
2. Never speaks first — zero unsolicited OLED displays
3. Every digital feature references the movement
4. Max 40% OLED brightness, amber colour only (never white or blue)
5. Physical crown only — 3 positions via Hall effect sensor

### OLED states

| State | Trigger | Duration |
|---|---|---|
| OFF | default | permanent |
| TILT_WAKE | wrist tilt | 4s, 15% brightness |
| NOTIFICATION | BLE notif | 6s, app icon only, 25% brightness |
| TIMEGRAPHER | crown push | until crown push again |
| COMPLICATIONS | crown half-pull | 8s then fade |
| ILLUMINATE | crown hold 2s | 10s then dim |

### Frame PCB: 40×40mm, 6-layer, 37mm circular bore

Corner placement: NW=nRF52840, NE=ESP32-C6, SW=nPM1100 PMIC+Qi, SE=sensor cluster.

### CAD files (already written, not in this repo)

- `GhostDial_Case.FCMacro` — FreeCAD Python, 718 lines, parametric. Update `MOVE_DIA` after measuring ST36 with callipers.
- `ghost_dial_case.scad` — OpenSCAD alternative, 652 lines, 7 render modes.

---

## Author

Atom — Master's student, IIT Roorkee. Philosophy: *"Never leave beauty for function and function for beauty."*
