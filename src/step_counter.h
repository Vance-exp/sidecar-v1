/*
 * SIDECAR V1 — Step Counter
 * MPU6886 IMU-based pedometer using simple threshold peak detection.
 */
#pragma once

extern int g_stepCount;     // cumulative since boot

void stepCtr_init();        // configure IMU
void stepCtr_update();      // call from loop() every ~100ms — reads accel, counts steps
void stepCtr_reset();
