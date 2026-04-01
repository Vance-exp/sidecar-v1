/*
 * SIDECAR V1 — Step Counter
 * Zero-crossing pedometer — more robust than a simple magnitude threshold.
 *
 * Algorithm:
 *   1. Compute acceleration vector magnitude (~1.0g at rest)
 *   2. Heavy low-pass filter (α=0.1) → gravity/posture baseline
 *   3. High-pass residual = mag - baseline  (removes DC/gravity)
 *   4. Light low-pass on residual (α=0.4) → smoothed signal, kills jitter
 *   5. Step detected on rising zero-crossing of smoothed signal AND
 *      peak in the crossing window exceeds adaptive threshold
 *   6. 350ms debounce (max ~2.8 steps/sec — fast walk, no false arm swings)
 *
 * Why zero-crossing instead of threshold:
 *   A simple "exceeds threshold" fires on any jolt (door slam, arm wave).
 *   Zero-crossing requires a full negative → positive transition, meaning a
 *   complete step impulse (arm/leg decelerates then accelerates). Spurious
 *   one-sided impulses are ignored.
 */
#include <Arduino.h>
#include <M5Unified.h>
#include "step_counter.h"
#include "display_mgr.h"  // g_screenOn

int g_stepCount = 0;

static float         s_baseline    = 1.0f;   // gravity baseline (slow LP)
static float         s_smooth      = 0.0f;   // smoothed high-pass residual
static float         s_prevSmooth  = 0.0f;   // previous smoothed value (for zero-cross)
static float         s_windowPeak  = 0.0f;   // peak magnitude seen since last zero-crossing
static unsigned long s_lastStep    = 0;

// Adaptive threshold state
static float         s_peakHi      = 0.0f;
static float         s_peakLo      = 0.0f;
static unsigned long s_peakReset   = 0;

void stepCtr_init() {
    s_baseline   = 1.0f;
    s_smooth     = 0.0f;
    s_prevSmooth = 0.0f;
    s_windowPeak = 0.0f;
    s_lastStep   = 0;
    s_peakHi     = 0.0f;
    s_peakLo     = 0.0f;
    s_peakReset  = millis();
}

void stepCtr_update() {
    // Screen-on:  50Hz (20ms)  — Nyquist-correct for 20Hz step signal.
    // Screen-off: 5Hz  (200ms) — step debounce is 300ms so 5Hz still catches all steps.
    //   Matches the wrist-wake accel poll rate (200ms) so both share the same I2C wake.
    //   Reduces I2C wakeups from 10/sec to 5/sec screen-off → more light sleep time.
    static unsigned long s_lastImuRead = 0;
    unsigned long now = millis();
    unsigned long imuInterval = g_screenOn ? 20UL : 200UL;
    if (now - s_lastImuRead < imuInterval) return;
    s_lastImuRead = now;

    float ax, ay, az;
    if (!M5.Imu.getAccel(&ax, &ay, &az)) return;

    // 1. Vector magnitude (~1.0g at rest)
    float mag = sqrtf(ax*ax + ay*ay + az*az);

    // 2. Heavy low-pass → gravity + posture baseline (α=0.1, ~10s time constant)
    s_baseline = s_baseline * 0.90f + mag * 0.10f;

    // 3. High-pass residual
    float residual = mag - s_baseline;

    // 4. Light low-pass on residual → smooth out IMU noise (α=0.4)
    s_smooth = s_smooth * 0.60f + residual * 0.40f;

    // Track window peak for adaptive threshold
    if (s_smooth > s_windowPeak) s_windowPeak = s_smooth;
    if (residual > s_peakHi) s_peakHi = residual;
    if (residual < s_peakLo) s_peakLo = residual;

    // Decay peaks every 2 seconds — but only if no step was detected recently.
    // Decaying while walking would shrink the threshold mid-gait and cause
    // false positives on the next crossing. The 300ms debounce ensures
    // s_lastStep is always set on a real step, so this guard is reliable.
    if (millis() - s_peakReset > 2000UL) {
        s_peakReset = millis();
        if (millis() - s_lastStep > 2000UL) {  // no step in last 2s = idle
            s_peakHi     *= 0.5f;
            s_peakLo     *= 0.5f;
            s_windowPeak *= 0.5f;
        }
    }

    // 5. Adaptive threshold — 40% of peak-to-peak, hard minimum 0.15g
    //    (0.15g is significantly higher than old 0.08g — filters arm waves)
    float threshold = (s_peakHi - s_peakLo) * 0.40f;
    if (threshold < 0.15f) threshold = 0.15f;

    // 6. Rising zero-crossing detection
    //    Previous sample was negative, current is positive → step candidate
    bool zeroCross = (s_prevSmooth < 0.0f) && (s_smooth >= 0.0f);

    if (zeroCross) {
        // Check that the peak seen since last crossing exceeds threshold
        // AND debounce (300ms → max ~3.3 steps/sec, handles a brisk walk/slow run)
        if (s_windowPeak >= threshold &&
            (millis() - s_lastStep) > 300UL) {
            g_stepCount++;
            s_lastStep   = millis();
            s_windowPeak = 0.0f;  // BUG 19: only reset on accepted step, not on rejection
        }
        // If debounce rejected, leave s_windowPeak intact so the accumulated
        // peak carries forward to the next crossing (avoids under-counting rapid steps)
    }

    s_prevSmooth = s_smooth;
}

void stepCtr_reset() {
    g_stepCount = 0;
}
