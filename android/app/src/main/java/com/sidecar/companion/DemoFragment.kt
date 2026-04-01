package com.sidecar.companion

import android.os.Bundle
import android.view.*
import android.widget.*
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import kotlinx.coroutines.*
import java.text.SimpleDateFormat
import java.util.*

/**
 * DemoFragment — Automated demonstration of every Sidecar V1 feature.
 *
 * Tap START to run a sequenced walkthrough that exercises every BLE packet
 * type and lets you visually confirm each watch screen responds correctly.
 * Each step is logged with a timestamp and pass/fail indicator.
 */
class DemoFragment : Fragment() {

    private val vm: SidecarViewModel by activityViewModels()
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    private lateinit var btnStart: Button
    private lateinit var btnStop: Button
    private lateinit var tvStatus: TextView
    private lateinit var logView: TextView
    private lateinit var scrollView: ScrollView
    private lateinit var progressBar: ProgressBar

    private var demoJob: Job? = null
    private val logLines = mutableListOf<String>()

    // ── Demo steps — (title, delay_after_ms) ──────────────────────────────────
    data class DemoStep(val title: String, val delayMs: Long, val action: suspend () -> Unit)

    override fun onCreateView(inflater: LayoutInflater, container: ViewGroup?, savedInstanceState: Bundle?): View {
        val root = LinearLayout(requireContext()).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(24, 24, 24, 24)
        }

        // Header
        root.addView(TextView(requireContext()).apply {
            text = "DEMO MODE"
            textSize = 18f
            setTypeface(null, android.graphics.Typeface.BOLD)
            setTextColor(0xFFFF8C00.toInt())
            gravity = android.view.Gravity.CENTER
            setPadding(0, 0, 0, 8)
        })

        // Status
        tvStatus = TextView(requireContext()).apply {
            text = "Ready — connect watch first"
            textSize = 13f
            setTextColor(0xFF888888.toInt())
            gravity = android.view.Gravity.CENTER
            setPadding(0, 0, 0, 12)
        }
        root.addView(tvStatus)

