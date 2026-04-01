/*
 * SIDECAR V1 — Notification Queue
 * Thread-safe: BLE callback sets g_nqNewNotif flag, main loop handles display/sound.
 */
#include <Arduino.h>
#include <cstring>
#include "notif_queue.h"

Notif         g_notifs[NOTIF_MAX];
int           g_notifCount  = 0;
int           g_notifIdx    = 0;
int           g_unreadCount = 0;
bool          g_bannerActive    = false;
unsigned long g_bannerAt        = 0;
int           g_bannerNotifIdx  = 0;   // BUG 8: captured at push time, not recomputed
volatile bool g_nqNewNotif      = false;
bool          g_notifOverlay          = false;
int           g_notifOverlayIdx       = 0;
Screen        g_notifPrevScreen       = SCR_CLOCK;
unsigned long g_notifOverlayShownAt   = 0;
int           g_notifOverlayTimeoutIdx = 1;  // default 10s

// ── Media state ───────────────────────────────────────────────────────────────
char  g_mediaArtist[40]  = "";
char  g_mediaSong[60]    = "";
bool  g_mediaPlaying     = false;
int   g_mediaVolume      = -1;    // 0-100, -1 = unknown
volatile bool g_mediaUpdated = false;  // set by BLE task, cleared by main loop

// ── Weather state ─────────────────────────────────────────────────────────────
WeatherData g_weather = { 0, 0, 0, "---", 0 };

// ── Find My Phone flag ────────────────────────────────────────────────────────
volatile bool g_findPhoneReq = false;

// ── Type classifier — heuristic based on app name ────────────────────────────
static NotifType classifyApp(const char* app) {
    // Check for call (C| packet hardcodes "CALL", but belt+suspenders here)
    if (strcmp(app, "CALL") == 0) return NOTIF_CALL;
    // Common messaging apps — partial, case-insensitive prefix match
    const char* msgApps[] = {
        "messages", "whatsapp", "telegram", "signal", "messenger",
        "instagram", "snapchat", "discord", "slack", "sms", "mms", nullptr
    };
    char lower[20];
    int i = 0;
    for (; app[i] && i < 19; i++) lower[i] = (char)tolower((unsigned char)app[i]);
    lower[i] = 0;
    for (int j = 0; msgApps[j]; j++) {
        if (strstr(lower, msgApps[j])) return NOTIF_MSG;
    }
    return NOTIF_APP;
}

// ── Push ──────────────────────────────────────────────────────────────────────
void nq_push(const char* app, const char* title, const char* body, NotifType type) {
    // Shift if full
    if (g_notifCount >= NOTIF_MAX) {
        for (int i = 0; i < NOTIF_MAX - 1; i++) g_notifs[i] = g_notifs[i+1];
        g_notifCount = NOTIF_MAX - 1;
    }
    Notif& n = g_notifs[g_notifCount++];
    strncpy(n.app,   app,   19); n.app[19]   = 0;
    strncpy(n.title, title, 39); n.title[39] = 0;
    strncpy(n.body,  body,  59); n.body[59]  = 0;
    n.unread = true;
    n.type   = type;
    g_unreadCount++;
    g_bannerActive   = true;
    g_bannerAt       = millis();
    g_bannerNotifIdx = g_notifCount - 1;  // BUG 8: capture index now, not when drawn
    g_nqNewNotif     = true;              // main loop will handle wake + sound
}

// ── Packet parsers ────────────────────────────────────────────────────────────

// Helper: find nth '|' in a C string, return pointer after it (or nullptr)
static const char* findPipe(const char* s, int n) {
    for (int i = 0; i < n; i++) {
        s = strchr(s, '|');
        if (!s) return nullptr;
        s++;
    }
    return s;
}

