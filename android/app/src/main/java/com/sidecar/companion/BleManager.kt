package com.sidecar.companion

import android.Manifest
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.core.content.ContextCompat
import java.util.UUID

/**
 * BleManager — owns the entire BLE lifecycle for Sidecar V1.
 *
 * Thread model: all GATT callbacks arrive on a Binder thread; we post UI
 * work back to the main thread via [mainHandler].  [write] may be called
 * from any thread.
 */
class BleManager(private val context: Context) {

    // ── Public callbacks (set by MainActivity / services) ──────────────────
    var onConnected: (() -> Unit)? = null
    var onDisconnected: (() -> Unit)? = null
    var onLog: ((String) -> Unit)? = null

    // ── State ───────────────────────────────────────────────────────────────
    private val mainHandler = Handler(Looper.getMainLooper())
    private val bluetoothManager =
        context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val bluetoothAdapter: BluetoothAdapter?
        get() = bluetoothManager.adapter

    private var scanner: BluetoothLeScanner? = null
    private var gatt: BluetoothGatt? = null
    private var characteristic: BluetoothGattCharacteristic? = null

    @Volatile private var scanning = false
    @Volatile private var connected = false
    @Volatile private var userStopped = false

    // ── UUIDs ───────────────────────────────────────────────────────────────
    companion object {
        private const val TAG = "BleManager"
        private const val DEVICE_NAME = "SIDECAR V1"
        val SERVICE_UUID: UUID = UUID.fromString("5ac9bc5e-f8ba-48d4-8908-98b80b566e49")
        val CHAR_UUID: UUID    = UUID.fromString("bcca872f-1a3e-4491-b8ec-bfc93c5dd91a")
        private const val RECONNECT_DELAY_MS = 6_000L
        private const val SCAN_TIMEOUT_MS    = 30_000L
    }

    // ── Permissions ─────────────────────────────────────────────────────────

