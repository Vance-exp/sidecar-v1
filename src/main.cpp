/*
 * SIDECAR V1 — Firmware
 * BLE UUIDs from Bellafaire/ESP32-Smart-Watch
 * Copyright (c) 2020-2021 Matthew James Bellafaire — MIT License
 *
 * BtnA  FRONT   short = primary action    long = Watch Face (home)
 * BtnB  SIDE    short = next tab          long = prev tab
 * BtnPWR        hardware only — not used in firmware (PMIC causes restart)
 *
 * SCREENS: 0=Clock  1=Stopwatch  2=Timer  3=Connect  4=Settings  5=Notifications
 */

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <sys/time.h>
#include <EEPROM.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <LittleFS.h>
#include "config.h"

// ── Bellafaire BLE UUIDs (MIT © 2020-2021 Matthew James Bellafaire) ──────────
#define BLE_SVC "5ac9bc5e-f8ba-48d4-8908-98b80b566e49"
#define BLE_CHR "bcca872f-1a3e-4491-b8ec-bfc93c5dd91a"

// ── Palette ───────────────────────────────────────────────────────────────────
#define NIXIE_ORANGE 0xFB60
#define NIXIE_DIM    0x8200
#define NIXIE_GHOST  0x3900
#define COL_BG       TFT_BLACK
#define COL_GREEN    0x07E0
#define COL_RED      0xF800
#define COL_BLUE     0x1C9F

// ── Display ───────────────────────────────────────────────────────────────────
#define W 240
#define H 135
LGFX_Sprite canvas(&M5.Display);

// ── Screens ───────────────────────────────────────────────────────────────────
enum Screen { SCR_CLOCK, SCR_STOPWATCH, SCR_TIMER, SCR_CONNECT, SCR_POWER, SCR_ALARM, SCR_VOICE, SCR_SETTINGS, SCR_NOTIFS, SCR_DIAG, SCR_COUNT };
Screen curScreen = SCR_CLOCK;

// ── Power ─────────────────────────────────────────────────────────────────────
enum PowerMode { PWR_NORMAL, PWR_EFFICIENT, PWR_EXTREME, PWR_SLEEP };
PowerMode powerMode = PWR_NORMAL, lastActiveMode = PWR_NORMAL;
const char* PWR_NAMES[] = {"NORMAL","EFFICIENT","EXTREME","SLEEP"};
#define PWR_MODE_COUNT 4

// ── Per-mode configurable settings ───────────────────────────────────────────
struct PwrCfg {
  int  brightness;    // 0-5
  int  timeoutSec;    // screen-off timeout in seconds (0 = never)
  bool wifiOff;       // force WiFi off in this mode
};
PwrCfg pwrCfg[PWR_MODE_COUNT] = {
  { 4, 60,  false },  // NORMAL:    bright 4, 60s timeout, WiFi allowed
  { 3, 30,  true  },  // EFFICIENT: bright 3, 30s timeout, WiFi off
  { 2, 10,  true  },  // EXTREME:   bright 2, 10s timeout, WiFi off
  { 1, 5,   true  },  // SLEEP:     bright 1, 5s timeout,  WiFi off
};
// Field names for customization sub-screen
#define PWRCFG_COUNT 3

// ── Power tab state ───────────────────────────────────────────────────────────
int  pwrTabSel   = 0;     // which mode card is highlighted (0-4)
bool pwrInCustom = false; // true when in customization sub-screen
int  pwrCustIdx  = 0;     // which customization field is active

unsigned long lastActivity = 0;
bool screenOn = true, inSleepLoop = false;
bool displaySleeping = false;   // true when SLPIN sent to LCD controller

// ── Auto-rotate ───────────────────────────────────────────────────────────────
bool autoRotate = true;
int curRotation = 1;
unsigned long lastRotCheck = 0;

// ── Wrist gesture ─────────────────────────────────────────────────────────────
bool wristWasUp = false;
unsigned long lastWristUpMs = 0;

// ── Stopwatch ─────────────────────────────────────────────────────────────────
bool swRunning = false;
unsigned long swStart=0, swElapsed=0, lapStart=0;
#define MAX_LAPS 8
unsigned long lapSplits[MAX_LAPS];
int lapCount = 0;

// ── Timer ─────────────────────────────────────────────────────────────────────
enum TimerState { TMR_IDLE, TMR_RUNNING, TMR_PAUSED, TMR_DONE };
TimerState timerState = TMR_IDLE;
int timerSetMin = 5;
unsigned long timerEndMs = 0, timerRemaining = 0;

// ── Settings (10 fields) ──────────────────────────────────────────────────────
// TIME col: 0=Hour 1=Minute 2=Date
// DATE col: 3=Month 4=Year 5=Power
// SYS col:  6=Bright 7=AutoRot 8=AOD 9=Sound 10=Log
#define SETTING_COUNT 11
int settingIdx = 0;
int brightLevel = 3;
unsigned long lastFastTick = 0;
#define FAST_STEP_MS 120

// ── WiFi ──────────────────────────────────────────────────────────────────────
enum WiFiSt { WF_IDLE, WF_SCANNING, WF_LIST, WF_AP };
WiFiSt wifiSt = WF_IDLE;
#define MAX_NETS 8
String wifiSSIDs[MAX_NETS];
int  wifiRSSI[MAX_NETS], wifiNetCount=0, wifiListIdx=0;
bool wifiIsOpen[MAX_NETS];   // true = no password needed
String wifiSelSSID = "";
bool wifiAPActive = false;
WebServer wifiSrv(80);
Preferences prefs;

// ── Notifications ─────────────────────────────────────────────────────────────
#define MAX_NOTIFS 10
struct Notif { char app[20]; char title[40]; char body[60]; bool unread; };
Notif notifs[MAX_NOTIFS];
int notifCount=0, notifIdx=0, unreadCount=0;
bool bannerActive = false;
unsigned long bannerAt = 0;
#define BANNER_MS 3000

// ── BLE ───────────────────────────────────────────────────────────────────────
bool bleConn = false;
NimBLEServer* pBSrv = nullptr;
NimBLECharacteristic* pBChr = nullptr;
uint16_t bleConnHandle = BLE_HS_CONN_HANDLE_NONE;
static bool bleInited = false;

// ── Display dirty flag ────────────────────────────────────────────────────────
// Only push sprite to LCD when something actually changed.
volatile bool displayDirty = true;

// ── WiFi inactivity timeout ───────────────────────────────────────────────────
unsigned long wifiConnectedAt = 0;       // when STA connected
unsigned long wifiFailingSince = 0;      // when STA started failing
bool          wifiAutoKilled   = false;  // true if we disconnected due to inactivity

// ── EEPROM ────────────────────────────────────────────────────────────────────
#define EE_BRIGHT      0
#define EE_AUTOROT     1
#define EE_PWRMODE     2
// bytes 3-23: 3 alarms × 7 bytes each
#define EE_ALARM_BASE  3
#define EE_ALARM_BYTES 7
#define EE_SOUND       24   // sound profile byte
#define EE_AOD         25   // aod enabled byte
#define EE_LOGGING     26   // data logging enabled byte
#define EE_SIZE        32

// ── Alarms ───────────────────────────────────────────────────────────────────
#define MAX_ALARMS 3
#define ALARM_EDIT_FIELDS 12  // H, M, Sun Mon Tue Wed Thu Fri Sat, useDate, day, month
struct AlarmCfg {
  uint8_t hour, minute;
  bool    enabled, fired;
  uint8_t daysOfWeek;  // bitmask bit0=Sun .. bit6=Sat; 0x7F = every day
  bool    useDate;
  uint8_t dateDay, dateMonth;
};
AlarmCfg alarms[MAX_ALARMS] = {
  {7,  0, false, false, 0x7F, false, 1, 1},
  {12, 0, false, false, 0x7F, false, 1, 1},
  {18, 0, false, false, 0x7F, false, 1, 1}
};
int  alarmSel       = 0;
bool alarmInEdit    = false;
int  alarmEditField = 0;
bool alarmRinging   = false;
unsigned long alarmRingAt = 0;

// ── Voice recorder ────────────────────────────────────────────────────────────
#define REC_RATE     8000
#define REC_SECONDS  3
#define REC_SAMPLES  (REC_RATE * REC_SECONDS)   // 24000 → 48KB
enum VoiceState { VC_IDLE, VC_RECORDING, VC_PLAYING, VC_DONE };
VoiceState voiceState = VC_IDLE;
int16_t    recBuffer[REC_SAMPLES];
size_t     recLen = 0;
int        vuLevel = 0;  // 0-100, for VU meter display

// ── Battery smoothing ─────────────────────────────────────────────────────────
int   battSmoothed   = -1;          // -1 = not yet sampled
unsigned long lastBattRead = 0;
#define BATT_SMOOTH_ALPHA 35        // EMA alpha out of 100 — faster response for small cell

// ── Battery diagnostic log ────────────────────────────────────────────────────
#define BATT_LOG_SIZE 60            // one sample per minute = 1 hour window
int8_t   battLog[BATT_LOG_SIZE];
uint16_t battLogSec[BATT_LOG_SIZE]; // seconds since boot at each log entry
int      battLogHead  = 0;          // next write position (ring buffer)
int      battLogCount = 0;          // how many valid samples
unsigned long lastBattLog = 0;

// ── AOD (always-on display) ───────────────────────────────────────────────────
bool aodEnabled = false;
bool aodActive  = false;

// ── Data logger ───────────────────────────────────────────────────────────────
bool     loggingEnabled = false;
#define  LOG_FILE       "/sidecar.log"
#define  LOG_MAX_BYTES  32768   // 32KB rolling — older entries trimmed on overflow

void logMsg(const char* fmt, ...) {
  if(!loggingEnabled) return;
  char buf[160];
  va_list args; va_start(args, fmt); vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);

  // Timestamp from RTC
  auto dt = M5.Rtc.getDateTime();
  char line[180];
  snprintf(line, sizeof(line), "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
    dt.date.year, dt.date.month, dt.date.date,
    dt.time.hours, dt.time.minutes, dt.time.seconds, buf);

  Serial.print(line);   // always mirror to serial

  if(!LittleFS.begin(false)) return;
  // Roll log if too large
  if(LittleFS.exists(LOG_FILE)) {
    File f = LittleFS.open(LOG_FILE, "r");
    size_t sz = f ? f.size() : 0;
    f.close();
    if(sz > LOG_MAX_BYTES) {
      // Read tail half, rewrite
      File src = LittleFS.open(LOG_FILE, "r");
      if(src) {
        src.seek(sz / 2);
        String tail = src.readString();
        src.close();
        File dst = LittleFS.open(LOG_FILE, "w");
        if(dst) { dst.print(tail); dst.close(); }
      }
    }
  }
  File f = LittleFS.open(LOG_FILE, "a");
  if(f) { f.print(line); f.close(); }
}

void logDump() {
  // Dump entire log to Serial — call by sending 'D' over USB
  Serial.println("──── SIDECAR LOG DUMP ────");
  if(!LittleFS.begin(false) || !LittleFS.exists(LOG_FILE)) {
    Serial.println("(no log file)"); return;
  }
  File f = LittleFS.open(LOG_FILE, "r");
  if(!f) { Serial.println("(open failed)"); return; }
  while(f.available()) Serial.write(f.read());
  f.close();
  Serial.println("\n──── END OF LOG ────");
}

void logClear() {
  if(!LittleFS.begin(false)) return;
  LittleFS.remove(LOG_FILE);
  Serial.println("[LOG] cleared");
}

// ── Sound profile ─────────────────────────────────────────────────────────────
// 0=SILENT  1=GENERAL (button clicks + alarm)
enum SoundProfile { SND_SILENT=0, SND_GENERAL=1 };
SoundProfile soundProfile = SND_GENERAL;

// Plays a short click if profile allows. Call on any confirmed button action.
void uiClick(int freq=1800, int dur=18) {
  if(soundProfile == SND_SILENT) return;
  M5.Speaker.tone(freq, dur);
}
// Softer tick for rapid-fire (settings scrolling)
void uiTick() { uiClick(1400, 10); }

// ── Buttons ───────────────────────────────────────────────────────────────────
unsigned long btnAAt=0, btnBAt=0, btnPAt=0;
bool btnALong=false, btnBLong=false, btnPLong=false;
#define LONG_MS 700

// ── Connect tab panel toggle (0=WiFi, 1=BT) ──────────────────────────────────
int connectPanel = 0;

