/*
 * SIDECAR V1 — Main
 * BLE UUIDs from Bellafaire/ESP32-Smart-Watch
 * Copyright (c) 2020-2021 Matthew James Bellafaire — MIT License
 *
 * BtnA  (FRONT)  short = primary action    long = go home (clock)
 * BtnB  (SIDE)   short = next screen       long = prev screen
 * BtnPWR         short = wake / dismiss     long = power cycle
 *
 * CRITICAL: wasPressed() is a one-shot latch that clears after first read.
 * All three button states are captured into local bools ONCE at the top of
 * handleButtons(), then every downstream check uses the locals.
 */
#include <Arduino.h>
#include <M5Unified.h>
#include <EEPROM.h>
#include <driver/gpio.h>
#include "hardware_config.h"
#include "power_manager.h"
#include "ble_manager.h"
#include "notif_queue.h"
#include "time_manager.h"
#include "step_counter.h"
#include "display_mgr.h"
#include "cfg_manager.h"
#include "sd_logger.h"

// ── Sound ────────────────────────────────────────────────────────────────────
SoundProfile g_soundProfile = SND_GENERAL;

static void uiClick(int freq = 1800, int dur = 18) {
    if (g_soundProfile == SND_SILENT) return;
    M5.Speaker.setVolume(80);
    M5.Speaker.tone(freq, dur);
}
static void uiTick() { uiClick(1400, 10); }

// ── Long-press tracking ──────────────────────────────────────────────────────
static unsigned long s_btnAAt  = 0, s_btnBAt  = 0, s_btnPAt  = 0;
static bool          s_btnALong = false, s_btnBLong = false, s_btnPLong = false;

// ── IMU recording mode ───────────────────────────────────────────────────────
// Hold BtnA+BtnB together for 2s to toggle. While active:
//   - streams CSV to serial: ts_ms,ax,ay,az,event
//   - BtnA press stamps "RAISE", BtnB press stamps "REST"
//   - normal watch functions suspended
static bool          s_imuRecording  = false;
static unsigned long s_bothHeldSince = 0;

// ── Forward declarations ─────────────────────────────────────────────────────
static void handleButtons();
static void handleBtnA_short();
static void handleBtnA_long();
static void handleBtnPWR_short();
static void handleBtnPWR_long();
static void incrementSetting(int idx, int dir);

// ═════════════════════════════════════════════════════════════════════════════
// SETUP
// ═════════════════════════════════════════════════════════════════════════════
void setup() {
    // ── GPIO4 power rail — FIRST thing, before anything touches I2C ──────
    // ORDER MATTERS: set HIGH first, THEN lock with gpio_hold_en.
    // gpio_hold_en() freezes the current pin state. Calling it before
    // digitalWrite(HIGH) locks GPIO4 LOW, killing the AXP2101 power rail
    // the moment USB is removed.
    pinMode(PIN_PWR_RAIL, OUTPUT);
    digitalWrite(PIN_PWR_RAIL, HIGH);
    gpio_hold_en(GPIO_NUM_4);

    // ── IR LED off to save power ─────────────────────────────────────────
    pinMode(PIN_IR_TX, OUTPUT);
    digitalWrite(PIN_IR_TX, LOW);

    // ── M5Unified ────────────────────────────────────────────────────────
    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    M5.begin(cfg);
    Serial.println("\n[SIDECAR V1] boot");

    // ── EEPROM ───────────────────────────────────────────────────────────
    EEPROM.begin(EE_SIZE);

    // ── Power manager (sets CPU freq, conditionally inits BLE) ───────────
    pwrMgr_init();

    // ── Display ──────────────────────────────────────────────────────────
    dispMgr_init();
    dispMgr_showSplash("SIDECAR V1", "ALIVE", 1200);

    // ── Load settings from EEPROM ────────────────────────────────────────
    uint8_t br = EEPROM.read(EE_BRIGHT);
    if (br <= 5) g_brightLevel = br; else g_brightLevel = 3;
    // In DEEPSLEEP mode, cap brightness to the mode's configured level (1).
    // EE_BRIGHT persists from a brighter mode and would otherwise override it.
    if (g_powerMode == PWR_DEEPSLEEP && g_brightLevel > g_pwrCfg[PWR_DEEPSLEEP].brightness) {
        g_brightLevel = g_pwrCfg[PWR_DEEPSLEEP].brightness;
    }
    dispMgr_setBrightness(g_brightLevel);

    uint8_t ar = EEPROM.read(EE_AUTOROT);
    g_autoRotate = (ar == 1);

    uint8_t sp = EEPROM.read(EE_SOUND);
    g_soundProfile = (sp <= 1) ? (SoundProfile)sp : SND_GENERAL;

    uint8_t aod = EEPROM.read(EE_AOD);
    g_aodEnabled = (aod == 1);

    uint8_t nt = EEPROM.read(EE_NOTIF_TIMEOUT);
    if (nt < NOTIF_TIMEOUT_COUNT) g_notifOverlayTimeoutIdx = nt;
    else g_notifOverlayTimeoutIdx = 1;  // default 10s

    uint8_t ww = EEPROM.read(EE_WRIST_WAKE);
    g_wristWakeEnabled = (ww != 0);  // default ON (0xFF unwritten = treat as ON)

    uint8_t dnd = EEPROM.read(EE_DND);
    g_dndEnabled = (dnd == 1);

    uint8_t aodFace = EEPROM.read(EE_AOD_FACE);
    g_aodFace = (aodFace < 3) ? aodFace : 0;

    // ── Subsystems ───────────────────────────────────────────────────────
    tmMgr_init();
    stepCtr_init();
    sdLog_init();  // graceful no-op if no SD card present

    g_lastActivity = millis();
    Serial.printf("[SIDECAR V1] ready  mode=%s  heap=%d\n",
        PWR_NAMES[g_powerMode], ESP.getFreeHeap());
}

