package com.sidecar.companion

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.text.SpannableStringBuilder
import android.text.style.ForegroundColorSpan
import android.view.View
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.sidecar.companion.databinding.ActivityMainBinding
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * MainActivity — single-screen UI.
 *
 *  ┌─────────────────────────────────┐
 *  │  SIDECAR V1          (title)    │
 *  │  ● CONNECTED / ● SEARCHING      │
 *  │  [  START  ]  [  STOP  ]        │
 *  │  [ GRANT NOTIFICATION ACCESS ]  │  (only when not granted)
 *  │  ┌───────────────────────────┐  │
 *  │  │ TX log (scrollable)        │  │
 *  │  └───────────────────────────┘  │
 *  └─────────────────────────────────┘
 */
class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private val logBuffer = ArrayDeque<String>(LOG_LINES + 1)
    private val timeFormat = SimpleDateFormat("HH:mm:ss", Locale.getDefault())

    private val bleManager: BleManager
        get() = (applicationContext as SidecarApp).bleManager

    // ── Permission launcher ──────────────────────────────────────────────────

    private val requestPermissionsLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { results ->
            val allGranted = results.values.all { it }
            if (allGranted) {
                appendLog("All permissions granted")
                autoStart()
            } else {
                val denied = results.filter { !it.value }.keys.joinToString()
                appendLog("Denied: $denied")
            }
            refreshNotifBanner()
        }

    // ── Lifecycle ────────────────────────────────────────────────────────────

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        setupCallbacks()
        setupButtons()
        requestRequiredPermissions()
        refreshNotifBanner()
        updateConnectionUi(bleManager.isConnected())
    }

    override fun onResume() {
        super.onResume()
        refreshNotifBanner()
        updateConnectionUi(bleManager.isConnected())
    }

    // ── Callbacks from BleManager ────────────────────────────────────────────

    private fun setupCallbacks() {
        bleManager.onConnected = {
            // Already on main thread (BleManager posts to mainHandler)
            updateConnectionUi(true)
            appendLog("Connected to SIDECAR V1")
        }
        bleManager.onDisconnected = {
            updateConnectionUi(false)
            appendLog("Disconnected — searching…")
        }
        bleManager.onLog = { msg ->
            appendLog(msg)
        }
    }

    // ── Buttons ──────────────────────────────────────────────────────────────

    private fun setupButtons() {
        binding.btnStart.setOnClickListener {
            appendLog("Start pressed")
            bleManager.start()
        }
        binding.btnStop.setOnClickListener {
            appendLog("Stop pressed")
            bleManager.stop()
            updateConnectionUi(false)
        }
        binding.btnGrantNotif.setOnClickListener {
            SidecarNotifService.openSettings(this)
        }
    }

    // ── Permission flow ──────────────────────────────────────────────────────

    private fun requiredPermissions(): Array<String> {
        val list = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            list += Manifest.permission.BLUETOOTH_SCAN
            list += Manifest.permission.BLUETOOTH_CONNECT
        } else {
            list += Manifest.permission.ACCESS_FINE_LOCATION
        }
        list += Manifest.permission.READ_PHONE_STATE
        return list.toTypedArray()
    }

    private fun allPermissionsGranted(): Boolean =
        requiredPermissions().all {
            ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED
        }

    private fun requestRequiredPermissions() {
        val missing = requiredPermissions().filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) {
            autoStart()
        } else {
            requestPermissionsLauncher.launch(missing.toTypedArray())
        }
    }

    private fun autoStart() {
        if (allPermissionsGranted() && !bleManager.isConnected()) {
            bleManager.start()
        }
    }

    // ── UI updates ───────────────────────────────────────────────────────────

    private fun updateConnectionUi(connected: Boolean) {
        if (connected) {
            binding.statusDot.setColorFilter(Color.parseColor("#00CC44"))
            binding.tvStatus.text = getString(R.string.status_connected)
            binding.tvStatus.setTextColor(Color.parseColor("#00CC44"))
        } else {
            binding.statusDot.setColorFilter(Color.parseColor("#FF3333"))
            binding.tvStatus.text = getString(R.string.status_searching)
            binding.tvStatus.setTextColor(Color.parseColor("#FF3333"))
        }
    }

    private fun refreshNotifBanner() {
        val granted = SidecarNotifService.isEnabled(this)
        binding.btnGrantNotif.visibility = if (granted) View.GONE else View.VISIBLE
    }

    // ── Log ──────────────────────────────────────────────────────────────────

    private fun appendLog(message: String) {
        val timestamp = timeFormat.format(Date())
        val line = "[$timestamp] $message"

        logBuffer.addLast(line)
        while (logBuffer.size > LOG_LINES) logBuffer.removeFirst()

        val sb = SpannableStringBuilder()
        for (entry in logBuffer) {
            sb.append(entry)
            sb.append("\n")
        }
        binding.tvLog.text = sb

        // Auto-scroll to bottom
        binding.scrollLog.post {
            binding.scrollLog.fullScroll(View.FOCUS_DOWN)
        }
    }

    companion object {
        private const val LOG_LINES = 12
    }
}
