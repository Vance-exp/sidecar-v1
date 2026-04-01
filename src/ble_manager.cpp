/*
 * SIDECAR V1 — BLE Manager
 * NimBLE GATT server with Bellafaire protocol parser.
 *
 * Phone→Watch packet formats:
 *   T|unix_timestamp          — time sync
 *   N|APP|TITLE|BODY          — notification
 *   S|artist|song|playing|vol — media state push
 *   C|name                    — incoming call
 *   X|KEY|VAL                 — config
 *   Q|                        — request state sync
 *
 * Watch→Phone packet formats:
 *   MC|CMD                    — media control (PLAY, NEXT, PREV, VOLU, VOLD)
 */
#include <Arduino.h>
#include <NimBLEDevice.h>
#include "esp_bt.h"   // esp_bt_sleep_enable() — BLE controller modem sleep
#include "ble_manager.h"
#include "hardware_config.h"
#include "notif_queue.h"
#include "time_manager.h"
#include "cfg_manager.h"

bool     g_bleConn    = false;
uint16_t g_bleConnHandle = BLE_HS_CONN_HANDLE_NONE;

static NimBLEServer*         s_srv = nullptr;
static NimBLECharacteristic* s_chr = nullptr;

// ── Server callbacks ──────────────────────────────────────────────────────────
class SrvCB : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* srv, ble_gap_conn_desc* d) override {
        g_bleConn       = true;
        g_bleConnHandle = d->conn_handle;
        // Initial fast params: 100-200ms interval, latency=0, timeout=6s.
        // display_mgr will tighten/relax these dynamically as screen sleeps/wakes.
        srv->updateConnParams(d->conn_handle, 80, 160, 0, 600);
        cfg_requestStateSync();
        Serial.printf("[BLE] connected handle=%d\n", d->conn_handle);
    }
    void onDisconnect(NimBLEServer*) override {
        g_bleConn       = false;
        g_bleConnHandle = BLE_HS_CONN_HANDLE_NONE;
        NimBLEDevice::startAdvertising();
        Serial.println("[BLE] disconnected, re-advertising");
    }
};

// ── Characteristic callbacks ──────────────────────────────────────────────────
class ChrCB : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        // Rate-limit incoming writes to 20 packets/sec.
        // Prevents DoS via packet flood (CPU exhaustion, queue overflow, battery drain).
        static unsigned long s_windowStart = 0;
        static uint8_t       s_windowCount = 0;
        unsigned long now = millis();
        if (now - s_windowStart >= 1000UL) {
            s_windowStart = now;
            s_windowCount = 0;
        }
        if (++s_windowCount > 20) {
            Serial.println("[BLE] rate limit — packet dropped");
            return;
        }

        std::string raw = c->getValue();
        // Reject packets that are empty, too long (>255 = beyond our max MTU),
        // or don't follow the mandatory X| two-char prefix format.
        if (raw.size() < 3 || raw.size() > 255 || raw[1] != '|') return;

        switch (raw[0]) {
            case 'T':
                tmMgr_onBleTime(atol(raw.c_str() + 2));
                break;
            case 'N':
                nq_onNotifPacket(raw.c_str(), raw.size());
                break;
            case 'S':
                nq_onMediaPacket(raw.c_str(), raw.size());
                break;
            case 'C':
                nq_onCallPacket(raw.c_str(), raw.size());
                break;
            case 'W':
                nq_onWeatherPacket(raw.c_str(), raw.size());
                break;
            case 'X':
                cfg_onPacket(raw.c_str(), raw.size());
                break;
            case 'Q':
                cfg_requestStateSync();
                break;
        }
    }
};

// ── Init ──────────────────────────────────────────────────────────────────────
void bleMgr_init() {
    NimBLEDevice::init(BLE_DEVICE_NAME);
    NimBLEDevice::setMTU(256);
    NimBLEDevice::setPower(ESP_PWR_LVL_N12);  // -12dBm — plenty for 1-2m wearable

    // BLE controller modem sleep: radio sleeps between advertising/connection events.
    // Works alongside esp_pm auto light sleep — both can be active simultaneously.
    // Does NOT affect host functionality; NimBLE handles all GATT/GAP normally.
    if (esp_bt_sleep_enable() != ESP_OK) {
        Serial.println("[BLE] modem sleep enable failed");
    } else {
        Serial.println("[BLE] modem sleep enabled");
    }

    s_srv = NimBLEDevice::createServer();
    s_srv->setCallbacks(new SrvCB());

    NimBLEService* svc = s_srv->createService(BLE_SVC_UUID);
    s_chr = svc->createCharacteristic(BLE_CHR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    s_chr->setCallbacks(new ChrCB());
    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_SVC_UUID);
    // Start with ultra-slow advertising — pwrMgr_apply() speeds it up for NORMAL mode
    bleMgr_setAdvInterval(BLE_ADV_ULTRA_SLOW, BLE_ADV_ULTRA_SLOW + 64);
    adv->start();

    Serial.println("[BLE] GATT server started");
}

// ── Advertising interval ──────────────────────────────────────────────────────
void bleMgr_setAdvInterval(int minSlots, int maxSlots) {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->setMinInterval(minSlots);
    adv->setMaxInterval(maxSlots);
}

// ── Connection interval update ────────────────────────────────────────────────
void bleMgr_setConnInterval(uint16_t minMs, uint16_t maxMs, uint16_t latency, uint16_t timeoutMs) {
    if (!g_bleConn || g_bleConnHandle == BLE_HS_CONN_HANDLE_NONE || !s_srv) return;
    // NimBLE units: interval in 1.25ms slots, timeout in 10ms slots
    uint16_t minSlots     = (uint16_t)(minMs * 4 / 5);    // ms → 1.25ms units
    uint16_t maxSlots     = (uint16_t)(maxMs * 4 / 5);
    uint16_t timeoutSlots = (uint16_t)(timeoutMs / 10);
    s_srv->updateConnParams(g_bleConnHandle, minSlots, maxSlots, latency, timeoutSlots);
    Serial.printf("[BLE] conn interval → %u-%ums lat=%u tmo=%ums\n",
        minMs, maxMs, latency, timeoutMs);
}

// ── NOTIFY ────────────────────────────────────────────────────────────────────
void bleMgr_notify(const char* packet) {
    if (!g_bleConn || !s_chr) return;
    s_chr->setValue((uint8_t*)packet, strlen(packet));
    s_chr->notify();
}
