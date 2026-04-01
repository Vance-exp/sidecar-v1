package com.sidecar.companion

import android.Manifest
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.util.Log
import androidx.core.content.ContextCompat
import java.util.UUID

/**
 * BleManager — owns the entire BLE lifecycle for Sidecar V1.
 *
 * Thread model: all GATT callbacks arrive on a Binder thread; we post UI
 * work back to the main thread via [mainHandler].
 *
 * GATT serialization: Android BLE GATT allows only one in-flight operation
 * at a time (write / descriptor write / read).  All operations go through
 * [opQueue] and are drained one-by-one via [opComplete] callbacks.
 *
 * Connection sequence:
 *   connect → requestMtu(512) → discoverServices
 *             → enqueue(CCCD enable)
 *             → onDescriptorWrite → opComplete
 *             → enqueue(T|) → onCharacteristicWrite → opComplete
 *             → enqueue(Q|) → onCharacteristicWrite → opComplete
 *             → queue drains normally for all subsequent write() calls
 */
class BleManager(private val context: Context) {

    // ── Public callbacks ──────────────────────────────────────────────────────
    var onConnected: (() -> Unit)? = null
    var onDisconnected: (() -> Unit)? = null
    var onLog: ((String) -> Unit)? = null
    var onIncomingPacket: ((String) -> Unit)? = null

    // ── State ─────────────────────────────────────────────────────────────────
    private val mainHandler = Handler(Looper.getMainLooper())
    private val bluetoothManager =
        context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val bluetoothAdapter: BluetoothAdapter?
        get() = bluetoothManager.adapter

    private var scanner: BluetoothLeScanner? = null
    private var gatt: BluetoothGatt? = null
    private var characteristic: BluetoothGattCharacteristic? = null

    @Volatile private var scanning    = false
    @Volatile private var connected   = false
    @Volatile private var userStopped = false

    // ── GATT operation queue ──────────────────────────────────────────────────
    // All GATT writes (characteristic + descriptor) are serialized through here.
    private val opQueue = ArrayDeque<() -> Unit>()
    @Volatile private var opInFlight = false

    private fun enqueueOp(op: () -> Unit) {
        synchronized(opQueue) { opQueue.addLast(op) }
        drainOps()
    }

    /** Pull and execute the next queued operation if none is in-flight. */
    private fun drainOps() {
        val op = synchronized(opQueue) {
            if (opInFlight || opQueue.isEmpty()) return
            opInFlight = true
            opQueue.removeFirst()
        }
        op.invoke()
    }

    /** Call from every GATT callback that signifies an operation completed. */
    private fun opComplete() {
        synchronized(opQueue) { opInFlight = false }
        drainOps()
    }

    private fun clearQueue() {
        synchronized(opQueue) { opQueue.clear(); opInFlight = false }
    }
    // ─────────────────────────────────────────────────────────────────────────

    companion object {
        private const val TAG             = "BleManager"
        private const val DEVICE_NAME     = "SIDECAR V1"
        val SERVICE_UUID: UUID = UUID.fromString("5ac9bc5e-f8ba-48d4-8908-98b80b566e49")
        val CHAR_UUID:    UUID = UUID.fromString("bcca872f-1a3e-4491-b8ec-bfc93c5dd91a")
        private val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        private const val RECONNECT_DELAY_MS = 2_000L
        private const val SCAN_TIMEOUT_MS    = 30_000L
    }

    // ── Permissions ───────────────────────────────────────────────────────────

