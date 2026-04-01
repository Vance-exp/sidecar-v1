package com.sidecar.companion

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.SeekBar
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import com.sidecar.companion.databinding.FragmentWatchSettingsBinding

class WatchSettingsFragment : Fragment() {

    private var _binding: FragmentWatchSettingsBinding? = null
    private val binding get() = _binding!!

    private val viewModel: SidecarViewModel by activityViewModels()

    // Timeout values for PWRCFG seekbar (index → seconds)
    private val timeoutValues = intArrayOf(0, 5, 10, 15, 20, 30, 45, 60, 90, 120)

    // Guard flag — prevents listener feedback loops when we update controls programmatically
    private var ignoreListeners = false

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentWatchSettingsBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        // 1. Load saved values and populate controls BEFORE attaching listeners
        loadSavedValues()

        // 2. Observe connection state
        viewModel.isConnected.observe(viewLifecycleOwner) { connected ->
            val alpha = if (connected) 1.0f else 0.4f
            binding.rgPowerMode.alpha = alpha
            binding.sbBrightness.alpha = alpha
            binding.tvBrightnessVal.alpha = alpha
            binding.swAod.alpha = alpha
            binding.swAutoRotate.alpha = alpha
            binding.rgNotifTimeout.alpha = alpha
            binding.swSound.alpha = alpha

            binding.tvSettingsStatus.text = if (connected) {
                "Watch connected"
            } else {
                "Watch not connected — changes apply when connected"
            }
        }

        // 3. Observe watch state synced from watch
        viewModel.watchState.observe(viewLifecycleOwner) { state ->
            applyWatchState(state)
        }

        // 4. Attach listeners
        setupListeners()
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    private fun loadSavedValues() {
        val ctx = requireContext()

        // Power mode (0=NORMAL 1=EFFICIENT 2=DEEPSLEEP)
        when (WatchPrefs.load(ctx, "POWER", 0)) {
            0 -> binding.rgPowerMode.check(R.id.rbPowerNormal)
            1 -> binding.rgPowerMode.check(R.id.rbPowerEfficient)
            2 -> binding.rgPowerMode.check(R.id.rbPowerDeep)
        }

        // Brightness
        val bright = WatchPrefs.load(ctx, "BRIGHT", 3)
        binding.sbBrightness.progress = bright
        binding.tvBrightnessVal.text = bright.toString()

        // AOD
        binding.swAod.isChecked = WatchPrefs.load(ctx, "AOD", 0) == 1

        // Auto-rotate
        binding.swAutoRotate.isChecked = WatchPrefs.load(ctx, "AUTOROT", 1) == 1

        // Notif timeout (0=5s 1=10s 2=20s 3=30s 4=OFF)
        when (WatchPrefs.load(ctx, "NOTIFTMO", 1)) {
            0 -> binding.rgNotifTimeout.check(R.id.rbNotif5s)
            1 -> binding.rgNotifTimeout.check(R.id.rbNotif10s)
            2 -> binding.rgNotifTimeout.check(R.id.rbNotif20s)
            3 -> binding.rgNotifTimeout.check(R.id.rbNotif30s)
            4 -> binding.rgNotifTimeout.check(R.id.rbNotifOff)
        }

        // Sound
        binding.swSound.isChecked = WatchPrefs.load(ctx, "SOUND", 1) == 1

        // Power mode config defaults
        val defaultBrights  = intArrayOf(4, 2, 1)
        val defaultTimeouts = intArrayOf(60, 30, 10)
        val brightBars  = listOf(binding.sbCfgBright0,  binding.sbCfgBright1,  binding.sbCfgBright2)
        val brightVals  = listOf(binding.tvCfgBright0,  binding.tvCfgBright1,  binding.tvCfgBright2)
        val toutBars    = listOf(binding.sbCfgTimeout0, binding.sbCfgTimeout1, binding.sbCfgTimeout2)
        val toutVals    = listOf(binding.tvCfgTimeout0, binding.tvCfgTimeout1, binding.tvCfgTimeout2)

        for (m in 0..2) {
            brightBars[m].progress = defaultBrights[m]
            brightVals[m].text     = defaultBrights[m].toString()
            val tidx = timeoutValues.indexOfFirst { it >= defaultTimeouts[m] }.coerceAtLeast(0)
            toutBars[m].progress   = tidx
            toutVals[m].text       = timeoutLabel(timeoutValues[tidx])
        }
    }

