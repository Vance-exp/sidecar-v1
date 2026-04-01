package com.sidecar.companion

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import com.sidecar.companion.databinding.FragmentConnectionBinding

class ConnectionFragment : Fragment() {

    private var _binding: FragmentConnectionBinding? = null
    private val binding get() = _binding!!

    private val viewModel: SidecarViewModel by activityViewModels()

    private val bleManager get() = (requireActivity().applicationContext as SidecarApp).bleManager

    private val requestPermissionsLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { results ->
            val allGranted = results.values.all { it }
            if (allGranted) {
                autoStart()
            } else {
                val denied = results.filter { !it.value }.keys.joinToString()
                // Log is handled by ViewModel observing bleManager
            }
            refreshNotifBanner()
        }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentConnectionBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        setupButtons()
        requestRequiredPermissions()
        refreshNotifBanner()

        // Observe connection state
        viewModel.isConnected.observe(viewLifecycleOwner) { connected ->
            updateConnectionUi(connected)
        }

        // Observe log lines
        viewModel.logLines.observe(viewLifecycleOwner) { lines ->
            binding.tvLog.text = lines.joinToString("\n")
            binding.scrollLog.post {
                binding.scrollLog.fullScroll(View.FOCUS_DOWN)
            }
        }
    }

    override fun onResume() {
        super.onResume()
        refreshNotifBanner()
        // Ensure BLE service is running — handles the case where SidecarApp.onCreate()
        // couldn't start it due to Android 14 background FGS restrictions
        try { (requireActivity().applicationContext as SidecarApp).startBleService() } catch (_: Exception) {}
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    private fun setupButtons() {
        binding.btnStart.setOnClickListener {
            (requireActivity().applicationContext as SidecarApp).startBleService()
        }
        binding.btnStop.setOnClickListener {
            (requireActivity().applicationContext as SidecarApp).stopBleService()
        }
        binding.btnGrantNotif.setOnClickListener {
            SidecarNotifService.openSettings(requireContext())
        }
    }

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
            ContextCompat.checkSelfPermission(requireContext(), it) == PackageManager.PERMISSION_GRANTED
        }

    private fun requestRequiredPermissions() {
        val missing = requiredPermissions().filter {
            ContextCompat.checkSelfPermission(requireContext(), it) != PackageManager.PERMISSION_GRANTED
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
        binding.btnGrantNotif.visibility =
            if (SidecarNotifService.isEnabled(requireContext())) View.GONE else View.VISIBLE
    }
}