int stepCount = 0;
const char* MON_NAMES[] = {"","JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};
const char* DOW_NAMES[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};

// ── Brightness lookup (level 0-5 → raw 0-200) ────────────────────────────────
// Level 0 = true off (0). Perceptually even steps — human eye is logarithmic.
static const uint8_t BRT_LUT[6] = {0, 12, 35, 70, 120, 200};
inline int brtValue(int lvl) { return BRT_LUT[constrain(lvl, 0, 5)]; }

// ── Helpers ───────────────────────────────────────────────────────────────────
int daysInMonth(int m, int y) {
  if(m<1||m>12) return 30;
  if(m==2) return ((y%4==0)&&(y%100!=0||y%400==0))?29:28;
  const int d[]={0,31,28,31,30,31,30,31,31,30,31,30,31};
  return d[m];
}

// Tomohiko Sakamoto weekday: returns 0=Sun..6=Sat
int calcDow(int y, int m, int d) {
  static const int t[]={0,3,2,5,0,3,5,1,4,6,2,4};
  if(m<3) y--;
  return (y+y/4-y/100+y/400+t[m-1]+d)%7;
}

void fmtMs(unsigned long ms, char* buf, int len) {
  snprintf(buf,len,"%02d:%02d.%02d",(int)((ms/60000)%60),(int)((ms/1000)%60),(int)((ms/10)%100));
}

// ── Notification push ─────────────────────────────────────────────────────────
void pushNotif(const char* app, const char* title, const char* body) {
  if(notifCount>=MAX_NOTIFS) {
    for(int i=0;i<MAX_NOTIFS-1;i++) notifs[i]=notifs[i+1];
    notifCount=MAX_NOTIFS-1;
  }
  Notif& n=notifs[notifCount++];
  strncpy(n.app,app,19);   n.app[19]=0;
  strncpy(n.title,title,39); n.title[39]=0;
  strncpy(n.body,body,59);  n.body[59]=0;
  n.unread=true; unreadCount++;
  bannerActive=true; bannerAt=millis();
  displayDirty = true;
  if(screenOn) M5.Speaker.tone(1200,80);
}

// ── BLE ───────────────────────────────────────────────────────────────────────
class BleCharCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string raw = c->getValue();
    if(raw.empty()) return;
    String v = String(raw.c_str());
    if(v.length()<3 || v.charAt(1)!='|') return;
    char t = v.charAt(0);
    if(t=='T') {
      // T|unix_timestamp
      time_t ts = (time_t)v.substring(2).toInt();
      if(ts>1000000000L) { struct timeval tv; tv.tv_sec=ts; tv.tv_usec=0; settimeofday(&tv,NULL); }
    } else if(t=='N') {
      // N|APP|TITLE|BODY
      int p1=v.indexOf('|',2), p2=p1>0?v.indexOf('|',p1+1):-1;
      if(p1>0&&p2>0)
        pushNotif(v.substring(2,p1).c_str(), v.substring(p1+1,p2).c_str(), v.substring(p2+1).c_str());
    }
    // S|artist|song and C|name handled in future BLE module
  }
};
class BleSrvCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, ble_gap_conn_desc* d) override {
    bleConn = true;
    bleConnHandle = d->conn_handle;
    logMsg("BLE connected handle=%d", d->conn_handle);
    // Connection interval 80–160 slots × 1.25ms = 100–200ms, latency=0, timeout=300 (3s).
    // Latency=0 means phone wakes every interval — ensures notifications arrive promptly.
    // (Previous 400/400/4 = 2.5s effective latency made BLE feel completely broken.)
    s->updateConnParams(d->conn_handle, 80, 160, 0, 300);
    displayDirty = true;
  }
  void onDisconnect(NimBLEServer*) override {
    bleConn = false;
    bleConnHandle = BLE_HS_CONN_HANDLE_NONE;
    logMsg("BLE disconnected");
    NimBLEDevice::startAdvertising();
    displayDirty = true;
  }
};

// Set BLE advertising interval based on power mode.
// Units are 0.625ms. 160=100ms (fast), 800=500ms (normal), 1600=1000ms (sleep).
void setBleAdvInterval(int minSlots, int maxSlots) {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setMinInterval(minSlots);
  adv->setMaxInterval(maxSlots);
}

void initBLE() {
  NimBLEDevice::init("SIDECAR V1");
  NimBLEDevice::setMTU(256);
  // Layer 6: -12dBm TX power — plenty for 1-2m wearable use, saves ~3mA
  NimBLEDevice::setPower(ESP_PWR_LVL_N12);
  pBSrv = NimBLEDevice::createServer();
  pBSrv->setCallbacks(new BleSrvCB());
  NimBLEService* svc = pBSrv->createService(BLE_SVC);
  pBChr = svc->createCharacteristic(BLE_CHR,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
  pBChr->setCallbacks(new BleCharCB());
  svc->start();
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SVC);
  // Layer 2: 500ms advertising interval by default (vs 100ms default = 5× less radio)
  setBleAdvInterval(800, 800);
  adv->start();
}

// ── WiFi AP ───────────────────────────────────────────────────────────────────
const char WIFI_HTML[] PROGMEM = R"html(<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>body{font-family:monospace;background:#0a0a0a;color:#fb6000;padding:24px;max-width:420px;margin:0 auto}
h2{letter-spacing:4px}.ssid{color:#fff;padding:8px 0;margin-bottom:14px;border-bottom:1px solid #333}
input{display:block;background:#1a1a1a;color:#fb6000;border:1px solid #fb6000;padding:10px;width:100%;
box-sizing:border-box;font-size:15px;margin:8px 0;border-radius:4px}
button{background:#fb6000;color:#000;border:none;padding:12px;width:100%;font-size:16px;
font-weight:bold;margin-top:10px;border-radius:4px}</style></head><body>
<h2>SIDECAR V1</h2>
<div class="ssid">Network: <b>%SSID%</b></div>
<form method="POST" action="/save">
<input type="hidden" name="ssid" value="%SSID%">
<input type="password" name="pass" placeholder="WiFi Password" autofocus>
<button type="submit">&#9654; Connect</button></form>
</body></html>)html";

void wifiHandleRoot() {
  String pg=FPSTR(WIFI_HTML); pg.replace("%SSID%",wifiSelSSID);
  wifiSrv.send(200,"text/html",pg);
}
void wifiHandleSave() {
  String ssid=wifiSrv.arg("ssid"), pass=wifiSrv.arg("pass");
  if(ssid.length()==0) ssid=wifiSelSSID;
  prefs.begin("wifi",false); prefs.putString("ssid",ssid); prefs.putString("pass",pass); prefs.end();
  wifiSrv.send(200,"text/html","<html><body style='background:#0a0a0a;color:#0f0;font-family:monospace;padding:20px'><h2>Saved! Reconnecting...</h2></body></html>");
  vTaskDelay(pdMS_TO_TICKS(500));
  // Tear down AP
  wifiSrv.stop(); WiFi.softAPdisconnect(true); wifiAPActive=false;
  // Try connecting with saved creds — brief attempt
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long t0 = millis();
  while(WiFi.status() != WL_CONNECTED && millis()-t0 < 5000) vTaskDelay(pdMS_TO_TICKS(100));
  Serial.printf("[WIFI] connect result: %d\n", WiFi.status());
  logMsg("wifi creds saved ssid=%s result=%d", ssid.c_str(), WiFi.status());
  // Tear down WiFi, reinit BLE
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  Serial.printf("[WIFI] WiFi off, heap=%d, reinit BLE\n", ESP.getFreeHeap());
  initBLE(); bleInited = true;
  wifiSt = WF_IDLE; displayDirty = true;
}

void drawHeader(const char* title);  // forward declaration
// ── Mutual Exclusion WiFi ────────────────────────────────────────────────────
// ESP32 has only 22KB heap left after WiFi+BLE are both up. WiFi scan needs
// ~30KB+ for buffers. Solution: deinit BLE → do WiFi → deinit WiFi → reinit BLE.
// BLE is offline for ~5-10s during WiFi operations. This is the Watchy approach.
void wifiStartScan() {
  // Show "scanning" immediately
  canvas.fillSprite(COL_BG);
  drawHeader("WiFi SCAN");
  canvas.setTextSize(1); canvas.setTextColor(NIXIE_DIM);
  canvas.setCursor(60, 60); canvas.print("Scanning...");
  canvas.setTextColor(NIXIE_GHOST);
  canvas.setCursor(40, 80); canvas.print("BLE offline during scan");
  canvas.pushSprite(0,0);

  // Step 1: Tear down BLE — frees ~21KB
  Serial.printf("[WIFI] pre-deinit heap=%d\n", ESP.getFreeHeap());
  NimBLEDevice::deinit(true);
  bleInited = false; bleConn = false;
  vTaskDelay(pdMS_TO_TICKS(200));
  Serial.printf("[WIFI] post-deinit heap=%d\n", ESP.getFreeHeap());

  // Step 2: Init WiFi
  WiFi.mode(WIFI_STA);
  vTaskDelay(pdMS_TO_TICKS(100));
  Serial.printf("[WIFI] WiFi up, heap=%d\n", ESP.getFreeHeap());

  // Step 3: Blocking scan (~3-5 seconds)
  int n = WiFi.scanNetworks();
  Serial.printf("[WIFI] found %d networks\n", n);

  wifiNetCount = constrain(n, 0, MAX_NETS);
  for(int i=0; i<wifiNetCount; i++) {
    wifiSSIDs[i]  = WiFi.SSID(i);
    wifiRSSI[i]   = WiFi.RSSI(i);
    wifiIsOpen[i] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
  }
  WiFi.scanDelete();
  wifiListIdx = 0;

  if(wifiNetCount > 0) {
    // Keep WiFi up for AP setup — BLE stays down until WiFi flow completes
    wifiSt = WF_LIST;
  } else {
    // No networks — tear down WiFi, reinit BLE
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
    initBLE(); bleInited = true;
    Serial.printf("[WIFI] no networks, BLE back, heap=%d\n", ESP.getFreeHeap());
    wifiSt = WF_IDLE;
  }
  displayDirty = true;
}

// Connect to an open (password-free) network directly — no AP portal needed.
// WiFi is already initialized from wifiStartScan. BLE is already down.
void connectOpenWifi(const String& ssid) {
  canvas.fillSprite(COL_BG);
  drawHeader("WiFi CONNECTING");
  canvas.setTextSize(1); canvas.setTextColor(NIXIE_DIM);
  canvas.setCursor(4, 50); canvas.printf("%.30s", ssid.c_str());
  canvas.setCursor(4, 65); canvas.setTextColor(NIXIE_GHOST);
  canvas.print("open network  BLE offline");
  canvas.pushSprite(0,0);

  WiFi.begin(ssid.c_str());   // no passphrase = open network
  unsigned long t0 = millis();
  while(WiFi.status() != WL_CONNECTED && millis()-t0 < 8000) vTaskDelay(pdMS_TO_TICKS(200));

  bool ok = (WiFi.status() == WL_CONNECTED);
  logMsg("wifi open connect ssid=%s %s", ssid.c_str(), ok?"ok":"failed");
  if(ok) { prefs.begin("wifi",false); prefs.putString("ssid",ssid); prefs.putString("pass",""); prefs.end(); }

  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  initBLE(); bleInited = true;
  wifiSt = WF_IDLE; displayDirty = true;
}

void startAP() {
  // WiFi is already initialized from the scan — just start the AP
  WiFi.softAP("SIDECAR-V1");
  vTaskDelay(pdMS_TO_TICKS(150));
  wifiSrv.on("/",HTTP_GET,wifiHandleRoot);
  wifiSrv.on("/save",HTTP_POST,wifiHandleSave);
  wifiSrv.onNotFound(wifiHandleRoot);
  wifiSrv.begin(); wifiAPActive=true; wifiSt=WF_AP;
  displayDirty = true;
}

// ── Power management ──────────────────────────────────────────────────────────

// Safe CPU frequency change: NimBLE stack is sensitive to runtime frequency changes.
// Once BLE is initialized, NEVER lower the freq — it crashes the RF subsystem.
// Raising back to 240 is safe because we never lower it after init.
static void safeCpuFreq(int mhz) {
  if((int)getCpuFrequencyMhz() == mhz) return;  // already there, no-op
  if(mhz < (int)getCpuFrequencyMhz() && bleInited) return;  // can't lower after BLE init
  setCpuFrequencyMhz(mhz);
}

// NOTE: esp_pm_configure() was removed. The precompiled arduino-esp32 SDK has
// CONFIG_PM_ENABLE=n, so esp_pm_configure() returns ESP_ERR_NOT_SUPPORTED.
// Light sleep via PM never actually worked. vTaskDelay() still yields CPU time.

void applyPowerMode(PowerMode m) {
  powerMode   = m;
  brightLevel = pwrCfg[m].brightness;
  int brt     = brtValue(brightLevel);

  switch(m) {
    case PWR_NORMAL:
      safeCpuFreq(240);
      screenOn = true; M5.Display.setBrightness(brt);
      // Layer 1: NORMAL — no light sleep (display + WiFi + BLE active, timing-sensitive)

      // Layer 2: fast advertising so phone reconnects quickly
      if(bleInited && !bleConn) setBleAdvInterval(160, 160);
      break;

    case PWR_EFFICIENT:
      safeCpuFreq(80);
      screenOn = true; M5.Display.setBrightness(brt);

      // Layer 1: light sleep between ticks, min 40MHz

      // Layer 2: 500ms adv interval
      if(bleInited && !bleConn) setBleAdvInterval(800, 800);
      break;

    case PWR_EXTREME:
      safeCpuFreq(80);
      screenOn = true; M5.Display.setBrightness(brt);

      lastActivity = millis();
      // Layer 1: aggressive light sleep, min 10MHz

      // Layer 2: 1s adv interval
      if(bleInited && !bleConn) setBleAdvInterval(1600, 1600);
      break;

    case PWR_SLEEP:
      safeCpuFreq(80);
      M5.Display.setBrightness(brt);

      // Layer 1: deepest light sleep, min 10MHz

      // Layer 2: 1s adv interval
      if(bleInited && !bleConn) setBleAdvInterval(1600, 1600);
      break;
  }

  lastActiveMode = m;
  EEPROM.write(EE_PWRMODE, (uint8_t)m); EEPROM.commit();
  logMsg("power mode -> %s heap=%d", PWR_NAMES[m], ESP.getFreeHeap());
  lastActivity = millis();
  displayDirty = true;
}

// Sleep = screen off only. Wake = any button press.

