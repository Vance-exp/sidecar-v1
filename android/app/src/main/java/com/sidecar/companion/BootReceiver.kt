package com.sidecar.companion

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

/**
 * BootReceiver — starts BleManager scanning automatically after device reboot
 * so the watch reconnects without the user needing to open the app.
 */
class BootReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED) return
        Log.d(TAG, "Boot completed — starting BleService")
        val app = context.applicationContext as? SidecarApp ?: return
        if (app.bleManager.hasBleScanPermission() && app.bleManager.hasBleConnectPermission()) {
            app.startBleService()
        }
    }

    companion object {
        private const val TAG = "BootReceiver"
    }
}
