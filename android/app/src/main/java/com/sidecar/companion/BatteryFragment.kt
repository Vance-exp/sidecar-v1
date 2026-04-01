package com.sidecar.companion

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.TextView
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class BatteryFragment : Fragment() {

    private val viewModel: SidecarViewModel by activityViewModels()

    private lateinit var tvBattPct: TextView
    private lateinit var tvBattVolt: TextView
    private lateinit var tvBattMa: TextView
    private lateinit var tvBattSampleCount: TextView
    private lateinit var tvBattStats: TextView
    private lateinit var battChartView: BatteryChartView
    private lateinit var btnExportCsv: Button
    private lateinit var btnClearSession: Button

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?, savedInstanceState: Bundle?
    ): View = inflater.inflate(R.layout.fragment_battery, container, false)

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        tvBattPct          = view.findViewById(R.id.tvBattPct)
        tvBattVolt         = view.findViewById(R.id.tvBattVolt)
        tvBattMa           = view.findViewById(R.id.tvBattMa)
        tvBattSampleCount  = view.findViewById(R.id.tvBattSampleCount)
        tvBattStats        = view.findViewById(R.id.tvBattStats)
        battChartView      = view.findViewById(R.id.battChartView)
        btnExportCsv       = view.findViewById(R.id.btnExportCsv)
        btnClearSession    = view.findViewById(R.id.btnClearSession)

        // Latest reading
        viewModel.battLatest.observe(viewLifecycleOwner) { sample ->
            if (sample != null) {
                tvBattPct.text  = "${sample.pct}%"
                tvBattVolt.text = String.format("%.3f V", sample.mvolt / 1000f)
                tvBattMa.text   = if (sample.estimatedMa > 0f)
                    String.format("%.1f mA", sample.estimatedMa) else "--.- mA"
            } else {
                tvBattPct.text  = "---%"
                tvBattVolt.text = "-.--- V"
                tvBattMa.text   = "--.- mA"
            }
        }

        // Full sample list → chart + stats
        viewModel.battSamples.observe(viewLifecycleOwner) { samples ->
            tvBattSampleCount.text = "${samples.size} samples"
            battChartView.setSamples(samples)
            updateStats(samples)
        }

        btnExportCsv.setOnClickListener { exportCsv() }
        btnClearSession.setOnClickListener {
            viewModel.battSamples.value?.clear()
            viewModel.battSamples.postValue(mutableListOf())
            viewModel.battLatest.postValue(null)
        }
    }

    private fun updateStats(samples: List<SidecarViewModel.BattSample>) {
        if (samples.size < 2) {
            tvBattStats.text = if (samples.isEmpty()) "No data yet" else "Need ≥2 samples for stats"
            return
        }

        val durationMs  = samples.last().timestampMs - samples.first().timestampMs
        val durationMin = durationMs / 60_000.0

        val minPct = samples.minOf { it.pct }
        val maxPct = samples.maxOf { it.pct }
        val avgPct = samples.map { it.pct }.average()

        val minV   = samples.minOf { it.mvolt } / 1000f
        val maxV   = samples.maxOf { it.mvolt } / 1000f
        val avgV   = samples.map { it.mvolt }.average() / 1000.0

        val drainPct = (samples.first().pct - samples.last().pct).toFloat()
        val drainRatePctPerHour = if (durationMin > 0) drainPct / durationMin * 60 else 0f

        val timeFmt = SimpleDateFormat("HH:mm:ss", Locale.getDefault())
        val start   = timeFmt.format(Date(samples.first().timestampMs))
        val end     = timeFmt.format(Date(samples.last().timestampMs))

        tvBattStats.text = buildString {
            appendLine("Session : $start → $end")
            appendLine("Duration: ${formatDuration(durationMs)}")
            appendLine("Pct     : min=${minPct}%  max=${maxPct}%  avg=${String.format("%.1f", avgPct)}%")
            appendLine("Voltage : min=${String.format("%.3f", minV)}V  max=${String.format("%.3f", maxV)}V  avg=${String.format("%.3f", avgV)}V")
            if (drainPct > 0) {
                appendLine("Drain   : ${String.format("%.1f", drainPct)}% over session")
                append("Rate    : ${String.format("%.2f", drainRatePctPerHour)}%/hr")
            } else {
                append("Drain   : charging or stable")
            }
        }
    }

    private fun formatDuration(ms: Long): String {
        val totalSec = ms / 1000
        val h = totalSec / 3600
        val m = (totalSec % 3600) / 60
        val s = totalSec % 60
        return if (h > 0) String.format("%dh %02dm %02ds", h, m, s)
        else String.format("%dm %02ds", m, s)
    }

    private fun exportCsv() {
        val samples = viewModel.battSamples.value ?: return
        if (samples.isEmpty()) return

        val timeFmt = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss", Locale.getDefault())
        val fileFmt = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.getDefault())
        val fileName = "sidecar_battery_${fileFmt.format(Date())}.csv"
        val dir = requireContext().getExternalFilesDir(null) ?: return
        val file = File(dir, fileName)

        file.bufferedWriter().use { w ->
            w.write("timestamp,pct,mvolt,estimated_ma\n")
            for (s in samples) {
                w.write("${timeFmt.format(Date(s.timestampMs))},${s.pct},${s.mvolt},${String.format("%.1f", s.estimatedMa)}\n")
            }
        }

        tvBattStats.text = "Exported: ${file.absolutePath}\n\n${tvBattStats.text}"
    }
}