// ── Settings field increment ──────────────────────────────────────────────────
void incrementSetting(int idx, int by) {
  auto dt = M5.Rtc.getDateTime();
  int yr=dt.date.year, mo=dt.date.month;
  switch(idx) {
    case 0: dt.time.hours=(dt.time.hours+by+24)%24; break;
    case 1: dt.time.minutes=(dt.time.minutes+by+60)%60; break;
    case 2: { int md=daysInMonth(mo,yr);
              dt.date.date=(((int)dt.date.date-1+by+md)%md)+1; break; }
    case 3: dt.date.month=(((int)dt.date.month-1+by+12)%12)+1;
            if(dt.date.date>daysInMonth(dt.date.month,yr)) dt.date.date=daysInMonth(dt.date.month,yr);
            break;
    case 4: { int y=(int)dt.date.year+by; if(y>2099) y=2000; if(y<2000) y=2099; dt.date.year=y; break; }
    case 5: { // Power mode — cycle through all 4
      PowerMode next=(PowerMode)(((int)powerMode+(by>0?1:PWR_MODE_COUNT-1))%PWR_MODE_COUNT);
      applyPowerMode(next); return; }
    case 6: brightLevel=((brightLevel+by)+6)%6;  // wraps 0-5
            pwrCfg[powerMode].brightness = brightLevel;  // keep mode config in sync
            M5.Display.setBrightness(brtValue(brightLevel));
            EEPROM.write(EE_BRIGHT,brightLevel); EEPROM.commit(); return;
    case 7: autoRotate=!autoRotate;
            EEPROM.write(EE_AUTOROT,autoRotate?1:0); EEPROM.commit(); return;
    case 8: aodEnabled=!aodEnabled;
            EEPROM.write(EE_AOD, aodEnabled?1:0); EEPROM.commit(); return;
    case 9: soundProfile=(soundProfile==SND_SILENT)?SND_GENERAL:SND_SILENT;
            EEPROM.write(EE_SOUND,(uint8_t)soundProfile); EEPROM.commit();
            if(soundProfile==SND_GENERAL) uiClick(1800,30);
            return;
    case 10: loggingEnabled=!loggingEnabled;
             EEPROM.write(EE_LOGGING, loggingEnabled?1:0); EEPROM.commit();
             if(loggingEnabled) logMsg("logging enabled");
             else               Serial.println("[LOG] logging disabled");
             return;
  }
  M5.Rtc.setDateTime(dt);
}

// ── Auto-rotate ───────────────────────────────────────────────────────────────
void checkRotation() {
  // Layer 4: skip IMU I2C poll when screen is off — saves I2C bus + MPU6886 read current
  if(!autoRotate || !screenOn || millis()-lastRotCheck<500) return;
  lastRotCheck=millis();
  float ax,ay,az; M5.Imu.getAccel(&ax,&ay,&az);
  int nr=curRotation;
  if(ax>0.3f) nr=1; else if(ax<-0.3f) nr=3;
  if(nr!=curRotation) { curRotation=nr; M5.Display.setRotation(curRotation); displayDirty=true; }
}



// ── Button handling ───────────────────────────────────────────────────────────
void cancelWifi() {
  if(wifiAPActive) { wifiSrv.stop(); WiFi.softAPdisconnect(true); wifiAPActive=false; }
  wifiSt = WF_IDLE;
  WiFi.scanDelete();
  wifiAutoKilled=false; wifiFailingSince=0;
  // If BLE was torn down for WiFi, bring it back
  if(!bleInited) {
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
    Serial.printf("[WIFI] cancel: reinit BLE, heap=%d\n", ESP.getFreeHeap());
    initBLE(); bleInited = true;
  }
  displayDirty = true;
}

void saveAlarms();                                  // forward declaration
float battLifeHours(int modeIdx);                   // forward declaration
void  fmtBattLife(float h, char* buf, int len);     // forward declaration
int   getBatt();                                    // forward declaration
void  uiClick(int freq, int dur);                   // forward declaration
void  uiTick();                                     // forward declaration
void handleButtons() {
  if(M5.BtnA.wasPressed())   { btnAAt=millis(); btnALong=false; lastActivity=millis(); uiClick(1800,15); }
  if(M5.BtnB.wasPressed())   { btnBAt=millis(); btnBLong=false; lastActivity=millis(); uiClick(1600,15); }
  if(M5.BtnPWR.wasPressed()) { btnPAt=millis(); btnPLong=false; lastActivity=millis(); uiClick(1400,15); }

  // Any press wakes the screen — consume the event, don't act on it
  if(!screenOn) {
    if(M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnPWR.wasPressed()) {
      screenOn=true;
      if(displaySleeping) { M5.Display.wakeup(); displaySleeping=false; }
      M5.Display.setBrightness(brtValue(brightLevel));
      lastActivity=millis();
    }
    return;
  }

  // Dismiss banner on any press
  if(bannerActive && (M5.BtnA.wasPressed()||M5.BtnB.wasPressed()||M5.BtnPWR.wasPressed()))
    bannerActive=false;

  // Fast-increment: hold BtnA in Settings
  if(curScreen==SCR_SETTINGS && M5.BtnA.isPressed()
     && (millis()-btnAAt)>500 && (millis()-lastFastTick)>FAST_STEP_MS) {
    lastFastTick=millis(); btnALong=true;
    uiTick();
    incrementSetting(settingIdx,1); return;
  }

  // ── BtnPWR (TOP) — secondary / navigation action ─────────────────────────
  // Safe for short presses; hardware shutoff only fires after ~4s hold.
  if(M5.BtnPWR.wasReleased()) {
    bool lng=(millis()-btnPAt)>=LONG_MS;
    lastActivity=millis();
    switch(curScreen) {
      case SCR_CLOCK:
        // Short: cycle power mode   Long: nothing
        if(!lng) {
          PowerMode next=(PowerMode)(((int)powerMode+1)%PWR_MODE_COUNT);
          applyPowerMode(next);
        }
        break;
      case SCR_STOPWATCH:
        // Short: lap while running
        if(!lng && swRunning && lapCount<MAX_LAPS) {
          unsigned long n=millis(); lapSplits[lapCount++]=n-lapStart; lapStart=n;
        }
        break;
      case SCR_TIMER:
        // Short: +1 min   Long: -1 min   (only while in IDLE/setting)
        if(timerState==TMR_IDLE) {
          if(!lng) timerSetMin=min(timerSetMin+1,99);
          else     timerSetMin=max(timerSetMin-1,1);
        }
        break;
      case SCR_CONNECT:
        if(wifiSt==WF_LIST) {
          // Scroll list: short=down, long=up
          if(!lng && wifiListIdx<wifiNetCount-1) wifiListIdx++;
          if( lng && wifiListIdx>0)              wifiListIdx--;
        } else {
          // Toggle between WiFi panel and BT panel
          connectPanel=(connectPanel+1)%2;
        }
        break;
      case SCR_POWER:
        if(pwrInCustom) {
          // Navigate customization fields
          if(!lng) pwrCustIdx=(pwrCustIdx+1)%PWRCFG_COUNT;
          else     pwrCustIdx=(pwrCustIdx+PWRCFG_COUNT-1)%PWRCFG_COUNT;
        } else {
          // Cycle highlighted mode card
          pwrTabSel=(pwrTabSel+1)%PWR_MODE_COUNT;
        }
        break;
      case SCR_SETTINGS:
        // Short: next field (down)   Long: prev field (up)
        if(!lng) settingIdx=(settingIdx+1)%SETTING_COUNT;
        else     settingIdx=(settingIdx+SETTING_COUNT-1)%SETTING_COUNT;
        break;
      case SCR_ALARM:
        if(alarmInEdit) {
          // Short: next field   Long: prev field
          if(!lng) alarmEditField = (alarmEditField + 1) % ALARM_EDIT_FIELDS;
          else     alarmEditField = (alarmEditField + ALARM_EDIT_FIELDS - 1) % ALARM_EDIT_FIELDS;
        } else {
          // Short: next alarm   Long: prev alarm
          if(!lng) alarmSel = (alarmSel + 1) % MAX_ALARMS;
          else     alarmSel = (alarmSel + MAX_ALARMS - 1) % MAX_ALARMS;
        }
        break;
      case SCR_VOICE:
        // TOP does nothing during voice
        break;
      case SCR_NOTIFS:
        // Short: scroll down   Long: scroll up
        if(!lng && notifIdx<notifCount-1) notifIdx++;
        if(lng  && notifIdx>0)            notifIdx--;
        break;
    }
    return;
  }

  // ── BtnA (FRONT) — primary action ────────────────────────────────────────
  if(M5.BtnA.wasReleased() && !btnALong) {
    bool lng=(millis()-btnAAt)>=LONG_MS;
    lastActivity=millis();

    switch(curScreen) {
      case SCR_CLOCK:
        // Short: brightness up   Long: go home (already home, so do nothing)
        if(!lng) {
          brightLevel=(brightLevel+1)%6;
          M5.Display.setBrightness(brtValue(brightLevel));
          EEPROM.write(EE_BRIGHT,brightLevel); EEPROM.commit();
        }
        break;

      case SCR_STOPWATCH:
        if(!lng) {
          // Short: start / stop
          if(!swRunning) { swStart=millis()-swElapsed; lapStart=millis(); swRunning=true; }
          else           { swElapsed=millis()-swStart; swRunning=false; }
        } else {
          // Long: reset to zero
          swRunning=false; swElapsed=0; lapStart=0; lapCount=0;
        }
        break;

      case SCR_TIMER:
        // Front button: start / pause / resume / dismiss
        if(!lng) {
          if(timerState==TMR_IDLE)
            { timerEndMs=millis()+(unsigned long)timerSetMin*60000UL; timerState=TMR_RUNNING; }
          else if(timerState==TMR_RUNNING)
            { timerRemaining=timerEndMs>millis()?timerEndMs-millis():0; timerState=TMR_PAUSED; }
          else if(timerState==TMR_PAUSED)
            { timerEndMs=millis()+timerRemaining; timerState=TMR_RUNNING; }
          else if(timerState==TMR_DONE)
            timerState=TMR_IDLE;
        } else {
          // Long: cancel timer
          timerState=TMR_IDLE;
        }
        break;

      case SCR_POWER:
        if(pwrInCustom) {
          // Adjust the selected field of the selected mode
          PwrCfg& c = pwrCfg[pwrTabSel];
          if(!lng) {
            switch(pwrCustIdx) {
              case 0: c.brightness=((c.brightness+1)+6)%6; break;
              case 1: { const int opts[]={5,10,20,30,60,120,0};
                        for(int i=0;i<7;i++) if(opts[i]==c.timeoutSec){ c.timeoutSec=opts[(i+1)%7]; break; }
                        break; }
              case 2: c.wifiOff=!c.wifiOff; break;
            }
          } else {
            // Long = back to mode list; reapply if editing active mode
            pwrInCustom=false;
            if(pwrTabSel==(int)powerMode) applyPowerMode(powerMode);
          }
        } else {
          if(!lng) {
            // Short: activate highlighted mode
            applyPowerMode((PowerMode)pwrTabSel);
          } else {
            // Long: enter customization sub-screen
            pwrInCustom=true; pwrCustIdx=0;
          }
        }
        break;

      case SCR_CONNECT:
        if(connectPanel==0) {
          // WiFi panel — mutual exclusion: BLE goes down during WiFi
          if(wifiSt==WF_IDLE) {
            Serial.printf("[BTN] WiFi scan requested, heap=%d\n", ESP.getFreeHeap());
            wifiAutoKilled=false; wifiFailingSince=0;
            wifiStartScan();  // blocks ~5s, tears down BLE, does scan, shows list
          } else if(wifiSt==WF_LIST && wifiNetCount>0) {
            wifiSelSSID=wifiSSIDs[wifiListIdx];
            if(wifiIsOpen[wifiListIdx]) connectOpenWifi(wifiSelSSID);
            else                        startAP();
          } else if(wifiSt==WF_AP) {
            cancelWifi();
          } else if(wifiSt==WF_LIST) {
            cancelWifi();
          }
        } else {
          // BT panel — restart advertising so phone can re-pair
          if(bleInited && !bleConn) {
            NimBLEDevice::stopAdvertising();
            vTaskDelay(pdMS_TO_TICKS(100));
            NimBLEDevice::startAdvertising();
            pushNotif("BLE","Advertising restarted","Open Bellafaire app on phone");
          }
          displayDirty = true;
        }
        break;

      case SCR_SETTINGS:
        // Short: increment current field   Long: decrement
        if(!lng) incrementSetting(settingIdx,1);
        else     incrementSetting(settingIdx,-1);
        break;

      case SCR_ALARM:
        if(alarmRinging) {
          alarmRinging = false;
          M5.Speaker.stop();
        } else if(alarmInEdit) {
          AlarmCfg& a = alarms[alarmSel];
          if(!lng) {
            // Short: +1 / toggle on active field
            if     (alarmEditField == 0)  a.hour    = (a.hour + 1) % 24;
            else if(alarmEditField == 1)  a.minute  = (a.minute + 1) % 60;
            else if(alarmEditField >= 2 && alarmEditField <= 8) {
              // Toggle day bit (fields 2-8 = Sun-Sat, bit 0-6)
              int bit = alarmEditField - 2;
              a.daysOfWeek ^= (1 << bit);
              if(a.daysOfWeek == 0) a.daysOfWeek = 1 << bit; // keep at least one
            }
            else if(alarmEditField == 9)  a.useDate = !a.useDate;
            else if(alarmEditField == 10) {
              a.dateDay++;
              if(a.dateDay > (uint8_t)daysInMonth(a.dateMonth, M5.Rtc.getDateTime().date.year)) a.dateDay = 1;
            }
            else if(alarmEditField == 11) {
              a.dateMonth = (a.dateMonth % 12) + 1;
              if(a.dateDay > (uint8_t)daysInMonth(a.dateMonth, M5.Rtc.getDateTime().date.year)) a.dateDay = 1;
            }
            saveAlarms();
          } else {
            // Long: back to list
            alarmInEdit = false;
            saveAlarms();
          }
        } else {
          if(!lng) {
            alarms[alarmSel].enabled = !alarms[alarmSel].enabled;
            saveAlarms();
          } else {
            alarmInEdit = true; alarmEditField = 0;
          }
        }
        break;

      case SCR_VOICE:
        if(voiceState == VC_IDLE || voiceState == VC_DONE) {
          if(!lng) {
            // Short: start recording
            recLen = 0; vuLevel = 0;
            M5.Mic.begin();
            voiceState = VC_RECORDING;
          } else {
            // Long: play last clip
            if(recLen > 0) {
              voiceState = VC_PLAYING;
              M5.Speaker.playRaw(recBuffer, recLen, REC_RATE, false, 1, 0);
              voiceState = VC_DONE;
            }
          }
        } else if(voiceState == VC_RECORDING) {
          // Short: stop recording early
          M5.Mic.end();
          voiceState = (recLen > 0) ? VC_DONE : VC_IDLE;
        }
        break;

      case SCR_NOTIFS:
        if(!lng) {
          // Short: dismiss selected
          if(notifCount>0 && notifIdx<notifCount) {
            for(int i=notifIdx;i<notifCount-1;i++) notifs[i]=notifs[i+1];
            notifCount--;
            if(notifIdx>=notifCount && notifIdx>0) notifIdx--;
            unreadCount=0;
            for(int i=0;i<notifCount;i++) if(notifs[i].unread) unreadCount++;
          }
        } else {
          // Long: clear all
          notifCount=0; notifIdx=0; unreadCount=0;
        }
        break;

      case SCR_DIAG:
        if(lng) {
          // Long: clear log — restart diagnostic window
          battLogHead=0; battLogCount=0;
          lastBattLog = millis() - 61000UL;  // re-trigger immediately
          uiClick(1200, 30);
        }
        break;
    }
  }

  // ── BtnB (SIDE) — TABS ONLY, always ──────────────────────────────────────
  if(M5.BtnB.wasReleased()) {
    bool lng=(millis()-btnBAt)>=LONG_MS;
    lastActivity=millis();
    cancelWifi();  // also reinits BLE if it was torn down for WiFi
    if(lng) curScreen=(Screen)((curScreen+SCR_COUNT-1)%SCR_COUNT);
    else    curScreen=(Screen)((curScreen+1)%SCR_COUNT);
    settingIdx=0; notifIdx=0; connectPanel=0; pwrInCustom=false; pwrTabSel=(int)powerMode;
    alarmInEdit=false; alarmSel=0;
    if(voiceState==VC_RECORDING){ M5.Mic.end(); voiceState=VC_IDLE; }
    if(curScreen==SCR_NOTIFS) {
      for(int i=0;i<notifCount;i++) notifs[i].unread=false;
      unreadCount=0;
    }
  }
}

