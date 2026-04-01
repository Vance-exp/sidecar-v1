#pragma once

// Called from BLE onWrite callback (BLE task, core 0).
// Parses "X|KEY|VALUE" and enqueues the change.
void cfg_onPacket(const char* raw, int len);

// Called from main loop (core 1) — applies any pending config changes thread-safely.
void cfg_processAll();

// Trigger a full state dump via BLE NOTIFY (called from BLE task on connect or Q| packet).
void cfg_requestStateSync();

// Call from main loop every frame — internally rate-limits to 60s.
void cfg_pushBatteryIfDue();

// Push a single changed value to the phone immediately (call from main loop after any
// on-watch setting change so the app stays in sync without a full Q| round-trip).
// Each enum maps to the matching ST| packet the phone already knows how to parse.
enum CfgKey : uint8_t {
    CFG_POWER = 0,
    CFG_BRIGHT,
    CFG_AOD,
    CFG_AUTOROT,
    CFG_SOUND,
    CFG_NOTIFTMO,
    CFG_PWRCFG,   // sends all 3 mode configs
    CFG_ALARM,    // sends all 3 alarms
    CFG_DND,
};
void cfg_pushChange(CfgKey key);
