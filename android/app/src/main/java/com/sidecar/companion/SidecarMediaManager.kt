package com.sidecar.companion

import android.content.ComponentName
import android.content.Context
import android.media.AudioManager
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.util.Log

/**
 * Hooks into the Android MediaSession API to track the active media session.
 *
 * Design goals:
 *  - Purely event-driven: no polling. Registers callbacks and reacts to changes.
 *  - Single active controller: always tracks the first session in the active list.
 *  - Forwards playback metadata + state to [SidecarViewModel.onMediaUpdate].
 *  - Dispatches MC| transport commands received from the watch to the active controller.
 *
 * Requires the app to have NotificationListenerService permission (already granted for
 * [SidecarNotifService]) — [MediaSessionManager.getActiveSessions] uses the same
 * ComponentName.
 */
class SidecarMediaManager(
    private val context: Context,
    private val viewModel: SidecarViewModel
) {

    private val msm: MediaSessionManager =
        context.getSystemService(Context.MEDIA_SESSION_SERVICE) as MediaSessionManager

    private var activeController: MediaController? = null

    // Callback registered on the currently tracked MediaController
    private val controllerCallback = object : MediaController.Callback() {
        override fun onMetadataChanged(metadata: MediaMetadata?) {
            pushUpdate(activeController, metadata)
        }
        override fun onPlaybackStateChanged(state: PlaybackState?) {
            pushUpdate(activeController, activeController?.metadata)
        }
        override fun onSessionDestroyed() {
            Log.d(TAG, "Session destroyed — waiting for next active session")
            detachController()
        }
    }

    // Listener fires when the list of active sessions changes
    private val sessionsChangedListener = MediaSessionManager.OnActiveSessionsChangedListener { controllers ->
        onActiveSessionsChanged(controllers)
    }

    fun start() {
        try {
            msm.addOnActiveSessionsChangedListener(
                sessionsChangedListener,
                ComponentName(context, SidecarNotifService::class.java)
            )
            // Attach to whatever is already playing
            val current = msm.getActiveSessions(
                ComponentName(context, SidecarNotifService::class.java)
            )
            onActiveSessionsChanged(current)
        } catch (e: SecurityException) {
            Log.w(TAG, "No notification listener permission — media tracking disabled: $e")
        }

        // Wire up command dispatch: watch → phone
        viewModel.onMediaCommand = { cmd -> dispatchCommand(cmd) }
    }

    fun stop() {
        try { msm.removeOnActiveSessionsChangedListener(sessionsChangedListener) }
        catch (_: Exception) {}
        detachController()
        viewModel.onMediaCommand = null
    }

    // ── Session tracking ──────────────────────────────────────────────────────

    private fun onActiveSessionsChanged(controllers: List<MediaController>?) {
        val first = controllers?.firstOrNull()
        if (first?.sessionToken == activeController?.sessionToken) return  // same session

        detachController()
        if (first != null) {
            activeController = first
            first.registerCallback(controllerCallback)
            Log.d(TAG, "Tracking session: ${first.packageName}")
            pushUpdate(first, first.metadata)
        } else {
            // No active session → clear media display
            viewModel.onMediaUpdate("", "", false, -1)
        }
    }

    private fun detachController() {
        activeController?.unregisterCallback(controllerCallback)
        activeController = null
    }

    // ── Metadata → ViewModel ──────────────────────────────────────────────────

    private fun pushUpdate(controller: MediaController?, meta: MediaMetadata?) {
        val artist = (meta?.getString(MediaMetadata.METADATA_KEY_ARTIST)
            ?: meta?.getString(MediaMetadata.METADATA_KEY_ALBUM_ARTIST)
            ?: "").trim().take(39)

        val song = (meta?.getString(MediaMetadata.METADATA_KEY_TITLE)
            ?: "").trim().take(59)

        val playing = controller?.playbackState?.state == PlaybackState.STATE_PLAYING

        // Volume: prefer remote volume info, fall back to -1 (unknown)
        val volInfo = controller?.playbackInfo
        val volume = if (volInfo != null) {
            val max = volInfo.maxVolume
            val cur = volInfo.currentVolume
            if (max > 0) (cur * 100) / max else -1
        } else -1

        Log.d(TAG, "Media update → artist=$artist song=$song playing=$playing vol=$volume")
        viewModel.onMediaUpdate(artist, song, playing, volume)
    }

    // ── Command dispatch (MC|CMD from watch) ──────────────────────────────────

    private fun dispatchCommand(cmd: String) {
        val transport = activeController?.transportControls ?: run {
            Log.w(TAG, "No active session for command: $cmd")
            return
        }
        Log.d(TAG, "MC dispatch: $cmd")
        when (cmd) {
            "PLAY"  -> {
                if (activeController?.playbackState?.state == PlaybackState.STATE_PLAYING) {
                    transport.pause()
                } else {
                    transport.play()
                }
            }
            "NEXT"  -> transport.skipToNext()
            "PREV"  -> transport.skipToPrevious()
            "VOLU"  -> adjustVolume(+1)
            "VOLD"  -> adjustVolume(-1)
        }
    }

    private fun adjustVolume(direction: Int) {
        val ctrl = activeController
        val info = ctrl?.playbackInfo

        // Local playback (phone speaker / headphones) — use AudioManager.
        // MediaController.setVolumeTo() is a no-op for PLAYBACK_TYPE_LOCAL.
        if (info == null || info.playbackType == MediaController.PlaybackInfo.PLAYBACK_TYPE_LOCAL) {
            val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
            val flag = if (direction > 0) AudioManager.ADJUST_RAISE else AudioManager.ADJUST_LOWER
            am.adjustStreamVolume(AudioManager.STREAM_MUSIC, flag, AudioManager.FLAG_SHOW_UI)
            return
        }

        // Remote / cast session — use MediaController volume API.
        // Step by ~7% of max so one press is noticeable regardless of scale.
        val step = (info.maxVolume * 0.07f).toInt().coerceAtLeast(1)
        val next = (info.currentVolume + direction * step).coerceIn(0, info.maxVolume)
        ctrl!!.setVolumeTo(next, 0)
    }

    companion object {
        private const val TAG = "SidecarMediaManager"
    }
}