// ── Alarm helpers ────────────────────────────────────────────────────────────
void saveAlarms() {
  for(int i = 0; i < MAX_ALARMS; i++) {
    int b = EE_ALARM_BASE + i * EE_ALARM_BYTES;
    EEPROM.write(b,   alarms[i].hour);
    EEPROM.write(b+1, alarms[i].minute);
    EEPROM.write(b+2, alarms[i].enabled    ? 1 : 0);
    EEPROM.write(b+3, alarms[i].daysOfWeek);
    EEPROM.write(b+4, alarms[i].useDate    ? 1 : 0);
    EEPROM.write(b+5, alarms[i].dateDay);
    EEPROM.write(b+6, alarms[i].dateMonth);
  }
  EEPROM.commit();
}

void loadAlarms() {
  for(int i = 0; i < MAX_ALARMS; i++) {
    int b = EE_ALARM_BASE + i * EE_ALARM_BYTES;
    alarms[i].hour       = EEPROM.read(b);
    alarms[i].minute     = EEPROM.read(b+1);
    alarms[i].enabled    = EEPROM.read(b+2) != 0;
    alarms[i].daysOfWeek = EEPROM.read(b+3);
    alarms[i].useDate    = EEPROM.read(b+4) != 0;
    alarms[i].dateDay    = EEPROM.read(b+5);
    alarms[i].dateMonth  = EEPROM.read(b+6);
    alarms[i].fired      = false;
    if(alarms[i].hour    > 23)  alarms[i].hour       = 7;
    if(alarms[i].minute  > 59)  alarms[i].minute     = 0;
    if(alarms[i].daysOfWeek == 0) alarms[i].daysOfWeek = 0x7F;  // default: all days
    if(alarms[i].dateDay   < 1 || alarms[i].dateDay   > 31) alarms[i].dateDay   = 1;
    if(alarms[i].dateMonth < 1 || alarms[i].dateMonth > 12) alarms[i].dateMonth = 1;
  }
}

void checkAlarms() {
  auto dt  = M5.Rtc.getDateTime();
  int  dow = calcDow(dt.date.year, dt.date.month, dt.date.date);  // 0=Sun..6=Sat
  for(int i = 0; i < MAX_ALARMS; i++) {
    if(!alarms[i].enabled) { alarms[i].fired = false; continue; }
    if(!((alarms[i].daysOfWeek >> dow) & 1)) { alarms[i].fired = false; continue; }
    if(alarms[i].useDate) {
      if(dt.date.date != alarms[i].dateDay || dt.date.month != alarms[i].dateMonth)
        { alarms[i].fired = false; continue; }
    }
    if(dt.time.hours == alarms[i].hour && dt.time.minutes == alarms[i].minute) {
      if(!alarms[i].fired) {
        alarms[i].fired  = true;
        alarmRinging     = true;
        alarmRingAt      = millis();
        logMsg("alarm fired #%d %02d:%02d", i, alarms[i].hour, alarms[i].minute);
        // Force screen on and display bright
        screenOn         = true;
        aodActive        = false;
        M5.Display.setBrightness(200);
        M5.Speaker.setVolume(255);
        lastActivity     = millis();
      }
    } else {
      alarms[i].fired = false;
    }
  }

  // Ring: 3-beep ascending pattern, 2s cycle, always fires regardless of sound profile
  if(alarmRinging) {
    if(millis() - alarmRingAt > 60000UL) {
      alarmRinging = false;
      M5.Speaker.stop();
      return;
    }
    static int lastAlarmStep = -1;
    unsigned long cyclePos = (millis() - alarmRingAt) % 2000UL;
    int step = -1;
    if     (cyclePos <  250UL) step = 0;
    else if(cyclePos <  500UL) step = 1;
    else if(cyclePos <  750UL) step = 2;
    if(step >= 0 && step != lastAlarmStep) {
      lastAlarmStep = step;
      M5.Speaker.setVolume(255);
      const int freqs[] = {1100, 1400, 1900};
      M5.Speaker.tone(freqs[step], 200);
    }
    if(cyclePos >= 750UL) lastAlarmStep = -1;
  }
}

// ── Draw helpers ──────────────────────────────────────────────────────────────
void drawSignalBars(int x, int y, int rssi) {
  int bars=rssi>-65?3:(rssi>-75?2:1);
  for(int b=0;b<3;b++)
    canvas.fillRect(x+b*5, y-b*3, 3, 4+b*3, b<bars ? (uint32_t)COL_GREEN : (uint32_t)NIXIE_GHOST);
}

void drawBattBar(int x, int y, int pct) {
  int segs=map(constrain(pct,0,100),0,100,0,5);
  uint32_t col=pct>30?COL_GREEN:COL_RED;
  for(int i=0;i<5;i++)
    canvas.fillRect(x+i*8, y, 6, 5, i<segs?(uint32_t)col:(uint32_t)NIXIE_GHOST);
}

// Header: y=0–14, divider at y=14
void drawHeader(const char* title) {
  // Tab dots — 8 tabs × 10px spacing, centered
  int dotSpacing = 10;
  int dotx = W/2 - ((SCR_COUNT-1)*dotSpacing)/2;
  for(int i=0;i<SCR_COUNT;i++) {
    bool active=(i==(int)curScreen);
    canvas.fillCircle(dotx+i*dotSpacing, 7, active?3:2, active?(uint32_t)NIXIE_ORANGE:(uint32_t)NIXIE_GHOST);
  }

  // Title — clip so it never reaches the dots
  int dotLeft = dotx - 8;
  canvas.setTextSize(1); canvas.setTextColor(NIXIE_DIM);
  canvas.setCursor(4,3);
  // Print char by char, stop before overlapping dots
  for(const char* p=title; *p; p++) {
    if(canvas.getCursorX() + 6 > dotLeft) break;
    canvas.print(*p);
  }

  // Power mode icon — right of dot strip
  int dotRight = dotx + (SCR_COUNT-1)*dotSpacing + 6;
  const char* pi[]={"N","E","X","Z"};
  canvas.setTextColor(NIXIE_GHOST);
  canvas.setCursor(dotRight+2, 3); canvas.printf("[%s]", pi[powerMode]);

  // Unread notif dot — far right
  if(unreadCount>0) canvas.fillCircle(W-3, 4, 3, (uint32_t)COL_RED);

  canvas.drawFastHLine(0,14,W,NIXIE_GHOST);
}

// Hints bar — 2 rows at bottom, each col 120px wide
// Row1: T:<top>  |  F:<frontShort>
// Row2: F+:<frontLong> OR blank  |  S:tabs
void drawHints(const char* topAct, const char* frontAct, const char* frontLong=nullptr) {
  canvas.drawFastHLine(0,H-20,W,NIXIE_GHOST);
  canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);

  // Row 1
  char lbuf[22], rbuf[22];
  snprintf(lbuf,22,"T:%.16s",topAct);
  snprintf(rbuf,22,"F:%.16s",frontAct);
  canvas.setCursor(2, H-17);  canvas.print(lbuf);
  canvas.setCursor(122,H-17); canvas.print(rbuf);

  // Row 2
  if(frontLong) snprintf(lbuf,22,"F+:%.14s",frontLong);
  else          lbuf[0]=0;
  canvas.setTextColor(NIXIE_GHOST);
  if(lbuf[0]) { canvas.setCursor(2,H-8); canvas.print(lbuf); }
  canvas.setCursor(122,H-8); canvas.print("S:tabs");
}


// Notification banner overlay (drawn on top of current frame before pushSprite)
void drawBanner() {
  if(!bannerActive) return;
  if(millis()-bannerAt > BANNER_MS) { bannerActive=false; return; }
  int bn=notifCount-1;
  if(bn<0) { bannerActive=false; return; }
  canvas.fillRoundRect(2,H-42,W-4,22,3,(uint32_t)COL_BG);
  canvas.drawRoundRect(2,H-42,W-4,22,3,(uint32_t)COL_RED);
  canvas.setTextSize(1);
  canvas.setTextColor(COL_RED);   canvas.setCursor(7,H-39); canvas.print(notifs[bn].app);
  canvas.setTextColor(NIXIE_DIM); canvas.setCursor(7,H-28);
  // truncate to fit
  char t[36]; strncpy(t,notifs[bn].title,35); t[35]=0;
  canvas.print(t);
}

// ─────────────────────────────────────────────────────────────────────────────
// SCREEN: CLOCK
// ─────────────────────────────────────────────────────────────────────────────
void drawClock() {
  auto dt=M5.Rtc.getDateTime();
  int hh=dt.time.hours, mm=dt.time.minutes, ss=dt.time.seconds;
  int dd=dt.date.date, mo=dt.date.month, yr=dt.date.year;
  int dow=calcDow(yr,mo,dd);

  canvas.fillSprite(COL_BG);
  drawHeader("SIDECAR V1");

  // Left column: day + time + date
  canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
  canvas.setCursor(6,17); canvas.print(dow>=0&&dow<7?DOW_NAMES[dow]:"---");

  char hhmm[6]; snprintf(hhmm,sizeof(hhmm),"%02d:%02d",hh,mm);
  canvas.setTextSize(4); canvas.setTextColor(NIXIE_ORANGE);
  canvas.setCursor(6,25); canvas.print(hhmm);

  // Seconds
  char sb[5]; snprintf(sb,sizeof(sb),":%02d",ss);
  canvas.setTextSize(2); canvas.setTextColor(NIXIE_DIM);
  canvas.setCursor(6,66); canvas.print(sb);

  // Date — size 2 so it's clearly readable
  char db[16]; snprintf(db,sizeof(db),"%02d %s %04d", dd, (mo>=1&&mo<=12)?MON_NAMES[mo]:"???", yr);
  canvas.setTextSize(2); canvas.setTextColor(NIXIE_GHOST);
  canvas.setCursor(6,86); canvas.print(db);

  // Divider
  canvas.drawFastVLine(152,15,H-35,(uint32_t)NIXIE_GHOST);

  // Right column: steps + battery
  canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
  canvas.setCursor(158,17); canvas.print("STEPS");
  canvas.setTextSize(2); canvas.setTextColor(NIXIE_DIM);
  canvas.setCursor(158,26);
  if(stepCount<10000) canvas.printf("%d",stepCount);
  else               canvas.printf("%dk",stepCount/1000);

  int bat=getBatt();
  canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
  canvas.setCursor(158,58); canvas.print("BATTERY");
  canvas.setTextSize(2); canvas.setTextColor(bat>30?(uint32_t)COL_GREEN:(uint32_t)COL_RED);
  canvas.setCursor(158,67); canvas.printf("%d%%",bat);
  drawBattBar(158,88,bat);

  // BLE status
  canvas.setTextSize(1); canvas.setTextColor(bleConn?(uint32_t)COL_BLUE:(uint32_t)NIXIE_GHOST);
  canvas.setCursor(158,100); canvas.print(bleConn?"BLE ON":"BLE --");

  // Thin seconds progress line just above hints
  canvas.drawFastHLine(2,H-20,W-4,(uint32_t)NIXIE_GHOST);
  int spx=(int)((float)ss/59.0f*(W-6))+3;
  if(ss>0) canvas.drawFastHLine(3,H-20,spx-3,(uint32_t)NIXIE_ORANGE);

  drawHints("power mode","brightness");
}

