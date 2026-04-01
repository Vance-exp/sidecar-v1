package com.sidecar.companion

import androidx.fragment.app.Fragment
import androidx.fragment.app.FragmentActivity
import androidx.viewpager2.adapter.FragmentStateAdapter

class MainPagerAdapter(fa: FragmentActivity) : FragmentStateAdapter(fa) {
    override fun getItemCount() = 6
    override fun createFragment(position: Int): Fragment = when (position) {
        0 -> ConnectionFragment()
        1 -> FilterFragment()
        2 -> WatchSettingsFragment()
        3 -> AlarmFragment()
        4 -> MediaFragment()
        else -> BatteryFragment()
    }
}
