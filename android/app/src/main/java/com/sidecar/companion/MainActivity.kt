package com.sidecar.companion

import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.ViewModelProvider
import com.google.android.material.tabs.TabLayoutMediator
import com.sidecar.companion.databinding.ActivityMainBinding

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var mediaManager: SidecarMediaManager

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.viewPager.adapter = MainPagerAdapter(this)
        TabLayoutMediator(binding.tabLayout, binding.viewPager) { tab, pos ->
            tab.text = when (pos) {
                0    -> "CONN"
                1    -> "FILTER"
                2    -> "WATCH"
                3    -> "ALARMS"
                4    -> "MEDIA"
                else -> "BATTERY"
            }
        }.attach()

        // Start media session tracking — event-driven, no polling
        val viewModel = ViewModelProvider(this)[SidecarViewModel::class.java]
        mediaManager = SidecarMediaManager(this, viewModel)
        mediaManager.start()
    }

    override fun onDestroy() {
        mediaManager.stop()
        super.onDestroy()
    }
}