// "N|APP|TITLE|BODY"
void nq_onNotifPacket(const char* raw, int len) {
    // raw[0]='N', raw[1]='|', rest is APP|TITLE|BODY
    const char* app = raw + 2;
    const char* ttl = findPipe(raw, 2);
    const char* bod = ttl ? findPipe(ttl - 1, 1) : nullptr;
    if (!ttl || !bod) return;

    // Guard: pointer arithmetic must produce non-negative lengths.
    // A malformed packet where ttl <= app or bod <= ttl would yield a negative
    // int which, when passed to %.*s, is UB. Reject those packets.
    if (ttl - 1 <= app || bod - 1 <= ttl) return;

    char a[20], t[40], b[60];
    int aLen = (int)(ttl - 1 - app);
    int tLen = (int)(bod - 1 - ttl);
    int bLen = len - (int)(bod - raw);
    if (aLen <= 0 || tLen <= 0 || bLen <= 0) return;

    snprintf(a, sizeof(a), "%.*s", min(aLen, 19), app);
    snprintf(t, sizeof(t), "%.*s", min(tLen, 39), ttl);
    snprintf(b, sizeof(b), "%.*s", min(bLen, 59), bod);

    nq_push(a, t, b, classifyApp(a));
}

// "S|artist|song|playing|volume"  (playing/volume optional for back-compat)
void nq_onMediaPacket(const char* raw, int len) {
    const char* p1 = raw + 2;              // artist starts after "S|"
    const char* p2 = findPipe(raw, 2);     // song
    if (!p2) return;
    const char* p3 = findPipe(raw, 3);     // playing (optional)
    const char* p4 = findPipe(raw, 4);     // volume  (optional)

    if (p2 - 1 <= p1) return;  // guard against malformed pointer order
    int aLen = (int)(p2 - 1 - p1);
    int sLen = p3 ? (int)(p3 - 1 - p2) : len - (int)(p2 - raw);
    if (aLen <= 0 || sLen <= 0) return;
    snprintf(g_mediaArtist, sizeof(g_mediaArtist), "%.*s", min(aLen, 39), p1);
    snprintf(g_mediaSong,   sizeof(g_mediaSong),   "%.*s", min(sLen, 59), p2);

    if (p3) g_mediaPlaying = (*p3 == '1');
    if (p4) g_mediaVolume  = atoi(p4);

    g_mediaUpdated = true;
}

// "C|name"
void nq_onCallPacket(const char* raw, int len) {
    char name[40];
    int nLen = len - 2;
    snprintf(name, sizeof(name), "%.*s", min(nLen, 39), raw + 2);
    nq_push("CALL", name, "Incoming call", NOTIF_CALL);
}

// "W|tempC|hi|lo|condition"  (e.g. "W|22|28|15|SUNNY")
void nq_onWeatherPacket(const char* raw, int len) {
    const char* p = raw + 2;            // skip "W|"
    // Clamp temperatures to sane range [-60, 60]°C to reject garbage values.
    int t = atoi(p);
    g_weather.tempC = (int8_t)constrain(t, -60, 60);
    p = strchr(p, '|'); if (!p) return; p++;
    int hi = atoi(p);
    g_weather.hiC   = (int8_t)constrain(hi, -60, 60);
    p = strchr(p, '|'); if (!p) return; p++;
    int lo = atoi(p);
    g_weather.loC   = (int8_t)constrain(lo, -60, 60);
    p = strchr(p, '|'); if (!p) return; p++;
    // snprintf guarantees null-termination; sizeof() guards against future struct changes.
    snprintf(g_weather.condition, sizeof(g_weather.condition), "%s", p);
    g_weather.updatedAt = millis();
    (void)len;
}

// ── UI operations ─────────────────────────────────────────────────────────────
void nq_markAllRead() {
    for (int i = 0; i < g_notifCount; i++) g_notifs[i].unread = false;
    g_unreadCount = 0;
}

void nq_dismiss(int idx) {
    if (idx < 0 || idx >= g_notifCount) return;
    for (int i = idx; i < g_notifCount - 1; i++) g_notifs[i] = g_notifs[i+1];
    g_notifCount--;
    if (g_notifIdx >= g_notifCount && g_notifIdx > 0) g_notifIdx--;
    // Recount unread — dismissing does NOT mark adjacent notifications as read.
    // The user chose to delete, not acknowledge. Marking the next visible notif
    // read was wrong: it silently consumed an unread notification.
    g_unreadCount = 0;
    for (int i = 0; i < g_notifCount; i++) if (g_notifs[i].unread) g_unreadCount++;
}

void nq_clearAll() {
    g_notifCount  = 0;
    g_notifIdx    = 0;
    g_unreadCount = 0;
}
