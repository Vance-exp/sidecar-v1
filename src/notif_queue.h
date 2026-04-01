/*
 * SIDECAR V1 — Notification Queue
 * 10-slot circular buffer for BLE notifications. Thread-safe via flag handoff.
 */
#pragma once
#include "hardware_config.h"

extern Notif         g_notifs[NOTIF_MAX];
extern int           g_notifCount;
extern int           g_notifIdx;        // currently viewed index
extern int           g_unreadCount;
extern bool          g_bannerActive;
extern unsigned long g_bannerAt;
extern int           g_bannerNotifIdx;  // index of the notif that triggered banner (BUG 8)
extern volatile bool g_nqNewNotif;      // set by BLE task, cleared by main loop
extern bool          g_notifOverlay;       // full-screen notification takeover active
extern int           g_notifOverlayIdx;   // which notif is shown in the overlay
extern Screen        g_notifPrevScreen;   // screen to return to on dismiss
extern unsigned long g_notifOverlayShownAt; // millis() when overlay was activated
extern int           g_notifOverlayTimeoutIdx; // index into notifTimeoutMs[] (0-4)

// Store a notification. Called from BLE callback (different FreeRTOS task).
void nq_push(const char* app, const char* title, const char* body, NotifType type = NOTIF_APP);

// ── Media state (updated by nq_onMediaPacket, read by display_mgr) ───────────
extern char          g_mediaArtist[40];
extern char          g_mediaSong[60];
extern bool          g_mediaPlaying;
extern int           g_mediaVolume;       // 0-100, -1 = unknown
extern volatile bool g_mediaUpdated;      // set by BLE task, cleared by main loop

// ── Weather state (updated by nq_onWeatherPacket, read by display_mgr) ──────
extern WeatherData   g_weather;

// ── Find My Phone request (set by button handler, cleared by main loop) ──────
extern volatile bool g_findPhoneReq;

// Parse Bellafaire packets — called from BLE onWrite callback
void nq_onNotifPacket(const char* raw, int len);     // "N|APP|TITLE|BODY"
void nq_onMediaPacket(const char* raw, int len);     // "S|artist|song|playing|volume"
void nq_onCallPacket(const char* raw, int len);      // "C|name"
void nq_onWeatherPacket(const char* raw, int len);   // "W|tempC|hi|lo|condition"

// UI operations — called from main loop only
void nq_markAllRead();
void nq_dismiss(int idx);
void nq_clearAll();