    fun hasBleScanPermission(): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_SCAN) ==
                PackageManager.PERMISSION_GRANTED
        else
            ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_FINE_LOCATION) ==
                PackageManager.PERMISSION_GRANTED

    fun hasBleConnectPermission(): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_CONNECT) ==
                PackageManager.PERMISSION_GRANTED
        else true

    // ── Public control ────────────────────────────────────────────────────────

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

    fun sendConfig(key: String, value: Int) = write("X|$key|$value")

    // ── Scanning ──────────────────────────────────────────────────────────────

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

        // Filter by service UUID — this is in the advertising packet itself and
        // works reliably across all Android versions.  Name-based filtering can
        // miss devices whose name is only in the scan response.
        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid(SERVICE_UUID))
            .build()

        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        log("Scanning for \"$DEVICE_NAME\"…")
        scanning = true

        try {
            scanner?.startScan(listOf(filter), settings, scanCallback)
        } catch (e: SecurityException) {
            log("Scan permission denied: ${e.message}")
            scanning = false
            return
        }

        // Auto-stop scan after timeout, then schedule retry
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
        } catch (_: SecurityException) { }
        scanner = null
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device = result.device
            // Software name guard — skip if we somehow matched a different service
            try {
                if (device.name != null && device.name != DEVICE_NAME) return
            } catch (_: SecurityException) { }
            log("Found ${try { device.name ?: device.address } catch (_: SecurityException) { device.address }} — connecting…")
            stopScan()
            connectDevice(device)
        }

        override fun onScanFailed(errorCode: Int) {
            scanning = false
            val reason = when (errorCode) {
                SCAN_FAILED_ALREADY_STARTED                  -> "already started"
                SCAN_FAILED_APPLICATION_REGISTRATION_FAILED -> "registration failed"
                SCAN_FAILED_FEATURE_UNSUPPORTED              -> "feature unsupported"
                SCAN_FAILED_INTERNAL_ERROR                   -> "internal error"
                else                                         -> "code $errorCode"
            }
            log("Scan failed: $reason")
            if (!userStopped) scheduleReconnect()
        }
    }

    // ── GATT connection ───────────────────────────────────────────────────────

    private fun connectDevice(device: BluetoothDevice) {
        if (!hasBleConnectPermission()) {
            log("BLUETOOTH_CONNECT permission missing")
            return
        }
        try {
            gatt = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
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
        clearQueue()
        try { gatt?.disconnect(); gatt?.close() } catch (_: SecurityException) { }
        gatt = null
    }

    private val gattCallback = object : BluetoothGattCallback() {

        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    log("GATT connected — negotiating MTU…")
                    // Request a larger MTU before service discovery.
                    // Default 23 bytes leaves only 20 bytes payload — too small for
                    // alarm/config packets (~26+ chars).  Watch supports up to 256.
                    try {
                        gatt.requestMtu(512)
                    } catch (e: SecurityException) {
                        log("requestMtu denied — proceeding without: ${e.message}")
                        try { gatt.discoverServices() } catch (_: SecurityException) { }
                    }
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    val wasConnected = connected
                    connected = false
                    characteristic = null
                    clearQueue()
                    try { gatt.close() } catch (_: SecurityException) { }
                    this@BleManager.gatt = null

                    if (wasConnected) {
                        log("Disconnected from SIDECAR V1")
                        mainHandler.post { onDisconnected?.invoke() }
                    }
                    if (!userStopped) scheduleReconnect()
                }
            }
        }

        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS)
                log("MTU = $mtu bytes")
            else
                log("MTU negotiation failed (status $status) — using default")
            // Always proceed to service discovery regardless of MTU outcome
            try {
                gatt.discoverServices()
            } catch (e: SecurityException) {
                log("discoverServices denied: ${e.message}")
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
            log("SIDECAR V1 services found — enabling notifications…")
            mainHandler.post { onConnected?.invoke() }

            // Step 1: Enable CCCD (notifications from watch → phone).
            // This MUST complete (onDescriptorWrite) before we send any writes,
            // otherwise the first write and the descriptor write collide.
            enqueueOp {
                try {
                    gatt.setCharacteristicNotification(char, true)
                    val cccd = char.getDescriptor(CCCD_UUID)
                    if (cccd != null) {
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                            gatt.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                        } else {
                            @Suppress("DEPRECATION")
                            cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                            @Suppress("DEPRECATION")
                            gatt.writeDescriptor(cccd)
                        }
                    } else {
                        log("No CCCD found — proceeding to T| + Q|")
                        opComplete()
                        // Steps 2 + 3 inline when no CCCD
                        enqueueOp { doWrite("T|${System.currentTimeMillis() / 1000}") }
                        enqueueOp { doWrite("Q|") }
                    }
                } catch (e: SecurityException) {
                    log("CCCD write denied: ${e.message}")
                    opComplete()
                }
            }
        }

        override fun onDescriptorWrite(
            gatt: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int
        ) {
            if (status == BluetoothGatt.GATT_SUCCESS)
                log("Notifications enabled")
            else
                log("CCCD write failed (status $status) — continuing anyway")

            opComplete()  // release CCCD op

            // Step 2: time sync — must complete before Q| so the watch clock is
            //         correct by the time it sends back its state dump
            enqueueOp { doWrite("T|${System.currentTimeMillis() / 1000}") }

            // Step 3: request full state dump — queued after T|, executes when
            //         onCharacteristicWrite for T| fires
            enqueueOp { doWrite("Q|") }
        }

        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            if (status != BluetoothGatt.GATT_SUCCESS)
                log("Write failed (status $status)")
            opComplete()
        }

        @Deprecated("Deprecated in API 33")
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            ch: BluetoothGattCharacteristic
        ) {
            @Suppress("DEPRECATION")
            val packet = ch.getStringValue(0) ?: return
            log("RX: $packet")
            mainHandler.post { onIncomingPacket?.invoke(packet) }
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            ch: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            val packet = String(value, Charsets.UTF_8)
            log("RX: $packet")
            mainHandler.post { onIncomingPacket?.invoke(packet) }
        }
    }

    // ── Write ─────────────────────────────────────────────────────────────────

    /**
     * Write a UTF-8 packet to the watch characteristic.
     * Safe to call from any thread. Automatically queued behind any in-flight
     * GATT operation (CCCD setup, time sync, state request).
     */
    fun write(packet: String) {
        if (!connected) {
            log("Write skipped — not connected: $packet")
            return
        }
        enqueueOp { doWrite(packet) }
    }

    /** Execute a characteristic write immediately (must only be called from opQueue). */
    private fun doWrite(packet: String) {
        val char = characteristic
        val g    = gatt
        if (char == null || g == null) {
            log("doWrite skipped — char/gatt null: $packet")
            opComplete()
            return
        }

        val bytes = packet.toByteArray(Charsets.UTF_8)
        val writeType = when {
            char.properties and BluetoothGattCharacteristic.PROPERTY_WRITE != 0 ->
                BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            char.properties and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE != 0 ->
                BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            else ->
                BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        }

        try {
            val initiated = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                g.writeCharacteristic(char, bytes, writeType) == BluetoothStatusCodes.SUCCESS
            } else {
                @Suppress("DEPRECATION")
                char.writeType = writeType
                @Suppress("DEPRECATION")
                char.value = bytes
                @Suppress("DEPRECATION")
                g.writeCharacteristic(char)
            }

            if (initiated) {
                log("TX: $packet")
                // WRITE_NO_RESPONSE has no onCharacteristicWrite callback — complete now
                if (writeType == BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE) opComplete()
            } else {
                log("Write returned false: $packet")
                opComplete()
            }
        } catch (e: SecurityException) {
            log("Write permission denied: ${e.message}")
            opComplete()
        }
    }

    // ── Reconnect ─────────────────────────────────────────────────────────────

    private fun scheduleReconnect() {
        if (userStopped) return
        log("Reconnecting in ${RECONNECT_DELAY_MS / 1000}s…")
        mainHandler.postDelayed({
            if (!userStopped && !connected) beginScan()
        }, RECONNECT_DELAY_MS)
    }

    // ── Logging ───────────────────────────────────────────────────────────────

    private fun log(message: String) {
        Log.d(TAG, message)
        mainHandler.post { onLog?.invoke(message) }
    }
}
