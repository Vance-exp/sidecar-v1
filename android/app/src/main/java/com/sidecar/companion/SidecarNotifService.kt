package com.sidecar.companion

import android.app.Notification
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.os.Build
import android.provider.Settings
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.util.Log

/**
 * NotificationListenerService — receives all posted notifications and
 * forwards relevant ones to the watch via BleManager.
 *
 * Filtering rules:
 *  - Skip ongoing notifications (persistent, not dismissible by user).
 *  - Skip system / launcher packages that produce noise.
 *  - Dedup: ignore the same notification key arriving within 2 seconds.
 *  - Truncate fields to firmware buffer limits:
 *      app   ≤ 19 chars
 *      title ≤ 39 chars
 *      body  ≤ 59 chars
 */
class SidecarNotifService : NotificationListenerService() {

    private val dedupCache = HashMap<String, Long>()  // key → last forwarded ms

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        val pkg = sbn.packageName ?: return
        val notif = sbn.notification ?: return

        // Skip ongoing (foreground services, media players in foreground, etc.)
        if (notif.flags and Notification.FLAG_ONGOING_EVENT != 0) return
        if (notif.flags and Notification.FLAG_FOREGROUND_SERVICE != 0) return

        // Skip noisy system packages
        if (pkg in BLOCKED_PACKAGES) return

        // Dedup: same key within 2 seconds → skip
        val now = System.currentTimeMillis()
        val lastSent = dedupCache[sbn.key] ?: 0L
        if (now - lastSent < DEDUP_WINDOW_MS) return
        dedupCache[sbn.key] = now

        // Prune dedup cache to avoid unbounded growth
        if (dedupCache.size > 200) {
            val threshold = now - DEDUP_WINDOW_MS * 10
            dedupCache.entries.removeAll { it.value < threshold }
        }

        val extras = notif.extras

        // ── Check for media / Spotify first ──────────────────────────────
        // EXTRA_TEMPLATE stores the style class name as a String, not a Parcelable
        val isMediaStyle = try {
            (extras.getString(Notification.EXTRA_TEMPLATE) ?: "").contains("MediaStyle")
        } catch (_: Exception) { false }

        if (isMediaStyle) {
            handleMediaNotification(extras)
            return
        }

        // ── Standard notification ─────────────────────────────────────────
        val appName = getAppLabel(pkg)
            .take(APP_MAX)

        val title = (extras.getCharSequence(Notification.EXTRA_TITLE)?.toString()
            ?: extras.getCharSequence(Notification.EXTRA_TITLE_BIG)?.toString()
            ?: "").trim().take(TITLE_MAX)

        val body = (extras.getCharSequence(Notification.EXTRA_TEXT)?.toString()
            ?: extras.getCharSequence(Notification.EXTRA_BIG_TEXT)?.toString()
            ?: extras.getCharSequence(Notification.EXTRA_SUMMARY_TEXT)?.toString()
            ?: "").trim().take(BODY_MAX)

        if (title.isEmpty() && body.isEmpty()) return   // nothing worth sending

        val packet = "N|$appName|$title|$body"
        Log.d(TAG, "Notif → $packet")
        bleManager?.write(packet)
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification) {
        dedupCache.remove(sbn.key)
    }

    // ── Media handling ───────────────────────────────────────────────────────

    private fun handleMediaNotification(extras: android.os.Bundle) {
        // Try to pull artist/title from media session manager first
        val mediaSessionManager = getSystemService(MEDIA_SESSION_SERVICE) as? MediaSessionManager
        val activeSessions: List<MediaController>? = try {
            mediaSessionManager?.getActiveSessions(
                ComponentName(this, SidecarNotifService::class.java)
            )
        } catch (_: SecurityException) { null }

        if (!activeSessions.isNullOrEmpty()) {
            val controller = activeSessions[0]
            val meta = controller.metadata
            if (meta != null) {
                val artist = (meta.getString(android.media.MediaMetadata.METADATA_KEY_ARTIST)
                    ?: meta.getString(android.media.MediaMetadata.METADATA_KEY_ALBUM_ARTIST)
                    ?: "").trim().take(TITLE_MAX)
                val song = (meta.getString(android.media.MediaMetadata.METADATA_KEY_TITLE)
                    ?: "").trim().take(BODY_MAX)
                if (song.isNotEmpty()) {
                    val packet = "S|$artist|$song"
                    Log.d(TAG, "Media → $packet")
                    bleManager?.write(packet)
                    return
                }
            }
        }

        // Fallback: read from notification extras
        val artist = (extras.getCharSequence(Notification.EXTRA_TEXT)?.toString()
            ?: "").trim().take(TITLE_MAX)
        val song = (extras.getCharSequence(Notification.EXTRA_TITLE)?.toString()
            ?: "").trim().take(BODY_MAX)
        if (song.isNotEmpty()) {
            val packet = "S|$artist|$song"
            Log.d(TAG, "Media fallback → $packet")
            bleManager?.write(packet)
        }
    }

    // ── Helpers ──────────────────────────────────────────────────────────────

    private fun getAppLabel(packageName: String): String {
        return try {
            val pm = applicationContext.packageManager
            val info = pm.getApplicationInfo(packageName, 0)
            pm.getApplicationLabel(info).toString()
        } catch (_: Exception) {
            packageName.substringAfterLast('.')
        }
    }

    private val bleManager: BleManager?
        get() = (applicationContext as? SidecarApp)?.bleManager

    // ── Companion ────────────────────────────────────────────────────────────

    companion object {
        private const val TAG = "SidecarNotifService"
        private const val DEDUP_WINDOW_MS = 2_000L

        // Firmware buffer limits
        private const val APP_MAX   = 19
        private const val TITLE_MAX = 39
        private const val BODY_MAX  = 59

        /** Packages whose notifications are always suppressed. */
        private val BLOCKED_PACKAGES = setOf(
            "android",
            "com.android.systemui",
            "com.android.phone",
            "com.android.launcher3",
            "com.android.launcher",
            "com.google.android.gms",
            "com.google.android.gsf",
            "com.android.settings",
            "com.android.packageinstaller",
            "com.android.providers.downloads",
            "com.android.vending",                  // Play Store install progress
            "com.google.android.setupwizard"
        )

        /**
         * Returns true if the notification listener service is enabled in
         * system settings.
         */
        fun isEnabled(context: Context): Boolean {
            val flat = Settings.Secure.getString(
                context.contentResolver,
                "enabled_notification_listeners"
            ) ?: return false
            val cn = ComponentName(context, SidecarNotifService::class.java).flattenToString()
            return flat.split(":").any { it.trim() == cn }
        }

        /**
         * Opens the system notification listener settings screen.
         */
        fun openSettings(context: Context) {
            val intent = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP_MR1) {
                Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)
            } else {
                Intent("android.settings.ACTION_NOTIFICATION_LISTENER_SETTINGS")
            }
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            context.startActivity(intent)
        }
    }
}
