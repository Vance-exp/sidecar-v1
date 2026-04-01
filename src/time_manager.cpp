/*
 * SIDECAR V1 — Time Manager
 * RTC wrapper + 3-alarm engine with day-of-week bitmask and optional date filter.
 * Alarm speaker: 3-beep ascending pattern (1100/1400/1900Hz), 2s cycle, 60s auto-dismiss.
 */
#include <Arduino.h>
#include <M5Unified.h>
#include <EEPROM.h>
#include <sys/time.h>
#include "time_manager.h"
#include "hardware_config.h"

// ── State ─────────────────────────────────────────────────────────────────────
static int    s_alarmLastStep = -1;   // BUG 16: module-level so dismissAlarm() can reset it

AlarmCfg      g_alarms[ALARM_MAX] = {
    {7,  0, false, false, 0x7F, false, 1, 1},
    {12, 0, false, false, 0x7F, false, 1, 1},
    {18, 0, false, false, 0x7F, false, 1, 1}
};
bool          g_alarmRinging   = false;
unsigned long g_alarmRingAt    = 0;
int           g_alarmSel       = 0;
bool          g_alarmInEdit    = false;
int           g_alarmEditField = 0;
volatile bool g_alarmJustFired = false;

// ── Init ──────────────────────────────────────────────────────────────────────
void tmMgr_init() {
    tmMgr_loadAlarms();
}

