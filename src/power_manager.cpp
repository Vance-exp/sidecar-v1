/*
 * SIDECAR V1 — Power Manager
 * 3-state machine with ESP.restart() for DEEPSLEEP transitions.
 *
 * Transition rules:
 *   NORMAL ↔ EFFICIENT : safe at runtime (both ≥80MHz, BLE stays alive)
 *   * → DEEPSLEEP      : write EEPROM, animate, ESP.restart() (BLE alive → can't go <80MHz)
 *   DEEPSLEEP → *      : write EEPROM, animate, ESP.restart() (no BLE → need to reinit)
 */
#include <Arduino.h>
#include <M5Unified.h>
#include <EEPROM.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include "esp_pm.h"
#include "power_manager.h"
#include "ble_manager.h"
#include "display_mgr.h"

// ── State ─────────────────────────────────────────────────────────────────────
PowerMode g_powerMode = PWR_NORMAL;
bool      g_bleInited = false;

PwrCfg g_pwrCfg[PWR_COUNT] = {
    { 3, 15 },     // NORMAL:    bright 3, 15s timeout (was 4/60 — shorter saves ~4mA avg)
    { 2, 10 },     // EFFICIENT: bright 2, 10s timeout
    { 1,  8 },     // DEEPSLEEP: bright 1,  8s timeout
};

// ── Init ──────────────────────────────────────────────────────────────────────
void pwrMgr_init() {
    uint8_t pm = EEPROM.read(EE_PWRMODE);
    if (pm < PWR_COUNT) g_powerMode = (PowerMode)pm;
    else                g_powerMode = PWR_NORMAL;

    // Set CPU freq BEFORE BLE init — changing it after crashes the RF stack
    // Also configure esp_pm BEFORE BLE init so the BLE controller picks up the
    // power management settings (modem sleep integrates with auto light sleep).
    switch (g_powerMode) {
        case PWR_NORMAL:
            setCpuFrequencyMhz(160);
            // No light sleep in NORMAL — screen-on rendering at 160MHz is plenty for 30fps
            break;
        case PWR_EFFICIENT: {
            setCpuFrequencyMhz(80);
            // Auto light sleep at 80MHz. BLE modem sleep is enabled in bleMgr_init().
            // Together they drop screen-off draw from ~20mA to ~3-4mA.
            esp_pm_config_esp32_t pm_cfg = {
                .max_freq_mhz       = 80,
                .min_freq_mhz       = 80,
                .light_sleep_enable = true
            };
            if (esp_pm_configure(&pm_cfg) == ESP_OK)
                Serial.println("[PWR] EFFICIENT auto light sleep enabled");
            break;
        }
        case PWR_DEEPSLEEP:
            setCpuFrequencyMhz(40);
            break;
    }

    // BLE requires ≥80MHz — skip in DEEPSLEEP
    if (g_powerMode != PWR_DEEPSLEEP) {
        Serial.printf("[PWR] pre-BLE heap=%d\n", ESP.getFreeHeap());
        bleMgr_init();
        g_bleInited = true;
        Serial.printf("[PWR] post-BLE heap=%d\n", ESP.getFreeHeap());

        // bleMgr_init() starts advertising at BLE_ADV_ULTRA_SLOW as a safe default.
        // Apply the correct interval for the loaded mode right away so the phone
        // can find the watch quickly on first boot without waiting 2s between events.
        {
            int advSlots = (g_powerMode == PWR_NORMAL) ? BLE_ADV_FAST : BLE_ADV_ULTRA_SLOW;
            bleMgr_setAdvInterval(advSlots, advSlots + 64);
        }
    } else {
        Serial.println("[PWR] DEEPSLEEP mode — BLE skipped");
        g_bleInited = false;

        // Shut down WiFi/BT power domain — never used in DEEPSLEEP, ~3mA saving
        WiFi.mode(WIFI_OFF);

        // Enable FreeRTOS automatic light sleep via esp_pm.
        // When all tasks are blocked (vTaskDelay), the system enters light sleep
        // (~0.8mA vs ~12mA active). GPIO4 is held HIGH by gpio_hold_en() set in
        // setup() before this runs, so the AXP2101 power rail stays live.
        // max=min=40 avoids dynamic freq scaling that would break I2C timing.
        esp_pm_config_esp32_t pm_cfg = {
            .max_freq_mhz       = 40,
            .min_freq_mhz       = 40,
            .light_sleep_enable = true
        };
        if (esp_pm_configure(&pm_cfg) != ESP_OK) {
            Serial.println("[PWR] esp_pm_configure failed — light sleep disabled");
        } else {
            Serial.println("[PWR] auto light sleep enabled");
        }
    }
}

// ── Transition ────────────────────────────────────────────────────────────────
void pwrMgr_apply(PowerMode m) {
    // ── Case 1: entering or leaving DEEPSLEEP → must restart ──────────────────
    bool crossingBoundary = (m == PWR_DEEPSLEEP) != (g_powerMode == PWR_DEEPSLEEP);

    if (crossingBoundary) {
        EEPROM.write(EE_PWRMODE, (uint8_t)m);
        EEPROM.commit();

        if (m == PWR_DEEPSLEEP) {
            dispMgr_animDeepSleep();
        } else {
            dispMgr_animWakeUp();
        }
        ESP.restart();
        // unreachable
    }

    // ── Case 2: NORMAL ↔ EFFICIENT — safe at runtime ─────────────────────────
    bool modeChanged = (m != g_powerMode);
    g_powerMode = m;
    // Only reset brightness to mode default when the mode actually changes.
    // If we're staying in the same mode (e.g. remote BRIGHT config packet triggered
    // a pwrMgr_apply on same mode), keep the current g_brightLevel unchanged.
    if (modeChanged) {
        g_brightLevel = g_pwrCfg[m].brightness;
    }

    if (m == PWR_NORMAL) {
        setCpuFrequencyMhz(160);
        // 160MHz is plenty for 30fps on a 240×135 screen; saves ~30% vs 240MHz
        esp_pm_config_esp32_t pm_cfg = { .max_freq_mhz=160, .min_freq_mhz=160, .light_sleep_enable=false };
        esp_pm_configure(&pm_cfg);
    } else {  // PWR_EFFICIENT
        setCpuFrequencyMhz(80);
        // Enable auto light sleep — saves ~15mA when screen is off
        esp_pm_config_esp32_t pm_cfg = { .max_freq_mhz=80, .min_freq_mhz=80, .light_sleep_enable=true };
        esp_pm_configure(&pm_cfg);
    }

    g_screenOn = true;
    M5.Display.setBrightness(brtValue(g_brightLevel));
    if (g_displaySleeping) {
        M5.Display.wakeup();
        g_displaySleeping = false;
    }

    // NORMAL=100ms (fast notif), EFFICIENT=2048ms (ultra-slow, ~1s notif delay, saves ~2mA)
    int advSlots = (m == PWR_NORMAL) ? BLE_ADV_FAST : BLE_ADV_ULTRA_SLOW;
    if (g_bleInited && !g_bleConn) {
        bleMgr_setAdvInterval(advSlots, advSlots + 64);
        NimBLEDevice::startAdvertising();
    }

    EEPROM.write(EE_PWRMODE, (uint8_t)m);
    EEPROM.commit();

    g_lastActivity = millis();
    g_displayDirty = true;

    Serial.printf("[PWR] -> %s cpu=%dMHz ble=%s heap=%d\n",
        PWR_NAMES[m], getCpuFrequencyMhz(),
        g_bleInited ? "on" : "off", ESP.getFreeHeap());
}
