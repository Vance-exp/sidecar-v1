/*
 * SIDECAR V1 — SD Logger
 * Writes CSV battery + step data to MicroSD via SPI.
 * Pins are defined in hardware_config.h (SD_CS/MOSI/MISO/SCK).
 * Gracefully no-ops if no card is inserted.
 */
#pragma once

// Call once from setup() — returns false if SD init fails (no card / wrong pins).
// Continues running fine even if this returns false.
bool sdLog_init();

// Call from loop() — internally rate-limits to LOG_INTERVAL_S seconds.
// Logs: unix_ts,batt_pct,batt_mv,steps
void sdLog_update();