// ── BLE time sync ─────────────────────────────────────────────────────────────
void tmMgr_onBleTime(long unix_ts) {
    if (unix_ts < 1000000000L) return;  // sanity: must be after ~2001

    // Sync ESP32 system clock
    struct timeval tv = { .tv_sec = unix_ts, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    // Convert unix_ts → IST (UTC+5:30) and write to hardware RTC
    struct tm t;
    time_t ts = (time_t)(unix_ts + 19800L);  // 5h30m = 19800s
    gmtime_r(&ts, &t);

    m5::rtc_datetime_t dt;
    dt.date.year   = t.tm_year + 1900;
    dt.date.month  = t.tm_mon  + 1;
    dt.date.date   = t.tm_mday;
    dt.time.hours  = t.tm_hour;
    dt.time.minutes = t.tm_min;
    dt.time.seconds = t.tm_sec;
    M5.Rtc.setDateTime(dt);

    Serial.printf("[TIME] BLE sync → %04d-%02d-%02d %02d:%02d:%02d IST\n",
        dt.date.year, dt.date.month, dt.date.date,
        dt.time.hours, dt.time.minutes, dt.time.seconds);
}

// ── Alarm check + speaker driver ──────────────────────────────────────────────
void tmMgr_update() {
    // Rate-limit RTC I2C reads — BM8563 on I2C, every read prevents light sleep.
    // Screen-on: 1Hz (every second is enough for alarm check + display)
    // Screen-off: 1Hz still (alarms need 1s resolution)
    // But don't call at all faster than 1Hz — saves ~10 unnecessary I2C transactions/sec.
    static unsigned long s_lastUpdate = 0;
    unsigned long now = millis();
    if (now - s_lastUpdate < 1000UL) return;
    s_lastUpdate = now;

    auto dt = M5.Rtc.getDateTime();

    // Guard: skip if RTC returns garbage (I2C failure at low CPU freq)
    if (dt.time.hours < 0 || dt.time.hours > 23 ||
        dt.time.minutes < 0 || dt.time.minutes > 59) return;

    int dow = calcDow(dt.date.year, dt.date.month, dt.date.date);

    for (int i = 0; i < ALARM_MAX; i++) {
        AlarmCfg& a = g_alarms[i];
        // BUG 15: only reset fired when the MINUTE changes, not when disabled.
        // Resetting fired on disable means re-enabling within the same minute re-fires.
        bool timeMatch = (dt.time.hours == a.hour && dt.time.minutes == a.minute);
        if (!timeMatch) { a.fired = false; }   // minute rolled over — arm for next time
        // Also reset fired when the alarm is disabled so that re-enabling it within
        // the same minute doesn't immediately fire again.
        if (!a.enabled) { a.fired = false; continue; }
        if (!((a.daysOfWeek >> dow) & 1)) continue;
        if (a.useDate) {
            if (dt.date.date != a.dateDay || dt.date.month != a.dateMonth) continue;
        }
        if (timeMatch && !a.fired) {
            a.fired          = true;
            g_alarmRinging   = true;
            g_alarmRingAt    = millis();
            g_alarmJustFired = true;
            M5.Speaker.setVolume(255);
            Serial.printf("[ALARM] fired #%d %02d:%02d\n", i, a.hour, a.minute);
        }
    }

    // Alarm speaker: 3-beep ascending, 2s cycle, 60s auto-dismiss
    if (g_alarmRinging) {
        if (millis() - g_alarmRingAt > 60000UL) {
            tmMgr_dismissAlarm();
            return;
        }
        // BUG 16: use module-level s_alarmLastStep so dismissAlarm() can reset it
        unsigned long pos = (millis() - g_alarmRingAt) % 2000UL;
        int step = -1;
        if      (pos < 250UL)  step = 0;
        else if (pos < 500UL)  step = 1;
        else if (pos < 750UL)  step = 2;
        if (step >= 0 && step != s_alarmLastStep) {
            s_alarmLastStep = step;
            M5.Speaker.setVolume(255);
            const int freqs[] = {1100, 1400, 1900};
            M5.Speaker.tone(freqs[step], 200);
        }
        if (pos >= 750UL) s_alarmLastStep = -1;
    }
}

// ── Dismiss ───────────────────────────────────────────────────────────────────
void tmMgr_dismissAlarm() {
    g_alarmRinging  = false;
    s_alarmLastStep = -1;   // BUG 16: reset so next alarm starts from step 0
    M5.Speaker.stop();
}

// ── EEPROM persistence ────────────────────────────────────────────────────────
void tmMgr_saveAlarms() {
    for (int i = 0; i < ALARM_MAX; i++) {
        int b = EE_ALARM_BASE + i * EE_ALARM_BYTES;
        EEPROM.write(b,   g_alarms[i].hour);
        EEPROM.write(b+1, g_alarms[i].minute);
        EEPROM.write(b+2, g_alarms[i].enabled    ? 1 : 0);
        EEPROM.write(b+3, g_alarms[i].daysOfWeek);
        EEPROM.write(b+4, g_alarms[i].useDate    ? 1 : 0);
        EEPROM.write(b+5, g_alarms[i].dateDay);
        EEPROM.write(b+6, g_alarms[i].dateMonth);
    }
    EEPROM.commit();
}

void tmMgr_loadAlarms() {
    for (int i = 0; i < ALARM_MAX; i++) {
        int b = EE_ALARM_BASE + i * EE_ALARM_BYTES;
        g_alarms[i].hour       = EEPROM.read(b);
        g_alarms[i].minute     = EEPROM.read(b+1);
        g_alarms[i].enabled    = EEPROM.read(b+2) != 0;
        g_alarms[i].daysOfWeek = EEPROM.read(b+3);
        g_alarms[i].useDate    = EEPROM.read(b+4) != 0;
        g_alarms[i].dateDay    = EEPROM.read(b+5);
        g_alarms[i].dateMonth  = EEPROM.read(b+6);
        g_alarms[i].fired      = false;

        // Clamp invalid EEPROM values
        if (g_alarms[i].hour > 23)        g_alarms[i].hour       = 7;
        if (g_alarms[i].minute > 59)      g_alarms[i].minute     = 0;
        if (g_alarms[i].daysOfWeek == 0)  g_alarms[i].daysOfWeek = 0x7F;
        if (g_alarms[i].dateDay < 1 || g_alarms[i].dateDay > 31)   g_alarms[i].dateDay   = 1;
        if (g_alarms[i].dateMonth < 1 || g_alarms[i].dateMonth > 12) g_alarms[i].dateMonth = 1;
    }
}
