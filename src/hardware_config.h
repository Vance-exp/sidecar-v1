/*
 * SIDECAR V1 — Hardware Configuration
 * Single source of truth for pins, constants, palette, shared types.
 * BLE UUIDs from Bellafaire/ESP32-Smart-Watch
 * Copyright (c) 2020-2021 Matthew James Bellafaire — MIT License
 */
#pragma once
#include <Arduino.h>

// ── Pins ──────────────────────────────────────────────────────────────────────
#define PIN_PWR_RAIL    4     // GPIO4 — must stay HIGH or AXP2101 kills power
#define PIN_IR_TX       9     // Onboard IR LED — hold LOW to save power

// ── Display ───────────────────────────────────────────────────────────────────
#define DISP_W          240
#define DISP_H          135
#define DISP_ROTATION   1     // landscape, USB left

// ── BLE (Bellafaire UUIDs — MIT © 2020-2021 Matthew James Bellafaire) ─────────
#define BLE_DEVICE_NAME "SIDECAR V1"
#define BLE_SVC_UUID    "5ac9bc5e-f8ba-48d4-8908-98b80b566e49"
#define BLE_CHR_UUID    "bcca872f-1a3e-4491-b8ec-bfc93c5dd91a"
#define BLE_ADV_FAST        160   // 0.625ms units → 100ms  (NORMAL — fast connection)
#define BLE_ADV_SLOW        800   // 0.625ms units → 500ms  (EFFICIENT screen-on)
#define BLE_ADV_ULTRA_SLOW 3276   // 0.625ms units → 2048ms (EFFICIENT screen-off, ~1s notif delay)

// ── Timing ────────────────────────────────────────────────────────────────────
#define LONG_PRESS_MS   700
#define BANNER_MS       3000
#define FAST_STEP_MS    120

// ── Battery ───────────────────────────────────────────────────────────────────
#define BATT_MAH        200   // single cell prototype (will be 1200 in final)
#define BATT_EMA_ALPHA  35    // EMA smoothing (0-100)

// ── Brightness LUT (level 0-5 → raw LCD brightness) ──────────────────────────
// Level 0 = true off (0). Perceptually even — human eye is logarithmic.
inline int brtValue(int lvl) {
    static const uint8_t lut[6] = {0, 12, 35, 70, 120, 200};
    return lut[constrain(lvl, 0, 5)];
}

// ── Palette (RGB565) ──────────────────────────────────────────────────────────
#define NIXIE_ORANGE    0xFB60    // phosphor orange
#define NIXIE_DIM       0x8200    // 50% orange — secondary info
#define NIXIE_GHOST     0x3900    // 25% orange — dots, inactive items
#define COL_BG          TFT_BLACK
#define COL_GREEN       0x07E0
#define COL_RED         0xF800
#define COL_BLUE        0x07FF  // bright cyan — clearly visible on black (was 0x1C9F which was near-invisible)

// ── EEPROM layout (32 bytes) ──────────────────────────────────────────────────
#define EE_BRIGHT       0     // uint8: brightness level 0-5
#define EE_AUTOROT      1     // uint8: auto-rotate 0/1
#define EE_PWRMODE      2     // uint8: PowerMode enum
#define EE_SOUND        3     // uint8: SoundProfile enum
#define EE_AOD          4     // uint8: AOD enabled 0/1
#define EE_NOTIF_TIMEOUT 5    // uint8: notif overlay timeout index (0-4)
#define EE_WRIST_WAKE   6     // uint8: wrist wake enabled 0/1
#define EE_DND          7     // uint8: do-not-disturb 0/1
#define EE_ALARM_BASE   8     // 3 alarms × 7 bytes = 21 bytes (8-28)
#define EE_ALARM_BYTES  7
#define EE_AOD_FACE     29    // uint8: AOD face index (0=clock 1=clock+steps 2=clock+alarm)
#define EE_SIZE         32

// ── SD card SPI pins (external SPI MicroSD via hat connector) ─────────────────
// Change these to match your wiring before enabling sd_logger.
#define SD_CS           26    // chip select — hat G26
#define SD_MOSI          0    // MOSI        — hat G0
#define SD_MISO         36    // MISO        — hat G36
#define SD_SCK          25    // SCK         — hat G25

// ── Power modes ───────────────────────────────────────────────────────────────
// NORMAL    — 160MHz, BLE on, full features
// EFFICIENT — 80MHz,  BLE on, dimmer, slower BLE adv
// DEEPSLEEP — 40MHz,  BLE off, local features only (alarm/timer/steps)
//             NOTE: 40MHz not lower — I2C (RTC/IMU) requires ≥40MHz
enum PowerMode : uint8_t { PWR_NORMAL = 0, PWR_EFFICIENT = 1, PWR_DEEPSLEEP = 2 };
#define PWR_COUNT       3