// ═════════════════════════════════════════════════════════════════════════════
// LOOP
// ═════════════════════════════════════════════════════════════════════════════
void loop() {
    M5.update();

    // ── IMU recording mode (hidden: hold BtnA+BtnB 2s to toggle) ────────────
    {
        bool aDown = M5.BtnA.isPressed();
        bool bDown = M5.BtnB.isPressed();
        if (aDown && bDown) {
            if (s_bothHeldSince == 0) s_bothHeldSince = millis();
            if (!s_imuRecording && (millis() - s_bothHeldSince) > 2000UL) {
                s_imuRecording  = true;
                s_bothHeldSince = 0;
                dispMgr_wakeScreen();
                Serial.println("# IMU_RECORD_START ts_ms,ax,ay,az,gx,gy,gz,event");
                M5.Display.fillScreen(TFT_BLACK);
                M5.Display.setTextSize(2);
                M5.Display.setTextColor(TFT_GREEN);
                M5.Display.setCursor(4, 4);
                M5.Display.println("IMU REC");
                M5.Display.setTextSize(1);
                M5.Display.println("A=RAISE  B=REST");
                M5.Display.println("A+B hold=stop");
            }
        } else {
            s_bothHeldSince = 0;
        }

        if (s_imuRecording) {
            // Check stop: both held again
            if (aDown && bDown) {
                if (s_bothHeldSince == 0) s_bothHeldSince = millis();
                if ((millis() - s_bothHeldSince) > 2000UL) {
                    s_imuRecording  = false;
                    s_bothHeldSince = 0;
                    Serial.println("# IMU_RECORD_STOP");
                    dispMgr_markDirty();
                    return;
                }
            }

            // Label buttons (wasPressed already consumed at top, use isPressed edge)
            static bool s_aPrev = false, s_bPrev = false;
            const char* event = "";
            if (!aDown && s_aPrev) event = "RAISE";   // released = intentional tap
            if (!bDown && s_bPrev) event = "REST";
            s_aPrev = aDown; s_bPrev = bDown;

            // Stream accel + gyro at 50Hz
            static unsigned long s_lastImuOut = 0;
            if (millis() - s_lastImuOut >= 20UL) {
                s_lastImuOut = millis();
                float ax, ay, az, gx, gy, gz;
                M5.Imu.getAccel(&ax, &ay, &az);
                M5.Imu.getGyro(&gx, &gy, &gz);
                Serial.printf("%lu,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%s\n",
                    millis(), ax, ay, az, gx, gy, gz, event);
            }
            return;  // suspend normal watch while recording
        }
    }

    handleButtons();
    cfg_processAll();
    cfg_pushBatteryIfDue();
    sdLog_update();

    // ── Subsystem ticks ──────────────────────────────────────────────────
    tmMgr_update();
    // In DEEPSLEEP with screen off: skip IMU step counting — the 200ms loop
    // delay already reduces wakeups; no need to spin the I2C bus for steps.
    // Steps resume the moment the screen is woken by a button press.
    if (!(g_powerMode == PWR_DEEPSLEEP && !g_screenOn)) {
        stepCtr_update();
    }

    // ── Notification overlay auto-dismiss ───────────────────────────────
    if (g_notifOverlay) {
        unsigned long ms = notifTimeoutMs(g_notifOverlayTimeoutIdx);
        if (ms > 0 && (millis() - g_notifOverlayShownAt) >= ms) {
            g_notifOverlay = false;
            g_curScreen    = g_notifPrevScreen;
            dispMgr_markDirty();
        } else {
            dispMgr_markDirty();  // redraw each frame so progress bar updates
        }
    }

    // ── Find My Phone: send FP| packet when flag is set by button handler ────
    if (g_findPhoneReq && g_bleConn) {
        g_findPhoneReq = false;
        bleMgr_notify("FP|");
        uiClick(1800, 30);
    } else if (g_findPhoneReq) {
        g_findPhoneReq = false;  // clear even if not connected
    }

    // ── Predictive auto mode: NORMAL → EFFICIENT after 2h idle (no BLE, no steps) ──
    // Writes EEPROM once per transition — restores manually via power screen.
    {
        static unsigned long s_lastStepChangeAt = 0;
        static int           s_lastAutoSteps    = -1;
        if (s_lastAutoSteps < 0) s_lastAutoSteps = g_stepCount;
        if (g_stepCount != s_lastAutoSteps) {
            s_lastAutoSteps    = g_stepCount;
            s_lastStepChangeAt = millis();
        }
        const unsigned long IDLE_2H = 2UL * 3600000UL;
        if (g_powerMode == PWR_NORMAL && !g_bleConn) {
            bool stepsIdle = (s_lastStepChangeAt == 0) || (millis() - s_lastStepChangeAt > IDLE_2H);
            bool btnIdle   = (millis() - g_lastActivity > IDLE_2H);
            if (stepsIdle && btnIdle) {
                Serial.println("[AUTO] 2h idle, no BLE -> EFFICIENT");
                pwrMgr_apply(PWR_EFFICIENT);
            }
        }
    }

    // ── BLE notification flag (set by NimBLE task on core 0) ─────────────
    if (g_nqNewNotif) {
        g_nqNewNotif = false;
        dispMgr_wakeScreen();
        if (!g_dndEnabled && g_soundProfile == SND_GENERAL) {
            M5.Speaker.setVolume(160);
            M5.Speaker.tone(2200, 80);
        }
        // Full-screen takeover: save current screen, show newest notif
        g_notifPrevScreen       = g_curScreen;
        g_notifOverlayIdx       = g_bannerNotifIdx;
        g_notifOverlay          = true;
        g_notifOverlayShownAt   = millis();
        dispMgr_markDirty();
    }

    // ── Alarm just-fired flag (set by tmMgr_update) ──────────────────────
    if (g_alarmJustFired) {
        g_alarmJustFired = false;
        dispMgr_wakeScreen();
        g_curScreen = SCR_ALARM;
        dispMgr_markDirty();
    }

    // ── Timer done check ─────────────────────────────────────────────────
    if (tmr_state == TMR_RUNNING && millis() >= tmr_endMs) {
        tmr_state = TMR_DONE;
        tmr_remaining = 0;
        dispMgr_wakeScreen();
        g_curScreen = SCR_TIMER;
        if (!g_dndEnabled && g_soundProfile == SND_GENERAL) {
            M5.Speaker.setVolume(200);
            M5.Speaker.tone(2400, 500);
        }
        dispMgr_markDirty();

        // Pomodoro auto-cycle: after 1.5s "DONE!" flash, auto-start next session
        if (g_timerMode == TMR_MODE_POMODORO) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            // Toggle between 25min work and 5min break
            tmr_setMin = (tmr_setMin == 25) ? 5 : 25;
            tmr_endMs  = millis() + (unsigned long)tmr_setMin * 60000UL;
            tmr_state  = TMR_RUNNING;
            if (!g_dndEnabled && g_soundProfile == SND_GENERAL) {
                M5.Speaker.tone(1800, 200);
            }
            dispMgr_markDirty();
        }
    }

    // ── Auto-rotate ──────────────────────────────────────────────────────
    if (g_autoRotate && g_screenOn) dispMgr_checkRotation();

    // ── Wrist-raise wake ─────────────────────────────────────────────────
    // Only poll IMU when feature is enabled AND screen is off (no point checking
    // wrist raise when the screen is already on).
    if (g_wristWakeEnabled && !g_screenOn) dispMgr_checkWristWake();

    // ── Display update (handles dirty flag, sleep/wake, AOD) ─────────────
    dispMgr_update();
}

