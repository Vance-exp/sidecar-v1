#include <Arduino.h>
#include <M5Unified.h>
#include <EEPROM.h>
#include "cfg_manager.h"
#include "hardware_config.h"
#include "display_mgr.h"
#include "power_manager.h"
#include "notif_queue.h"
#include "ble_manager.h"
#include "time_manager.h"

extern SoundProfile g_soundProfile;  // defined in main.cpp
extern PwrCfg       g_pwrCfg[];      // defined in power_manager.cpp
extern PowerMode    g_powerMode;     // defined in power_manager.cpp
extern bool         g_dndEnabled;    // defined in display_mgr.cpp

// ── Existing simple config flags ─────────────────────────────────────────────
static volatile bool    s_pendBright   = false; static volatile uint8_t s_valBright   = 3;
static volatile bool    s_pendPower    = false; static volatile uint8_t s_valPower    = 0;
static volatile bool    s_pendSound    = false; static volatile uint8_t s_valSound    = 1;
static volatile bool    s_pendAod      = false; static volatile uint8_t s_valAod      = 0;
static volatile bool    s_pendAutorot  = false; static volatile uint8_t s_valAutorot  = 1;
static volatile bool    s_pendNotifTmo = false; static volatile uint8_t s_valNotifTmo = 1;

// ── New config flags ──────────────────────────────────────────────────────────
static volatile bool    s_pendStateSync = false;
static volatile bool    s_battPushNow   = false;  // set on connect to send battery immediately
static volatile bool    s_pendDnd       = false; static volatile uint8_t s_valDnd = 0;

static volatile bool    s_pendPwrCfg        = false;
static volatile uint8_t s_valPwrCfgMode     = 0;
static volatile uint8_t s_valPwrCfgBright   = 4;
static volatile uint8_t s_valPwrCfgTimeout  = 60;

static volatile bool    s_pendAlarm         = false;
static volatile uint8_t s_valAlarmIdx       = 0;
static volatile uint8_t s_valAlarmH         = 7;
static volatile uint8_t s_valAlarmM         = 0;
static volatile uint8_t s_valAlarmEn        = 0;
static volatile uint8_t s_valAlarmDow       = 0x7F;
static volatile uint8_t s_valAlarmUseDate   = 0;
static volatile uint8_t s_valAlarmDay       = 1;
static volatile uint8_t s_valAlarmMonth     = 1;

