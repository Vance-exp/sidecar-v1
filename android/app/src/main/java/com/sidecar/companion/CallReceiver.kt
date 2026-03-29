package com.sidecar.companion

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.telephony.TelephonyManager
import android.util.Log

/**
 * CallReceiver — listens for incoming calls and forwards the caller ID
 * to the watch via BleManager as "C|name_or_number".
 *
 * Registered statically in the manifest for ACTION_PHONE_STATE_CHANGED.
 * Requires READ_PHONE_STATE permission.
 */
class CallReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != TelephonyManager.ACTION_PHONE_STATE_CHANGED) return

        val state = intent.getStringExtra(TelephonyManager.EXTRA_STATE) ?: return

        if (state == TelephonyManager.EXTRA_STATE_RINGING) {
            // EXTRA_INCOMING_NUMBER is deprecated in API 29 but still populated
            // when READ_PHONE_STATE is granted.  On API 29+ with no permission,
            // it will simply be null and we send "C|Unknown".
            @Suppress("DEPRECATION")
            val number = intent.getStringExtra(TelephonyManager.EXTRA_INCOMING_NUMBER)
                ?.trim()
                ?.takeIf { it.isNotEmpty() }
                ?: "Unknown"

            val packet = "C|$number"
            Log.d(TAG, "Incoming call → $packet")

            val bleManager = (context.applicationContext as? SidecarApp)?.bleManager
            bleManager?.write(packet) ?: Log.w(TAG, "BleManager not available")
        }
    }

    companion object {
        private const val TAG = "CallReceiver"
    }
}
