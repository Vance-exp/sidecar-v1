package com.sidecar.companion

import android.app.Application
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
    }

    override fun onTerminate() {
        bleManager.stop()
        super.onTerminate()
    }

    companion object {
        private const val TAG = "SidecarApp"
    }
}
