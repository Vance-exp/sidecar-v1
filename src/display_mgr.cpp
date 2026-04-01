/*
 * SIDECAR V1 — Display Manager
 * All LCD drawing, screen sleep/wake, AOD, auto-rotate, battery diagnostics.
 * 8 screens: Clock, Stopwatch, Timer, Alarm, Notifications, Power, Diag, Settings
 *
 * This module OWNS the LGFX_Sprite canvas. No other module touches the display.
 */
#include <Arduino.h>
#include <M5Unified.h>
#include <EEPROM.h>
#include "esp_pm.h"
#include "display_mgr.h"
#include "hardware_config.h"
#include "power_manager.h"
#include "ble_manager.h"
#include "notif_queue.h"
#include "time_manager.h"
#include "step_counter.h"

// ── Canvas ────────────────────────────────────────────────────────────────────
static LGFX_Sprite canvas(&M5.Display);

// ── Screen state ──────────────────────────────────────────────────────────────
Screen        g_curScreen      = SCR_CLOCK;
bool          g_screenOn       = true;
bool          g_displaySleeping = false;
bool          g_displayDirty   = true;
unsigned long g_lastActivity   = 0;

// ── Display settings ──────────────────────────────────────────────────────────
bool          g_autoRotate     = true;
int           g_curRotation    = DISP_ROTATION;
int           g_brightLevel    = 3;
bool          g_aodEnabled        = false;
bool          g_aodActive         = false;
bool          g_wristWakeEnabled  = true;
bool          g_dndEnabled        = false;
int           g_aodFace           = 0;   // 0=clock 1=clock+steps 2=clock+alarm
TimerMode     g_timerMode         = TMR_MODE_NORMAL;

// ── Settings UI ───────────────────────────────────────────────────────────────
int           g_settingIdx     = 0;

// ── Power screen UI ───────────────────────────────────────────────────────────
int           g_pwrTabSel      = 0;
bool          g_pwrInCustom    = false;
int           g_pwrCustIdx     = 0;

// ── Stopwatch ─────────────────────────────────────────────────────────────────
bool          sw_running       = false;
unsigned long sw_start         = 0;
unsigned long sw_elapsed       = 0;
unsigned long sw_laps[SW_LAPS_MAX] = {};
int           sw_lapCount      = 0;
unsigned long sw_lapStart      = 0;

// ── Timer ─────────────────────────────────────────────────────────────────────
TimerState    tmr_state        = TMR_IDLE;
int           tmr_setMin       = 5;
unsigned long tmr_endMs        = 0;
unsigned long tmr_remaining    = 0;

// ── Battery smoothing ─────────────────────────────────────────────────────────
static int           s_battSmoothed  = -1;
static unsigned long s_lastBattRead  = 0;

// ── Battery diagnostic log ────────────────────────────────────────────────────
#define BATT_LOG_SIZE 60
static int8_t        s_battLog[BATT_LOG_SIZE];
static uint32_t      s_battLogSec[BATT_LOG_SIZE];  // uint32: survives >18h (uint16 wraps)
static int           s_battLogHead  = 0;
static int           s_battLogCount = 0;
static unsigned long s_lastBattLog  = 0;

// ── Auto-rotate ───────────────────────────────────────────────────────────────
static unsigned long s_lastRotCheck = 0;

// ── Forward declarations (static draw functions) ──────────────────────────────
static void drawHeader(const char* title);
static void drawHints(const char* top, const char* front, const char* flong = nullptr);
static void drawBanner();
static void drawNotifOverlay();
static void drawBattBar(int x, int y, int pct);
static void drawClock();
static void drawStopwatch();
static void drawTimer();
static void drawAlarm();
static void drawNotifs();
static void drawMedia();
static void drawWeather();
static void drawPower();
static void drawDiag();
static void drawSettings();
static void drawAOD();

// ── Battery helpers ───────────────────────────────────────────────────────────
float dispMgr_getEstimatedMa(int modeIdx) {
    // Average draw estimates — screen-OFF state (dominates battery life).
    // NORMAL: 160MHz screen-on ~54mA, but screen-off 80MHz+light sleep drops to ~5mA.
    //   At ~5% screen-on duty (3s/min glance): avg ≈ (0.05×54)+(0.95×5) ≈ 7.5mA.
    //   We use 7.0 as the base (screen-off is the common case the model tracks).
    // EFFICIENT: 80MHz + light sleep always → ~3mA base.
    // DEEPSLEEP: 40MHz, no BLE, no display → ~2mA base.
    // AXP2101 on PLUS2 has no current ADC — model only.
    const float BASE_MA[PWR_COUNT] = {7.0f, 3.0f, 2.0f};
    int idx = constrain(modeIdx, 0, PWR_COUNT - 1);
    float mA = BASE_MA[idx];
    if (g_bleConn) mA += 2.0f;   // relaxed BLE intervals: ~2mA avg when connected
    return mA;
}

static float battLifeHours(int modeIdx) {
    float remaining = (float)BATT_MAH * (float)dispMgr_getBatt() / 100.0f;
    return remaining / dispMgr_getEstimatedMa(modeIdx);
}

static void fmtBattLife(float h, char* buf, int len) {
    if (h >= 24.0f)     snprintf(buf, len, "~%dd%dh", (int)(h/24.0f), (int)h % 24);
    else if (h >= 1.0f) snprintf(buf, len, "~%.1fh", h);
    else                snprintf(buf, len, "~%dm", (int)(h * 60.0f));
}

static float logMa(int idx_new, int idx_old) {
    int delta = (int)s_battLog[idx_old] - (int)s_battLog[idx_new];
    if (delta <= 0) return 0.0f;
    // uint32 subtraction is always correct even across a wrap
    uint32_t dt = s_battLogSec[idx_new] - s_battLogSec[idx_old];
    if (dt == 0) dt = 300;
    return (float)delta * (float)BATT_MAH * 3600.0f / (100.0f * (float)dt);
}

// ══════════════════════════════════════════════════════════════════════════════
// PUBLIC FUNCTIONS
// ══════════════════════════════════════════════════════════════════════════════

int dispMgr_getBatt() {
    unsigned long now = millis();
    if (s_battSmoothed < 0 || now - s_lastBattRead > 15000UL) {
        int raw = constrain(M5.Power.getBatteryLevel(), 0, 100);
        if (s_battSmoothed < 0) s_battSmoothed = raw;
        else s_battSmoothed += (BATT_EMA_ALPHA * (raw - s_battSmoothed)) / 100;
        s_lastBattRead = now;
    }
    return s_battSmoothed;
}

void dispMgr_init() {
    M5.Display.setRotation(g_curRotation);
    M5.Display.setBrightness(brtValue(g_brightLevel));
    canvas.createSprite(DISP_W, DISP_H);
    s_lastBattLog = millis() - 61000UL;  // trigger first sample immediately
}

void dispMgr_wakeScreen() {
    g_screenOn   = true;
    g_aodActive  = false;
    if (g_displaySleeping) {
        M5.Display.wakeup();
        g_displaySleeping = false;
    }
    M5.Display.setBrightness(brtValue(g_brightLevel));
    g_lastActivity = millis();
    g_displayDirty = true;
    // Restore fast connection params so notifications arrive quickly while
    // the user is actively looking at the watch.
    bleMgr_setConnInterval(100, 200, 0, 6000);

    // Restore CPU speed if we dropped it on screen-off.
    if (g_powerMode == PWR_NORMAL) {
        setCpuFrequencyMhz(160);
        esp_pm_config_esp32_t pm_cfg = {
            .max_freq_mhz       = 160,
            .min_freq_mhz       = 160,
            .light_sleep_enable = false
        };
        esp_pm_configure(&pm_cfg);
        Serial.println("[DISP] screen on → 160MHz, no light sleep");
    }
}

void dispMgr_markDirty() {
    g_displayDirty = true;
}

void dispMgr_setBrightness(int lvl) {
    g_brightLevel = constrain(lvl, 0, 5);
    M5.Display.setBrightness(brtValue(g_brightLevel));
    g_pwrCfg[g_powerMode].brightness = g_brightLevel;
    EEPROM.write(EE_BRIGHT, g_brightLevel);
    EEPROM.commit();
}

// ── Shared geometry for all three animations ──────────────────────────────────
// Pure circles + 4-point cross pattern — rotationally symmetric at 90°,
// meaning it looks identical after a 180° flip. No text, no arrows, no directionality.
static const int  ANIM_CX  = DISP_W / 2;   // 120
static const int  ANIM_CY  = DISP_H / 2;   // 67
static const int  ANIM_R[] = {18, 32, 46};  // inner / mid / outer ring radii
static const float ANIM_PI = 3.14159f;

// Draw 4 cardinal dots at radius r, scaled by frac (0..1)
static void animDots4(int r, float frac, uint32_t col) {
    int dr = (int)(4 * frac);
    if (dr < 1) return;
    for (int d = 0; d < 4; d++) {
        float a = d * ANIM_PI / 2.0f;
        int x = ANIM_CX + (int)(r * cosf(a));
        int y = ANIM_CY + (int)(r * sinf(a));
        canvas.fillCircle(x, y, dr, col);
    }
}