// ── Parse incoming packet (runs on BLE task, core 0) ─────────────────────────
void cfg_onPacket(const char* raw, int len) {
    // raw = "X|KEY|..." or "Q|"
    if (len < 2) return;

    // Q| is handled by ble_manager.cpp which calls cfg_requestStateSync()
    // X| packets parsed here
    if (raw[0] != 'X' || raw[1] != '|') return;

    const char* p = raw + 2;   // skip "X|"
    const char* sep = strchr(p, '|');
    if (!sep) return;

    char key[12] = {};
    int klen = (int)(sep - p);
    if (klen <= 0 || klen >= 12) return;
    strncpy(key, p, klen);
    int value = atoi(sep + 1);

    if      (strcmp(key, "BRIGHT")   == 0) { s_valBright   = constrain(value, 0, 5); s_pendBright   = true; }
    else if (strcmp(key, "POWER")    == 0) { s_valPower    = constrain(value, 0, 2); s_pendPower    = true; }
    else if (strcmp(key, "SOUND")    == 0) { s_valSound    = constrain(value, 0, 1); s_pendSound    = true; }
    else if (strcmp(key, "AOD")      == 0) { s_valAod      = constrain(value, 0, 1); s_pendAod      = true; }
    else if (strcmp(key, "AUTOROT")  == 0) { s_valAutorot  = constrain(value, 0, 1); s_pendAutorot  = true; }
    else if (strcmp(key, "NOTIFTMO") == 0) { s_valNotifTmo = constrain(value, 0, 4); s_pendNotifTmo = true; }
    else if (strcmp(key, "DND")      == 0) { s_valDnd      = constrain(value, 0, 1); s_pendDnd      = true; }
    else if (strcmp(key, "PWRCFG")  == 0) {
        // X|PWRCFG|mode|bright|timeout
        // sep points to first '|' after key; value already parsed as mode
        const char* p2 = sep + 1;
        int mode    = atoi(p2); p2 = strchr(p2,'|'); if (!p2) return; p2++;
        int bright  = atoi(p2); p2 = strchr(p2,'|'); if (!p2) return; p2++;
        int timeout = atoi(p2);
        s_valPwrCfgMode    = (uint8_t)constrain(mode,    0, 2);
        s_valPwrCfgBright  = (uint8_t)constrain(bright,  0, 5);
        s_valPwrCfgTimeout = (uint8_t)constrain(timeout, 0, 120);
        s_pendPwrCfg = true;
    }
    else if (strcmp(key, "ALARM")   == 0) {
        // X|ALARM|idx|hour|min|enabled|daysOfWeek|useDate|dateDay|dateMonth
        // Need 8 pipe-separated integers after the key. Validate count first.
        const char* q = sep + 1;
        // Count available fields
        int fieldCount = 1;
        for (const char* c = q; *c; c++) if (*c == '|') fieldCount++;
        if (fieldCount < 8) return;  // malformed — ignore rather than over-read

        auto nextInt = [&]() -> int {
            int v = atoi(q);
            const char* nx = strchr(q, '|');
            q = nx ? nx + 1 : q + strlen(q);
            return v;
        };
        int idx = nextInt(), h = nextInt(), m = nextInt(), en = nextInt();
        int dow = nextInt(), ud = nextInt(), dd = nextInt(), dm = nextInt();
        if (idx < 0 || idx >= ALARM_MAX) return;
        s_valAlarmIdx     = (uint8_t)idx;
        s_valAlarmH       = (uint8_t)(h   % 24);
        s_valAlarmM       = (uint8_t)(m   % 60);
        s_valAlarmEn      = (uint8_t)(en  != 0 ? 1 : 0);
        s_valAlarmDow     = (uint8_t)dow;
        s_valAlarmUseDate = (uint8_t)(ud  != 0 ? 1 : 0);
        s_valAlarmDay     = (uint8_t)constrain(dd, 1, 31);
        s_valAlarmMonth   = (uint8_t)constrain(dm, 1, 12);
        s_pendAlarm = true;
    }
}

// ── State sync request (called from BLE task — sets flag only) ───────────────
void cfg_requestStateSync() {
    s_pendStateSync = true;
    s_battPushNow   = true;  // also push battery immediately next loop
}

