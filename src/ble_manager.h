/*
 * SIDECAR V1 — BLE Manager
 * NimBLE GATT server with Bellafaire protocol (T|/N|/S|/C| packets).
 * deinit() NOT provided — crashes RF stack at runtime. Use ESP.restart() instead.
 */
#pragma once

extern bool     g_bleConn;
extern uint16_t g_bleConnHandle;

// Create GATT server, register service + characteristic, start advertising
void bleMgr_init();

// Update advertising interval without restart (safe between 80-240MHz)
void bleMgr_setAdvInterval(int minSlots, int maxSlots);

// Send NOTIFY packet to connected phone (no-op if not connected)
void bleMgr_notify(const char* packet);

// Update connection parameters on the live connection.
// Call when screen sleeps (slow = save power) or wakes (fast = responsive).
// minMs/maxMs in milliseconds; latency = allowed missed events; timeoutMs = supervision.
void bleMgr_setConnInterval(uint16_t minMs, uint16_t maxMs, uint16_t latency, uint16_t timeoutMs);