// ── Boot animation ────────────────────────────────────────────────────────────
// 3 concentric rings expand from centre one by one, then 4-point dots bloom.
// No text. Perfectly symmetric — looks identical right-side-up or upside-down.
void dispMgr_showSplash(const char* /*line1*/, const char* /*line2*/, int ms) {
    M5.Display.setRotation(DISP_ROTATION);

    const int STEPS   = 72;
    const int STEP_MS = max(1, ms / STEPS);

    for (int s = 0; s <= STEPS; s++) {
        canvas.fillSprite(COL_BG);
        float t = (float)s / STEPS;

        // ── Centre dot pulses in first ─────────────────────────────────
        {
            float f = min(t / 0.12f, 1.0f);
            int r = (int)(6 * f);
            if (r > 0) canvas.fillCircle(ANIM_CX, ANIM_CY, r, (uint32_t)NIXIE_ORANGE);
        }

        // ── 3 rings expand sequentially (staggered by 0.18t each) ──────
        const uint32_t ringCol[3] = {NIXIE_ORANGE, NIXIE_DIM, NIXIE_GHOST};
        for (int ri = 0; ri < 3; ri++) {
            float start = 0.10f + ri * 0.18f;
            float f     = constrain((t - start) / 0.25f, 0.0f, 1.0f);
            int r = (int)(ANIM_R[ri] * f);
            if (r > 1) canvas.drawCircle(ANIM_CX, ANIM_CY, r, ringCol[ri]);
        }

        // ── 4-point dots bloom on each ring once it's full ─────────────
        // Inner ring dots (NIXIE_ORANGE) at t=0.42
        animDots4(ANIM_R[0], constrain((t - 0.42f) / 0.12f, 0.0f, 1.0f), (uint32_t)NIXIE_ORANGE);
        // Mid ring dots (NIXIE_DIM) at t=0.58
        animDots4(ANIM_R[1], constrain((t - 0.58f) / 0.12f, 0.0f, 1.0f), (uint32_t)NIXIE_DIM);
        // Outer ring dots (NIXIE_GHOST) at t=0.74
        animDots4(ANIM_R[2], constrain((t - 0.74f) / 0.12f, 0.0f, 1.0f), (uint32_t)NIXIE_GHOST);

        canvas.pushSprite(0, 0);
        vTaskDelay(pdMS_TO_TICKS(STEP_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(250));
}

void dispMgr_animDeepSleep() {
    if (g_displaySleeping) { M5.Display.wakeup(); g_displaySleeping = false; }
    M5.Display.setRotation(DISP_ROTATION);
    M5.Display.setBrightness(brtValue(3));
    // Reverse boot: rings collapse inward, dots shrink, centre dot fades last
    const int STEPS = 54;
    for (int s = 0; s <= STEPS; s++) {
        canvas.fillSprite(COL_BG);
        float t = 1.0f - (float)s / STEPS;  // t goes 1→0
        // centre dot
        int r = (int)(6 * min(t / 0.12f, 1.0f));
        if (r > 0) canvas.fillCircle(ANIM_CX, ANIM_CY, r, (uint32_t)NIXIE_ORANGE);
        // 3 rings collapse (reverse of boot sequence)
        const uint32_t ringCol[3] = {NIXIE_ORANGE, NIXIE_DIM, NIXIE_GHOST};
        for (int ri = 0; ri < 3; ri++) {
            float f = constrain((t - 0.10f - ri * 0.18f) / 0.25f, 0.0f, 1.0f);
            int rr = (int)(ANIM_R[ri] * f);
            if (rr > 1) canvas.drawCircle(ANIM_CX, ANIM_CY, rr, ringCol[ri]);
        }
        // 4-point dots shrink back
        animDots4(ANIM_R[0], constrain((t - 0.42f) / 0.12f, 0, 1), NIXIE_ORANGE);
        animDots4(ANIM_R[1], constrain((t - 0.58f) / 0.12f, 0, 1), NIXIE_DIM);
        animDots4(ANIM_R[2], constrain((t - 0.74f) / 0.12f, 0, 1), NIXIE_GHOST);
        canvas.pushSprite(0, 0);
        vTaskDelay(pdMS_TO_TICKS(14));
    }
    // Dim fill to black — device is going to sleep
    for (int s = 0; s <= 16; s++) {
        canvas.fillSprite(COL_BG);
        int r = (int)(ANIM_R[2] * (1.0f - (float)s / 16));
        if (r > 1) canvas.drawCircle(ANIM_CX, ANIM_CY, r, (uint32_t)NIXIE_GHOST);
        canvas.pushSprite(0, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(300));
}

void dispMgr_animWakeUp() {
    if (g_displaySleeping) { M5.Display.wakeup(); g_displaySleeping = false; }
    M5.Display.setRotation(DISP_ROTATION);
    M5.Display.setBrightness(brtValue(3));
    // Quick boot-style bloom: centre dot → rings expand, dots bloom
    const int STEPS = 36;
    for (int s = 0; s <= STEPS; s++) {
        canvas.fillSprite(COL_BG);
        float t = (float)s / STEPS;
        int r = (int)(6 * min(t / 0.12f, 1.0f));
        if (r > 0) canvas.fillCircle(ANIM_CX, ANIM_CY, r, (uint32_t)NIXIE_ORANGE);
        const uint32_t ringCol[3] = {NIXIE_ORANGE, NIXIE_DIM, NIXIE_GHOST};
        for (int ri = 0; ri < 3; ri++) {
            float f = constrain((t - 0.10f - ri * 0.18f) / 0.25f, 0.0f, 1.0f);
            int rr = (int)(ANIM_R[ri] * f);
            if (rr > 1) canvas.drawCircle(ANIM_CX, ANIM_CY, rr, ringCol[ri]);
        }
        animDots4(ANIM_R[0], constrain((t - 0.42f) / 0.12f, 0, 1), NIXIE_ORANGE);
        animDots4(ANIM_R[1], constrain((t - 0.58f) / 0.12f, 0, 1), NIXIE_DIM);
        animDots4(ANIM_R[2], constrain((t - 0.74f) / 0.12f, 0, 1), NIXIE_GHOST);
        canvas.pushSprite(0, 0);
        vTaskDelay(pdMS_TO_TICKS(14));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
}

void dispMgr_checkWristWake() {
    // No-op if screen already on, wrist wake disabled, or DND active
    if (g_screenOn || !g_wristWakeEnabled || g_dndEnabled) return;

    // Two-rate polling: accel-only at 5Hz as cheap pre-filter.
    // Only read gyro (more expensive, wakes I2C longer) when accel shows
    // significant motion (|accel magnitude - 1g| > 0.2g).
    // This keeps average I2C load very low while still catching wrist raises.
    static unsigned long s_lastWristCheck = 0;
    static unsigned long s_gyroUntil = 0;   // read gyro at 20Hz until this timestamp
    unsigned long now = millis();

    // Accel check at 5Hz (every 200ms) — aligns with 200ms screen-off loop delay
    if (now - s_lastWristCheck < 200UL) return;
    s_lastWristCheck = now;

    float ax, ay, az;
    if (!M5.Imu.getAccel(&ax, &ay, &az)) return;

    // If accel shows motion, enter 20Hz gyro-read window for 400ms
    float mag = sqrtf(ax*ax + ay*ay + az*az);
    if (fabsf(mag - 1.0f) > 0.2f) s_gyroUntil = now + 400UL;

    float gx = 0, gy = 0, gz = 0;
    if (now < s_gyroUntil) {
        if (!M5.Imu.getGyro(&gx, &gy, &gz)) return;
    }

    // Two-stage classifier (same principle as Apple Watch):
    //
    // Stage 1 — MOTION: gyroscope detects a fast wrist rotation.
    //   A raise produces ~100-250°/s around the Y axis of the device.
    //   Walking, arm swing, or resting produce much lower sustained rotation.
    //   This is the discriminating signal — very hard to false-trigger.
    //
    // Stage 2 — ORIENTATION: after the motion, check final position.
    //   Slow accel baseline (α=0.05, ~2s time constant) tracks resting position.
    //   On raise, ay departs from baseline (face tilting toward eyes).
    //   This rejects raises that end pointing the wrong way (e.g. table pickup).
    //
    // Both must be true simultaneously to fire.

    static float s_ayBaseline = 0.0f;
    static float s_azBaseline = 1.0f;
    static bool  s_baselineReady = false;
    static unsigned long s_lastWake = 0;

    // Warm up baseline for 3s after screen sleeps — let it settle
    if (!s_baselineReady) {
        s_ayBaseline = ay;
        s_azBaseline = az;
        static unsigned long s_startedAt = 0;
        if (s_startedAt == 0) s_startedAt = now;
        if (now - s_startedAt > 3000UL) { s_baselineReady = true; s_startedAt = 0; }
        return;
    }

    // Update slow accel baseline
    s_ayBaseline = s_ayBaseline * 0.95f + ay * 0.05f;
    s_azBaseline = s_azBaseline * 0.95f + az * 0.05f;

    float ayDelta = ay - s_ayBaseline;

    // Stage 1: gyro rotation spike (either direction — accounts for left/right wrist)
    // Threshold 120°/s: fast enough to require deliberate raise, slow enough to catch it.
    // Use gy (rotation around device Y axis = wrist rotation when worn on arm).
    bool motionDetected = fabsf(gy) > 120.0f;

    // Stage 2: orientation check — face moving toward eyes
    // ayDelta < -0.4g: less strict than before because gyro already gates it.
    bool orientationOk = (ayDelta < -0.4f);

    if (motionDetected && orientationOk && (now - s_lastWake > 5000UL)) {
        s_lastWake = now;
        s_baselineReady = false;  // re-warm baseline after next sleep
        dispMgr_wakeScreen();
    }
}

void dispMgr_checkRotation() {
    if (!g_autoRotate || !g_screenOn || millis() - s_lastRotCheck < 1000) return;
    s_lastRotCheck = millis();
    float ax, ay, az;
    M5.Imu.getAccel(&ax, &ay, &az);
    int nr = g_curRotation;
    if (ax > 0.3f) nr = 1; else if (ax < -0.3f) nr = 3;
    if (nr != g_curRotation) {
        g_curRotation = nr;
        M5.Display.setRotation(g_curRotation);
        g_displayDirty = true;
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// dispMgr_update() — called from loop(), handles everything display-related
// ══════════════════════════════════════════════════════════════════════════════
void dispMgr_update() {
    unsigned long now = millis();

    // ── Battery log: 1 sample/min (2min when screen off) ─────────────────────
    unsigned long logInterval = g_screenOn ? 60000UL : 120000UL;
    if (now - s_lastBattLog > logInterval) {
        s_lastBattLog = now;
        s_battLog[s_battLogHead]    = (int8_t)dispMgr_getBatt();
        s_battLogSec[s_battLogHead] = (uint32_t)(now / 1000UL);
        s_battLogHead = (s_battLogHead + 1) % BATT_LOG_SIZE;
        if (s_battLogCount < BATT_LOG_SIZE) s_battLogCount++;
    }

    // ── Dirty flag: running timers + media screen ────────────────────────────
    if (sw_running || tmr_state == TMR_RUNNING || tmr_state == TMR_DONE)
        g_displayDirty = true;
    // Media: redraw when new data arrives, or when playing (pulse animation 600ms period)
    // Rate-limit to 2Hz — no need to redraw at loop rate for a 600ms blink.
    if (g_mediaUpdated) { g_mediaUpdated = false; g_displayDirty = true; }
    if (g_curScreen == SCR_MEDIA && g_mediaPlaying) {
        static unsigned long s_lastMediaFrame = 0;
        if (now - s_lastMediaFrame >= 300UL) { s_lastMediaFrame = now; g_displayDirty = true; }
    }
    // Breathing mode: animate at 50ms (20fps) for smooth arc expansion/contraction.
    // Pomodoro: just inherits normal timer dirty flag (running timer already marks dirty).
    if (g_curScreen == SCR_TIMER && g_timerMode == TMR_MODE_BREATHE && tmr_state == TMR_RUNNING) {
        static unsigned long s_lastBreathFrame = 0;
        if (now - s_lastBreathFrame >= 50UL) { s_lastBreathFrame = now; g_displayDirty = true; }
    }

    // ── Clock second tick + charging flash ──────────────────────────────────
    {
        static int lastSec = -1;
        auto dt = M5.Rtc.getDateTime();
        if (dt.time.seconds != lastSec) { lastSec = dt.time.seconds; g_displayDirty = true; }
        // Charging animation flips at 500ms — rate-limit dirty marking to match.
        // Without this the clock redraws every loop iteration while on charge.
        if (M5.Power.isCharging() == m5::Power_Class::is_charging) {
            static unsigned long s_lastChargeFlip = 0;
            if (now - s_lastChargeFlip >= 500UL) { s_lastChargeFlip = now; g_displayDirty = true; }
        }
    }

    // ── Auto-sleep ───────────────────────────────────────────────────────────
    unsigned long sleepMs = (unsigned long)g_pwrCfg[g_powerMode].timeoutSec * 1000UL;
    if (g_screenOn && sleepMs > 0 && (now - g_lastActivity) > sleepMs) {
        g_screenOn = false;
        g_displayDirty = true;
        if (g_aodEnabled && g_powerMode == PWR_NORMAL) {
            g_aodActive = true;
            M5.Display.setBrightness(12);
        } else {
            g_aodActive = false;
            M5.Display.setBrightness(0);
            M5.Display.sleep();
            g_displaySleeping = true;
        }
        // Relax BLE connection interval while screen is off.
        // 2000-4000ms interval + latency=8 → radio wakes once every ~18-36s.
        // Notifications still arrive; max latency ~8×2s = 16s (fine for a watch, not a phone).
        // Supervision timeout 60s covers the 8-event skip with margin.
        bleMgr_setConnInterval(2000, 4000, 8, 60000);

        // NORMAL mode screen-off CPU scaling.
        // Drop to 80MHz and enable auto light sleep — same as EFFICIENT mode.
        // The BLE stack handles the freq change fine at runtime (it's already live).
        // Restore to 240MHz on wake (dispMgr_wakeScreen).
        if (g_powerMode == PWR_NORMAL) {
            setCpuFrequencyMhz(80);
            esp_pm_config_esp32_t pm_cfg = {
                .max_freq_mhz       = 80,
                .min_freq_mhz       = 80,
                .light_sleep_enable = true
            };
            esp_pm_configure(&pm_cfg);
            Serial.println("[DISP] screen off → 80MHz + light sleep");
        }
    }

    // ── Screen-off path ──────────────────────────────────────────────────────
    if (!g_screenOn && !g_alarmRinging) {
        if (g_aodActive) {
            static unsigned long lastAod = 0;
            if (now - lastAod > 1000UL) { lastAod = now; drawAOD(); canvas.pushSprite(0, 0); }
            vTaskDelay(pdMS_TO_TICKS(250));
        } else {
            // Longer sleep window = more time in auto light sleep per loop cycle.
            // Light sleep entry/exit overhead ~2ms, so longer = better efficiency.
            // NORMAL: 200ms  — screen off but BLE notifs still wanted fast
            // EFFICIENT: 500ms — user accepts slightly slower button response
            // DEEPSLEEP: 1000ms — minimal wakeups, alarm check is the only reason to wake
            // Button presses wake immediately via GPIO interrupt regardless of delay.
            uint32_t delayMs = 200;
            if (g_powerMode == PWR_EFFICIENT)  delayMs = 500;
            if (g_powerMode == PWR_DEEPSLEEP)  delayMs = 1000;
            vTaskDelay(pdMS_TO_TICKS(delayMs));
        }
        return;
    }

    // ── Redraw if dirty ──────────────────────────────────────────────────────
    if (g_displayDirty) {
        g_displayDirty = false;
        canvas.fillSprite(COL_BG);

        if (g_notifOverlay) {
            drawNotifOverlay();
        } else {
            switch (g_curScreen) {
                case SCR_CLOCK:     drawClock();     break;
                case SCR_STOPWATCH: drawStopwatch(); break;
                case SCR_TIMER:     drawTimer();     break;
                case SCR_ALARM:     drawAlarm();     break;
                case SCR_NOTIFS:    drawNotifs();    break;
                case SCR_MEDIA:     drawMedia();     break;
                case SCR_WEATHER:   drawWeather();   break;
                case SCR_POWER:     drawPower();     break;
                case SCR_DIAG:      drawDiag();      break;
                case SCR_SETTINGS:  drawSettings();  break;
                default: break;
            }
            drawBanner();
        }
        canvas.pushSprite(0, 0);
    }

    // ── Loop rate cap ─────────────────────────────────────────────────────────
    // Clock/settings/notifs: 100ms — draws happen at 1fps (dirty flag), so this
    // is purely button poll rate. 100ms lag is imperceptible on a wearable.
    // Stopwatch/timer: 33ms — sub-second display needs smooth updates.
    // Both let FreeRTOS idle between wakeups (WFI reduces CPU power even without
    // full light sleep).
    bool needsFastLoop = (sw_running ||
                          tmr_state == TMR_RUNNING ||
                          tmr_state == TMR_DONE);
    vTaskDelay(pdMS_TO_TICKS(needsFastLoop ? 33 : 100));
}

// ══════════════════════════════════════════════════════════════════════════════
// SHARED DRAW HELPERS
// ══════════════════════════════════════════════════════════════════════════════

static void drawBattBar(int x, int y, int pct) {
    int segs = map(constrain(pct, 0, 100), 0, 100, 0, 5);
    uint32_t col = pct > 30 ? COL_GREEN : COL_RED;
    for (int i = 0; i < 5; i++)
        canvas.fillRect(x + i*8, y, 6, 5, i < segs ? col : (uint32_t)NIXIE_GHOST);
}

static void drawHeader(const char* title) {
    // Tab dots — centred, 9px spacing fits 10 screens in ~90px
    int dotSpacing = 9;
    int dotx = DISP_W/2 - ((SCR_COUNT-1) * dotSpacing) / 2;
    for (int i = 0; i < SCR_COUNT; i++) {
        bool active = (i == (int)g_curScreen);
        canvas.fillCircle(dotx + i*dotSpacing, 7, active ? 3 : 2,
            active ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_GHOST);
    }

    // Title (clip before dots)
    int dotLeft = dotx - 8;
    canvas.setTextSize(1);
    canvas.setTextColor(NIXIE_DIM);
    canvas.setCursor(4, 3);
    for (const char* p = title; *p; p++) {
        if (canvas.getCursorX() + 6 > dotLeft) break;
        canvas.print(*p);
    }

    // Right side: HH:MM mini-time (replaces power-mode badge — more useful at a glance)
    // DND indicator dot if active
    int dotRight = dotx + (SCR_COUNT-1) * dotSpacing + 6;
    auto dt = M5.Rtc.getDateTime();
    char tmbuf[6]; snprintf(tmbuf, sizeof(tmbuf), "%02d:%02d", dt.time.hours, dt.time.minutes);
    canvas.setTextSize(1);
    canvas.setTextColor(NIXIE_GHOST);
    canvas.setCursor(dotRight + 2, 3);
    canvas.print(tmbuf);
    if (g_dndEnabled) {
        // Small orange dot to indicate DND is on
        canvas.fillCircle(dotRight + 2 + canvas.textWidth(tmbuf) + 4, 6, 2, (uint32_t)NIXIE_DIM);
    }

    // Unread notification dot (top-right corner)
    if (g_unreadCount > 0) canvas.fillCircle(DISP_W - 3, 4, 3, (uint32_t)COL_RED);

    canvas.drawFastHLine(0, 14, DISP_W, NIXIE_GHOST);
}

static void drawHints(const char* topAct, const char* frontAct, const char* frontLong) {
    canvas.drawFastHLine(0, DISP_H - 20, DISP_W, NIXIE_GHOST);
    canvas.setTextSize(1);
    canvas.setTextColor(NIXIE_GHOST);
    char lbuf[22], rbuf[22];
    snprintf(lbuf, 22, "T:%.16s", topAct);
    snprintf(rbuf, 22, "F:%.16s", frontAct);
    canvas.setCursor(2, DISP_H - 17);   canvas.print(lbuf);
    canvas.setCursor(122, DISP_H - 17);  canvas.print(rbuf);
    if (frontLong) {
        snprintf(lbuf, 22, "F+:%.14s", frontLong);
        canvas.setCursor(2, DISP_H - 8); canvas.print(lbuf);
    }
    canvas.setCursor(122, DISP_H - 8); canvas.print("S:tabs");
}

static void drawBanner() {
    if (!g_bannerActive) return;
    if (millis() - g_bannerAt > BANNER_MS) { g_bannerActive = false; return; }
    // BUG 8: use g_bannerNotifIdx captured at push time, not g_notifCount-1
    int bn = g_bannerNotifIdx;
    if (bn < 0 || bn >= g_notifCount) { g_bannerActive = false; return; }
    canvas.fillRoundRect(2, DISP_H-42, DISP_W-4, 22, 3, (uint32_t)COL_BG);
    canvas.drawRoundRect(2, DISP_H-42, DISP_W-4, 22, 3, (uint32_t)COL_RED);
    canvas.setTextSize(1);
    canvas.setTextColor(COL_RED);
    canvas.setCursor(7, DISP_H-39); canvas.print(g_notifs[bn].app);
    canvas.setTextColor(NIXIE_DIM);
    canvas.setCursor(7, DISP_H-28);
    char t[36]; strncpy(t, g_notifs[bn].title, 35); t[35] = 0;
    canvas.print(t);
}

// ══════════════════════════════════════════════════════════════════════════════
// FULL-SCREEN NOTIFICATION OVERLAY
// Replaces the watch face entirely when a notification arrives.
// Any button press dismisses and returns to the previous screen.
// ══════════════════════════════════════════════════════════════════════════════
static void drawNotifOverlay() {
    int idx = g_notifOverlayIdx;
    if (idx < 0 || idx >= g_notifCount) {
        // Notif was dismissed from queue while overlay was up — close overlay
        g_notifOverlay = false;
        return;
    }
    const Notif& n = g_notifs[idx];

    // ── App name bar — colour by category ────────────────────────────────────
    uint32_t barCol = (n.type == NOTIF_CALL) ? (uint32_t)COL_BLUE
                    : (n.type == NOTIF_MSG)  ? (uint32_t)COL_GREEN
                                             : (uint32_t)COL_RED;
    canvas.fillRoundRect(0, 0, DISP_W, 18, 0, barCol);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_BLACK);
    int tw = canvas.textWidth(n.app);
    canvas.setCursor((DISP_W - tw) / 2, 5);
    canvas.print(n.app);

    // ── Title ─────────────────────────────────────────────────────────────────
    canvas.setTextSize(2);
    canvas.setTextColor(NIXIE_ORANGE);
    // Word-wrap title across up to 2 lines (max 18 chars per line at size 2)
    const int LINE_CHARS = 18;
    char line1[20] = {}, line2[20] = {};
    int tlen = strlen(n.title);
    if (tlen <= LINE_CHARS) {
        strncpy(line1, n.title, LINE_CHARS);
    } else {
        // Try to break at a space
        int brk = LINE_CHARS;
        for (int i = LINE_CHARS; i > 0; i--) {
            if (n.title[i] == ' ') { brk = i; break; }
        }
        strncpy(line1, n.title, brk); line1[brk] = 0;
        strncpy(line2, n.title + brk + (n.title[brk] == ' ' ? 1 : 0), LINE_CHARS);
        line2[LINE_CHARS] = 0;
    }
    canvas.setCursor(4, 23);
    canvas.print(line1);
    if (line2[0]) {
        canvas.setCursor(4, 43);
        canvas.print(line2);
    }

    // ── Divider ───────────────────────────────────────────────────────────────
    int divY = line2[0] ? 66 : 46;
    canvas.drawFastHLine(4, divY, DISP_W - 8, (uint32_t)NIXIE_GHOST);

    // ── Body — word-wrap at size 1 (max ~38 chars per line, 3 lines) ─────────
    canvas.setTextSize(1);
    canvas.setTextColor(NIXIE_DIM);
    const int BODY_CHARS = 36;
    const int BODY_LINES = 3;
    int by = divY + 5;
    const char* bp = n.body;
    for (int ln = 0; ln < BODY_LINES && *bp; ln++) {
        char buf[40] = {};
        int rem = strlen(bp);
        if (rem <= BODY_CHARS) {
            strncpy(buf, bp, BODY_CHARS);
            bp += rem;
        } else {
            int brk = BODY_CHARS;
            for (int i = BODY_CHARS; i > 0; i--) {
                if (bp[i] == ' ') { brk = i; break; }
            }
            strncpy(buf, bp, brk); buf[brk] = 0;
            bp += brk + (bp[brk] == ' ' ? 1 : 0);
        }
        canvas.setCursor(4, by + ln * 12);
        canvas.print(buf);
    }

    // ── Progress bar (auto-dismiss countdown) ─────────────────────────────────
    unsigned long tms = notifTimeoutMs(g_notifOverlayTimeoutIdx);
    if (tms > 0) {
        unsigned long elapsed = millis() - g_notifOverlayShownAt;
        int barW = DISP_W - 8;
        int filled = (int)(barW * (tms - min(elapsed, tms)) / tms);
        canvas.drawRoundRect(4, DISP_H - 14, barW, 5, 2, (uint32_t)NIXIE_GHOST);
        if (filled > 0) canvas.fillRoundRect(4, DISP_H - 14, filled, 5, 2, (uint32_t)NIXIE_DIM);
    }

    // ── Button hints ──────────────────────────────────────────────────────────
    canvas.setTextSize(1);
    canvas.setTextColor(NIXIE_GHOST);
    canvas.setCursor(4, DISP_H - 9);          canvas.print("F=read");
    canvas.setCursor(DISP_W - 52, DISP_H - 9); canvas.print("F+hold=del");
}

// ══════════════════════════════════════════════════════════════════════════════
// SCREEN: CLOCK
// ══════════════════════════════════════════════════════════════════════════════
static void drawClock() {
    auto dt = M5.Rtc.getDateTime();
    int hh = dt.time.hours, mm = dt.time.minutes, ss = dt.time.seconds;
    int dd = dt.date.date, mo = dt.date.month, yr = dt.date.year;
    int dow = calcDow(yr, mo, dd);

    drawHeader("SIDECAR V1");

    // Day label
    canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
    canvas.setCursor(6, 17); canvas.print(dow >= 0 && dow < 7 ? DOW_NAMES[dow] : "---");

    // Time HH:MM
    char hhmm[6]; snprintf(hhmm, sizeof(hhmm), "%02d:%02d", hh, mm);
    canvas.setTextSize(4); canvas.setTextColor(NIXIE_ORANGE);
    canvas.setCursor(6, 25); canvas.print(hhmm);

    // Seconds
    char sb[5]; snprintf(sb, sizeof(sb), ":%02d", ss);
    canvas.setTextSize(2); canvas.setTextColor(NIXIE_DIM);
    canvas.setCursor(6, 66); canvas.print(sb);

    // Date
    char db[16]; snprintf(db, sizeof(db), "%02d %s %04d", dd,
        (mo >= 1 && mo <= 12) ? MON_NAMES[mo] : "???", yr);
    canvas.setTextSize(2); canvas.setTextColor(NIXIE_GHOST);
    canvas.setCursor(6, 86); canvas.print(db);

    // Divider
    canvas.drawFastVLine(152, 15, DISP_H - 35, (uint32_t)NIXIE_GHOST);

    // Steps
    canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
    canvas.setCursor(158, 17); canvas.print("STEPS");
    canvas.setTextSize(2); canvas.setTextColor(NIXIE_DIM);
    canvas.setCursor(158, 26);
    if (g_stepCount < 10000) canvas.printf("%d", g_stepCount);
    else                     canvas.printf("%dk", g_stepCount / 1000);

    // Battery
    int bat = dispMgr_getBatt();
    bool charging = (M5.Power.isCharging() == m5::Power_Class::is_charging);
    canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
    canvas.setCursor(158, 58); canvas.print("BATTERY");
    canvas.setTextSize(2);
    if (charging) {
        // Flash "CHG" green at 1Hz while charging
        bool flash = ((millis() / 500) % 2) == 0;
        canvas.setTextColor(flash ? (uint32_t)COL_GREEN : (uint32_t)NIXIE_GHOST);
        canvas.setCursor(158, 67); canvas.printf("%d%%", bat);
        canvas.setTextSize(1);
        canvas.setTextColor(flash ? (uint32_t)COL_GREEN : (uint32_t)NIXIE_GHOST);
        canvas.setCursor(158, 83); canvas.print("* CHG");
    } else {
        canvas.setTextColor(bat > 30 ? (uint32_t)COL_GREEN : (uint32_t)COL_RED);
        canvas.setCursor(158, 67); canvas.printf("%d%%", bat);
        drawBattBar(158, 88, bat);
    }

    // BLE / mode status
    canvas.setTextSize(1);
    if (g_powerMode == PWR_DEEPSLEEP) {
        canvas.setTextColor(COL_RED);
        canvas.setCursor(158, 93);  canvas.print("DEEP");
        canvas.setCursor(158, 103); canvas.print("SLEEP");
    } else {
        // Mode letter: N = NORMAL, E = EFFICIENT
        canvas.setTextColor(NIXIE_GHOST);
        canvas.setCursor(158, 93);
        canvas.print(g_powerMode == PWR_NORMAL ? "NORMAL" : "EFFIC.");
        // BLE connection indicator
        canvas.setTextColor(g_bleConn ? (uint32_t)COL_BLUE : (uint32_t)NIXIE_GHOST);
        canvas.setCursor(158, 103);
        canvas.print(g_bleConn ? "* CONN" : "  BLE-");
    }

    // Seconds progress line
    canvas.drawFastHLine(2, DISP_H-20, DISP_W-4, (uint32_t)NIXIE_GHOST);
    int spx = (int)((float)ss / 59.0f * (DISP_W - 6)) + 3;
    if (ss > 0) canvas.drawFastHLine(3, DISP_H-20, spx-3, (uint32_t)NIXIE_ORANGE);

    drawHints("power mode", "---");
}

// ══════════════════════════════════════════════════════════════════════════════
// SCREEN: STOPWATCH
// ══════════════════════════════════════════════════════════════════════════════
static void drawStopwatch() {
    if (sw_running) sw_elapsed = millis() - sw_start;
    drawHeader("STOPWATCH");

    char tbuf[12]; fmtMs(sw_elapsed, tbuf, sizeof(tbuf));
    canvas.setTextSize(3);
    canvas.setTextColor(sw_running ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_DIM);
    int tw = canvas.textWidth(tbuf);
    canvas.setCursor((160 - tw) / 2 + 4, 17); canvas.print(tbuf);

    canvas.setTextSize(1);
    canvas.setTextColor(sw_running ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_GHOST);
    canvas.setCursor(6, 50); canvas.print(sw_running ? "* RUNNING" : "  STOPPED");

    // Laps
    if (sw_lapCount > 0) {
        canvas.drawFastHLine(4, 57, 148, (uint32_t)NIXIE_GHOST);
        int show = min(sw_lapCount, 4), y = 60;
        for (int i = sw_lapCount - 1; i >= sw_lapCount - show; i--) {
            char lbuf[14]; fmtMs(sw_laps[i], lbuf, sizeof(lbuf));
            bool newest = (i == sw_lapCount - 1);
            canvas.setTextColor(newest ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_GHOST);
            canvas.setCursor(6, y); canvas.printf("L%02d %s", i+1, lbuf);
            y += 14;
        }
    }

    // Right panel
    canvas.drawFastVLine(153, 15, DISP_H - 35, (uint32_t)NIXIE_GHOST);
    canvas.setTextSize(1);
    canvas.fillRoundRect(157, 18, 80, 18, 3, sw_running ? (uint32_t)NIXIE_DIM : (uint32_t)NIXIE_GHOST);
    canvas.setTextColor(sw_running ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_DIM);
    canvas.setCursor(165, 24); canvas.print(sw_running ? "  STOP" : "  START");
    canvas.fillRoundRect(157, 42, 80, 18, 3, (uint32_t)NIXIE_GHOST);
    canvas.setTextColor(sw_running ? (uint32_t)NIXIE_DIM : (uint32_t)NIXIE_GHOST);
    canvas.setCursor(165, 48); canvas.print("  LAP");
    canvas.fillRoundRect(157, 66, 80, 18, 3, (uint32_t)NIXIE_GHOST);
    canvas.setTextColor(!sw_running && sw_elapsed > 0 ? (uint32_t)NIXIE_DIM : (uint32_t)NIXIE_GHOST);
    canvas.setCursor(165, 72); canvas.print("  RESET");
    canvas.setTextColor(NIXIE_GHOST);
    canvas.setCursor(160, 90); canvas.printf("%d laps", sw_lapCount);

    drawHints(sw_running ? "lap" : "---", "start/stop", "reset");
}

// ══════════════════════════════════════════════════════════════════════════════
// SCREEN: TIMER
// ══════════════════════════════════════════════════════════════════════════════
static const char* timerModeLabel(TimerMode m) {
    switch (m) {
        case TMR_MODE_POMODORO: return "POMODORO";
        case TMR_MODE_BREATHE:  return "BREATHE";
        default:                return "TIMER";
    }
}

static void drawTimer() {
    drawHeader(timerModeLabel(g_timerMode));

    // ── BREATHE mode: guided breathing animation ──────────────────────────────
    if (g_timerMode == TMR_MODE_BREATHE) {
        // 4s inhale, 4s hold, 4s exhale — 12s total cycle
        // TMR_RUNNING = session active; TMR_IDLE = ready to start
        if (tmr_state == TMR_IDLE) {
            canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
            int tw = canvas.textWidth("FRONT to begin");
            canvas.setCursor((DISP_W-tw)/2, 35); canvas.print("FRONT to begin");
            canvas.setTextSize(2); canvas.setTextColor(NIXIE_DIM);
            tw = canvas.textWidth("4-4-4");
            canvas.setCursor((DISP_W-tw)/2, 50); canvas.print("4-4-4");
            canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
            tw = canvas.textWidth("inhale - hold - exhale");
            canvas.setCursor((DISP_W-tw)/2, 74); canvas.print("inhale - hold - exhale");
            drawHints("mode", "start", "cancel");
            return;
        }
        if (tmr_state == TMR_DONE || tmr_state == TMR_PAUSED) {
            canvas.setTextSize(2); canvas.setTextColor(NIXIE_DIM);
            int tw = canvas.textWidth("PAUSED");
            canvas.setCursor((DISP_W-tw)/2, 50); canvas.print("PAUSED");
            drawHints("---", "resume", "cancel");
            return;
        }
        // Running — compute phase from time within 12s cycle
        unsigned long elapsed = millis() - (tmr_endMs - 12000UL); // tmr_endMs used as cycle ref
        unsigned long phase_ms = elapsed % 12000UL;
        const char* phaseName;
        float arcFrac;   // 0..1 — fraction of arc to fill (inhale=growing, hold=full, exhale=shrinking)
        uint32_t arcCol;
        if (phase_ms < 4000UL) {
            // INHALE 0-4s — arc expands
            phaseName = "INHALE";
            arcFrac   = (float)phase_ms / 4000.0f;
            arcCol    = NIXIE_ORANGE;
        } else if (phase_ms < 8000UL) {
            // HOLD 4-8s — arc full, pulsing colour
            phaseName = "HOLD";
            arcFrac   = 1.0f;
            arcCol    = ((millis() / 400) % 2) ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_DIM;
        } else {
            // EXHALE 8-12s — arc contracts
            phaseName = "EXHALE";
            arcFrac   = 1.0f - (float)(phase_ms - 8000UL) / 4000.0f;
            arcCol    = NIXIE_DIM;
        }
        // Phase countdown (seconds remaining in this phase)
        unsigned long phaseEnd = (phase_ms < 4000UL) ? 4000UL
                               : (phase_ms < 8000UL) ? 8000UL : 12000UL;
        int phaseSec = (int)((phaseEnd - phase_ms + 999UL) / 1000UL);

        int cx = DISP_W/2, cy = 72, outr = 46, inr = 32;
        canvas.fillArc(cx, cy, outr, inr, -90.0f, -90.0f + arcFrac*360.0f, arcCol);
        canvas.fillArc(cx, cy, outr, inr, -90.0f + arcFrac*360.0f, 270.0f, (uint32_t)NIXIE_GHOST);

        // Phase label and countdown inside arc
        canvas.setTextSize(1); canvas.setTextColor(NIXIE_ORANGE);
        int tw = canvas.textWidth(phaseName);
        canvas.setCursor(cx - tw/2, cy - 8); canvas.print(phaseName);
        canvas.setTextSize(2); canvas.setTextColor(NIXIE_DIM);
        char sb[4]; snprintf(sb, sizeof(sb), "%d", phaseSec);
        tw = canvas.textWidth(sb);
        canvas.setCursor(cx - tw/2, cy + 2); canvas.print(sb);

        drawHints("---", "pause", "cancel");
        return;
    }

    // ── POMODORO mode: 25min work / 5min break, auto-cycles ──────────────────
    // TMR_MODE_POMODORO uses tmr_setMin to distinguish work(25) vs break(5).
    // Round count stored in tmr_remaining when IDLE.
    if (g_timerMode == TMR_MODE_POMODORO) {
        bool isBreak = (tmr_setMin == 5);
        if (tmr_state == TMR_IDLE) {
            canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
            int tw = canvas.textWidth(isBreak ? "BREAK TIME" : "FOCUS TIME");
            canvas.setCursor((DISP_W-tw)/2, 17); canvas.print(isBreak ? "BREAK TIME" : "FOCUS TIME");
            canvas.setTextSize(3); canvas.setTextColor(isBreak ? (uint32_t)COL_GREEN : (uint32_t)NIXIE_ORANGE);
            char ss[6]; snprintf(ss, sizeof(ss), "%02d:00", tmr_setMin);
            tw = canvas.textWidth(ss);
            canvas.setCursor((DISP_W-tw)/2, 32); canvas.print(ss);
            drawHints("mode", "start");
            return;
        }
        if (tmr_state == TMR_DONE) {
            // Auto-restart handled in main.cpp — this frame shows the flash
            canvas.setTextSize(3); canvas.setTextColor(COL_RED);
            int tw = canvas.textWidth("DONE!");
            canvas.setCursor((DISP_W-tw)/2, 36); canvas.print("DONE!");
            drawHints("---", "dismiss");
            return;
        }
    }

    // ── NORMAL timer + POMODORO running/paused ────────────────────────────────
    if (tmr_state == TMR_IDLE) {
        canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
        int lw = canvas.textWidth("PRESS FRONT TO START");
        canvas.setCursor((DISP_W-lw)/2, 17); canvas.print("PRESS FRONT TO START");
        char ss[6]; snprintf(ss, sizeof(ss), "%02d:00", tmr_setMin);
        canvas.setTextSize(5); canvas.setTextColor(NIXIE_ORANGE);
        int tw = canvas.textWidth(ss);
        canvas.setCursor((DISP_W-tw)/2, 30); canvas.print(ss);
        canvas.setTextSize(1); canvas.setTextColor(NIXIE_DIM);
        canvas.setCursor(6, 90); canvas.print("TOP: +1min  TOP-long: -1min");
        drawHints("+1/-1 min", "start", "mode");
    } else {
        unsigned long rem = (tmr_state == TMR_PAUSED) ? tmr_remaining
                          : (tmr_endMs > millis() ? tmr_endMs - millis() : 0);

        if (tmr_state == TMR_DONE) {
            canvas.setTextSize(4); canvas.setTextColor(COL_RED);
            int tw = canvas.textWidth("DONE!");
            canvas.setCursor((DISP_W-tw)/2, 36); canvas.print("DONE!");
            canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
            tw = canvas.textWidth("FRONT to dismiss");
            canvas.setCursor((DISP_W-tw)/2, 80); canvas.print("FRONT to dismiss");
            drawHints("---", "dismiss");
            return;
        }

        // Arc progress
        float frac = 1.0f - (float)rem / ((float)tmr_setMin * 60000.0f);
        int cx = 60, cy = 67, outr = 46, inr = 36;
        canvas.fillArc(cx, cy, outr, inr, -90.0f, -90.0f + frac*360.0f,
            rem < 60000 ? (uint32_t)COL_RED : (uint32_t)NIXIE_ORANGE);
        canvas.fillArc(cx, cy, outr, inr, -90.0f + frac*360.0f, 270.0f, (uint32_t)NIXIE_GHOST);
        canvas.setTextSize(1); canvas.setTextColor(NIXIE_DIM);
        char pct[6]; snprintf(pct, sizeof(pct), "%d%%", (int)((1.0f - frac) * 100));
        int pw = canvas.textWidth(pct);
        canvas.setCursor(cx - pw/2, cy - 4); canvas.print(pct);

        // Countdown
        canvas.drawFastVLine(120, 15, DISP_H - 35, (uint32_t)NIXIE_GHOST);
        char rbuf[6]; snprintf(rbuf, sizeof(rbuf), "%02d:%02d", (int)(rem/60000), (int)((rem/1000)%60));
        canvas.setTextSize(3);
        canvas.setTextColor(rem < 60000 ? (uint32_t)COL_RED : (uint32_t)NIXIE_ORANGE);
        int tw = canvas.textWidth(rbuf);
        canvas.setCursor(124 + (116-tw)/2, 25); canvas.print(rbuf);
        canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
        canvas.setCursor(130, 62);
        canvas.print(tmr_state == TMR_RUNNING ? "* RUNNING" : "  PAUSED");
        canvas.setCursor(130, 76); canvas.printf("of %02d:00 min", tmr_setMin);
        drawHints("---", "pause/resume", "cancel");
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// SCREEN: ALARM
// ══════════════════════════════════════════════════════════════════════════════
static void drawAlarm() {
    drawHeader("ALARM");

    // Ringing overlay
    if (g_alarmRinging) {
        uint32_t flash = ((millis()/300)%2) ? (uint32_t)COL_RED : (uint32_t)0x4000;
        canvas.fillRoundRect(6, 18, DISP_W-12, 62, 6, flash);
        canvas.setTextSize(2); canvas.setTextColor(TFT_WHITE);
        int tw = canvas.textWidth("ALARM!");
        canvas.setCursor((DISP_W-tw)/2, 30); canvas.print("ALARM!");
        canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
        tw = canvas.textWidth("FRONT to dismiss");
        canvas.setCursor((DISP_W-tw)/2, 62); canvas.print("FRONT to dismiss");
        drawHints("", "dismiss");
        return;
    }

    // Edit sub-screen
    if (g_alarmInEdit) {
        AlarmCfg& a = g_alarms[g_alarmSel];
        const char* DS[] = {"S","M","T","W","T","F","S"};

        // HH:MM
        canvas.setTextSize(2);
        char tstr[6]; snprintf(tstr, sizeof(tstr), "%02d:%02d", a.hour, a.minute);
        int tw = canvas.textWidth(tstr);
        int tx = (DISP_W - tw) / 2;
        canvas.setTextColor(NIXIE_ORANGE); canvas.setCursor(tx, 18); canvas.print(tstr);
        if (g_alarmEditField == 0)
            canvas.drawRoundRect(tx-2, 16, canvas.textWidth("00")+4, 18, 2, NIXIE_DIM);
        else if (g_alarmEditField == 1)
            canvas.drawRoundRect(tx + canvas.textWidth("00:")-2, 16, canvas.textWidth("00")+4, 18, 2, NIXIE_DIM);

        // Day toggles
        const int DOW_W = 24, DOW_Y = 40;
        int dstart = (DISP_W - 7*DOW_W) / 2;
        for (int d = 0; d < 7; d++) {
            bool on   = (a.daysOfWeek >> d) & 1;
            bool hlit = (g_alarmEditField == 2 + d);
            int dx    = dstart + d * DOW_W;
            uint32_t bg  = on  ? (uint32_t)NIXIE_ORANGE : (uint32_t)0x1080;
            uint32_t bdr = hlit ? (uint32_t)TFT_WHITE   : (uint32_t)NIXIE_GHOST;
            canvas.fillRoundRect(dx, DOW_Y, DOW_W-2, 16, 3, bg);
            canvas.drawRoundRect(dx, DOW_Y, DOW_W-2, 16, 3, bdr);
            canvas.setTextSize(1);
            canvas.setTextColor(on ? (uint32_t)COL_BG : (uint32_t)NIXIE_GHOST);
            canvas.setCursor(dx + (DOW_W-8)/2, DOW_Y+4); canvas.print(DS[d]);
        }

        // Date filter
        const int DY = 63;
        canvas.setTextSize(1);
        canvas.setTextColor(g_alarmEditField == 9 ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_DIM);
        canvas.setCursor(6, DY); canvas.print("DATE:");
        if (!a.useDate) {
            canvas.setTextColor(g_alarmEditField == 9 ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_GHOST);
            canvas.print(" OFF");
        } else {
            char dd[3]; snprintf(dd, 3, "%02d", a.dateDay);
            canvas.setTextColor(g_alarmEditField == 10 ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_DIM);
            if (g_alarmEditField == 10) canvas.fillRoundRect(38, DY-1, 16, 10, 2, (uint32_t)NIXIE_GHOST);
            canvas.setCursor(40, DY); canvas.print(dd);
            canvas.setTextColor(g_alarmEditField == 11 ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_DIM);
            if (g_alarmEditField == 11) canvas.fillRoundRect(57, DY-1, 24, 10, 2, (uint32_t)NIXIE_GHOST);
            canvas.setCursor(58, DY);
            canvas.print(a.dateMonth >= 1 && a.dateMonth <= 12 ? MON_NAMES[a.dateMonth] : "???");
        }
        drawHints("next field", "+1", "back/save");
        return;
    }

    // List view
    int y = 18;
    for (int i = 0; i < ALARM_MAX; i++) {
        bool sel = (i == g_alarmSel);
        if (sel) canvas.fillRoundRect(4, y, DISP_W-8, 30, 4, (uint32_t)0x0840);
        canvas.drawRoundRect(4, y, DISP_W-8, 30, 4, sel ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_GHOST);
        canvas.setTextSize(2);
        canvas.setTextColor(g_alarms[i].enabled
            ? (sel ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_DIM) : (uint32_t)NIXIE_GHOST);
        char tstr[6]; snprintf(tstr, sizeof(tstr), "%02d:%02d", g_alarms[i].hour, g_alarms[i].minute);
        canvas.setCursor(10, y+6); canvas.print(tstr);

        // Day summary
        canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
        if (g_alarms[i].daysOfWeek == 0x7F)      { canvas.setCursor(88, y+6);  canvas.print("daily"); }
        else if (g_alarms[i].daysOfWeek == 0x3E)  { canvas.setCursor(88, y+6);  canvas.print("M-F"); }
        else if (g_alarms[i].daysOfWeek == 0x41)  { canvas.setCursor(88, y+6);  canvas.print("S/S"); }
        else {
            const char* DS[] = {"S","M","T","W","T","F","S"};
            int dx = 88;
            for (int d = 0; d < 7; d++) if ((g_alarms[i].daysOfWeek >> d) & 1) {
                canvas.setCursor(dx, y+6); canvas.print(DS[d]); dx += 8;
            }
        }
        if (g_alarms[i].useDate) {
            char dbuf[8]; snprintf(dbuf, 8, "%d/%s", g_alarms[i].dateDay,
                g_alarms[i].dateMonth >= 1 && g_alarms[i].dateMonth <= 12 ? MON_NAMES[g_alarms[i].dateMonth] : "?");
            canvas.setCursor(88, y+16); canvas.print(dbuf);
        }

        // ON/OFF pill
        canvas.setTextSize(1);
        if (g_alarms[i].enabled) {
            canvas.fillRoundRect(DISP_W-48, y+8, 38, 13, 3, (uint32_t)COL_GREEN);
            canvas.setTextColor(COL_BG); canvas.setCursor(DISP_W-44, y+11); canvas.print("ON");
        } else {
            canvas.drawRoundRect(DISP_W-48, y+8, 38, 13, 3, (uint32_t)NIXIE_GHOST);
            canvas.setTextColor(NIXIE_GHOST); canvas.setCursor(DISP_W-46, y+11); canvas.print("OFF");
        }
        y += 34;
    }
    drawHints("next alarm", "toggle on/off", "edit");
}

// ══════════════════════════════════════════════════════════════════════════════
// SCREEN: NOTIFICATIONS
// ══════════════════════════════════════════════════════════════════════════════
static void drawNotifs() {
    drawHeader("NOTIFICATIONS");

    if (g_notifCount == 0) {
        canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
        int tw = canvas.textWidth("No notifications");
        canvas.setCursor((DISP_W-tw)/2, 60); canvas.print("No notifications");
        canvas.setTextColor(NIXIE_GHOST);
        tw = canvas.textWidth("Connect BLE to receive");
        canvas.setCursor((DISP_W-tw)/2, 76); canvas.print("Connect BLE to receive");
        drawHints("scroll", "dismiss");
        return;
    }

    canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
    canvas.setCursor(DISP_W-36, 3); canvas.printf("%d/%d", g_notifIdx+1, g_notifCount);

    int vis = 3, start = max(0, min(g_notifIdx-1, g_notifCount - vis));
    int y = 17;
    for (int i = start; i < g_notifCount && i < start + vis; i++) {
        bool sel = (i == g_notifIdx);
        // Border colour: type-coded when unread, selection-coded when read
        uint32_t typeCol = (g_notifs[i].type == NOTIF_CALL) ? (uint32_t)COL_BLUE
                         : (g_notifs[i].type == NOTIF_MSG)  ? (uint32_t)COL_GREEN
                                                             : (uint32_t)COL_RED;
        uint32_t border = g_notifs[i].unread ? typeCol : (sel ? NIXIE_ORANGE : NIXIE_GHOST);
        canvas.drawRoundRect(3, y, DISP_W-6, 30, 3, border);
        canvas.setTextSize(1);
        canvas.setTextColor(g_notifs[i].unread ? typeCol : (sel ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_GHOST));
        canvas.setCursor(9, y+4); canvas.print(g_notifs[i].app);
        canvas.setTextColor(sel ? (uint32_t)NIXIE_DIM : (uint32_t)NIXIE_GHOST);
        canvas.setCursor(9, y+16);
        char t[30]; strncpy(t, g_notifs[i].title, 29); t[29] = 0;
        canvas.print(t);
        y += 34;
    }
    drawHints("scroll up/down", "dismiss", "clear all");
}

// ══════════════════════════════════════════════════════════════════════════════
// SCREEN: MEDIA PLAYER
// ══════════════════════════════════════════════════════════════════════════════
static void drawMedia() {
    drawHeader("MEDIA");

    bool hasMedia = (g_mediaArtist[0] != '\0' || g_mediaSong[0] != '\0');
    bool connected = g_bleConn;

    if (!connected) {
        canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
        canvas.setCursor(6, 40); canvas.print("NO PHONE");
        canvas.setCursor(6, 54); canvas.print("CONNECTED");
        drawHints("---", "---");
        return;
    }

    if (!hasMedia) {
        canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
        canvas.setCursor(6, 40); canvas.print("NO MEDIA");
        canvas.setCursor(6, 54); canvas.print("PLAYING");
        drawHints("---", "play/pause", "next");
        return;
    }

    // ── Song title (large, wraps at display width) ────────────────────────
    canvas.setTextSize(2); canvas.setTextColor(NIXIE_ORANGE);
    // Truncate to fit: textSize 2 = 12px wide per char, display 240px → 20 chars
    char songBuf[21];
    snprintf(songBuf, sizeof(songBuf), "%s", g_mediaSong);
    canvas.setCursor(4, 17); canvas.print(songBuf);

    // Second line if song title long
    if (strlen(g_mediaSong) > 20) {
        char songBuf2[21];
        snprintf(songBuf2, sizeof(songBuf2), "%s", g_mediaSong + 20);
        canvas.setCursor(4, 35); canvas.print(songBuf2);
    }

    // ── Artist (smaller, dim) ─────────────────────────────────────────────
    canvas.setTextSize(1); canvas.setTextColor(NIXIE_DIM);
    char artistBuf[32];
    snprintf(artistBuf, sizeof(artistBuf), "%s", g_mediaArtist);
    canvas.setCursor(4, 57); canvas.print(artistBuf);

    // ── Divider ───────────────────────────────────────────────────────────
    canvas.drawFastHLine(0, 68, DISP_W, (uint32_t)NIXIE_GHOST);

    // ── Play/pause indicator ──────────────────────────────────────────────
    canvas.setTextSize(1);
    if (g_mediaPlaying) {
        // Animated ▐▐ pulse using millis
        bool beat = ((millis() / 600) % 2) == 0;
        canvas.setTextColor(beat ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_DIM);
        canvas.setCursor(4, 75); canvas.print(">> PLAYING");
    } else {
        canvas.setTextColor(NIXIE_GHOST);
        canvas.setCursor(4, 75); canvas.print("|| PAUSED");
    }

    // ── Volume bar ────────────────────────────────────────────────────────
    if (g_mediaVolume >= 0) {
        canvas.setTextColor(NIXIE_GHOST);
        canvas.setCursor(4, 88); canvas.print("VOL");
        int barX = 28, barW = DISP_W - 36, barH = 4, barY = 90;
        canvas.drawRect(barX, barY, barW, barH, (uint32_t)NIXIE_GHOST);
        int fill = (int)((float)g_mediaVolume / 100.0f * (barW - 2));
        if (fill > 0) canvas.fillRect(barX + 1, barY + 1, fill, barH - 2, (uint32_t)NIXIE_ORANGE);
        char volBuf[5]; snprintf(volBuf, sizeof(volBuf), "%d%%", g_mediaVolume);
        canvas.setCursor(DISP_W - canvas.textWidth(volBuf) - 2, 88);
        canvas.print(volBuf);
    }

    // ── Controls hint ─────────────────────────────────────────────────────
    drawHints("vol up/down", "play/pause", "next  [hold=prev]");
}

// ══════════════════════════════════════════════════════════════════════════════
// SCREEN: WEATHER
// ══════════════════════════════════════════════════════════════════════════════
static void drawWeather() {
    drawHeader("WEATHER");

    if (g_weather.updatedAt == 0) {
        // No data yet
        canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
        int tw = canvas.textWidth("No weather data");
        canvas.setCursor((DISP_W-tw)/2, 45); canvas.print("No weather data");
        tw = canvas.textWidth("Connect phone to sync");
        canvas.setCursor((DISP_W-tw)/2, 59); canvas.print("Connect phone to sync");
        drawHints("---", "---");
        return;
    }

    // ── Temperature (large) ───────────────────────────────────────────────────
    canvas.setTextSize(4); canvas.setTextColor(NIXIE_ORANGE);
    char tbuf[8]; snprintf(tbuf, sizeof(tbuf), "%d\xB0", g_weather.tempC); // °
    int tw = canvas.textWidth(tbuf);
    canvas.setCursor((DISP_W/2 - tw) / 2, 18); canvas.print(tbuf);

    // ── Condition ─────────────────────────────────────────────────────────────
    canvas.setTextSize(2); canvas.setTextColor(NIXIE_DIM);
    int cw = canvas.textWidth(g_weather.condition);
    canvas.setCursor((DISP_W/2 - cw) / 2, 60); canvas.print(g_weather.condition);

    // ── Divider ───────────────────────────────────────────────────────────────
    canvas.drawFastVLine(DISP_W/2, 15, DISP_H - 35, (uint32_t)NIXIE_GHOST);

    // ── Right panel: Hi/Lo + age ──────────────────────────────────────────────
    int rx = DISP_W/2 + 8;
    canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
    canvas.setCursor(rx, 18); canvas.print("HIGH");
    canvas.setTextSize(2); canvas.setTextColor(NIXIE_ORANGE);
    char hibuf[6]; snprintf(hibuf, sizeof(hibuf), "%d\xB0", g_weather.hiC);
    canvas.setCursor(rx, 27); canvas.print(hibuf);

    canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
    canvas.setCursor(rx, 52); canvas.print("LOW");
    canvas.setTextSize(2); canvas.setTextColor(NIXIE_DIM);
    char lobuf[6]; snprintf(lobuf, sizeof(lobuf), "%d\xB0", g_weather.loC);
    canvas.setCursor(rx, 61); canvas.print(lobuf);

    // Age of weather data
    unsigned long ageMin = (millis() - g_weather.updatedAt) / 60000UL;
    canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
    if (ageMin < 60) {
        canvas.setCursor(rx, 85); canvas.printf("%lum ago", ageMin);
    } else {
        canvas.setCursor(rx, 85); canvas.printf("%luh ago", ageMin/60);
    }

    drawHints("---", "---");
}

// ══════════════════════════════════════════════════════════════════════════════
// SCREEN: POWER MODES
// ══════════════════════════════════════════════════════════════════════════════
static void drawPower() {
    if (g_pwrInCustom) {
        // Customization sub-screen
        char title[24]; snprintf(title, sizeof(title), "POWER / %s", PWR_NAMES[g_pwrTabSel]);
        drawHeader(title);

        PwrCfg& c = g_pwrCfg[g_pwrTabSel];
        const char* fnames[PWRCFG_FIELDS] = {"BRIGHTNESS", "TIMEOUT"};
        char tvals[PWRCFG_FIELDS][12];
        snprintf(tvals[0], 12, "%d/5", c.brightness);
        if (c.timeoutSec == 0)       snprintf(tvals[1], 12, "OFF");
        else if (c.timeoutSec < 60)  snprintf(tvals[1], 12, "%ds", c.timeoutSec);
        else                         snprintf(tvals[1], 12, "%dm", c.timeoutSec / 60);

        if (g_pwrTabSel == (int)g_powerMode) {
            canvas.setTextSize(1); canvas.setTextColor(COL_GREEN);
            int aw = canvas.textWidth("* ACTIVE");
            canvas.setCursor((DISP_W-aw)/2, 17); canvas.print("* ACTIVE");
        } else {
            canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
            int aw = canvas.textWidth("not active");
            canvas.setCursor((DISP_W-aw)/2, 17); canvas.print("not active");
        }

        int y = 32;
        for (int i = 0; i < PWRCFG_FIELDS; i++) {
            bool sel = (i == g_pwrCustIdx);
            if (sel) canvas.fillRoundRect(10, y-2, DISP_W-20, 20, 3, (uint32_t)NIXIE_GHOST);
            canvas.setTextSize(1);
            canvas.setTextColor(sel ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_DIM);
            canvas.setCursor(18, y+3); canvas.print(fnames[i]);
            canvas.setTextColor(sel ? (uint32_t)NIXIE_ORANGE : (uint32_t)TFT_WHITE);
            int vw = canvas.textWidth(tvals[i]);
            canvas.setCursor(DISP_W-18-vw, y+3); canvas.print(tvals[i]);
            y += 26;
        }
        drawHints("next/prev field", "+1 value", "back");
    } else {
        // Mode selector
        drawHeader("POWER MODES");
        const char LETTERS[PWR_COUNT] = {'N', 'E', 'D'};
        const char* fullDesc[PWR_COUNT] = {
            "160MHz  BLE on",
            "80MHz   BLE on",
            "40MHz   BLE off"
        };
        const int STRIP_Y = 18, STRIP_H = 44;
        const int CELL_W = DISP_W / PWR_COUNT;

        for (int i = 0; i < PWR_COUNT; i++) {
            bool sel    = (i == g_pwrTabSel);
            bool active = (i == (int)g_powerMode);
            int cx      = i * CELL_W;

            if (sel) {
                canvas.fillRoundRect(cx+2, STRIP_Y, CELL_W-4, STRIP_H, 5, (uint32_t)0x1880);
                uint32_t border = active ? (uint32_t)COL_GREEN : (uint32_t)NIXIE_ORANGE;
                canvas.drawRoundRect(cx+2, STRIP_Y, CELL_W-4, STRIP_H, 5, border);
                canvas.drawRoundRect(cx+3, STRIP_Y+1, CELL_W-6, STRIP_H-2, 4, border);
            }

            int tsz = sel ? 3 : 2;
            canvas.setTextSize(tsz);
            uint32_t col = active ? (uint32_t)COL_GREEN
                         : (sel ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_GHOST);
            canvas.setTextColor(col);
            int charW = 6 * tsz, charH = 8 * tsz;
            canvas.setCursor(cx + (CELL_W-charW)/2, STRIP_Y + (STRIP_H-charH)/2);
            canvas.print(LETTERS[i]);

            if (active && !sel)
                canvas.fillCircle(cx + CELL_W/2, STRIP_Y + STRIP_H - 5, 2, (uint32_t)COL_GREEN);
        }

        // Detail panel
        const int DY = STRIP_Y + STRIP_H + 4;
        bool active = (g_pwrTabSel == (int)g_powerMode);
        canvas.setTextSize(2);
        canvas.setTextColor(active ? (uint32_t)COL_GREEN : (uint32_t)NIXIE_ORANGE);
        int nw = canvas.textWidth(PWR_NAMES[g_pwrTabSel]);
        canvas.setCursor((DISP_W-nw)/2, DY); canvas.print(PWR_NAMES[g_pwrTabSel]);

        canvas.setTextSize(1); canvas.setTextColor(NIXIE_DIM);
        int dw = canvas.textWidth(fullDesc[g_pwrTabSel]);
        canvas.setCursor((DISP_W-dw)/2, DY+18); canvas.print(fullDesc[g_pwrTabSel]);

        // Stats
        char tbuf[8];
        if (g_pwrCfg[g_pwrTabSel].timeoutSec == 0)       snprintf(tbuf, 8, "off");
        else if (g_pwrCfg[g_pwrTabSel].timeoutSec < 60)   snprintf(tbuf, 8, "%ds", g_pwrCfg[g_pwrTabSel].timeoutSec);
        else                                               snprintf(tbuf, 8, "%dm", g_pwrCfg[g_pwrTabSel].timeoutSec/60);
        char stats[40];
        snprintf(stats, sizeof(stats), "B:%d/5  tmout:%s  ble:%s",
            g_pwrCfg[g_pwrTabSel].brightness, tbuf,
            (g_pwrTabSel == (int)PWR_DEEPSLEEP) ? "off" : "on");
        canvas.setTextColor(NIXIE_GHOST);
        int sw = canvas.textWidth(stats);
        canvas.setCursor((DISP_W-sw)/2, DY+28); canvas.print(stats);

        // Battery life
        float bLife = battLifeHours(g_pwrTabSel);
        char blifeBuf[16]; fmtBattLife(bLife, blifeBuf, sizeof(blifeBuf));
        char battLine[32]; snprintf(battLine, sizeof(battLine), "bat %d%%  life%s",
            M5.Power.getBatteryLevel(), blifeBuf);
        canvas.setTextColor(active ? (uint32_t)COL_GREEN : (uint32_t)NIXIE_DIM);
        sw = canvas.textWidth(battLine);
        canvas.setCursor((DISP_W-sw)/2, DY+39); canvas.print(battLine);

        drawHints("scroll modes", "activate", "customize");
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// SCREEN: BATTERY DIAGNOSTICS
// ══════════════════════════════════════════════════════════════════════════════
static void drawDiag() {
    drawHeader("BATTERY DIAG");

    float batV    = M5.Power.getBatteryVoltage() / 1000.0f;
    int   batPct  = dispMgr_getBatt();
    float modelMA = dispMgr_getEstimatedMa(g_powerMode);
    int n = min(s_battLogCount, (int)BATT_LOG_SIZE);

    // Rolling 5-sample mA windows
    float sumMA = 0; int maCount = 0; float lastWindowMA = -1;
    if (n >= 6) {
        for (int i = 5; i < n; i++) {
            int idx_n = (s_battLogHead - n + i     + BATT_LOG_SIZE) % BATT_LOG_SIZE;
            int idx_o = (s_battLogHead - n + (i-5) + BATT_LOG_SIZE) % BATT_LOG_SIZE;
            float ma = logMa(idx_n, idx_o);
            sumMA += ma; maCount++;
            if (i == n-1) lastWindowMA = ma;
        }
    }
    float avgMA = maCount > 0 ? sumMA / maCount : modelMA;
    bool usingModel = (lastWindowMA <= 0);
    float nowMA     = usingModel ? modelMA : lastWindowMA;
    float hoursLeft = nowMA > 0 ? (batPct / 100.0f * (float)BATT_MAH) / nowMA : 0;

    // Stats
    canvas.setTextSize(1); canvas.setTextColor(NIXIE_ORANGE);
    canvas.setCursor(4, 16);
    canvas.printf("%.2fV  %d%%  %s", batV, batPct, PWR_NAMES[g_powerMode]);
    char nowBuf[10], avgBuf[10];
    snprintf(nowBuf, sizeof(nowBuf), nowMA < 10.0f ? "%.1f" : "%.0f", nowMA);
    snprintf(avgBuf, sizeof(avgBuf), avgMA < 10.0f ? "%.1f" : "%.0f", avgMA);
    canvas.setTextColor(NIXIE_DIM);
    canvas.setCursor(4, 26);
    if (g_powerMode == PWR_EFFICIENT || g_powerMode == PWR_DEEPSLEEP) {
        // AXP2101 on PLUS2 has no real-time current ADC.
        // Show screen-off (sleep) estimate — this is what determines battery life.
        // Active draw when screen is on is always higher (~8-10mA for EFFICIENT, ~6mA for DEEPSLEEP).
        if (usingModel) canvas.printf("SLEEP:~%smA  LEFT:%.0fh", nowBuf, hoursLeft);
        else            canvas.printf("SLEEP:%smA  AVG:%smA  LEFT:%.1fh", nowBuf, avgBuf, hoursLeft);
    } else {
        if (usingModel) canvas.printf("NOW:%smA(mdl %d/6) LEFT:%.0fh", nowBuf, min(n,6), hoursLeft);
        else            canvas.printf("NOW:%smA  AVG:%smA  LEFT:%.1fh", nowBuf, avgBuf, hoursLeft);
    }

    // Battery % graph
    const int GX = 4, GY = 37, GW = DISP_W-8, GH = 31, GLEFT = GX+22;
    canvas.drawRoundRect(GX, GY, GW, GH, 2, (uint32_t)NIXIE_GHOST);
    canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
    canvas.setCursor(GX+2, GY+1);     canvas.print("100");
    canvas.setCursor(GX+2, GY+GH-9);  canvas.print("0%");
    if (n >= 2) {
        int prevX = -1, prevY = -1;
        for (int i = 0; i < n; i++) {
            int idx = (s_battLogHead - n + i + BATT_LOG_SIZE) % BATT_LOG_SIZE;
            int v   = (int)s_battLog[idx];
            int px  = GLEFT + (i * (GX+GW-2-GLEFT)) / max(n-1, 1);
            int py  = GY+GH-2 - (v * (GH-4)) / 100;
            uint32_t col = v > 30 ? (uint32_t)COL_GREEN : (uint32_t)COL_RED;
            if (prevX >= 0) canvas.drawLine(prevX, prevY, px, py, col);
            canvas.drawPixel(px, py, col);
            prevX = px; prevY = py;
        }
        canvas.setTextColor(NIXIE_GHOST);
        canvas.setCursor(DISP_W-32, GY+GH-9); canvas.printf("-%dm", n);
    } else {
        canvas.setCursor(GLEFT+4, GY+GH/2-4);
        canvas.printf("sample 1/%d...", max(2, n+1));
    }

    // mA graph
    const int MX = 4, MY = 72, MW = DISP_W-8, MH = 31, MLEFT = MX+22;
    canvas.drawRoundRect(MX, MY, MW, MH, 2, (uint32_t)NIXIE_GHOST);
    canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
    if (n >= 6 && maCount >= 1) {
        float hiMA = 0;
        for (int i = 5; i < n; i++) {
            int idx_n = (s_battLogHead-n+i    +BATT_LOG_SIZE)%BATT_LOG_SIZE;
            int idx_o = (s_battLogHead-n+(i-5)+BATT_LOG_SIZE)%BATT_LOG_SIZE;
            float ma = logMa(idx_n, idx_o);
            if (ma > hiMA) hiMA = ma;
        }
        if (hiMA < avgMA * 1.5f) hiMA = avgMA * 1.5f;
        if (hiMA < 10.0f)        hiMA = max(modelMA * 1.5f, 10.0f);
        canvas.setCursor(MX+2, MY+1);    canvas.printf("%.0f", hiMA);
        canvas.setCursor(MX+2, MY+MH-9); canvas.print("0");
        int avgLineY = MY+MH-2 - (int)(avgMA*(MH-4)/hiMA);
        avgLineY = constrain(avgLineY, MY+1, MY+MH-2);
        for (int x = MLEFT; x < MX+MW-2; x += 4) canvas.drawPixel(x, avgLineY, (uint32_t)NIXIE_DIM);
        int prevX2 = -1, prevY2 = -1;
        for (int i = 5; i < n; i++) {
            int idx_n = (s_battLogHead-n+i    +BATT_LOG_SIZE)%BATT_LOG_SIZE;
            int idx_o = (s_battLogHead-n+(i-5)+BATT_LOG_SIZE)%BATT_LOG_SIZE;
            float ma = logMa(idx_n, idx_o);
            int j  = i - 5;
            int px = MLEFT + (j*(MX+MW-2-MLEFT)) / max(maCount-1, 1);
            int py = MY+MH-2 - (int)(ma*(MH-4)/hiMA);
            py = constrain(py, MY+1, MY+MH-2);
            uint32_t col = ma > avgMA*1.3f ? (uint32_t)COL_RED : (uint32_t)NIXIE_ORANGE;
            if (prevX2 >= 0) canvas.drawLine(prevX2, prevY2, px, py, col);
            canvas.drawPixel(px, py, col);
            prevX2 = px; prevY2 = py;
        }
    } else {
        canvas.setCursor(MLEFT+4, MY+MH/2-4);
        canvas.printf("%d/6 samples for mA graph", n);
    }

    canvas.setTextSize(1); canvas.setTextColor(NIXIE_DIM);
    canvas.setCursor(4, MY+MH+3);
    char modelBuf[8];
    snprintf(modelBuf, sizeof(modelBuf), modelMA < 10.0f ? "%.1f" : "%.0f", modelMA);
    canvas.printf("%dpts  mdl:%smA  F+hold=clear", s_battLogCount, modelBuf);
    drawHints("", "", "");
}

// ══════════════════════════════════════════════════════════════════════════════
// SCREEN: SETTINGS
// ══════════════════════════════════════════════════════════════════════════════
static void drawSettings() {
    auto dt = M5.Rtc.getDateTime();
    int hh = dt.time.hours, mm = dt.time.minutes;
    int dd = dt.date.date, mo = dt.date.month, yr = dt.date.year;

    extern SoundProfile g_soundProfile;

    const char* labels[SETTING_COUNT] = {
        "HOUR","MIN","DATE","MONTH","YEAR","POWER","BRIGHT","ROTATE","AOD","SOUND","NOTIF","WRIST","DND","AOD FACE"
    };
    const char* aodFaceNames[] = {"CLOCK","CLK+STP","CLK+ALM"};
    char vals[SETTING_COUNT][12];
    snprintf(vals[0],  12, "%02d", hh);
    snprintf(vals[1],  12, "%02d", mm);
    snprintf(vals[2],  12, "%02d", dd);
    snprintf(vals[3],  12, "%s", (mo >= 1 && mo <= 12) ? MON_NAMES[mo] : "???");
    snprintf(vals[4],  12, "%04d", yr);
    snprintf(vals[5],  12, "%s", PWR_NAMES[g_powerMode]);
    snprintf(vals[6],  12, "%d/5", g_brightLevel);
    snprintf(vals[7],  12, "%s", g_autoRotate ? "ON" : "OFF");
    snprintf(vals[8],  12, "%s", g_aodEnabled ? "ON" : "OFF");
    snprintf(vals[9],  12, "%s", g_soundProfile == SND_SILENT ? "SILENT" : "GENERAL");
    snprintf(vals[10], 12, "%s", notifTimeoutLabel(g_notifOverlayTimeoutIdx));
    snprintf(vals[11], 12, "%s", g_wristWakeEnabled ? "ON" : "OFF");
    snprintf(vals[12], 12, "%s", g_dndEnabled ? "ON" : "OFF");
    snprintf(vals[13], 12, "%s", aodFaceNames[constrain(g_aodFace, 0, 2)]);

    drawHeader("SETTINGS");

    // 3-column layout
    canvas.setTextSize(1); canvas.setTextColor(NIXIE_DIM);
    canvas.setCursor(4, 17);   canvas.print("TIME");
    canvas.setCursor(84, 17);  canvas.print("DATE");
    canvas.setCursor(164, 17); canvas.print("SYS");
    canvas.drawFastHLine(0, 25, DISP_W, (uint32_t)NIXIE_GHOST);
    canvas.drawFastVLine(80, 15, DISP_H-36, (uint32_t)NIXIE_GHOST);
    canvas.drawFastVLine(160, 15, DISP_H-36, (uint32_t)NIXIE_GHOST);

    const int colX[3]    = {4, 84, 164};
    const int rowH[3]    = {22, 22, 12};   // SYS col tighter: 8 items × 12px = 96px
    const int cw = 74;
    for (int i = 0; i < SETTING_COUNT; i++) {
        int col = (i < 3) ? 0 : (i < 6) ? 1 : 2;
        int row = (i < 3) ? i : (i < 6) ? i-3 : i-6;
        int x   = colX[col];
        int y   = 28 + row * rowH[col];
        bool active = (i == g_settingIdx);

        if (active) canvas.fillRoundRect(x-2, y-2, cw, 15, 3, (uint32_t)NIXIE_GHOST);
        canvas.setTextSize(1);
        canvas.setTextColor(active ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_DIM);
        canvas.setCursor(x, y); canvas.print(labels[i]);
        canvas.setTextColor(active ? (uint32_t)NIXIE_ORANGE : (uint32_t)TFT_WHITE);
        int vw = canvas.textWidth(vals[i]);
        canvas.setCursor(x + cw - vw - 2, y); canvas.print(vals[i]);
    }
    drawHints("next field", "+1 value", "-1 value");
}

// ══════════════════════════════════════════════════════════════════════════════
// AOD — minimal dim clock (3 faces: clock / clock+steps / clock+alarm)
// ══════════════════════════════════════════════════════════════════════════════
static void drawAOD() {
    auto dt = M5.Rtc.getDateTime();
    canvas.fillSprite(COL_BG);

    // Face 0 & 1 & 2 all share the centred clock
    canvas.setTextSize(4); canvas.setTextColor(0x2100);
    char hhmm[6]; snprintf(hhmm, 6, "%02d:%02d", dt.time.hours, dt.time.minutes);
    int tw = canvas.textWidth(hhmm);
    int clockY = (g_aodFace == 0) ? (DISP_H-32)/2 : 24;  // shift up if we need bottom row
    canvas.setCursor((DISP_W-tw)/2, clockY); canvas.print(hhmm);

    // Battery % always in bottom-right
    int b = dispMgr_getBatt();
    canvas.setTextSize(1);
    canvas.setTextColor(b > 20 ? (uint32_t)0x0300 : (uint32_t)0x4000);
    char bb[5]; snprintf(bb, 5, "%d%%", b);
    canvas.setCursor(DISP_W-24, DISP_H-10); canvas.print(bb);

    if (g_aodFace == 1) {
        // Clock + step count
        canvas.setTextSize(2); canvas.setTextColor(0x0840);
        char sbuf[8];
        if (g_stepCount < 10000) snprintf(sbuf, sizeof(sbuf), "%d", g_stepCount);
        else                     snprintf(sbuf, sizeof(sbuf), "%dk", g_stepCount/1000);
        tw = canvas.textWidth(sbuf);
        canvas.setCursor((DISP_W-tw)/2, 72); canvas.print(sbuf);
        canvas.setTextSize(1); canvas.setTextColor(0x0420);
        tw = canvas.textWidth("steps");
        canvas.setCursor((DISP_W-tw)/2, 92); canvas.print("steps");
    } else if (g_aodFace == 2) {
        // Clock + next enabled alarm time
        bool found = false;
        for (int i = 0; i < ALARM_MAX; i++) {
            if (g_alarms[i].enabled) {
                canvas.setTextSize(2); canvas.setTextColor(0x0840);
                char abuf[6]; snprintf(abuf, sizeof(abuf), "%02d:%02d", g_alarms[i].hour, g_alarms[i].minute);
                tw = canvas.textWidth(abuf);
                canvas.setCursor((DISP_W-tw)/2, 72); canvas.print(abuf);
                canvas.setTextSize(1); canvas.setTextColor(0x0420);
                tw = canvas.textWidth("alarm");
                canvas.setCursor((DISP_W-tw)/2, 92); canvas.print("alarm");
                found = true;
                break;
            }
        }
        if (!found) {
            canvas.setTextSize(1); canvas.setTextColor(0x0420);
            tw = canvas.textWidth("no alarm");
            canvas.setCursor((DISP_W-tw)/2, 82); canvas.print("no alarm");
        }
    }
}