// ── Process all pending changes (runs on main loop, core 1) ──────────────────
void cfg_processAll() {
    if (s_pendBright) {
        s_pendBright = false;
        g_pwrCfg[g_powerMode].brightness = s_valBright;
        dispMgr_setBrightness(s_valBright);
        Serial.printf("[CFG] BRIGHT=%d\n", s_valBright);
    }
    if (s_pendPower) {
        s_pendPower = false;
        Serial.printf("[CFG] POWER=%d\n", s_valPower);
        pwrMgr_apply((PowerMode)s_valPower);
    }
    if (s_pendSound) {
        s_pendSound = false;
        g_soundProfile = (SoundProfile)s_valSound;
        EEPROM.write(EE_SOUND, (uint8_t)g_soundProfile);
        EEPROM.commit();
        Serial.printf("[CFG] SOUND=%d\n", s_valSound);
    }
    if (s_pendAod) {
        s_pendAod = false;
        g_aodEnabled = s_valAod != 0;
        EEPROM.write(EE_AOD, g_aodEnabled ? 1 : 0);
        EEPROM.commit();
        dispMgr_markDirty();
        Serial.printf("[CFG] AOD=%d\n", s_valAod);
    }
    if (s_pendAutorot) {
        s_pendAutorot = false;
        g_autoRotate = s_valAutorot != 0;
        EEPROM.write(EE_AUTOROT, g_autoRotate ? 1 : 0);
        EEPROM.commit();
        Serial.printf("[CFG] AUTOROT=%d\n", s_valAutorot);
    }
    if (s_pendNotifTmo) {
        s_pendNotifTmo = false;
        g_notifOverlayTimeoutIdx = s_valNotifTmo;
        EEPROM.write(EE_NOTIF_TIMEOUT, g_notifOverlayTimeoutIdx);
        EEPROM.commit();
        Serial.printf("[CFG] NOTIFTMO=%d\n", s_valNotifTmo);
    }
    if (s_pendPwrCfg) {
        s_pendPwrCfg = false;
        int m = s_valPwrCfgMode;
        g_pwrCfg[m].brightness  = s_valPwrCfgBright;
        g_pwrCfg[m].timeoutSec  = s_valPwrCfgTimeout;
        // If this is the active mode, apply brightness immediately
        if (m == (int)g_powerMode) {
            dispMgr_setBrightness(s_valPwrCfgBright);
        }
        Serial.printf("[CFG] PWRCFG mode=%d bright=%d timeout=%d\n",
            m, s_valPwrCfgBright, s_valPwrCfgTimeout);
    }
    if (s_pendAlarm) {
        s_pendAlarm = false;
        int i = s_valAlarmIdx;
        g_alarms[i].hour        = s_valAlarmH;
        g_alarms[i].minute      = s_valAlarmM;
        g_alarms[i].enabled     = s_valAlarmEn != 0;
        g_alarms[i].daysOfWeek  = s_valAlarmDow;
        g_alarms[i].useDate     = s_valAlarmUseDate != 0;
        g_alarms[i].dateDay     = max(1, min((int)s_valAlarmDay,   31));
        g_alarms[i].dateMonth   = max(1, min((int)s_valAlarmMonth, 12));
        g_alarms[i].fired       = false;
        tmMgr_saveAlarms();
        dispMgr_markDirty();
        Serial.printf("[CFG] ALARM[%d] %02d:%02d en=%d dow=%02X\n",
            i, g_alarms[i].hour, g_alarms[i].minute,
            g_alarms[i].enabled, g_alarms[i].daysOfWeek);
    }
    if (s_pendDnd) {
        s_pendDnd = false;
        g_dndEnabled = (s_valDnd != 0);
        EEPROM.write(EE_DND, g_dndEnabled ? 1 : 0);
        EEPROM.commit();
        Serial.printf("[CFG] DND=%d\n", s_valDnd);
    }
    if (s_pendStateSync) {
        s_pendStateSync = false;
        char buf[64];

        // Helper: notify + 5ms gap so the BLE stack can flush each indication
        // before the next one queues. Without this, a 12-packet burst can
        // overflow the ATT layer and silently drop packets on reconnect.
        auto notifyPaced = [&](const char* b) {
            bleMgr_notify(b);
            vTaskDelay(pdMS_TO_TICKS(5));
        };

        // Current settings
        snprintf(buf, sizeof(buf), "ST|POWER|%d",    (int)g_powerMode);         notifyPaced(buf);
        snprintf(buf, sizeof(buf), "ST|BRIGHT|%d",   g_brightLevel);             notifyPaced(buf);
        snprintf(buf, sizeof(buf), "ST|AOD|%d",      g_aodEnabled  ? 1 : 0);    notifyPaced(buf);
        snprintf(buf, sizeof(buf), "ST|AUTOROT|%d",  g_autoRotate  ? 1 : 0);    notifyPaced(buf);
        snprintf(buf, sizeof(buf), "ST|SOUND|%d",    (int)g_soundProfile);       notifyPaced(buf);
        snprintf(buf, sizeof(buf), "ST|NOTIFTMO|%d", g_notifOverlayTimeoutIdx);  notifyPaced(buf);
        snprintf(buf, sizeof(buf), "ST|DND|%d",      g_dndEnabled  ? 1 : 0);    notifyPaced(buf);
        // Per-mode power configs
        for (int m = 0; m < PWR_COUNT; m++) {
            snprintf(buf, sizeof(buf), "ST|PWRCFG|%d|%d|%d",
                m, g_pwrCfg[m].brightness, g_pwrCfg[m].timeoutSec);
            notifyPaced(buf);
        }
        // Alarms
        for (int i = 0; i < ALARM_MAX; i++) {
            snprintf(buf, sizeof(buf), "ST|ALARM|%d|%d|%d|%d|%d|%d|%d|%d",
                i, g_alarms[i].hour, g_alarms[i].minute,
                g_alarms[i].enabled  ? 1 : 0,
                g_alarms[i].daysOfWeek,
                g_alarms[i].useDate  ? 1 : 0,
                g_alarms[i].dateDay, g_alarms[i].dateMonth);
            notifyPaced(buf);
        }
        Serial.println("[CFG] state sync sent");
    }
}