    fun hasBleScanPermission(): Boolean {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            ContextCompat.checkSelfPermission(
                context, Manifest.permission.BLUETOOTH_SCAN
            ) == PackageManager.PERMISSION_GRANTED
        } else {
            ContextCompat.checkSelfPermission(
                context, Manifest.permission.ACCESS_FINE_LOCATION
            ) == PackageManager.PERMISSION_GRANTED
        }
    }

    fun hasBleConnectPermission(): Boolean {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            ContextCompat.checkSelfPermission(
                context, Manifest.permission.BLUETOOTH_CONNECT
            ) == PackageManager.PERMISSION_GRANTED
        } else {
            true // not needed below API 31
        }
    }

    // ── Public control ──────────────────────────────────────────────────────

    fun start() {
        userStopped = false
        log("BleManager starting…")
        beginScan()
    }

    fun stop() {
        userStopped = true
        log("BleManager stopped by user")
        stopScan()
        disconnectGatt()
    }

    fun isConnected(): Boolean = connected

    // ── Scanning ────────────────────────────────────────────────────────────

    private fun beginScan() {
        if (!hasBleScanPermission() || !hasBleConnectPermission()) {
            log("Missing BLE permissions — cannot scan")
            return
        }
        val adapter = bluetoothAdapter
        if (adapter == null || !adapter.isEnabled) {
            log("Bluetooth not available / not enabled")
            scheduleReconnect()
            return
        }
        if (scanning) return

        scanner = adapter.bluetoothLeScanner
        if (scanner == null) {
            log("BluetoothLeScanner is null")
            scheduleReconnect()
            return
        }

        val filter = ScanFilter.Builder()
            .setDeviceName(DEVICE_NAME)
            .build()

        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .setCallbackType(ScanSettings.CALLBACK_TYPE_FIRST_MATCH)
            .setMatchMode(ScanSettings.MATCH_MODE_AGGRESSIVE)
            .setNumOfMatches(ScanSettings.MATCH_NUM_ONE_ADVERTISEMENT)
            .build()

        log("Scanning for "$DEVICE_NAME"…")
        scanning = true

        try {
            scanner?.startScan(listOf(filter), settings, scanCallback)
        } catch (e: SecurityException) {
            log("Scan permission denied: ${e.message}")
            scanning = false
            return
        }

        // Auto-stop scan after timeout to save battery, then retry
        mainHandler.postDelayed({
            if (scanning && !connected) {
                log("Scan timeout — restarting")
                stopScan()
                if (!userStopped) scheduleReconnect()
            }
        }, SCAN_TIMEOUT_MS)
    }

    private fun stopScan() {
        if (!scanning) return
        scanning = false
        try {
            scanner?.stopScan(scanCallback)
        } catch (_: SecurityException) { /* permission revoked */ }
        scanner = null
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device = result.device
            log("Found ${device.name ?: device.address} — connecting…")
            stopScan()
            connectDevice(device)
        }

        override fun onScanFailed(errorCode: Int) {
            scanning = false
            val reason = when (errorCode) {
                SCAN_FAILED_ALREADY_STARTED          -> "already started"
                SCAN_FAILED_APPLICATION_REGISTRATION_FAILED -> "registration failed"
                SCAN_FAILED_FEATURE_UNSUPPORTED      -> "feature unsupported"
                SCAN_FAILED_INTERNAL_ERROR           -> "internal error"
                else -> "code $errorCode"
            }
            log("Scan failed: $reason")
            if (!userStopped) scheduleReconnect()
        }
    }

    // ── GATT connection ─────────────────────────────────────────────────────

    private fun connectDevice(device: BluetoothDevice) {
        if (!hasBleConnectPermission()) {
            log("BLUETOOTH_CONNECT permission missing")
            return
        }
        try {
            gatt = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                device.connectGatt(
                    context,
                    false,
                    gattCallback,
                    BluetoothDevice.TRANSPORT_LE
                )
            } else {
                device.connectGatt(context, false, gattCallback)
            }
        } catch (e: SecurityException) {
            log("connectGatt denied: ${e.message}")
        }
    }

    private fun disconnectGatt() {
        connected = false
        characteristic = null
        try {
            gatt?.disconnect()
            gatt?.close()
        } catch (_: SecurityException) { }
        gatt = null
    }

    private val gattCallback = object : BluetoothGattCallback() {

        override fun onConnectionStateChange(
            gatt: BluetoothGatt, status: Int, newState: Int
        ) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    log("GATT connected — discovering services…")
                    try {
                        gatt.discoverServices()
                    } catch (e: SecurityException) {
                        log("discoverServices denied: ${e.message}")
                    }
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    val wasConnected = connected
                    connected = false
                    characteristic = null
                    try {
                        gatt.close()
                    } catch (_: SecurityException) { }
                    this@BleManager.gatt = null

                    if (wasConnected) {
                        log("Disconnected from SIDECAR V1")
                        mainHandler.post { onDisconnected?.invoke() }
                    }
                    if (!userStopped) scheduleReconnect()
                }
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                log("Service discovery failed (status $status) — retrying")
                disconnectGatt()
                if (!userStopped) scheduleReconnect()
                return
            }

            val service = gatt.getService(SERVICE_UUID)
            if (service == null) {
                log("Sidecar service not found on device")
                disconnectGatt()
                if (!userStopped) scheduleReconnect()
                return
            }

            val char = service.getCharacteristic(CHAR_UUID)
            if (char == null) {
                log("Sidecar characteristic not found")
                disconnectGatt()
                if (!userStopped) scheduleReconnect()
                return
            }

            characteristic = char
            connected = true
            log("SIDECAR V1 ready")
            mainHandler.post { onConnected?.invoke() }

            // Send time sync immediately
            val unixSeconds = System.currentTimeMillis() / 1000L
            write("T|$unixSeconds")
        }

        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                log("Write failed (status $status)")
            }
        }
    }

    // ── Write ───────────────────────────────────────────────────────────────

    /**
     * Write a UTF-8 packet to the watch characteristic.
     * Safe to call from any thread.
     * Handles both the API 33+ and legacy write paths, and auto-detects
     * write type from the characteristic's properties.
     */
    fun write(packet: String) {
        if (!connected) {
            log("Write skipped — not connected: $packet")
            return
        }
        val char = characteristic ?: run {
            log("Write skipped — characteristic null: $packet")
            return
        }
        val g = gatt ?: run {
            log("Write skipped — gatt null: $packet")
            return
        }

        val bytes = packet.toByteArray(Charsets.UTF_8)

        // Determine write type from characteristic properties
        val writeType = when {
            char.properties and BluetoothGattCharacteristic.PROPERTY_WRITE != 0 ->
                BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            char.properties and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE != 0 ->
                BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            else ->
                BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        }

        try {
            val success = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                // API 33+ — new atomic API
                val result = g.writeCharacteristic(char, bytes, writeType)
                result == BluetoothStatusCodes.SUCCESS
            } else {
                @Suppress("DEPRECATION")
                char.writeType = writeType
                @Suppress("DEPRECATION")
                char.value = bytes
                @Suppress("DEPRECATION")
                g.writeCharacteristic(char)
            }

            if (success) {
                log("TX: $packet")
            } else {
                log("Write returned false: $packet")
            }
        } catch (e: SecurityException) {
            log("Write permission denied: ${e.message}")
        }
    }

    // ── Reconnect ───────────────────────────────────────────────────────────

    private fun scheduleReconnect() {
        if (userStopped) return
        log("Reconnecting in ${RECONNECT_DELAY_MS / 1000}s…")
        mainHandler.postDelayed({
            if (!userStopped && !connected) {
                beginScan()
            }
        }, RECONNECT_DELAY_MS)
    }

    // ── Logging ─────────────────────────────────────────────────────────────

    private fun log(message: String) {
        Log.d(TAG, message)
        mainHandler.post { onLog?.invoke(message) }
    }
}
