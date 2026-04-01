package com.sidecar.companion

import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.ProgressBar
import android.widget.TextView
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels

class MediaFragment : Fragment() {

    private val viewModel: SidecarViewModel by activityViewModels()

    private lateinit var tvPlayingDot: TextView
    private lateinit var tvSongTitle: TextView
    private lateinit var tvArtist: TextView
    private lateinit var volumeBar: ProgressBar
    private lateinit var tvVolPct: TextView
    private lateinit var btnPrev: Button
    private lateinit var btnPlayPause: Button
    private lateinit var btnNext: Button
    private lateinit var btnVolDown: Button
    private lateinit var btnVolUp: Button
    private lateinit var tvNoSession: TextView

    override fun onCreateView(
        inflater: LayoutInflater, container: ViewGroup?, savedInstanceState: Bundle?
    ): View = inflater.inflate(R.layout.fragment_media, container, false)

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        tvPlayingDot = view.findViewById(R.id.tvPlayingDot)
        tvSongTitle  = view.findViewById(R.id.tvSongTitle)
        tvArtist     = view.findViewById(R.id.tvArtist)
        volumeBar    = view.findViewById(R.id.volumeBar)
        tvVolPct     = view.findViewById(R.id.tvVolPct)
        btnPrev      = view.findViewById(R.id.btnPrev)
        btnPlayPause = view.findViewById(R.id.btnPlayPause)
        btnNext      = view.findViewById(R.id.btnNext)
        btnVolDown   = view.findViewById(R.id.btnVolDown)
        btnVolUp     = view.findViewById(R.id.btnVolUp)
        tvNoSession  = view.findViewById(R.id.tvNoSession)

        viewModel.mediaState.observe(viewLifecycleOwner) { state ->
            val hasSession = state.song.isNotEmpty() || state.artist.isNotEmpty()
            tvNoSession.visibility  = if (hasSession) View.GONE else View.VISIBLE

            tvSongTitle.text = state.song.ifEmpty { "—" }
            tvArtist.text    = state.artist.ifEmpty { "—" }

            if (state.playing) {
                tvPlayingDot.text = "◉ PLAYING"
                tvPlayingDot.setTextColor(0xFFFB6000.toInt())
                btnPlayPause.text = "⏸ PAUSE"
            } else {
                tvPlayingDot.text = "○ PAUSED"
                tvPlayingDot.setTextColor(0xFF4A4A4A.toInt())
                btnPlayPause.text = "▶ PLAY"
            }

            if (state.volume >= 0) {
                volumeBar.progress = state.volume
                tvVolPct.text      = "${state.volume}%"
                volumeBar.visibility = View.VISIBLE
            } else {
                volumeBar.visibility = View.INVISIBLE
                tvVolPct.text        = "—%"
            }
        }

        // Buttons send MC| commands through BLE (ViewModel routes to BleManager)
        btnPrev.setOnClickListener      { viewModel.sendMediaCommand("PREV") }
        btnPlayPause.setOnClickListener { viewModel.sendMediaCommand("PLAY") }
        btnNext.setOnClickListener      { viewModel.sendMediaCommand("NEXT") }
        btnVolDown.setOnClickListener   { viewModel.sendMediaCommand("VOLD") }
        btnVolUp.setOnClickListener     { viewModel.sendMediaCommand("VOLU") }
    }
}