// ═════════════════════════════════════════════════════════════════════════════
// BUTTON HANDLER
// ═════════════════════════════════════════════════════════════════════════════
static void handleButtons() {
    // ── CRITICAL FIX: capture all wasPressed() into locals ONCE ──────────
    // wasPressed() is a latch — reading it clears it.  If we read it in
    // multiple if-branches, only the first branch sees the event.
    bool aPrs = M5.BtnA.wasPressed();
    bool bPrs = M5.BtnB.wasPressed();
    bool pPrs = M5.BtnPWR.wasPressed();

    bool aHeld = M5.BtnA.isPressed();
    bool bHeld = M5.BtnB.isPressed();
    bool pHeld = M5.BtnPWR.isPressed();

    // ── Record press timestamps ──────────────────────────────────────────
    if (aPrs) { s_btnAAt = millis(); s_btnALong = false; g_lastActivity = millis(); uiClick(1800, 15); }
    if (bPrs) { s_btnBAt = millis(); s_btnBLong = false; g_lastActivity = millis(); uiClick(1600, 15); }
    if (pPrs) { s_btnPAt = millis(); s_btnPLong = false; g_lastActivity = millis(); uiClick(1400, 15); }

    // ── Wake from sleep — any press wakes, consumes the event ────────────
    if (!g_screenOn) {
        if (aPrs || bPrs || pPrs) {
            dispMgr_wakeScreen();
            // Clear timestamps so the held button doesn't trigger long-press
            // or short-press navigation on the next loop iteration.
            s_btnAAt = 0; s_btnBAt = 0; s_btnPAt = 0;
        }
        return;  // don't process further on the wake press
    }

    // ── Dismiss alarm on any press ───────────────────────────────────────
    if (g_alarmRinging && (aPrs || bPrs || pPrs)) {
        tmMgr_dismissAlarm();
        dispMgr_markDirty();
        return;
    }

    // ── Full-screen notification overlay button handling ──────────────────
    if (g_notifOverlay) {
        int idx = g_notifOverlayIdx;
        // BtnA long = delete notification
        if (aHeld && !s_btnALong && s_btnAAt > 0 && (millis() - s_btnAAt) > LONG_PRESS_MS) {
            s_btnALong = true;
            uiClick(1000, 30);
            if (idx >= 0 && idx < g_notifCount) nq_dismiss(idx);
            g_notifOverlay = false;
            g_curScreen    = g_notifPrevScreen;
            dispMgr_markDirty();
            return;
        }
        // BtnA short = mark read + close overlay
        if (aPrs) {
            if (idx >= 0 && idx < g_notifCount) {
                if (g_notifs[idx].unread && g_unreadCount > 0) g_unreadCount--;
                g_notifs[idx].unread = false;
            }
            g_notifOverlay = false;
            g_curScreen    = g_notifPrevScreen;
            dispMgr_markDirty();
            return;
        }
        // BtnPWR or BtnB = close overlay, keep unread
        if (pPrs || bPrs) {
            g_notifOverlay = false;
            g_curScreen    = g_notifPrevScreen;
            dispMgr_markDirty();
            return;
        }
        return;  // consume all other events while overlay is up
    }

    // ── Dismiss notification banner on any press ─────────────────────────
    if (g_bannerActive && (aPrs || bPrs || pPrs)) {
        g_bannerActive = false;
        dispMgr_markDirty();
        // fall through — BtnB/BtnPWR might also navigate
    }

    // ── Long-press detection (BtnA) ──────────────────────────────────────
    if (aHeld && !s_btnALong && s_btnAAt > 0 && (millis() - s_btnAAt) > LONG_PRESS_MS) {
        s_btnALong = true;
        uiClick(1200, 30);
        handleBtnA_long();
        dispMgr_markDirty();
    }

    // ── Long-press detection (BtnB) ──────────────────────────────────────
    if (bHeld && !s_btnBLong && s_btnBAt > 0 && (millis() - s_btnBAt) > LONG_PRESS_MS) {
        s_btnBLong = true;
        uiTick();
        // Save alarm edits if navigating away mid-edit (BUG 5)
        if (g_alarmInEdit) { g_alarmInEdit = false; tmMgr_saveAlarms(); cfg_pushChange(CFG_ALARM); }
        g_pwrInCustom = false;  // reset power custom edit (BUG 7)
        int s = (int)g_curScreen - 1;
        if (s < 0) s = SCR_COUNT - 1;
        g_curScreen = (Screen)s;
        dispMgr_markDirty();
    }

    // ── Long-press detection (BtnPWR) ─────────────────────────────────────
    if (pHeld && !s_btnPLong && s_btnPAt > 0 && (millis() - s_btnPAt) > LONG_PRESS_MS) {
        s_btnPLong = true;
        uiClick(1000, 30);
        handleBtnPWR_long();
        dispMgr_markDirty();
    }

    // ── Short press: BtnA (released without long) ────────────────────────
    if (!aHeld && s_btnAAt > 0 && !s_btnALong) {
        s_btnAAt = 0;
        handleBtnA_short();
    }
    if (!aHeld) s_btnAAt = 0;

    // ── Short press: BtnB → next screen ──────────────────────────────────
    if (!bHeld && s_btnBAt > 0 && !s_btnBLong) {
        s_btnBAt = 0;
        // Save alarm edits if navigating away mid-edit (BUG 5)
        if (g_alarmInEdit) { g_alarmInEdit = false; tmMgr_saveAlarms(); cfg_pushChange(CFG_ALARM); }
        g_pwrInCustom = false;  // reset power custom edit (BUG 7)
        int s = (int)g_curScreen + 1;
        if (s >= SCR_COUNT) s = 0;
        g_curScreen = (Screen)s;
        dispMgr_markDirty();
    }
    if (!bHeld) s_btnBAt = 0;

    // ── BtnPWR short press (only if long didn't fire) ─────────────────────
    if (!pHeld && s_btnPAt > 0 && !s_btnPLong) {
        s_btnPAt = 0;
        handleBtnPWR_short();
    }
    if (!pHeld) s_btnPAt = 0;
}

