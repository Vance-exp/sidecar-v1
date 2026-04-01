/*
 * SIDECAR V1 — Display Manager
 * Owns: sprite canvas, all draw functions, screen sleep/wake, rotation, battery.
 * All drawing to LCD happens exclusively through this module.
 */
#pragma once
#include "hardware_config.h"

// ── Screen state ──────────────────────────────────────────────────────────────
extern Screen        g_curScreen;
extern bool          g_screenOn;
extern bool          g_displaySleeping;     // true when SLPIN sent to LCD
extern bool          g_displayDirty;
extern unsigned long g_lastActivity;

// ── Display settings ──────────────────────────────────────────────────────────
extern bool          g_autoRotate;
extern int           g_curRotation;
extern int           g_brightLevel;
extern bool          g_aodEnabled;
extern bool          g_aodActive;
extern bool          g_wristWakeEnabled;
extern bool          g_dndEnabled;
extern int           g_aodFace;         // 0=clock 1=clock+steps 2=clock+alarm
extern TimerMode     g_timerMode;       // NORMAL / POMODORO / BREATHE

// ── Settings UI ───────────────────────────────────────────────────────────────
extern int           g_settingIdx;

// ── Power screen UI ───────────────────────────────────────────────────────────
extern int           g_pwrTabSel;
extern bool          g_pwrInCustom;
extern int           g_pwrCustIdx;
#define PWRCFG_FIELDS 2     // brightness, timeout (no WiFi toggle)

// ── Stopwatch state (owned here, modified by main.cpp button handlers) ────────
extern bool          sw_running;
extern unsigned long sw_start, sw_elapsed;
extern unsigned long sw_laps[SW_LAPS_MAX];
extern int           sw_lapCount;
extern unsigned long sw_lapStart;

// ── Timer state ───────────────────────────────────────────────────────────────
extern TimerState    tmr_state;
extern int           tmr_setMin;
extern unsigned long tmr_endMs;
extern unsigned long tmr_remaining;

// ── Functions ─────────────────────────────────────────────────────────────────

// Init sprite, set rotation
void dispMgr_init();

// Call from loop() — handles dirty check, sleep/wake, AOD, drawing, battery log
void dispMgr_update();

// Wake screen from sleep, reset activity timer
void dispMgr_wakeScreen();

// Force redraw next frame
void dispMgr_markDirty();

// Set brightness level (0-5) and apply to hardware
void dispMgr_setBrightness(int lvl);

// Show splash screen for ms milliseconds
void dispMgr_showSplash(const char* line1, const char* line2, int ms);

// Transition animations (block until complete)
void dispMgr_animDeepSleep();
void dispMgr_animWakeUp();

// Auto-rotate check — call from loop()
void dispMgr_checkRotation();

// Wrist-raise wake — call from loop(), no-op when screen is on
void dispMgr_checkWristWake();

// Smoothed battery % (EMA filtered)
int  dispMgr_getBatt();

// Estimated current draw for the given power mode index (0=NORMAL 1=EFFICIENT 2=DEEPSLEEP).
// Model-based — AXP2101 has no current ADC.  Used by cfg_manager for battery telemetry.
float dispMgr_getEstimatedMa(int modeIdx);
