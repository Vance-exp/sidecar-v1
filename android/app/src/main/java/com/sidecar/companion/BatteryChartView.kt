package com.sidecar.companion

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.util.AttributeSet
import android.view.View
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class BatteryChartView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    private var samples: List<SidecarViewModel.BattSample> = emptyList()

    private val bgPaint   = Paint().apply { color = Color.parseColor("#0F0F0F") }
    private val gridPaint = Paint().apply {
        color = Color.parseColor("#1E1E1E")
        strokeWidth = 1f
        style = Paint.Style.STROKE
    }
    private val axisLabelPaint = Paint().apply {
        color = Color.parseColor("#4A4A4A")
        textSize = 24f
        isAntiAlias = true
    }
    private val battPaint = Paint().apply {
        color = Color.parseColor("#FB6000")
        strokeWidth = 3f
        style = Paint.Style.STROKE
        isAntiAlias = true
        strokeJoin = Paint.Join.ROUND
        strokeCap  = Paint.Cap.ROUND
    }
    private val voltPaint = Paint().apply {
        color = Color.parseColor("#FFD700")
        strokeWidth = 2f
        style = Paint.Style.STROKE
        isAntiAlias = true
        strokeJoin = Paint.Join.ROUND
    }
    private val legendPaint = Paint().apply {
        textSize = 24f
        isAntiAlias = true
    }
    private val timeFmt = SimpleDateFormat("HH:mm", Locale.getDefault())

    fun setSamples(list: List<SidecarViewModel.BattSample>) {
        samples = list
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val w = width.toFloat()
        val h = height.toFloat()

        // Background
        canvas.drawRect(0f, 0f, w, h, bgPaint)

        val padL = 52f; val padR = 52f; val padT = 20f; val padB = 36f
        val chartW = w - padL - padR
        val chartH = h - padT - padB

        // Grid lines at 25/50/75/100%
        for (pct in listOf(0, 25, 50, 75, 100)) {
            val y = padT + chartH * (1f - pct / 100f)
            canvas.drawLine(padL, y, padL + chartW, y, gridPaint)
            canvas.drawText("$pct%", 2f, y + 8f, axisLabelPaint)
        }

        if (samples.size < 2) {
            val msg = if (samples.isEmpty()) "Waiting for data…" else "Need ≥2 samples"
            val mp = Paint().apply { color = Color.parseColor("#4A4A4A"); textSize = 28f; isAntiAlias = true }
            canvas.drawText(msg, padL + 8f, h / 2f, mp)
            return
        }

        val tMin = samples.first().timestampMs
        val tMax = samples.last().timestampMs
        val tRange = (tMax - tMin).coerceAtLeast(1L)

        // Voltage range 3200..4200 mV → mapped to 0..100%
        val vMin = 3200f; val vMax = 4200f

        fun xOf(ts: Long) = padL + chartW * (ts - tMin).toFloat() / tRange
        fun yOfPct(pct: Int) = padT + chartH * (1f - pct / 100f)
        fun yOfVolt(mv: Int): Float {
            val norm = ((mv - vMin) / (vMax - vMin)).coerceIn(0f, 1f)
            return padT + chartH * (1f - norm)
        }

        // Battery % line
        val battPath = Path()
        samples.forEachIndexed { idx, s ->
            val x = xOf(s.timestampMs); val y = yOfPct(s.pct)
            if (idx == 0) battPath.moveTo(x, y) else battPath.lineTo(x, y)
        }
        canvas.drawPath(battPath, battPaint)

        // Voltage line
        val voltPath = Path()
        samples.forEachIndexed { idx, s ->
            val x = xOf(s.timestampMs); val y = yOfVolt(s.mvolt)
            if (idx == 0) voltPath.moveTo(x, y) else voltPath.lineTo(x, y)
        }
        canvas.drawPath(voltPath, voltPaint)

        // Right axis — voltage labels
        for (mv in listOf(3200, 3600, 4000, 4200)) {
            val y = yOfVolt(mv)
            axisLabelPaint.textAlign = Paint.Align.RIGHT
            canvas.drawText("${mv / 1000f}V", w - 2f, y + 8f, axisLabelPaint)
        }
        axisLabelPaint.textAlign = Paint.Align.LEFT

        // X-axis time labels (first and last)
        val xFirst = xOf(tMin)
        val xLast  = xOf(tMax)
        canvas.drawText(timeFmt.format(Date(tMin)), xFirst, h - 4f, axisLabelPaint)
        axisLabelPaint.textAlign = Paint.Align.RIGHT
        canvas.drawText(timeFmt.format(Date(tMax)), xLast, h - 4f, axisLabelPaint)
        axisLabelPaint.textAlign = Paint.Align.LEFT

        // Legend
        legendPaint.color = Color.parseColor("#FB6000")
        canvas.drawText("■ BAT%", padL, padT - 4f, legendPaint)
        legendPaint.color = Color.parseColor("#FFD700")
        canvas.drawText("■ VOLT", padL + 90f, padT - 4f, legendPaint)
    }
}
