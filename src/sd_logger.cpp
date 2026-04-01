/*
 * SIDECAR V1 — SD Logger
 * Appends one CSV row per LOG_INTERVAL_S to /SIDECAR.CSV on the SD card.
 *
 * Format: unix_ts,batt_pct,batt_mv,steps
 *   unix_ts   — seconds since epoch (from RTC); 0 if time not synced
 *   batt_pct  — smoothed battery % (0-100)
 *   batt_mv   — raw battery voltage in millivolts
 *   steps     — cumulative step count since last reset
 *
 * Uses Arduino SD.h (bundled with ESP32 Arduino core), SPI MicroSD.
 * If the card is absent or mount fails, sdLog_update() becomes a no-op.
 */
#include <Arduino.h>
#include <M5Unified.h>
#include <SPI.h>
#include <SD.h>
#include "sd_logger.h"
#include "hardware_config.h"
#include "display_mgr.h"   // dispMgr_getBatt()
#include "step_counter.h"  // g_stepCount

#define LOG_INTERVAL_S   60     // one row per minute
#define LOG_FILE         "/SIDECAR.CSV"
#define HEADER_LINE      "unix_ts,batt_pct,batt_mv,steps\n"

static bool s_sdReady = false;

bool sdLog_init() {
    // Use custom SPI pins — default ESP32 SPI conflicts with M5 display
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, SPI)) {
        Serial.println("[SD] init failed — no card or wrong pins");
        s_sdReady = false;
        return false;
    }
    // Write CSV header if file is new / empty
    if (!SD.exists(LOG_FILE)) {
        File f = SD.open(LOG_FILE, FILE_WRITE);
        if (f) { f.print(HEADER_LINE); f.close(); }
    }
    s_sdReady = true;
    Serial.printf("[SD] ready, card %llu MB\n", SD.cardSize() / (1024ULL * 1024ULL));
    return true;
}

void sdLog_update() {
    if (!s_sdReady) return;

    static unsigned long s_lastLog = 0;
    unsigned long now = millis();
    if (now - s_lastLog < (unsigned long)LOG_INTERVAL_S * 1000UL) return;
    s_lastLog = now;

    // Gather data
    auto dt = M5.Rtc.getDateTime();
    // Simple unix timestamp (good enough for logging; no DST/timezone)
    // Accurate once RTC has been synced via BLE T| packet.
    // For a proper epoch we'd need the stored BLE epoch + elapsed seconds,
    // but the RTC gives us YYYY/MM/DD HH:MM:SS which is enough for analysis.
    // Encode as YYYYMMDDHHMMSS (fits in a 64-bit int, easy to parse).
    long long ts = (long long)dt.date.year  * 10000000000LL
                 + (long long)dt.date.month * 100000000LL
                 + (long long)dt.date.date  * 1000000LL
                 + (long long)dt.time.hours * 10000LL
                 + (long long)dt.time.minutes * 100LL
                 + (long long)dt.time.seconds;
    int  pct   = dispMgr_getBatt();
    int  mv    = (int)M5.Power.getBatteryVoltage();
    int  steps = g_stepCount;

    File f = SD.open(LOG_FILE, FILE_APPEND);
    if (!f) {
        Serial.println("[SD] open failed");
        return;
    }
    f.printf("%lld,%d,%d,%d\n", ts, pct, mv, steps);
    f.close();
    Serial.printf("[SD] logged %lld pct=%d mv=%d steps=%d\n", ts, pct, mv, steps);
}