// ═════════════════════════════════════════════════════════════════════════════
// BtnA SHORT — per-screen primary action
// ═════════════════════════════════════════════════════════════════════════════
static void handleBtnA_short() {
    g_lastActivity = millis();
    dispMgr_markDirty();

    switch (g_curScreen) {
    case SCR_CLOCK:
        // no action — clock face
        break;

    case SCR_STOPWATCH:
        // Front button: start / stop toggle
        if (!sw_running) {
            sw_start    = millis() - sw_elapsed;
            sw_lapStart = millis();
            sw_running  = true;
        } else {
            sw_elapsed = millis() - sw_start;
            sw_running = false;
        }
        break;

    case SCR_TIMER:
        switch (tmr_state) {
        case TMR_IDLE:
            if (g_timerMode == TMR_MODE_BREATHE) {
                // For breathing, tmr_endMs holds the session start reference for phase calc
                tmr_endMs = millis() + 12000UL;  // first cycle ref (12s total)
                tmr_state = TMR_RUNNING;
            } else {
                // Normal / Pomodoro
                if (g_timerMode == TMR_MODE_POMODORO && tmr_setMin != 5) tmr_setMin = 25;
                tmr_endMs = millis() + (unsigned long)tmr_setMin * 60000UL;
                tmr_state = TMR_RUNNING;
            }
            break;
        case TMR_RUNNING:
            // Pause
            tmr_remaining = tmr_endMs - millis();
            tmr_state = TMR_PAUSED;
            break;
        case TMR_PAUSED:
            // Resume
            tmr_endMs = millis() + tmr_remaining;
            tmr_state = TMR_RUNNING;
            break;
        case TMR_DONE:
            // Reset
            tmr_state = TMR_IDLE;
            M5.Speaker.stop();
            break;
        }
        break;

    case SCR_ALARM:
        if (!g_alarmInEdit) {
            // Toggle enable
            g_alarms[g_alarmSel].enabled = !g_alarms[g_alarmSel].enabled;
            tmMgr_saveAlarms();
            cfg_pushChange(CFG_ALARM);
        } else {
            // Increment current alarm edit field
            AlarmCfg& a = g_alarms[g_alarmSel];
            int f = g_alarmEditField;
            if      (f == 0) a.hour = (a.hour + 1) % 24;
            else if (f == 1) a.minute = (a.minute + 1) % 60;
            else if (f >= 2 && f <= 8) {
                // Toggle day-of-week bit (f-2 = 0..6 = Sun..Sat)
                a.daysOfWeek ^= (1 << (f - 2));
            }
            else if (f == 9)  a.useDate = !a.useDate;
            else if (f == 10) a.dateDay = (a.dateDay % 31) + 1;
            else if (f == 11) a.dateMonth = (a.dateMonth % 12) + 1;
        }
        break;

    case SCR_NOTIFS:
        if (g_notifCount > 0) {
            nq_dismiss(g_notifIdx);
        }
        break;

    case SCR_MEDIA: {
        // Double-tap within 400ms = PREV; single tap = PLAY/PAUSE
        static unsigned long s_lastMediaTap = 0;
        unsigned long now = millis();
        if (now - s_lastMediaTap < 400UL) {
            // Second tap — send PREV
            bleMgr_notify("MC|PREV");
            uiTick();
            s_lastMediaTap = 0;  // reset so triple-tap doesn't re-trigger
        } else {
            // First tap — send PLAY, record time
            bleMgr_notify("MC|PLAY");
            uiClick(1800, 20);
            s_lastMediaTap = now;
        }
        break;
    }

    case SCR_POWER:
        if (!g_pwrInCustom) {
            // Front short = apply highlighted mode
            pwrMgr_apply((PowerMode)g_pwrTabSel);
            cfg_pushChange(CFG_POWER);
        } else {
            // In custom edit: front short = increment field value
            PwrCfg& pc = g_pwrCfg[g_pwrTabSel];
            if (g_pwrCustIdx == 0) {
                pc.brightness = (pc.brightness + 1) % 6;
            } else {
                pc.timeoutSec += 5;
                if (pc.timeoutSec > 120) pc.timeoutSec = 0;
            }
        }
        break;

    case SCR_DIAG:
        // Reset step counter
        stepCtr_reset();
        break;

    case SCR_SETTINGS:
        // Front short = increment current field value
        incrementSetting(g_settingIdx, 1);
        break;

    default: break;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// BtnA LONG — per-screen long-press action
// ═════════════════════════════════════════════════════════════════════════════
static void handleBtnA_long() {
    switch (g_curScreen) {
    case SCR_STOPWATCH:
        // Long A = reset
        sw_running  = false;
        sw_elapsed  = 0;
        sw_lapCount = 0;
        sw_lapStart = 0;
        break;

    case SCR_TIMER:
        if (tmr_state == TMR_IDLE) {
            // Long A on idle timer = cycle mode: NORMAL → POMODORO → BREATHE → NORMAL
            g_timerMode = (TimerMode)(((int)g_timerMode + 1) % 3);
            // Reset setMin to sensible defaults for the new mode
            if (g_timerMode == TMR_MODE_POMODORO) tmr_setMin = 25;
            else if (g_timerMode == TMR_MODE_NORMAL) tmr_setMin = 5;
            uiTick();
        } else {
            // Long A while running/paused = cancel and reset
            tmr_state = TMR_IDLE;
            M5.Speaker.stop();
        }
        break;

    case SCR_ALARM:
        // Long A = toggle edit mode
        if (!g_alarmInEdit) {
            g_alarmInEdit    = true;
            g_alarmEditField = 0;
        } else {
            g_alarmInEdit = false;
            tmMgr_saveAlarms();
            cfg_pushChange(CFG_ALARM);
        }
        break;

    case SCR_MEDIA:
        // BtnA long = next track
        bleMgr_notify("MC|NEXT");
        uiTick();
        break;

    case SCR_SETTINGS:
        // Front long = decrement current field value
        incrementSetting(g_settingIdx, -1);
        break;

    case SCR_POWER:
        if (!g_pwrInCustom) {
            // Front long = enter customization for highlighted mode
            g_pwrInCustom = true;
            g_pwrCustIdx  = 0;
        } else {
            // Front long = exit customization
            g_pwrInCustom = false;
        }
        break;

    case SCR_NOTIFS:
        // Long A on notifications = Find My Phone
        g_findPhoneReq = true;
        uiClick(2000, 40);
        break;

    default:
        // All other screens: long A = go home
        g_curScreen   = SCR_CLOCK;
        g_alarmInEdit = false;
        break;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// BtnPWR SHORT — secondary action per screen
// ═════════════════════════════════════════════════════════════════════════════
static void handleBtnPWR_short() {
    g_lastActivity = millis();
    dispMgr_markDirty();

    switch (g_curScreen) {
    case SCR_CLOCK:
        // Top button on clock face: cycle power mode
        {
            int pm = ((int)g_powerMode + 1) % PWR_COUNT;
            pwrMgr_apply((PowerMode)pm);
            cfg_pushChange(CFG_POWER);
        }
        break;

    case SCR_STOPWATCH:
        // Top button: lap (only while running)
        if (sw_running && sw_lapCount < SW_LAPS_MAX) {
            sw_laps[sw_lapCount++] = millis() - sw_lapStart;
            sw_lapStart = millis();
        }
        break;

    case SCR_TIMER:
        if (tmr_state == TMR_IDLE) {
            // +1 minute (wraps 99→1)
            tmr_setMin += 1;
            if (tmr_setMin > 99) tmr_setMin = 1;
        } else {
            // Running, paused, or done → cancel and reset
            tmr_state = TMR_IDLE;
            M5.Speaker.stop();
        }
        break;

    case SCR_ALARM:
        if (!g_alarmInEdit) {
            // Top button (not editing) = cycle to next alarm
            g_alarmSel = (g_alarmSel + 1) % ALARM_MAX;
        } else {
            // Top button (editing) = advance to next field, save on exit
            g_alarmEditField++;
            if (g_alarmEditField >= ALARM_EDIT_FIELDS) {
                g_alarmInEdit = false;
                tmMgr_saveAlarms();
                cfg_pushChange(CFG_ALARM);
            }
        }
        break;

    case SCR_NOTIFS:
        // Scroll to next notification
        if (g_notifCount > 0) {
            g_notifIdx = (g_notifIdx + 1) % g_notifCount;
            g_notifs[g_notifIdx].unread = false;
        }
        break;

    case SCR_MEDIA:
        // BtnPWR short = volume up
        bleMgr_notify("MC|VOLU");
        uiTick();
        break;

    case SCR_SETTINGS:
        // Top short = advance to next setting field
        g_settingIdx = (g_settingIdx + 1) % SETTING_COUNT;
        break;

    case SCR_POWER:
        if (!g_pwrInCustom) {
            // Top short = cycle highlighted tab
            g_pwrTabSel = (g_pwrTabSel + 1) % PWR_COUNT;
        } else {
            // In custom edit: advance to next field, exit on last
            g_pwrCustIdx++;
            if (g_pwrCustIdx >= PWRCFG_FIELDS) {
                g_pwrInCustom = false;
                cfg_pushChange(CFG_PWRCFG);  // push all 3 mode configs to app
            }
        }
        break;

    default: break;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// BtnPWR LONG — per-screen long-press action
// ═════════════════════════════════════════════════════════════════════════════
static void handleBtnPWR_long() {
    switch (g_curScreen) {
    case SCR_TIMER:
        if (tmr_state == TMR_IDLE && g_timerMode == TMR_MODE_NORMAL) {
            // Top long (idle, normal mode) = -1 minute
            tmr_setMin -= 1;
            if (tmr_setMin < 1) tmr_setMin = 99;
        }
        break;

    case SCR_MEDIA:
        // BtnPWR long = volume down
        bleMgr_notify("MC|VOLD");
        uiTick();
        break;

    default:
        // All other screens: long PWR = jump to settings
        if (g_alarmInEdit) { g_alarmInEdit = false; tmMgr_saveAlarms(); cfg_pushChange(CFG_ALARM); }
        g_pwrInCustom = false;
        g_curScreen   = SCR_SETTINGS;
        dispMgr_markDirty();
        break;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// INCREMENT SETTING
// ═════════════════════════════════════════════════════════════════════════════
static void incrementSetting(int idx, int dir) {
    auto dt = M5.Rtc.getDateTime();
    int y = dt.date.year, mo = dt.date.month, d = dt.date.date;
    int h = dt.time.hours, mi = dt.time.minutes;

    switch (idx) {
    case 0: // Hour
        h = (h + dir + 24) % 24;
        break;
    case 1: // Minute
        mi = (mi + dir + 60) % 60;
        break;
    case 2: // Date day
        d = ((d - 1 + dir + daysInMonth(mo, y)) % daysInMonth(mo, y)) + 1;
        break;
    case 3: // Month
        mo = ((mo - 1 + dir + 12) % 12) + 1;
        break;
    case 4: // Year
        y += dir;
        if (y < 2024) y = 2024;
        if (y > 2099) y = 2099;
        break;
    case 5: { // Power mode cycle
        int pm = (int)g_powerMode + dir;
        pm = (pm + PWR_COUNT) % PWR_COUNT;
        pwrMgr_apply((PowerMode)pm);
        cfg_pushChange(CFG_POWER);
        return;
    }
    case 6: // Brightness
        g_brightLevel = (g_brightLevel + dir + 6) % 6;
        dispMgr_setBrightness(g_brightLevel);
        EEPROM.write(EE_BRIGHT, g_brightLevel);
        EEPROM.commit();
        cfg_pushChange(CFG_BRIGHT);
        return;
    case 7: // Auto-rotate
        g_autoRotate = !g_autoRotate;
        EEPROM.write(EE_AUTOROT, g_autoRotate ? 1 : 0);
        EEPROM.commit();
        cfg_pushChange(CFG_AUTOROT);
        return;
    case 8: // AOD
        g_aodEnabled = !g_aodEnabled;
        EEPROM.write(EE_AOD, g_aodEnabled ? 1 : 0);
        EEPROM.commit();
        cfg_pushChange(CFG_AOD);
        return;
    case 9: // Sound
        g_soundProfile = (g_soundProfile == SND_SILENT) ? SND_GENERAL : SND_SILENT;
        EEPROM.write(EE_SOUND, (uint8_t)g_soundProfile);
        EEPROM.commit();
        if (g_soundProfile == SND_GENERAL) uiClick(1800, 30);
        cfg_pushChange(CFG_SOUND);
        return;
    case 10: // Notif overlay timeout
        g_notifOverlayTimeoutIdx = (g_notifOverlayTimeoutIdx + dir + NOTIF_TIMEOUT_COUNT) % NOTIF_TIMEOUT_COUNT;
        EEPROM.write(EE_NOTIF_TIMEOUT, (uint8_t)g_notifOverlayTimeoutIdx);
        EEPROM.commit();
        cfg_pushChange(CFG_NOTIFTMO);
        return;
    case 11: // Wrist wake
        g_wristWakeEnabled = !g_wristWakeEnabled;
        EEPROM.write(EE_WRIST_WAKE, g_wristWakeEnabled ? 1 : 0);
        EEPROM.commit();
        return;
    case 12: // DND
        g_dndEnabled = !g_dndEnabled;
        EEPROM.write(EE_DND, g_dndEnabled ? 1 : 0);
        EEPROM.commit();
        cfg_pushChange(CFG_DND);
        return;
    case 13: // AOD face
        g_aodFace = (g_aodFace + (dir > 0 ? 1 : 2)) % 3;  // wrap 0-2 in both directions
        EEPROM.write(EE_AOD_FACE, (uint8_t)g_aodFace);
        EEPROM.commit();
        return;
    default:
        return;
    }

    // Apply date/time change to RTC
    m5::rtc_datetime_t ndt;
    ndt.date.year  = y;
    ndt.date.month = mo;
    ndt.date.date  = d;
    ndt.time.hours   = h;
    ndt.time.minutes = mi;
    ndt.time.seconds = 0;
    M5.Rtc.setDateTime(ndt);
}
