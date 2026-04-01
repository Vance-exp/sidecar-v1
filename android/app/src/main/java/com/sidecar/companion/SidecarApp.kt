package com.sidecar.companion

import android.app.Application
import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Intent
import android.os.Build
import android.util.Log

/**
 * Application singleton — holds the single BleManager instance so every
 * component (service, receiver, activity) can reach it through
 * (applicationContext as SidecarApp).bleManager without a service binding.
 */
class SidecarApp : Application() {

    lateinit var bleManager: BleManager
        private set

    override fun onCreate() {
        super.onCreate()
        Log.d(TAG, "SidecarApp created")
        bleManager = BleManager(this)
        createNotifChannel()
        // Auto-start the foreground service so BLE survives background/screen-off.
        // On Android 14+ this throws ForegroundServiceStartNotAllowedException when
        // launched cold from background (e.g. boot before user unlocks). Ignore safely —
        // ConnectionFragment.onResume() will call startBleService() once the user opens the app.
        try { startBleService() } catch (_: Exception) {}
    }

    override fun onTerminate() {
        bleManager.stop()
        super.onTerminate()
    }

    private fun createNotifChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                NOTIF_CHANNEL_ID,
                "Sidecar Connection",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Shows BLE connection status to SIDECAR V1"
                setShowBadge(false)
            }
            getSystemService(NotificationManager::class.java)?.createNotificationChannel(channel)
        }
    }

    fun startBleService() {
        val intent = BleService.buildStartIntent(this)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent)
        } else {
            startService(intent)
        }
    }

    fun stopBleService() {
        startService(BleService.buildStopIntent(this))
    }

    companion object {
        private const val TAG = "SidecarApp"
        const val NOTIF_CHANNEL_ID = "sidecar_ble"
    }
}