struct PwrCfg {
    uint8_t brightness;     // 0-5
    uint8_t timeoutSec;     // screen-off delay; 0 = never
};

static const char* PWR_NAMES[PWR_COUNT] = {"NORMAL", "EFFICIENT", "DEEPSLEEP"};

// ── Screens ───────────────────────────────────────────────────────────────────
enum Screen : uint8_t {
    SCR_CLOCK = 0, SCR_STOPWATCH, SCR_TIMER, SCR_ALARM,
    SCR_NOTIFS, SCR_MEDIA, SCR_WEATHER, SCR_POWER, SCR_DIAG, SCR_SETTINGS, SCR_COUNT
};

// ── Sound ─────────────────────────────────────────────────────────────────────
enum SoundProfile : uint8_t { SND_SILENT = 0, SND_GENERAL = 1 };

// ── Alarms ────────────────────────────────────────────────────────────────────
#define ALARM_MAX        3
#define ALARM_EDIT_FIELDS 12    // H, M, Sun-Sat(7), useDate, day, month

struct AlarmCfg {
    uint8_t hour, minute;
    bool    enabled, fired;
    uint8_t daysOfWeek;         // bitmask bit0=Sun .. bit6=Sat; 0x7F = every day
    bool    useDate;
    uint8_t dateDay, dateMonth;
};

// ── Notifications ─────────────────────────────────────────────────────────────
#define NOTIF_MAX        10

enum NotifType : uint8_t { NOTIF_APP = 0, NOTIF_CALL = 1, NOTIF_MSG = 2 };

struct Notif {
    char app[20];
    char title[40];
    char body[60];
    bool unread;
    NotifType type;
};

// ── Weather ───────────────────────────────────────────────────────────────────
struct WeatherData {
    int8_t  tempC;          // current temperature (°C)
    int8_t  hiC, loC;       // day high / low (°C)
    char    condition[8];   // "SUNNY" "CLOUD" "RAIN" "SNOW" "STORM" "FOG" "---"
    unsigned long updatedAt; // millis() when last received; 0 = never
};

// ── Stopwatch ─────────────────────────────────────────────────────────────────
#define SW_LAPS_MAX      8

// ── Timer ─────────────────────────────────────────────────────────────────────
enum TimerState : uint8_t { TMR_IDLE = 0, TMR_RUNNING, TMR_PAUSED, TMR_DONE };
enum TimerMode  : uint8_t { TMR_MODE_NORMAL = 0, TMR_MODE_POMODORO = 1, TMR_MODE_BREATHE = 2 };

// ── Settings ──────────────────────────────────────────────────────────────────
// 0=Hour 1=Min 2=Date | 3=Month 4=Year 5=Power | 6=Bright 7=Rotate 8=AOD 9=Sound 10=Notif 11=Wrist 12=DND 13=AOD_FACE
#define SETTING_COUNT    14

// ── Notification overlay timeout ──────────────────────────────────────────────
#define NOTIF_TIMEOUT_COUNT 5
inline unsigned long notifTimeoutMs(int idx) {
    static const unsigned long t[NOTIF_TIMEOUT_COUNT] = {5000, 10000, 20000, 30000, 0};
    return t[constrain(idx, 0, NOTIF_TIMEOUT_COUNT - 1)];
}
inline const char* notifTimeoutLabel(int idx) {
    static const char* lbl[NOTIF_TIMEOUT_COUNT] = {"5s", "10s", "20s", "30s", "OFF"};
    return lbl[constrain(idx, 0, NOTIF_TIMEOUT_COUNT - 1)];
}

// ── Helpers ───────────────────────────────────────────────────────────────────
inline int daysInMonth(int m, int y) {
    if (m < 1 || m > 12) return 30;
    if (m == 2) return ((y%4==0) && (y%100!=0 || y%400==0)) ? 29 : 28;
    static const int d[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    return d[m];
}

// Tomohiko Sakamoto weekday: 0=Sun..6=Sat
inline int calcDow(int y, int m, int d) {
    static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (m < 3) y--;
    return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}

inline void fmtMs(unsigned long ms, char* buf, int len) {
    snprintf(buf, len, "%02d:%02d.%02d",
        (int)((ms/60000)%60), (int)((ms/1000)%60), (int)((ms/10)%100));
}

static const char* MON_NAMES[] = {
    "","JAN","FEB","MAR","APR","MAY","JUN",
    "JUL","AUG","SEP","OCT","NOV","DEC"
};
static const char* DOW_NAMES[] = {
    "SUN","MON","TUE","WED","THU","FRI","SAT"
};