    private fun applyWatchState(state: SidecarViewModel.WatchState) {
        ignoreListeners = true

        // Power mode
        when (state.powerMode) {
            0 -> binding.rgPowerMode.check(R.id.rbPowerNormal)
            1 -> binding.rgPowerMode.check(R.id.rbPowerEfficient)
            2 -> binding.rgPowerMode.check(R.id.rbPowerDeep)
        }

        // Brightness
        binding.sbBrightness.progress = state.brightness
        binding.tvBrightnessVal.text = state.brightness.toString()

        // AOD
        binding.swAod.isChecked = state.aod

        // Auto-rotate
        binding.swAutoRotate.isChecked = state.autoRotate

        // Sound
        binding.swSound.isChecked = state.sound

        // Notif timeout
        when (state.notifTmo) {
            0 -> binding.rgNotifTimeout.check(R.id.rbNotif5s)
            1 -> binding.rgNotifTimeout.check(R.id.rbNotif10s)
            2 -> binding.rgNotifTimeout.check(R.id.rbNotif20s)
            3 -> binding.rgNotifTimeout.check(R.id.rbNotif30s)
            4 -> binding.rgNotifTimeout.check(R.id.rbNotifOff)
        }

        // Per-mode power config
        val brightBars  = listOf(binding.sbCfgBright0,  binding.sbCfgBright1,  binding.sbCfgBright2)
        val brightVals  = listOf(binding.tvCfgBright0,  binding.tvCfgBright1,  binding.tvCfgBright2)
        val toutBars    = listOf(binding.sbCfgTimeout0, binding.sbCfgTimeout1, binding.sbCfgTimeout2)
        val toutVals    = listOf(binding.tvCfgTimeout0, binding.tvCfgTimeout1, binding.tvCfgTimeout2)

        for (m in 0..2) {
            brightBars[m].progress = state.pwrCfg[m][0]
            brightVals[m].text     = state.pwrCfg[m][0].toString()
            val timeout = state.pwrCfg[m][1]
            val tidx = timeoutValues.indexOfFirst { it >= timeout }.coerceAtLeast(0)
            toutBars[m].progress   = tidx
            toutVals[m].text       = timeoutLabel(timeoutValues[tidx])
        }

        ignoreListeners = false
    }

    private fun setupListeners() {
        // Power mode
        binding.rgPowerMode.setOnCheckedChangeListener { _, checkedId ->
            if (ignoreListeners) return@setOnCheckedChangeListener
            val value = when (checkedId) {
                R.id.rbPowerNormal    -> 0
                R.id.rbPowerEfficient -> 1
                R.id.rbPowerDeep      -> 2
                else                  -> 0
            }
            sendConfig("POWER", value)
        }

        // Brightness — send on stop tracking
        binding.sbBrightness.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                if (fromUser) binding.tvBrightnessVal.text = progress.toString()
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {
                if (ignoreListeners) return
                val progress = seekBar?.progress ?: return
                binding.tvBrightnessVal.text = progress.toString()
                sendConfig("BRIGHT", progress)
            }
        })

        // AOD
        binding.swAod.setOnCheckedChangeListener { _, isChecked ->
            if (ignoreListeners) return@setOnCheckedChangeListener
            sendConfig("AOD", if (isChecked) 1 else 0)
        }

        // Auto-rotate
        binding.swAutoRotate.setOnCheckedChangeListener { _, isChecked ->
            if (ignoreListeners) return@setOnCheckedChangeListener
            sendConfig("AUTOROT", if (isChecked) 1 else 0)
        }

        // Notif timeout
        binding.rgNotifTimeout.setOnCheckedChangeListener { _, checkedId ->
            if (ignoreListeners) return@setOnCheckedChangeListener
            val value = when (checkedId) {
                R.id.rbNotif5s  -> 0
                R.id.rbNotif10s -> 1
                R.id.rbNotif20s -> 2
                R.id.rbNotif30s -> 3
                R.id.rbNotifOff -> 4
                else            -> 1
            }
            sendConfig("NOTIFTMO", value)
        }

        // Sound
        binding.swSound.setOnCheckedChangeListener { _, isChecked ->
            if (ignoreListeners) return@setOnCheckedChangeListener
            sendConfig("SOUND", if (isChecked) 1 else 0)
        }

        // Per-mode power config seekbars
        val brightBars = listOf(binding.sbCfgBright0,  binding.sbCfgBright1,  binding.sbCfgBright2)
        val brightVals = listOf(binding.tvCfgBright0,  binding.tvCfgBright1,  binding.tvCfgBright2)
        val toutBars   = listOf(binding.sbCfgTimeout0, binding.sbCfgTimeout1, binding.sbCfgTimeout2)
        val toutVals   = listOf(binding.tvCfgTimeout0, binding.tvCfgTimeout1, binding.tvCfgTimeout2)

        for (m in 0..2) {
            val mode = m
            brightBars[m].setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(sb: SeekBar?, progress: Int, fromUser: Boolean) {
                    if (fromUser) brightVals[mode].text = progress.toString()
                }
                override fun onStartTrackingTouch(sb: SeekBar?) {}
                override fun onStopTrackingTouch(sb: SeekBar?) {
                    if (ignoreListeners) return
                    val bright  = sb?.progress ?: return
                    val timeout = timeoutValues[toutBars[mode].progress]
                    brightVals[mode].text = bright.toString()
                    viewModel.sendPwrCfg(mode, bright, timeout)
                }
            })
            toutBars[m].setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(sb: SeekBar?, progress: Int, fromUser: Boolean) {
                    if (fromUser) toutVals[mode].text = timeoutLabel(timeoutValues[progress])
                }
                override fun onStartTrackingTouch(sb: SeekBar?) {}
                override fun onStopTrackingTouch(sb: SeekBar?) {
                    if (ignoreListeners) return
                    val tidx    = sb?.progress ?: return
                    val timeout = timeoutValues[tidx]
                    val bright  = brightBars[mode].progress
                    toutVals[mode].text = timeoutLabel(timeout)
                    viewModel.sendPwrCfg(mode, bright, timeout)
                }
            })
        }
    }

    private fun timeoutLabel(seconds: Int): String = if (seconds == 0) "never" else "${seconds}s"

    private fun sendConfig(key: String, value: Int) {
        WatchPrefs.save(requireContext(), key, value)
        viewModel.sendConfig(key, value)
    }
}