        // Progress bar
        progressBar = ProgressBar(requireContext(), null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 100
            progress = 0
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 12).apply {
                bottomMargin = 12
            }
        }
        root.addView(progressBar)

        // Buttons row
        val btnRow = LinearLayout(requireContext()).apply {
            orientation = LinearLayout.HORIZONTAL
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                bottomMargin = 16
            }
        }
        btnStart = Button(requireContext()).apply {
            text = "▶  START DEMO"
            setBackgroundColor(0xFFFF8C00.toInt())
            setTextColor(0xFF000000.toInt())
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply { rightMargin = 8 }
            setOnClickListener { startDemo() }
        }
        btnStop = Button(requireContext()).apply {
            text = "■  STOP"
            setBackgroundColor(0xFF333333.toInt())
            setTextColor(0xFFFFFFFF.toInt())
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.5f)
            isEnabled = false
            setOnClickListener { stopDemo() }
        }
        btnRow.addView(btnStart)
        btnRow.addView(btnStop)
        root.addView(btnRow)

        // Log scroll view
        scrollView = ScrollView(requireContext()).apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
        }
        logView = TextView(requireContext()).apply {
            text = "Tap START to begin demo.\n\nEnsure the watch is connected via BLE before starting.\n\nThe demo will cycle through every feature automatically."
            textSize = 11f
            setTextColor(0xFFCCCCCC.toInt())
            setTypeface(android.graphics.Typeface.MONOSPACE)
            setPadding(8, 8, 8, 8)
            setBackgroundColor(0xFF111111.toInt())
        }
        scrollView.addView(logView)
        root.addView(scrollView)

        return root
    }

    private fun ts(): String = SimpleDateFormat("HH:mm:ss", Locale.getDefault()).format(Date())

    private fun log(msg: String, pass: Boolean? = null) {
        val prefix = when (pass) {
            true  -> "[ OK ] "
            false -> "[FAIL] "
            null  -> "[ -- ] "
        }
        val line = "${ts()} $prefix$msg"
        logLines.add(line)
        if (logLines.size > 200) logLines.removeAt(0)
        logView.text = logLines.joinToString("\n")
        scrollView.post { scrollView.fullScroll(ScrollView.FOCUS_DOWN) }
    }

    private fun startDemo() {
        if (demoJob?.isActive == true) return
        if (!vm.isConnected.value!!) {
            log("Not connected — pair the watch first", false)
            return
        }

        logLines.clear()
        log("=== SIDECAR V1 DEMO START ===")

        btnStart.isEnabled = false
        btnStop.isEnabled = true
        progressBar.progress = 0

        val steps = buildDemoSteps()
        val total = steps.size

        demoJob = scope.launch {
            steps.forEachIndexed { i, step ->
                if (!isActive) return@launch
                val pct = ((i.toFloat() / total) * 100).toInt()
                progressBar.progress = pct
                tvStatus.text = "Step ${i + 1}/$total — ${step.title}"
                log("► ${step.title}")
                try {
                    step.action()
                    delay(step.delayMs)
                    log("  ${step.title} done", true)
                } catch (e: Exception) {
                    log("  ERROR: ${e.message}", false)
                }
            }
            progressBar.progress = 100
            tvStatus.text = "Demo complete"
            log("")
            log("=== DEMO COMPLETE — ${total} steps ===", true)
            log("Check the watch for visual confirmation of each feature.")
            btnStart.isEnabled = true
            btnStop.isEnabled = false
        }
    }

    private fun stopDemo() {
        demoJob?.cancel()
        tvStatus.text = "Demo stopped"
        log("Demo stopped by user", false)
        btnStart.isEnabled = true
        btnStop.isEnabled = false
    }

    // ── Build the ordered list of demo steps ───────────────────────────────────
    private fun buildDemoSteps(): List<DemoStep> {
        val bleManager = (requireActivity().application as SidecarApp).bleManager
        return listOf(

            // 1. Time sync
            DemoStep("Time sync", 2000L) {
                val epoch = System.currentTimeMillis() / 1000L
                bleManager.write("T|$epoch")
                log("  Sent T|$epoch  →  watch clock should update")
            },

            // 2. Weather update
            DemoStep("Weather fetch + push", 4000L) {
                log("  Fetching live weather from wttr.in …")
                vm.fetchAndSendWeather()
                log("  Switch watch to WEATHER screen to see result")
            },

            // 3. CALL notification
            DemoStep("Notification — CALL (cyan)", 3500L) {
                bleManager.write("C|Mum")
                log("  Sent C|Mum  →  watch shows CYAN incoming call overlay")
            },

            // 4. MSG notification (WhatsApp)
            DemoStep("Notification — MSG/WhatsApp (green)", 3500L) {
                bleManager.write("N|WhatsApp|Alice|Hey are you coming tonight?")
                log("  Sent WhatsApp notif  →  watch shows GREEN overlay")
            },

            // 5. MSG notification (Telegram)
            DemoStep("Notification — MSG/Telegram (green)", 3500L) {
                bleManager.write("N|Telegram|Bob|Check this out 🔥")
                log("  Sent Telegram notif  →  watch shows GREEN overlay")
            },

            // 6. Generic APP notification
            DemoStep("Notification — APP/Gmail (orange)", 3500L) {
                bleManager.write("N|Gmail|Order shipped|Your package is on its way!")
                log("  Sent Gmail notif  →  watch shows ORANGE overlay")
            },

            // 7. Media state — playing
            DemoStep("Media state — now playing", 3000L) {
                bleManager.write("S|Daft Punk|Get Lucky|1|72")
                log("  Sent media state playing  →  MEDIA screen: Daft Punk / Get Lucky")
            },

            // 8. Media — pause
            DemoStep("Media command — pause", 2000L) {
                bleManager.write("S|Daft Punk|Get Lucky|0|72")
                log("  Sent paused state  →  MEDIA screen shows paused icon")
            },

            // 9. Media — volume up
            DemoStep("Media command — volume up ×3", 2000L) {
                repeat(3) {
                    vm.sendMediaCommand("VOLU")
                    delay(400)
                }
                log("  Sent 3× VOLU  →  phone volume increases")
            },

            // 10. DND enable
            DemoStep("DND — enable", 2500L) {
                vm.sendDnd(true)
                log("  DND ON  →  watch header shows orange dot, buzz suppressed")
            },

            // 11. Send notif while DND (should arrive silently)
            DemoStep("Notification while DND (silent)", 3000L) {
                bleManager.write("N|Twitter|@elon|Just posted something controversial")
                log("  Notif sent while DND  →  watch shows but NO buzz/beep")
            },

            // 12. DND disable
            DemoStep("DND — disable", 2000L) {
                vm.sendDnd(false)
                log("  DND OFF  →  orange dot gone, normal alerts resume")
            },

            // 13. Alarm — set alarm 0 to 2 minutes from now on all days
            DemoStep("Alarm — set alarm #1 to 2min from now", 3000L) {
                val cal = Calendar.getInstance()
                cal.add(Calendar.MINUTE, 2)
                val a = SidecarViewModel.AlarmState(
                    hour = cal.get(Calendar.HOUR_OF_DAY),
                    minute = cal.get(Calendar.MINUTE),
                    enabled = true,
                    daysOfWeek = 0x7F,
                    useDate = false
                )
                vm.sendAlarm(0, a)
                log("  Alarm #1 set for ${String.format("%02d:%02d", a.hour, a.minute)}  →  ALARM screen shows it enabled")
            },

            // 14. Power mode → EFFICIENT
            DemoStep("Power mode → EFFICIENT", 3000L) {
                bleManager.sendConfig("POWER", 1)
                log("  Sent POWER=1  →  watch drops to 80MHz, slower BLE adv")
            },

            // 15. Power mode → NORMAL
            DemoStep("Power mode → NORMAL", 2500L) {
                bleManager.sendConfig("POWER", 0)
                log("  Sent POWER=0  →  watch back to 160MHz full speed")
            },

            // 16. Brightness change
            DemoStep("Brightness — level 1 (dim)", 2000L) {
                bleManager.sendConfig("BRIGHT", 1)
                log("  Brightness = 1  →  watch dims noticeably")
            },
            DemoStep("Brightness — level 3 (default)", 1500L) {
                bleManager.sendConfig("BRIGHT", 3)
                log("  Brightness = 3  →  watch back to default")
            },

            // 17. AOD face cycle
            DemoStep("AOD face — clock+steps", 2500L) {
                bleManager.sendConfig("AOD_FACE", 1)
                log("  AOD face = 1  →  AOD shows clock + step count")
            },
            DemoStep("AOD face — clock+alarm", 2500L) {
                bleManager.sendConfig("AOD_FACE", 2)
                log("  AOD face = 2  →  AOD shows clock + next alarm")
            },
            DemoStep("AOD face — clock only", 1500L) {
                bleManager.sendConfig("AOD_FACE", 0)
                log("  AOD face = 0  →  AOD shows clock only (default)")
            },

            // 18. Power config push
            DemoStep("Power config sync to watch", 2000L) {
                vm.sendPwrCfg(0, 3, 60)  // NORMAL: brightness 3, timeout 60s
                vm.sendPwrCfg(1, 2, 30)  // EFFICIENT: brightness 2, timeout 30s
                vm.sendPwrCfg(2, 1, 10)  // DEEPSLEEP: brightness 1, timeout 10s
                log("  Pushed power config for all 3 modes")
            },

            // 19. Find My Phone (inform user to trigger from watch)
            DemoStep("Find My Phone — trigger from watch", 5000L) {
                log("  ACTION REQUIRED: on the watch go to NOTIFS screen")
                log("  then hold BtnA for 1s  →  phone will ring + vibrate")
                log("  (This is watch-initiated; demo waits 5s for you to try)")
            },

            // 20. Battery telemetry — just observe
            DemoStep("Battery telemetry (observe)", 3000L) {
                val b = vm.battLatest.value
                if (b != null) {
                    log("  Battery: ${b.pct}% · ${b.mvolt}mV · ~${String.format("%.1f", b.estimatedMa)}mA")
                    log("  Real current via AXP2101 readback", true)
                } else {
                    log("  No battery sample yet — watch sends B| every 60s")
                }
            },

            // 21. Restore clean state
            DemoStep("Restore defaults", 2000L) {
                bleManager.sendConfig("BRIGHT", 3)
                vm.sendDnd(false)
                log("  Restored brightness=3, DND=off")
            }
        )
    }

    override fun onDestroyView() {
        super.onDestroyView()
        scope.cancel()
    }
}
