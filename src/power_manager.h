/*
 * SIDECAR V1 — Power Manager
 * 3-state machine: NORMAL (160MHz+BLE), EFFICIENT (80MHz+BLE), DEEPSLEEP (40MHz, no BLE)
 * Transitions that cross the 80MHz BLE boundary require ESP.restart().
 */
#pragma once
#include "hardware_config.h"

extern PowerMode g_powerMode;
extern PwrCfg    g_pwrCfg[PWR_COUNT];
extern bool      g_bleInited;

// Call from setup() — reads EEPROM power mode, sets CPU freq, inits BLE if needed
void pwrMgr_init();

// Transition to new mode. May call ESP.restart() for DEEPSLEEP transitions.
void pwrMgr_apply(PowerMode m);