// ── Push single changed value to phone ───────────────────────────────────────
void cfg_pushChange(CfgKey key) {
    if (!g_bleConn) return;
    char buf[64];
    switch (key) {
        case CFG_POWER:
            snprintf(buf, sizeof(buf), "ST|POWER|%d", (int)g_powerMode);
            bleMgr_notify(buf);
            break;
        case CFG_BRIGHT:
            snprintf(buf, sizeof(buf), "ST|BRIGHT|%d", g_brightLevel);
            bleMgr_notify(buf);
            break;
        case CFG_AOD:
            snprintf(buf, sizeof(buf), "ST|AOD|%d", g_aodEnabled ? 1 : 0);
            bleMgr_notify(buf);
            break;
        case CFG_AUTOROT:
            snprintf(buf, sizeof(buf), "ST|AUTOROT|%d", g_autoRotate ? 1 : 0);
            bleMgr_notify(buf);
            break;
        case CFG_SOUND:
            snprintf(buf, sizeof(buf), "ST|SOUND|%d", (int)g_soundProfile);
            bleMgr_notify(buf);
            break;
        case CFG_NOTIFTMO:
            snprintf(buf, sizeof(buf), "ST|NOTIFTMO|%d", g_notifOverlayTimeoutIdx);
            bleMgr_notify(buf);
            break;
        case CFG_PWRCFG:
            for (int m = 0; m < PWR_COUNT; m++) {
                snprintf(buf, sizeof(buf), "ST|PWRCFG|%d|%d|%d",
                    m, g_pwrCfg[m].brightness, g_pwrCfg[m].timeoutSec);
                bleMgr_notify(buf);
            }
            break;
        case CFG_ALARM:
            for (int i = 0; i < ALARM_MAX; i++) {
                snprintf(buf, sizeof(buf), "ST|ALARM|%d|%d|%d|%d|%d|%d|%d|%d",
                    i, g_alarms[i].hour, g_alarms[i].minute,
                    g_alarms[i].enabled  ? 1 : 0,
                    g_alarms[i].daysOfWeek,
                    g_alarms[i].useDate  ? 1 : 0,
                    g_alarms[i].dateDay, g_alarms[i].dateMonth);
                bleMgr_notify(buf);
            }
            break;
        case CFG_DND:
            snprintf(buf, sizeof(buf), "ST|DND|%d", g_dndEnabled ? 1 : 0);
            bleMgr_notify(buf);
            break;
    }
}

// ── Battery push (rate-limited to 300s, or immediately when s_battPushNow) ──
// 60s was overkill — 1440 packets/day. At 300s: 288/day. Battery % changes
// at most ~1%/5min in normal use so the resolution loss is imperceptible.
void cfg_pushBatteryIfDue() {
    if (!g_bleConn) return;
    static unsigned long s_lastBattPush = 0;
    unsigned long now = millis();
    if (!s_battPushNow && (now - s_lastBattPush < 300000UL)) return;
    s_battPushNow  = false;
    s_lastBattPush = now;
    int pct   = M5.Power.getBatteryLevel();
    int mvolt = (int)(M5.Power.getBatteryVoltage());
    // Prefer real current from AXP2101 coulomb counter (available on PLUS2).
    // getBatteryCurrent() returns mA; positive = charging, negative = discharging.
    // If it returns 0 (not supported / no valid reading), fall back to model.
    float realMa = M5.Power.getBatteryCurrent();
    float mA = (realMa != 0.0f) ? fabsf(realMa) : dispMgr_getEstimatedMa((int)g_powerMode);
    int maX10 = (int)(mA * 10.0f);
    char buf[32];
    snprintf(buf, sizeof(buf), "B|%d|%d|%d", pct, mvolt, maX10);
    bleMgr_notify(buf);
    Serial.printf("[BAT] pushed %s\n", buf);
}
