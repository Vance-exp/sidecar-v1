package com.sidecar.companion

import android.app.TimePickerDialog
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.CompoundButton
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.ToggleButton
import androidx.appcompat.widget.SwitchCompat
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels

class AlarmFragment : Fragment() {

    private val viewModel: SidecarViewModel by activityViewModels()

    // Local copies of alarm state (hours, minutes, enabled, dow)
    private val alarmHour   = intArrayOf(7,  8,  9)
    private val alarmMinute = intArrayOf(0,  0,  0)
    private val alarmEn     = BooleanArray(3) { false }
    private val alarmDow    = IntArray(3) { 0x7F }

    // UI references — indexed per alarm
    private lateinit var tvTimes:   Array<TextView>
    private lateinit var swEnabled: Array<SwitchCompat>
    private lateinit var dayRows:   Array<LinearLayout>

    // Guard flag to prevent listener feedback loops while populating from watch state
    private var ignoreChanges = false

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?, savedInstanceState: Bundle?
    ): View = inflater.inflate(R.layout.fragment_alarms, container, false)

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        tvTimes = arrayOf(
            view.findViewById(R.id.tvAlarmTime0),
            view.findViewById(R.id.tvAlarmTime1),
            view.findViewById(R.id.tvAlarmTime2)
        )
        swEnabled = arrayOf(
            view.findViewById(R.id.swAlarmEn0),
            view.findViewById(R.id.swAlarmEn1),
            view.findViewById(R.id.swAlarmEn2)
        )
        dayRows = arrayOf(
            view.findViewById(R.id.llAlarmDays0),
            view.findViewById(R.id.llAlarmDays1),
            view.findViewById(R.id.llAlarmDays2)
        )

        setupListeners()

        // Observe watch state sync
        viewModel.alarms.observe(viewLifecycleOwner) { arr ->
            ignoreChanges = true
            for (i in 0..2) {
                alarmHour[i]   = arr[i].hour
                alarmMinute[i] = arr[i].minute
                alarmEn[i]     = arr[i].enabled
                alarmDow[i]    = arr[i].daysOfWeek
                updateTimeText(i)
                swEnabled[i].isChecked = alarmEn[i]
                updateDayToggles(i)
            }
            ignoreChanges = false
        }
    }

    private fun setupListeners() {
        for (i in 0..2) {
            // Time tap → TimePickerDialog
            tvTimes[i].setOnClickListener {
                TimePickerDialog(requireContext(), { _, h, m ->
                    alarmHour[i]   = h
                    alarmMinute[i] = m
                    updateTimeText(i)
                    sendAlarm(i)
                }, alarmHour[i], alarmMinute[i], true).show()
            }

            // Enable switch
            swEnabled[i].setOnCheckedChangeListener { _, checked ->
                if (ignoreChanges) return@setOnCheckedChangeListener
                alarmEn[i] = checked
                sendAlarm(i)
            }

            // Day toggles (7 children = Sun..Sat, bit 0..6)
            val row = dayRows[i]
            for (bit in 0..6) {
                val tb = row.getChildAt(bit) as? ToggleButton ?: continue
                tb.setOnCheckedChangeListener { _, _ ->
                    if (ignoreChanges) return@setOnCheckedChangeListener
                    var dow = 0
                    for (b in 0..6) {
                        val t = row.getChildAt(b) as? ToggleButton ?: continue
                        if (t.isChecked) dow = dow or (1 shl b)
                    }
                    alarmDow[i] = dow
                    sendAlarm(i)
                }
            }
        }
    }

    private fun updateTimeText(i: Int) {
        tvTimes[i].text = String.format("%02d:%02d", alarmHour[i], alarmMinute[i])
    }

    private fun updateDayToggles(i: Int) {
        val row = dayRows[i]
        for (bit in 0..6) {
            val tb = row.getChildAt(bit) as? ToggleButton ?: continue
            tb.isChecked = (alarmDow[i] shr bit) and 1 == 1
        }
    }

    private fun sendAlarm(i: Int) {
        val alarm = SidecarViewModel.AlarmState(
            hour       = alarmHour[i],
            minute     = alarmMinute[i],
            enabled    = alarmEn[i],
            daysOfWeek = alarmDow[i],
            useDate    = false,
            dateDay    = 1,
            dateMonth  = 1
        )
        viewModel.sendAlarm(i, alarm)
    }
}