// ─────────────────────────────────────────────────────────────────────────────
// SCREEN: STOPWATCH
// ─────────────────────────────────────────────────────────────────────────────
void drawStopwatch() {
  if(swRunning) swElapsed=millis()-swStart;
  canvas.fillSprite(COL_BG); drawHeader("STOPWATCH");

  // Big timer
  char tbuf[12]; fmtMs(swElapsed,tbuf,sizeof(tbuf));
  canvas.setTextSize(3); canvas.setTextColor(swRunning?(uint32_t)NIXIE_ORANGE:(uint32_t)NIXIE_DIM);
  int tw=canvas.textWidth(tbuf);
  canvas.setCursor((160-tw)/2+4, 17); canvas.print(tbuf);

  // Status tag
  canvas.setTextSize(1); canvas.setTextColor(swRunning?(uint32_t)NIXIE_ORANGE:(uint32_t)NIXIE_GHOST);
  canvas.setCursor(6,50); canvas.print(swRunning?"● RUNNING":"■ STOPPED");

  // Lap list (most recent first, up to 5 shown)
  if(lapCount>0) {
    canvas.drawFastHLine(4,57,148,(uint32_t)NIXIE_GHOST);
    int show=min(lapCount,4), y=60;
    for(int i=lapCount-1;i>=lapCount-show;i--) {
      char lbuf[14]; fmtMs(lapSplits[i],lbuf,sizeof(lbuf));
      bool newest=(i==lapCount-1);
      canvas.setTextSize(1); canvas.setTextColor(newest?(uint32_t)NIXIE_ORANGE:(uint32_t)NIXIE_GHOST);
      canvas.setCursor(6,y); canvas.printf("L%02d %s",i+1,lbuf);
      y+=14;
    }
  }

  // Right panel: button guide
  canvas.drawFastVLine(153,15,H-35,(uint32_t)NIXIE_GHOST);
  canvas.setTextSize(1);
  // Start/Stop
  canvas.fillRoundRect(157,18,80,18,3,swRunning?(uint32_t)NIXIE_DIM:(uint32_t)NIXIE_GHOST);
  canvas.setTextColor(swRunning?(uint32_t)NIXIE_ORANGE:(uint32_t)NIXIE_DIM);
  canvas.setCursor(165,24); canvas.print(swRunning?"■ STOP":"▶ START");
  // Lap
  canvas.fillRoundRect(157,42,80,18,3,(uint32_t)NIXIE_GHOST);
  canvas.setTextColor(swRunning?(uint32_t)NIXIE_DIM:(uint32_t)NIXIE_GHOST);
  canvas.setCursor(165,48); canvas.print("◎ LAP");
  // Reset
  canvas.fillRoundRect(157,66,80,18,3,(uint32_t)NIXIE_GHOST);
  canvas.setTextColor(!swRunning&&swElapsed>0?(uint32_t)NIXIE_DIM:(uint32_t)NIXIE_GHOST);
  canvas.setCursor(165,72); canvas.print("↺ RESET");
  canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
  canvas.setCursor(160,90); canvas.printf("%d laps",lapCount);

  drawHints(swRunning?"lap":"---","start/stop","reset");
}

