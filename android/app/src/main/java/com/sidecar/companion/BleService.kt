package com.sidecar.companion

import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat

/**
 * BleService — foreground service that keeps the BleManager alive in background.
 *
 * Without this, Android kills the app process after a few minutes off-screen,
 * dropping the BLE connection and silencing all notifications.
 *
 * START_STICKY ensures Android restarts us if killed under memory pressure.
 * The foreground notification is required on API 26+ to prevent this from
 * being treated as a background service.
 */
class BleService : Service() {

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            Log.d(TAG, "Stop requested")
            (applicationContext as? SidecarApp)?.bleManager?.stop()
            stopForegroundCompat()
            stopSelf()
            return START_NOT_STICKY
        }

        Log.d(TAG, "Starting foreground")
        startForegroundCompat()
        (applicationContext as? SidecarApp)?.bleManager?.start()
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        (applicationContext as? SidecarApp)?.bleManager?.stop()
        super.onDestroy()
    }

    private fun startForegroundCompat() {
        val notif = NotificationCompat.Builder(this, SidecarApp.NOTIF_CHANNEL_ID)
            .setContentTitle("SIDECAR V1")
            .setContentText("Searching for watch…")
            .setSmallIcon(R.drawable.ic_circle)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setOngoing(true)
            .setSilent(true)
            .build()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            // API 34+: must declare foreground service type
            startForeground(NOTIF_ID, notif, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } else {
            startForeground(NOTIF_ID, notif)
        }
    }

    private fun stopForegroundCompat() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_REMOVE)
        } else {
            @Suppress("DEPRECATION")
            stopForeground(true)
        }
    }

    companion object {
        private const val TAG     = "BleService"
        const val ACTION_STOP     = "com.sidecar.companion.BLE_STOP"
        private const val NOTIF_ID = 1001

        fun buildStartIntent(context: android.content.Context) =
            Intent(context, BleService::class.java)

        fun buildStopIntent(context: android.content.Context) =
            Intent(context, BleService::class.java).apply { action = ACTION_STOP }
    }
}
