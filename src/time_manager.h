/*
 * SIDECAR V1 — Time Manager
 * RTC wrapper, alarm engine (3 alarms), BLE time sync.
 */
#pragma once
#include "hardware_config.h"

extern AlarmCfg      g_alarms[ALARM_MAX];
extern bool          g_alarmRinging;
extern unsigned long g_alarmRingAt;
extern int           g_alarmSel;
extern bool          g_alarmInEdit;
extern int           g_alarmEditField;
extern volatile bool g_alarmJustFired;  // one-shot flag, cleared by main loop

void tmMgr_init();                      // load alarms from EEPROM
void tmMgr_update();                    // call from loop() — checks alarms, drives speaker
void tmMgr_onBleTime(long unix_ts);     // called from BLE callback
void tmMgr_saveAlarms();
void tmMgr_loadAlarms();
void tmMgr_dismissAlarm();              // stop ringing