// ─────────────────────────────────────────────────────────────────────────────
// SCREEN: TIMER
// ─────────────────────────────────────────────────────────────────────────────
void drawTimer() {
  canvas.fillSprite(COL_BG); drawHeader("TIMER");

  if(timerState==TMR_IDLE) {
    // Set screen: large MM:00 centered
    canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
    int lw=canvas.textWidth("HOLD FRONT TO START"); canvas.setCursor((W-lw)/2,17); canvas.print("HOLD FRONT TO START");

    char ss[6]; snprintf(ss,sizeof(ss),"%02d:00",timerSetMin);
    canvas.setTextSize(5); canvas.setTextColor(NIXIE_ORANGE);
    int tw=canvas.textWidth(ss); canvas.setCursor((W-tw)/2,30); canvas.print(ss);

    canvas.setTextSize(1); canvas.setTextColor(NIXIE_DIM);
    canvas.setCursor(6,90); canvas.print("SIDE: +1min  SIDE-long: -1min");
    drawHints("+1/-1 min","start timer");

  } else {
    unsigned long rem = (timerState==TMR_PAUSED) ? timerRemaining
                       : (timerEndMs>millis() ? timerEndMs-millis() : 0);

    // Check alarm
    if(timerState==TMR_RUNNING && rem==0) {
      timerState=TMR_DONE;
      M5.Speaker.tone(2000,300); delay(350); M5.Speaker.tone(2000,300);
    }

    if(timerState==TMR_DONE) {
      canvas.setTextSize(4); canvas.setTextColor(COL_RED);
      int tw=canvas.textWidth("DONE!"); canvas.setCursor((W-tw)/2,36); canvas.print("DONE!");
      canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
      tw=canvas.textWidth("FRONT to dismiss"); canvas.setCursor((W-tw)/2,80); canvas.print("FRONT to dismiss");
      drawHints("---","dismiss");
      return;
    }

    // Arc progress (left side)
    float frac=1.0f-(float)rem/((float)timerSetMin*60000.0f);
    int cx=60, cy=67, or_=46, ir_=36;
    canvas.fillArc(cx,cy,or_,ir_,-90.0f,-90.0f+frac*360.0f, rem<60000?(uint32_t)COL_RED:(uint32_t)NIXIE_ORANGE);
    canvas.fillArc(cx,cy,or_,ir_,-90.0f+frac*360.0f,270.0f,(uint32_t)NIXIE_GHOST);
    // Center text: % remaining
    canvas.setTextSize(1); canvas.setTextColor(NIXIE_DIM);
    char pct[6]; snprintf(pct,sizeof(pct),"%d%%",(int)((1.0f-frac)*100));
    int pw=canvas.textWidth(pct); canvas.setCursor(cx-pw/2,cy-4); canvas.print(pct);

    // Right side: countdown
    canvas.drawFastVLine(120,15,H-35,(uint32_t)NIXIE_GHOST);
    char rbuf[6]; snprintf(rbuf,sizeof(rbuf),"%02d:%02d",(int)(rem/60000),(int)((rem/1000)%60));
    canvas.setTextSize(3); canvas.setTextColor(rem<60000?(uint32_t)COL_RED:(uint32_t)NIXIE_ORANGE);
    int tw=canvas.textWidth(rbuf); canvas.setCursor(124+(116-tw)/2,25); canvas.print(rbuf);

    canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
    canvas.setCursor(130,62);
    canvas.print(timerState==TMR_RUNNING?"● RUNNING":"❚❚ PAUSED");
    canvas.setCursor(130,76);
    canvas.printf("of %02d:00 min",timerSetMin);

    drawHints("---","pause/resume","cancel");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// SCREEN: CONNECT (WiFi + BLE)
// ─────────────────────────────────────────────────────────────────────────────
void drawConnect() {
  canvas.fillSprite(COL_BG);

  // ── Idle overview: two panels, active one highlighted ──────────────────────
  if(wifiSt==WF_IDLE) {
    drawHeader("CONNECT");

    bool wUp=(WiFi.status()==WL_CONNECTED);
    bool wSel=(connectPanel==0);
    bool bSel=(connectPanel==1);

    // WiFi panel (left) — highlighted border when selected
    uint32_t wBorder = wSel ? (uint32_t)NIXIE_ORANGE : (wUp ? (uint32_t)COL_GREEN : (uint32_t)NIXIE_GHOST);
    canvas.drawRoundRect(2,16,116,80,4,wBorder);
    if(wSel) canvas.drawRoundRect(3,17,114,78,3,wBorder); // double border = selected
    canvas.setTextSize(1);
    canvas.setTextColor(wUp?(uint32_t)COL_GREEN:(uint32_t)NIXIE_DIM);
    canvas.setCursor(8,21); canvas.print(wUp?"WiFi  CONNECTED":"WiFi  OFFLINE");
    canvas.setTextColor(NIXIE_GHOST);
    if(wUp) {
      canvas.setCursor(8,34); canvas.print(WiFi.SSID().substring(0,14));
      canvas.setCursor(8,47); canvas.print(WiFi.localIP().toString());
    } else {
      prefs.begin("wifi",true);
      String sv=prefs.getString("ssid","(none)"); prefs.end();
      canvas.setCursor(8,34); canvas.printf("saved: %.13s",sv.c_str());
      canvas.setTextColor(NIXIE_DIM);
      canvas.setCursor(8,50); canvas.print(wSel?"FRONT: scan":"TOP: switch");
    }

    // BLE panel (right)
    uint32_t bBorder = bSel ? (uint32_t)NIXIE_ORANGE : (bleConn ? (uint32_t)COL_BLUE : (uint32_t)NIXIE_GHOST);
    canvas.drawRoundRect(121,16,116,80,4,bBorder);
    if(bSel) canvas.drawRoundRect(122,17,114,78,3,bBorder);
    canvas.setTextSize(1);
    canvas.setTextColor(bleConn?(uint32_t)COL_BLUE : bleInited?(uint32_t)NIXIE_DIM:(uint32_t)COL_RED);
    canvas.setCursor(127,21);
    canvas.print(bleConn ? "BLE  CONNECTED" : bleInited ? "BLE  WAITING" : "BLE  OFFLINE");
    canvas.setTextColor(NIXIE_GHOST);
    canvas.setCursor(127,34); canvas.print("SIDECAR V1");
    canvas.setCursor(127,47);
    canvas.print(bleConn ? "paired" : bleInited ? "advertising" : "wifi active");
    canvas.setCursor(127,60); canvas.print("Bellafaire proto");

    if(wSel) drawHints("switch panel","scan");
    else     drawHints("switch panel","re-advertise");

  // ── Network list ───────────────────────────────────────────────────────────
  } else if(wifiSt==WF_LIST) {
    drawHeader("SELECT NETWORK");
    if(wifiNetCount==0) {
      canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
      int tw=canvas.textWidth("No networks found"); canvas.setCursor((W-tw)/2,55); canvas.print("No networks found");
      canvas.setTextColor(NIXIE_DIM);
      tw=canvas.textWidth("FRONT to go back"); canvas.setCursor((W-tw)/2,70); canvas.print("FRONT to go back");
    } else {
      if(wifiListIdx >= wifiNetCount) wifiListIdx = max(0, wifiNetCount-1);
      int vis=4, start=max(0,wifiListIdx-vis+1), y=17;
      for(int i=start;i<wifiNetCount&&i<start+vis;i++) {
        bool sel=(i==wifiListIdx);
        canvas.drawRoundRect(3,y,W-6,22,3,sel?(uint32_t)NIXIE_ORANGE:(uint32_t)NIXIE_GHOST);
        canvas.setTextSize(1); canvas.setTextColor(sel?(uint32_t)NIXIE_ORANGE:(uint32_t)NIXIE_DIM);
        String nm=wifiSSIDs[i]; if(nm.length()>23) nm=nm.substring(0,23);
        canvas.setCursor(9,y+7); canvas.print(nm);
        if(wifiIsOpen[i]) {
          canvas.setTextColor(sel?(uint32_t)COL_GREEN:(uint32_t)NIXIE_GHOST);
          canvas.print(" open");
        }
        drawSignalBars(W-22,y+17,wifiRSSI[i]);
        y+=26;
      }
      if(wifiNetCount>vis) {
        canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
        canvas.setCursor(W-28,8); canvas.printf("%d/%d",wifiListIdx+1,wifiNetCount);
      }
    }
    drawHints("scroll","connect");

  // ── AP setup instructions ──────────────────────────────────────────────────
  } else if(wifiSt==WF_AP) {
    drawHeader("WiFi SETUP");
    canvas.setTextSize(1);
    canvas.setTextColor(NIXIE_ORANGE); canvas.setCursor(4,18); canvas.print("Network: ");
    canvas.setTextColor(NIXIE_DIM);
    String nm=wifiSelSSID; if(nm.length()>20) nm=nm.substring(0,20);
    canvas.print(nm);
    canvas.drawFastHLine(0,30,W,(uint32_t)NIXIE_GHOST);
    canvas.setTextColor(NIXIE_GHOST);
    canvas.setCursor(4,35); canvas.print("1. Connect phone WiFi:");
    canvas.setTextColor(NIXIE_ORANGE);
    canvas.setCursor(4,46); canvas.print("   SIDECAR-V1  (no password)");
    canvas.setTextColor(NIXIE_GHOST);
    canvas.setCursor(4,58); canvas.print("2. Open:  192.168.4.1");
    canvas.setCursor(4,70); canvas.print("3. Enter password + tap Connect");
    drawHints("---","cancel AP");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// SCREEN: SETTINGS (two-column)
// Left:  HOUR MINUTE DATE MONTH
// Right: YEAR POWER BRIGHTNESS AUTO-ROT
// ─────────────────────────────────────────────────────────────────────────────
void drawSettings() {
  auto dt=M5.Rtc.getDateTime();
  int hh=dt.time.hours, mm=dt.time.minutes, dd=dt.date.date, mo=dt.date.month, yr=dt.date.year;

  // Field labels and values
  const char* labels[SETTING_COUNT]={"HOUR","MIN","DATE","MONTH","YEAR","POWER","BRIGHT","ROTATE","AOD","SOUND","LOG"};
  char vals[SETTING_COUNT][12];
  snprintf(vals[0],12,"%02d",hh);
  snprintf(vals[1],12,"%02d",mm);
  snprintf(vals[2],12,"%02d",dd);
  snprintf(vals[3],12,"%s",(mo>=1&&mo<=12)?MON_NAMES[mo]:"???");
  snprintf(vals[4],12,"%04d",yr);
  snprintf(vals[5],12,"%s",PWR_NAMES[powerMode]);
  snprintf(vals[6],12,"%d/5",brightLevel);
  snprintf(vals[7],12,"%s",autoRotate?"ON":"OFF");
  snprintf(vals[8],12,"%s",aodEnabled?"ON":"OFF");
  snprintf(vals[9],12,"%s",soundProfile==SND_SILENT?"SILENT":"GENERAL");
  snprintf(vals[10],12,"%s",loggingEnabled?"ON":"OFF");

  canvas.fillSprite(COL_BG); drawHeader("SETTINGS");

  // 3-column layout: TIME(0-2) | DATE(3-5) | SYS(6-9, 4 rows)
  // Columns at x=4, x=84, x=164. Row spacing: 19px fits 4 rows in usable area.
  canvas.setTextSize(1); canvas.setTextColor(NIXIE_DIM);
  canvas.setCursor(4,17);   canvas.print("TIME");
  canvas.setCursor(84,17);  canvas.print("DATE");
  canvas.setCursor(164,17); canvas.print("SYS");
  canvas.drawFastHLine(0,25,W,(uint32_t)NIXIE_GHOST);
  canvas.drawFastVLine(80,15,H-36,(uint32_t)NIXIE_GHOST);
  canvas.drawFastVLine(160,15,H-36,(uint32_t)NIXIE_GHOST);

  // Field layout: cols 0&1 → 3 rows @ 22px, col 2 → 5 rows @ 15px
  const int colX[3]   = {4, 84, 164};
  const int colRows[3]= {3, 3,  5};
  const int rowH[3]   = {22,22, 15};
  const int cw = 74;
  for(int i = 0; i < SETTING_COUNT; i++) {
    int col = (i < 3) ? 0 : (i < 6) ? 1 : 2;
    int row = (i < 3) ? i : (i < 6) ? i-3 : i-6;
    int x   = colX[col];
    int y   = 28 + row * rowH[col];
    bool active = (i == settingIdx);

    if(active) canvas.fillRoundRect(x-2,y-2,cw,15,3,(uint32_t)NIXIE_GHOST);
    canvas.setTextSize(1);
    canvas.setTextColor(active?(uint32_t)NIXIE_ORANGE:(uint32_t)NIXIE_DIM);
    canvas.setCursor(x,y); canvas.print(labels[i]);
    canvas.setTextColor(active?(uint32_t)NIXIE_ORANGE:(uint32_t)TFT_WHITE);
    int vw=canvas.textWidth(vals[i]);
    canvas.setCursor(x+cw-vw-2,y); canvas.print(vals[i]);
  }

  drawHints("next/prev field","+1 value","-1 value");
}

// ─────────────────────────────────────────────────────────────────────────────
/// ─────────────────────────────────────────────────────────────────────────────
// SCREEN: POWER
// ─────────────────────────────────────────────────────────────────────────────
void drawPower() {
  canvas.fillSprite(COL_BG);

  if(pwrInCustom) {
    // ── Customization sub-screen ──────────────────────────────────────────────
    char title[24]; snprintf(title,sizeof(title),"POWER / %s",PWR_NAMES[pwrTabSel]);
    drawHeader(title);

    PwrCfg& c = pwrCfg[pwrTabSel];
    const char* fnames[PWRCFG_COUNT] = {"BRIGHTNESS","TIMEOUT","WIFI"};

    // Timeout label helpers
    auto fmtTimeout=[](int s, char* buf){ if(s==0) snprintf(buf,8,"OFF"); else if(s<60) snprintf(buf,8,"%ds",s); else snprintf(buf,8,"%dm",s/60); };

    char tvals[PWRCFG_COUNT][12];
    snprintf(tvals[0],12,"%d/5",c.brightness);
    char tbuf[8]; fmtTimeout(c.timeoutSec,tbuf); snprintf(tvals[1],12,"%s",tbuf);
    snprintf(tvals[2],12,"%s",c.wifiOff?"OFF":"ON");

    // Active mode indicator
    if(pwrTabSel==(int)powerMode) {
      canvas.setTextSize(1); canvas.setTextColor(COL_GREEN);
      int aw=canvas.textWidth("● ACTIVE"); canvas.setCursor((W-aw)/2,17); canvas.print("● ACTIVE");
    } else {
      canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
      int aw=canvas.textWidth("not active"); canvas.setCursor((W-aw)/2,17); canvas.print("not active");
    }

    // Three fields
    int y=32;
    for(int i=0;i<PWRCFG_COUNT;i++) {
      bool sel=(i==pwrCustIdx);
      if(sel) canvas.fillRoundRect(10,y-2,W-20,20,3,(uint32_t)NIXIE_GHOST);
      canvas.setTextSize(1);
      canvas.setTextColor(sel?(uint32_t)NIXIE_ORANGE:(uint32_t)NIXIE_DIM);
      canvas.setCursor(18,y+3); canvas.print(fnames[i]);
      canvas.setTextColor(sel?(uint32_t)NIXIE_ORANGE:(uint32_t)TFT_WHITE);
      int vw=canvas.textWidth(tvals[i]); canvas.setCursor(W-18-vw,y+3); canvas.print(tvals[i]);
      y+=26;
    }

    drawHints("next/prev field","+1 value","back");

  } else {
    // ── Horizontal letter-roller: N  E  [X]  S ───────────────────────────────
    drawHeader("POWER MODES");

    // First letters and short descriptions for each mode
    const char LETTERS[PWR_MODE_COUNT] = {'N','E','X','S'};
    const char* fullDesc[PWR_MODE_COUNT] = {
      "240MHz  WiFi on",
      "80MHz   WiFi off",
      "80MHz   short timeout",
      "80MHz   max saving"
    };

    // ── Top strip: 5 equal cells, each W/5 = 48px wide, height 44px ──────────
    const int STRIP_Y = 18;
    const int STRIP_H = 44;
    const int CELL_W  = W / PWR_MODE_COUNT;  // 48

    for(int i = 0; i < PWR_MODE_COUNT; i++) {
      bool sel    = (i == pwrTabSel);
      bool active = (i == (int)powerMode);
      int  cx     = i * CELL_W;

      if(sel) {
        // filled cell for selected
        canvas.fillRoundRect(cx+2, STRIP_Y, CELL_W-4, STRIP_H, 5, (uint32_t)0x1880);
        uint32_t border = active ? (uint32_t)COL_GREEN : (uint32_t)NIXIE_ORANGE;
        canvas.drawRoundRect(cx+2, STRIP_Y, CELL_W-4, STRIP_H, 5, border);
        canvas.drawRoundRect(cx+3, STRIP_Y+1, CELL_W-6, STRIP_H-2, 4, border);
      }

      // Letter — size 3 (18px wide) for selected, size 2 for others
      int tsz = sel ? 3 : 2;
      canvas.setTextSize(tsz);
      uint32_t col = active
        ? (uint32_t)COL_GREEN
        : (sel ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_GHOST);
      canvas.setTextColor(col);
      int charW = 6 * tsz;
      int charH = 8 * tsz;
      int lx = cx + (CELL_W - charW) / 2;
      int ly = STRIP_Y + (STRIP_H - charH) / 2;
      canvas.setCursor(lx, ly);
      canvas.print(LETTERS[i]);

      // Active dot below letter for the currently running mode
      if(active && !sel) {
        canvas.fillCircle(cx + CELL_W/2, STRIP_Y + STRIP_H - 5, 2, (uint32_t)COL_GREEN);
      }
    }

    // ── Detail panel for selected mode ────────────────────────────────────────
    const int DETAIL_Y = STRIP_Y + STRIP_H + 4;
    bool active = (pwrTabSel == (int)powerMode);

    // Mode full name
    canvas.setTextSize(2);
    canvas.setTextColor(active ? (uint32_t)COL_GREEN : (uint32_t)NIXIE_ORANGE);
    int nw = canvas.textWidth(PWR_NAMES[pwrTabSel]);
    canvas.setCursor((W - nw) / 2, DETAIL_Y);
    canvas.print(PWR_NAMES[pwrTabSel]);
    if(active) { canvas.setTextColor(COL_GREEN); canvas.print(" \x07"); }

    // Description line
    canvas.setTextSize(1);
    canvas.setTextColor(NIXIE_DIM);
    int dw = canvas.textWidth(fullDesc[pwrTabSel]);
    canvas.setCursor((W - dw) / 2, DETAIL_Y + 18);
    canvas.print(fullDesc[pwrTabSel]);

    // Stats row: B:x/5  T:Xs  WiFi:on/off
    char tbuf[8];
    if(pwrCfg[pwrTabSel].timeoutSec == 0)        snprintf(tbuf,8,"off");
    else if(pwrCfg[pwrTabSel].timeoutSec < 60)   snprintf(tbuf,8,"%ds",pwrCfg[pwrTabSel].timeoutSec);
    else                                          snprintf(tbuf,8,"%dm",pwrCfg[pwrTabSel].timeoutSec/60);
    char stats[40];
    snprintf(stats,sizeof(stats),"B:%d/5  tmout:%s  wifi:%s",
      pwrCfg[pwrTabSel].brightness, tbuf,
      pwrCfg[pwrTabSel].wifiOff ? "off" : "on");
    canvas.setTextColor(NIXIE_GHOST);
    int sw = canvas.textWidth(stats);
    canvas.setCursor((W - sw) / 2, DETAIL_Y + 28);
    canvas.print(stats);

    // Battery life estimate
    float bLife = battLifeHours(pwrTabSel);
    char blifeBuf[16]; fmtBattLife(bLife, blifeBuf, sizeof(blifeBuf));
    char battLine[32]; snprintf(battLine, sizeof(battLine), "bat %d%%  life%s",
      M5.Power.getBatteryLevel(), blifeBuf);
    canvas.setTextColor(active ? (uint32_t)COL_GREEN : (uint32_t)NIXIE_DIM);
    sw = canvas.textWidth(battLine);
    canvas.setCursor((W - sw) / 2, DETAIL_Y + 39);
    canvas.print(battLine);

    drawHints("scroll modes","activate","customize");
  }
}

// ── Battery life estimation ───────────────────────────────────────────────────
// Measured base currents (mA) per mode (CLAUDE.md, includes CPU + PMIC + BLE adv
// + display at brightness 3/5).  Brightness sensitivity ≈ 3.5 mA per step.
// BLE connected adds ~5 mA.  WiFi steady-state connected adds ~10 mA.
// Battery: 2 × 600 mAh in parallel = 1200 mAh.
// Returns estimated current draw for a mode in mA (no capacity factor).
float currentDrawMA(int modeIdx) {
  // BASE: SLEEP updated from 0.8→3.0 to reflect real light sleep + BLE advertising
  const float BASE_MA[PWR_MODE_COUNT] = {58.0f, 41.0f, 15.0f, 3.0f};
  int idx = constrain(modeIdx, 0, PWR_MODE_COUNT-1);
  float mA = BASE_MA[idx];
  // Brightness delta — but can't pull mA below the base (screen-off floor is already the base)
  float brtDelta = (pwrCfg[idx].brightness - 3) * 3.5f;
  mA = max(BASE_MA[idx], mA + brtDelta);
  if(bleConn) mA += 5.0f;
  if(!pwrCfg[idx].wifiOff && WiFi.status() == WL_CONNECTED) mA += 10.0f;
  return mA;
}

float battLifeHours(int modeIdx) {
  float remaining_mAh = 200.0f * (float)getBatt() / 100.0f;
  return remaining_mAh / currentDrawMA(modeIdx);
}

// Format hours → "~Xd Xh" / "~X.Xh" / "~Xm"
void fmtBattLife(float h, char* buf, int len) {
  if(h >= 24.0f)       snprintf(buf, len, "~%dd %dh", (int)(h/24.0f), (int)(h)%24);
  else if(h >= 1.0f)   snprintf(buf, len, "~%.1fh", h);
  else                  snprintf(buf, len, "~%dm", (int)(h*60.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
// SCREEN: ALARM
// ─────────────────────────────────────────────────────────────────────────────
void drawAlarm() {
  canvas.fillSprite(COL_BG);
  drawHeader("ALARM");

  // ── Ringing overlay ────────────────────────────────────────────────────────
  if(alarmRinging) {
    uint32_t flash = ((millis()/300)%2) ? (uint32_t)COL_RED : (uint32_t)0x4000;
    canvas.fillRoundRect(6, 18, W-12, 62, 6, flash);
    canvas.setTextSize(2); canvas.setTextColor(TFT_WHITE);
    int tw = canvas.textWidth("ALARM!"); canvas.setCursor((W-tw)/2, 30); canvas.print("ALARM!");
    canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
    tw = canvas.textWidth("FRONT to dismiss"); canvas.setCursor((W-tw)/2, 62); canvas.print("FRONT to dismiss");
    drawHints("","dismiss");
    return;
  }

  // ── Edit sub-screen ────────────────────────────────────────────────────────
  if(alarmInEdit) {
    AlarmCfg& a = alarms[alarmSel];
    const char* DOW_SHORT[] = {"S","M","T","W","T","F","S"};

    // Row 1: HH:MM — fields 0 & 1
    canvas.setTextSize(2);
    char tstr[6]; snprintf(tstr, sizeof(tstr), "%02d:%02d", a.hour, a.minute);
    int tw = canvas.textWidth(tstr);
    int tx = (W - tw) / 2;
    canvas.setTextColor(NIXIE_ORANGE); canvas.setCursor(tx, 18); canvas.print(tstr);
    // Highlight active time field
    if(alarmEditField == 0)
      canvas.drawRoundRect(tx-2, 16, canvas.textWidth("00")+4, 18, 2, NIXIE_DIM);
    else if(alarmEditField == 1)
      canvas.drawRoundRect(tx + canvas.textWidth("00:") - 2, 16, canvas.textWidth("00")+4, 18, 2, NIXIE_DIM);

    // Row 2: day-of-week toggles — fields 2-8
    const int DOW_W = 24, DOW_Y = 40;
    int dstart = (W - 7*DOW_W) / 2;
    for(int d = 0; d < 7; d++) {
      bool on   = (a.daysOfWeek >> d) & 1;
      bool hlit = (alarmEditField == 2 + d);
      int  dx   = dstart + d * DOW_W;
      uint32_t bg  = on  ? (uint32_t)NIXIE_ORANGE : (uint32_t)0x1080;
      uint32_t bdr = hlit ? (uint32_t)TFT_WHITE   : (uint32_t)NIXIE_GHOST;
      canvas.fillRoundRect(dx, DOW_Y, DOW_W-2, 16, 3, bg);
      canvas.drawRoundRect(dx, DOW_Y, DOW_W-2, 16, 3, bdr);
      canvas.setTextSize(1);
      canvas.setTextColor(on ? (uint32_t)COL_BG : (uint32_t)NIXIE_GHOST);
      canvas.setCursor(dx + (DOW_W-8)/2, DOW_Y+4);
      canvas.print(DOW_SHORT[d]);
    }

    // Row 3: Date filter — fields 9, 10, 11
    const int DY = 63;
    canvas.setTextSize(1);
    // "DATE:" label
    canvas.setTextColor(alarmEditField==9 ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_DIM);
    canvas.setCursor(6, DY); canvas.print("DATE:");
    if(!a.useDate) {
      canvas.setTextColor(alarmEditField==9 ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_GHOST);
      canvas.print(" OFF");
    } else {
      // DD field
      char dd[3]; snprintf(dd, 3, "%02d", a.dateDay);
      canvas.setTextColor(alarmEditField==10 ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_DIM);
      if(alarmEditField==10) canvas.fillRoundRect(38, DY-1, 16, 10, 2, (uint32_t)NIXIE_GHOST);
      canvas.setCursor(40, DY); canvas.print(dd);
      // MMM field
      canvas.setTextColor(alarmEditField==11 ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_DIM);
      if(alarmEditField==11) canvas.fillRoundRect(57, DY-1, 24, 10, 2, (uint32_t)NIXIE_GHOST);
      canvas.setCursor(58, DY);
      canvas.print(a.dateMonth>=1&&a.dateMonth<=12 ? MON_NAMES[a.dateMonth] : "???");
    }

    drawHints("next field","+1","back/save");
    return;
  }

  // ── List view ──────────────────────────────────────────────────────────────
  int y = 18;
  for(int i = 0; i < MAX_ALARMS; i++) {
    bool sel = (i == alarmSel);
    if(sel) canvas.fillRoundRect(4, y, W-8, 30, 4, (uint32_t)0x0840);
    canvas.drawRoundRect(4, y, W-8, 30, 4, sel ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_GHOST);

    // Time
    canvas.setTextSize(2);
    canvas.setTextColor(alarms[i].enabled
      ? (sel ? (uint32_t)NIXIE_ORANGE : (uint32_t)NIXIE_DIM) : (uint32_t)NIXIE_GHOST);
    char tstr[6]; snprintf(tstr, sizeof(tstr), "%02d:%02d", alarms[i].hour, alarms[i].minute);
    canvas.setCursor(10, y+6); canvas.print(tstr);

    // Day summary (compact)
    canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
    if(alarms[i].daysOfWeek == 0x7F)       canvas.setCursor(88, y+6),  canvas.print("daily");
    else if(alarms[i].daysOfWeek == 0x3E)  canvas.setCursor(88, y+6),  canvas.print("M-F");
    else if(alarms[i].daysOfWeek == 0x41)  canvas.setCursor(88, y+6),  canvas.print("S/S");
    else {
      const char* DS[]={"S","M","T","W","T","F","S"};
      int dx = 88;
      for(int d=0;d<7;d++) if((alarms[i].daysOfWeek>>d)&1) {
        canvas.setCursor(dx, y+6); canvas.print(DS[d]); dx+=8;
      }
    }
    if(alarms[i].useDate) {
      char dbuf[8]; snprintf(dbuf,8,"%d/%s",alarms[i].dateDay,
        alarms[i].dateMonth>=1&&alarms[i].dateMonth<=12?MON_NAMES[alarms[i].dateMonth]:"?");
      canvas.setCursor(88, y+16); canvas.print(dbuf);
    }

    // ON/OFF pill
    canvas.setTextSize(1);
    if(alarms[i].enabled) {
      canvas.fillRoundRect(W-48, y+8, 38, 13, 3, (uint32_t)COL_GREEN);
      canvas.setTextColor(COL_BG); canvas.setCursor(W-44, y+11); canvas.print("ON");
    } else {
      canvas.drawRoundRect(W-48, y+8, 38, 13, 3, (uint32_t)NIXIE_GHOST);
      canvas.setTextColor(NIXIE_GHOST); canvas.setCursor(W-46, y+11); canvas.print("OFF");
    }
    y += 34;
  }
  drawHints("next alarm","toggle on/off","edit");
}

// ─────────────────────────────────────────────────────────────────────────────
// SCREEN: VOICE RECORDER
// ─────────────────────────────────────────────────────────────────────────────
void drawVoice() {
  canvas.fillSprite(COL_BG);
  drawHeader("VOICE");

  const int CX = W / 2;
  const int METER_Y = 55;
  const int METER_H = 12;
  const int METER_W = W - 40;

  if(voiceState == VC_IDLE) {
    canvas.setTextSize(2); canvas.setTextColor(NIXIE_DIM);
    int tw = canvas.textWidth("READY"); canvas.setCursor((W-tw)/2, 38); canvas.print("READY");
    canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
    char info[32]; snprintf(info, sizeof(info), "%ds @ %dHz  mono", REC_SECONDS, REC_RATE);
    tw = canvas.textWidth(info); canvas.setCursor((W-tw)/2, 62); canvas.print(info);
    if(recLen > 0) {
      canvas.setTextColor(NIXIE_ORANGE);
      tw = canvas.textWidth("clip saved"); canvas.setCursor((W-tw)/2, 78); canvas.print("clip saved");
    }
    drawHints("","record","play last");

  } else if(voiceState == VC_RECORDING) {
    // Pulsing REC indicator
    uint32_t recCol = ((millis()/400)%2) ? (uint32_t)COL_RED : (uint32_t)0x8000;
    canvas.fillCircle(CX, 32, 10, recCol);
    canvas.setTextSize(1); canvas.setTextColor(COL_RED);
    int tw = canvas.textWidth("REC"); canvas.setCursor((W-tw)/2, 47); canvas.print("REC");

    // Progress bar
    int progress = recLen > 0 ? (int)((recLen * METER_W) / REC_SAMPLES) : 0;
    canvas.drawRoundRect(20, METER_Y, METER_W, METER_H, 3, NIXIE_GHOST);
    if(progress > 0) canvas.fillRoundRect(20, METER_Y, progress, METER_H, 3, COL_RED);

    // VU level bar
    int vuW = (vuLevel * METER_W) / 100;
    canvas.drawRoundRect(20, METER_Y+16, METER_W, 8, 2, NIXIE_GHOST);
    if(vuW > 0) canvas.fillRoundRect(20, METER_Y+16, vuW, 8, 2, NIXIE_ORANGE);

    // Time remaining
    canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
    float secLeft = (float)(REC_SAMPLES - recLen) / REC_RATE;
    char tbuf[12]; snprintf(tbuf, sizeof(tbuf), "%.1fs left", secLeft);
    tw = canvas.textWidth(tbuf); canvas.setCursor((W-tw)/2, METER_Y+28); canvas.print(tbuf);
    drawHints("","stop","");

  } else if(voiceState == VC_PLAYING) {
    canvas.setTextSize(2); canvas.setTextColor(COL_GREEN);
    int tw = canvas.textWidth("PLAYING"); canvas.setCursor((W-tw)/2, 38); canvas.print("PLAYING");
    canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
    char info[20]; snprintf(info, sizeof(info), "%d samples", (int)recLen);
    tw = canvas.textWidth(info); canvas.setCursor((W-tw)/2, 62); canvas.print(info);
    drawHints("","","");

  } else {  // VC_DONE
    canvas.setTextSize(2); canvas.setTextColor(NIXIE_ORANGE);
    int tw = canvas.textWidth("DONE"); canvas.setCursor((W-tw)/2, 38); canvas.print("DONE");
    canvas.setTextSize(1); canvas.setTextColor(NIXIE_DIM);
    float secs = (float)recLen / REC_RATE;
    char info[20]; snprintf(info, sizeof(info), "%.1f seconds", secs);
    tw = canvas.textWidth(info); canvas.setCursor((W-tw)/2, 62); canvas.print(info);
    drawHints("","play","record new");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// SCREEN: NOTIFICATIONS
// ─────────────────────────────────────────────────────────────────────────────
void drawNotifs() {
  canvas.fillSprite(COL_BG); drawHeader("NOTIFICATIONS");

  if(notifCount==0) {
    canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
    int tw=canvas.textWidth("No notifications"); canvas.setCursor((W-tw)/2,60); canvas.print("No notifications");
    canvas.setTextColor(NIXIE_GHOST);
    tw=canvas.textWidth("Connect BLE to receive"); canvas.setCursor((W-tw)/2,76); canvas.print("Connect BLE to receive");
    drawHints("scroll","dismiss");
    return;
  }

  // Header count
  canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
  canvas.setCursor(W-36,3); canvas.printf("%d/%d",notifIdx+1,notifCount);

  // Show 3 notifications, centered on notifIdx
  int vis=3, start=max(0,min(notifIdx-1,notifCount-vis));
  int y=17;
  for(int i=start;i<notifCount&&i<start+vis;i++) {
    bool sel=(i==notifIdx);
    uint32_t border=notifs[i].unread?COL_RED:(sel?NIXIE_ORANGE:NIXIE_GHOST);
    canvas.drawRoundRect(3,y,W-6,30,3,border);
    canvas.setTextSize(1);
    canvas.setTextColor(notifs[i].unread?(uint32_t)COL_RED:(sel?(uint32_t)NIXIE_ORANGE:(uint32_t)NIXIE_GHOST));
    canvas.setCursor(9,y+4); canvas.print(notifs[i].app);
    canvas.setTextColor(sel?(uint32_t)NIXIE_DIM:(uint32_t)NIXIE_GHOST);
    canvas.setCursor(9,y+16);
    char t[30]; strncpy(t,notifs[i].title,29); t[29]=0;
    canvas.print(t);
    y+=34;
  }

  drawHints("scroll up/down","dismiss","clear all");
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  auto cfg=M5.config();
  M5.begin(cfg);

  EEPROM.begin(EE_SIZE);
  brightLevel   = constrain(EEPROM.read(EE_BRIGHT), 0, 5);
  autoRotate    = EEPROM.read(EE_AUTOROT) != 0;
  uint8_t pm    = EEPROM.read(EE_PWRMODE);
  // Never restore CUSTOM from EEPROM — it requires user confirmation at runtime.
  // Clamp to SLEEP (3) max so CUSTOM (4) always boots as NORMAL.
  if(pm < PWR_SLEEP) { lastActiveMode=(PowerMode)pm; powerMode=(PowerMode)pm; }
  else               { lastActiveMode=PWR_NORMAL;     powerMode=PWR_NORMAL; }
  uint8_t sp    = EEPROM.read(EE_SOUND);
  soundProfile  = (sp <= 1) ? (SoundProfile)sp : SND_GENERAL;
  aodEnabled    = EEPROM.read(EE_AOD) != 0;
  loggingEnabled= EEPROM.read(EE_LOGGING) != 0;
  loadAlarms();
  M5.Speaker.setVolume(200);

  // Apply CPU frequency BEFORE BLE init — changing it after BLE starts crashes the stack.
  // NORMAL=240MHz, everything else=80MHz.
  setCpuFrequencyMhz(powerMode == PWR_NORMAL ? 240 : 80);

  M5.Display.setRotation(curRotation);
  M5.Display.setBrightness(brtValue(brightLevel));
  canvas.createSprite(W,H);
  Serial.begin(115200);
  LittleFS.begin(true);  // true = format on first use if not mounted
  Serial.printf("\n[BOOT] heap=%d cpu=%dMHz\n", ESP.getFreeHeap(), getCpuFrequencyMhz());
  Serial.println("[BOOT] Send 'D' to dump log, 'C' to clear log");
  logMsg("BOOT heap=%d mode=%s", ESP.getFreeHeap(), PWR_NAMES[powerMode]);

  // WiFi is NOT initialized at boot — mutual exclusion with BLE.
  // WiFi.mode(WIFI_STA) takes ~49KB heap. BLE takes ~21KB.
  // Both together leave only ~22KB — not enough for WiFi scan buffers.
  // WiFi is initialized on-demand from the Connect tab (wifiStartScan).
  Serial.println("[BOOT] WiFi skipped (mutual exclusion with BLE)");

  // Layer 10: power down unused AXP2101 peripherals
  M5.Power.setLed(0);                        // onboard LED off
  pinMode(9, OUTPUT); digitalWrite(9, LOW);  // IR LED off (GPIO9 = IR TX)

  // Splash
  canvas.fillSprite(COL_BG);
  canvas.setTextColor(NIXIE_ORANGE); canvas.setTextSize(3);
  int tw=canvas.textWidth("SIDECAR V1"); canvas.setCursor((W-tw)/2,42); canvas.print("SIDECAR V1");
  canvas.setTextSize(1); canvas.setTextColor(NIXIE_DIM);
  tw=canvas.textWidth("ALIVE"); canvas.setCursor((W-tw)/2,86); canvas.print("ALIVE");
  canvas.pushSprite(0,0);
  delay(1200);

  Serial.printf("[BOOT] pre-BLE heap=%d\n", ESP.getFreeHeap());
  initBLE();
  bleInited = true;
  Serial.printf("[BOOT] post-BLE heap=%d\n", ESP.getFreeHeap());
  Serial.println("[BOOT] Setup complete");

  pwrTabSel=(int)powerMode;
  lastActivity=millis();
  M5.Display.setBrightness(map(pwrCfg[powerMode].brightness, 0, 5, 30, 200));
  lastBattLog = millis() - 61000UL;  // trigger first battery log sample immediately
}

// ── Smoothed battery level ────────────────────────────────────────────────────
int getBatt() {
  unsigned long now = millis();
  if(battSmoothed < 0 || now - lastBattRead > 15000UL) {
    int raw = M5.Power.getBatteryLevel();
    raw = constrain(raw, 0, 100);
    if(battSmoothed < 0) battSmoothed = raw;
    else battSmoothed = battSmoothed + (BATT_SMOOTH_ALPHA * (raw - battSmoothed)) / 100;
    lastBattRead = now;
  }
  return battSmoothed;
}

// ── AOD — minimal dim clock, shown when screen would otherwise be blank ────────
void drawAOD() {
  auto dt = M5.Rtc.getDateTime();
  canvas.fillSprite(COL_BG);
  canvas.setTextSize(4); canvas.setTextColor(0x2100);  // very dim orange
  char hhmm[6]; snprintf(hhmm,6,"%02d:%02d",dt.time.hours,dt.time.minutes);
  int tw = canvas.textWidth(hhmm);
  canvas.setCursor((W-tw)/2, (H-32)/2); canvas.print(hhmm);
  // Tiny battery %
  int b = getBatt();
  canvas.setTextSize(1);
  canvas.setTextColor(b>20 ? (uint32_t)0x0300 : (uint32_t)0x4000);
  char bb[5]; snprintf(bb,5,"%d%%",b);
  canvas.setCursor(W-24, H-10); canvas.print(bb);
  canvas.pushSprite(0,0);
}

// ── Battery Diagnostic Screen ─────────────────────────────────────────────────
// Helper: compute mA for a pair of log indices using actual timestamps.
// Returns 0 if timestamps are invalid or battery is charging.
static float logMa(int idx_new, int idx_old) {
  int delta = (int)battLog[idx_old] - (int)battLog[idx_new];
  if(delta <= 0) return 0.0f;
  int dt = (int)battLogSec[idx_new] - (int)battLogSec[idx_old];
  if(dt <= 0) dt = 300;  // fallback: assume 5 min
  // mA = (delta_pct/100) * 200mAh / (dt_seconds / 3600)
  return (float)delta * 200.0f * 3600.0f / (100.0f * (float)dt);
}

void drawDiag() {
  canvas.fillSprite(COL_BG); drawHeader("BATTERY DIAG");

  float batV   = M5.Power.getBatteryVoltage() / 1000.0f;
  int   batPct = getBatt();
  float modelMA = currentDrawMA(powerMode);   // mA from model, independent of remaining capacity

  int n = min(battLogCount, (int)BATT_LOG_SIZE);

  // Compute rolling 5-sample mA windows for avg / current
  float sumMA = 0; int maCount = 0; float lastWindowMA = -1;
  if(n >= 6) {
    for(int i = 5; i < n; i++) {
      int idx_n = (battLogHead - n + i     + BATT_LOG_SIZE) % BATT_LOG_SIZE;
      int idx_o = (battLogHead - n + (i-5) + BATT_LOG_SIZE) % BATT_LOG_SIZE;
      float ma = logMa(idx_n, idx_o);
      sumMA += ma; maCount++;
      if(i == n-1) lastWindowMA = ma;
    }
  }
  float avgMA = maCount > 0 ? sumMA / maCount : modelMA;
  // Use real measured value when available; fall back to model only if no data yet
  bool usingModel = (lastWindowMA <= 0);
  float nowMA     = usingModel ? modelMA : lastWindowMA;
  float hoursLeft = nowMA > 0 ? (batPct / 100.0f * 200.0f) / nowMA : 0;

  // ── Stats rows ──────────────────────────────────────────────────────────────
  canvas.setTextSize(1);
  canvas.setTextColor(NIXIE_ORANGE);
  canvas.setCursor(4, 16);
  canvas.printf("%.2fV  %d%%  %s", batV, batPct, PWR_NAMES[powerMode]);

  // Format mA: 1 decimal for <10mA, whole number otherwise
  char nowBuf[10], avgBuf[10];
  snprintf(nowBuf, sizeof(nowBuf), nowMA < 10.0f ? "%.1f" : "%.0f", nowMA);
  snprintf(avgBuf, sizeof(avgBuf), avgMA < 10.0f ? "%.1f" : "%.0f", avgMA);

  canvas.setTextColor(NIXIE_DIM);
  canvas.setCursor(4, 26);
  if(usingModel)
    canvas.printf("NOW:%smA(mdl %d/6) LEFT:%.0fh", nowBuf, min(n,6), hoursLeft);
  else
    canvas.printf("NOW:%smA  AVG:%smA  LEFT:%.1fh", nowBuf, avgBuf, hoursLeft);

  // ── Battery % graph (fixed 0-100 Y axis) ────────────────────────────────────
  const int GX=4, GY=37, GW=W-8, GH=31;
  const int GLEFT = GX+22;   // indent for Y-axis labels
  canvas.drawRoundRect(GX, GY, GW, GH, 2, (uint32_t)NIXIE_GHOST);
  canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);
  canvas.setCursor(GX+2, GY+1);      canvas.print("100");
  canvas.setCursor(GX+2, GY+GH-9);  canvas.print("0%");

  if(n >= 2) {
    int prevX=-1, prevY=-1;
    for(int i=0; i<n; i++) {
      int idx = (battLogHead - n + i + BATT_LOG_SIZE) % BATT_LOG_SIZE;
      int v   = (int)battLog[idx];
      int px  = GLEFT + (i*(GX+GW-2-GLEFT)) / max(n-1,1);
      int py  = GY+GH-2 - (v*(GH-4))/100;
      uint32_t col = v > 30 ? (uint32_t)COL_GREEN : (uint32_t)COL_RED;
      if(prevX>=0) canvas.drawLine(prevX,prevY,px,py,col);
      canvas.drawPixel(px,py,col);
      prevX=px; prevY=py;
    }
    canvas.setTextColor(NIXIE_GHOST);
    canvas.setCursor(W-32, GY+GH-9); canvas.printf("-%dm", n);
  } else {
    canvas.setCursor(GLEFT+4, GY+GH/2-4);
    canvas.printf("sample 1/%d...", max(2,n+1));
  }

  // ── mA consumption graph (autoscaled, 5-sample windows) ─────────────────────
  const int MX=4, MY=72, MW=W-8, MH=31;
  const int MLEFT = MX+22;
  canvas.drawRoundRect(MX, MY, MW, MH, 2, (uint32_t)NIXIE_GHOST);
  canvas.setTextSize(1); canvas.setTextColor(NIXIE_GHOST);

  if(n >= 6 && maCount >= 1) {
    // Find peak for autoscale
    float hiMA = 0;
    for(int i=5; i<n; i++) {
      int idx_n = (battLogHead-n+i    +BATT_LOG_SIZE)%BATT_LOG_SIZE;
      int idx_o = (battLogHead-n+(i-5)+BATT_LOG_SIZE)%BATT_LOG_SIZE;
      float ma = logMa(idx_n, idx_o);
      if(ma > hiMA) hiMA = ma;
    }
    if(hiMA < avgMA * 1.5f) hiMA = avgMA * 1.5f;
    if(hiMA < 10.0f)        hiMA = max(modelMA * 1.5f, 10.0f);

    // Y-axis labels
    canvas.setCursor(MX+2, MY+1);    canvas.printf("%.0f", hiMA);
    canvas.setCursor(MX+2, MY+MH-9); canvas.print("0");

    // Average reference line (dimmed horizontal)
    int avgLineY = MY+MH-2 - (int)(avgMA*(MH-4)/hiMA);
    avgLineY = constrain(avgLineY, MY+1, MY+MH-2);
    for(int x=MLEFT; x<MX+MW-2; x+=4) canvas.drawPixel(x, avgLineY, (uint32_t)NIXIE_DIM);

    // Plot mA line
    int pts   = maCount;
    int prevX = -1, prevY = -1;
    for(int i=5; i<n; i++) {
      int idx_n = (battLogHead-n+i    +BATT_LOG_SIZE)%BATT_LOG_SIZE;
      int idx_o = (battLogHead-n+(i-5)+BATT_LOG_SIZE)%BATT_LOG_SIZE;
      float ma = logMa(idx_n, idx_o);
      int j  = i-5;
      int px = MLEFT + (j*(MX+MW-2-MLEFT)) / max(pts-1,1);
      int py = MY+MH-2 - (int)(ma*(MH-4)/hiMA);
      py = constrain(py, MY+1, MY+MH-2);
      uint32_t col = ma > avgMA*1.3f ? (uint32_t)COL_RED : (uint32_t)NIXIE_ORANGE;
      if(prevX>=0) canvas.drawLine(prevX,prevY,px,py,col);
      canvas.drawPixel(px,py,col);
      prevX=px; prevY=py;
    }
  } else {
    canvas.setCursor(MLEFT+4, MY+MH/2-4);
    canvas.printf("%d/6 samples for mA graph", n);
  }

  // ── Bottom status bar ────────────────────────────────────────────────────────
  canvas.setTextSize(1); canvas.setTextColor(NIXIE_DIM);
  canvas.setCursor(4, MY+MH+3);
  char modelBuf[8];
  snprintf(modelBuf, sizeof(modelBuf), modelMA < 10.0f ? "%.1f" : "%.0f", modelMA);
  canvas.printf("%dpts  mdl:%smA  F+hold=clear", battLogCount, modelBuf);

  drawHints("","","");
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  // Serial log commands: send 'D' to dump, 'C' to clear
  if(Serial.available()) {
    char c = Serial.read();
    if(c == 'D' || c == 'd') logDump();
    else if(c == 'C' || c == 'c') logClear();
  }

  M5.update();
  checkRotation();     // Layer 4: already gated on screenOn inside
  handleButtons();
  checkAlarms();

  // Voice recording — runs regardless of screen state
  if(voiceState == VC_RECORDING) {
    const size_t CHUNK = 400;
    if(recLen + CHUNK <= REC_SAMPLES) {
      M5.Mic.record(recBuffer + recLen, CHUNK, REC_RATE, false);
      long sum = 0;
      for(size_t i = recLen; i < recLen + CHUNK; i++) { int32_t s=recBuffer[i]; sum += s<0?-s:s; }
      vuLevel = (int)constrain(sum / (CHUNK * 200), 0, 100);
      recLen += CHUNK;
    } else {
      M5.Mic.end();
      voiceState = VC_DONE;
    }
    displayDirty = true;
  }

  // Battery log: one sample per minute (stretch to 2min when screen off)
  {
    unsigned long now = millis();
    unsigned long logInterval = screenOn ? 60000UL : 120000UL;
    if(now - lastBattLog > logInterval) {
      lastBattLog = now;
      battLog[battLogHead]    = (int8_t)getBatt();
      battLogSec[battLogHead] = (uint16_t)(now / 1000UL);
      battLogHead = (battLogHead + 1) % BATT_LOG_SIZE;
      if(battLogCount < BATT_LOG_SIZE) battLogCount++;
    }
  }

  // Dirty flag: running timers always need a redraw
  if(swRunning || timerState == TMR_RUNNING || timerState == TMR_DONE)
    displayDirty = true;
  // Clock second tick
  {
    static int lastSec = -1;
    auto dt = M5.Rtc.getDateTime();
    if(dt.time.seconds != lastSec) { lastSec = dt.time.seconds; displayDirty = true; }
  }

  // Auto-sleep: turn screen off on inactivity
  unsigned long sleepMs = (unsigned long)pwrCfg[powerMode].timeoutSec * 1000UL;
  if(screenOn && sleepMs > 0 && (millis() - lastActivity) > sleepMs) {
    screenOn = false;
    displayDirty = true;
    if(aodEnabled && powerMode != PWR_EXTREME && powerMode != PWR_SLEEP) {
      aodActive = true;
      M5.Display.setBrightness(12);
    } else {
      aodActive = false;
      M5.Display.setBrightness(0);
      M5.Display.sleep();      // SLPIN: cuts LCD controller from ~1.5mA to ~0.01mA
      displaySleeping = true;
    }
  }

  // Screen-off path
  if(!screenOn && !alarmRinging) {
    if(aodActive) {
      static unsigned long lastAodDraw = 0;
      if(millis() - lastAodDraw > 1000UL) { lastAodDraw = millis(); drawAOD(); }
      // Layer 1: vTaskDelay yields to PM — CPU light sleeps between AOD updates
      vTaskDelay(pdMS_TO_TICKS(250));
    } else {
      // Real light sleep — CPU halts, BLE controller wakes on schedule.
      // Unlike vTaskDelay, this actually stops the CPU and cuts draw to ~3mA.
      // GPIO37=BtnA, GPIO39=BtnB wake on active-low press.
      gpio_wakeup_enable(GPIO_NUM_37, GPIO_INTR_LOW_LEVEL);
      gpio_wakeup_enable(GPIO_NUM_39, GPIO_INTR_LOW_LEVEL);
      esp_sleep_enable_gpio_wakeup();
      esp_sleep_enable_timer_wakeup(200 * 1000ULL);  // 200ms in microseconds
      esp_light_sleep_start();
      // Resumes here after timer or button wakeup — M5.update() re-polls buttons next iteration
    }
    return;
  }

  // WiFi AP: per official ESP32 WebServer example, handleClient + delay(2) to yield CPU
  if(wifiAPActive) { wifiSrv.handleClient(); delay(2); }


  // Layer 3: only redraw when dirty
  if(displayDirty) {
    displayDirty = false;
    canvas.fillSprite(COL_BG);
    switch(curScreen) {
      case SCR_CLOCK:     drawClock();     break;
      case SCR_STOPWATCH: drawStopwatch(); break;
      case SCR_TIMER:     drawTimer();     break;
      case SCR_CONNECT:   drawConnect();   break;
      case SCR_POWER:     drawPower();     break;
      case SCR_ALARM:     drawAlarm();     break;
      case SCR_VOICE:     drawVoice();     break;
      case SCR_SETTINGS:  drawSettings();  break;
      case SCR_NOTIFS:    drawNotifs();    break;
      case SCR_DIAG:      drawDiag();      break;
    }
    drawBanner();
    canvas.pushSprite(0, 0);
  }

  // Layer 1: yield after draw — PM light sleeps until next event
  // NORMAL=40ms (25fps, snappy), EFFICIENT=80ms, EXTREME/SLEEP=200ms
  int loopMs = 40;
  if(powerMode == PWR_EFFICIENT) loopMs = 80;
  if(powerMode == PWR_EXTREME || powerMode == PWR_SLEEP) loopMs = 200;
  vTaskDelay(pdMS_TO_TICKS(loopMs));
}
