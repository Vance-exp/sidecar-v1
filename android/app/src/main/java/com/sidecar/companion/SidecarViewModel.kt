package com.sidecar.companion

import android.app.Application
import android.content.Context
import android.media.AudioAttributes
import android.media.AudioManager
import android.media.RingtoneManager
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.os.Build
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.MutableLiveData
import kotlinx.coroutines.*
import java.net.URL

class SidecarViewModel(app: Application) : AndroidViewModel(app) {

    val isConnected = MutableLiveData(false)
    private val _logLines = mutableListOf<String>()
    val logLines = MutableLiveData<List<String>>(emptyList())

    private val viewModelScope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    // ── Watch state (synced from ST| packets) ───────────────────────────────
    data class WatchState(
        var powerMode: Int = 0,
        var brightness: Int = 3,
        var aod: Boolean = false,
        var autoRotate: Boolean = true,
        var sound: Boolean = true,
        var notifTmo: Int = 1,
        var dnd: Boolean = false,
        val pwrCfg: Array<IntArray> = arrayOf(
            intArrayOf(4, 60),
            intArrayOf(2, 30),
            intArrayOf(1, 10)
        )
    ) {
        override fun equals(other: Any?): Boolean = false  // always trigger observers
        override fun hashCode(): Int = System.identityHashCode(this)
    }

    // ── Weather state ─────────────────────────────────────────────────────────
    data class WeatherState(
        val tempC: Int = 0,
        val hiC: Int = 0,
        val loC: Int = 0,
        val condition: String = "---",
        val fetchedAt: Long = 0L
    )

    data class AlarmState(
        var hour: Int = 0,
        var minute: Int = 0,
        var enabled: Boolean = false,
        var daysOfWeek: Int = 0x7F,
        var useDate: Boolean = false,
        var dateDay: Int = 1,
        var dateMonth: Int = 1
    )

    val watchState   = MutableLiveData(WatchState())
    val alarms       = MutableLiveData(Array(3) { AlarmState() })
    val weatherState = MutableLiveData(WeatherState())

    // ── Battery data ─────────────────────────────────────────────────────────
    data class BattSample(val timestampMs: Long, val pct: Int, val mvolt: Int, val estimatedMa: Float = 0f)

    val battSamples = MutableLiveData<MutableList<BattSample>>(mutableListOf())
    val battLatest  = MutableLiveData<BattSample?>(null)

    // ── BleManager reference ─────────────────────────────────────────────────
    private val bleManager = (app as SidecarApp).bleManager

    init {
        isConnected.value = bleManager.isConnected()
        bleManager.onConnected = {
            isConnected.postValue(true)
            appendLog("Connected to SIDECAR V1")
            // Auto-send weather on connect so the watch screen is up-to-date
            fetchAndSendWeather()
        }
        bleManager.onDisconnected = {
            isConnected.postValue(false)
            appendLog("Disconnected — searching…")
        }
        bleManager.onLog = { msg -> appendLog(msg) }
        bleManager.onIncomingPacket = { packet -> handleIncoming(packet) }
    }

    // ── Media command dispatch (MC| from watch OR app UI → SidecarMediaManager) ─
    var onMediaCommand: ((String) -> Unit)? = null

    /** Called by [MediaFragment] buttons — dispatches transport command locally. */
    fun sendMediaCommand(cmd: String) {
        onMediaCommand?.invoke(cmd)
    }

    // ── Incoming packet handler ───────────────────────────────────────────────
    private fun handleIncoming(packet: String) {
        val parts = packet.split("|")
        when (parts[0]) {
            "MC" -> {
                if (parts.size >= 2) onMediaCommand?.invoke(parts[1])
            }
            "FP" -> {
                // Find My Phone — ring and vibrate
                triggerFindMyPhone()
            }
            "B" -> {
                if (parts.size >= 3) {
                    val estimatedMa = if (parts.size >= 4)
                        (parts[3].toIntOrNull() ?: 0) / 10f else 0f
                    val sample = BattSample(
                        System.currentTimeMillis(),
                        parts[1].toIntOrNull() ?: 0,
                        parts[2].toIntOrNull() ?: 0,
                        estimatedMa
                    )
                    val list = battSamples.value ?: mutableListOf()
                    list.add(sample)
                    // Cap at 10 000 samples (~7 days at 1/min, ~3h at 1/s) to prevent OOM
                    if (list.size > 10_000) list.removeAt(0)
                    battSamples.postValue(list)
                    battLatest.postValue(sample)
                }
            }
            "ST" -> {
                if (parts.size < 3) return
                val state = watchState.value ?: WatchState()
                when (parts[1]) {
                    "POWER"    -> state.powerMode  = parts[2].toIntOrNull() ?: 0
                    "BRIGHT"   -> state.brightness = parts[2].toIntOrNull() ?: 3
                    "AOD"      -> state.aod        = parts[2] == "1"
                    "AUTOROT"  -> state.autoRotate = parts[2] == "1"
                    "SOUND"    -> state.sound      = parts[2] == "1"
                    "NOTIFTMO" -> state.notifTmo   = parts[2].toIntOrNull() ?: 1
                    "DND"      -> state.dnd        = parts[2] == "1"
                    "PWRCFG"   -> if (parts.size >= 5) {
                        val m = parts[2].toIntOrNull() ?: return
                        if (m in 0..2) {
                            state.pwrCfg[m][0] = parts[3].toIntOrNull() ?: 3
                            state.pwrCfg[m][1] = parts[4].toIntOrNull() ?: 30
                        }
                    }
                    "ALARM" -> if (parts.size >= 9) {
                        val i = parts[2].toIntOrNull() ?: return
                        if (i in 0..2) {
                            // Copy array so we never mutate the live LiveData reference in-place
                            val src = alarms.value
                            val arr = Array(3) { idx -> src?.get(idx) ?: AlarmState() }
                            arr[i] = AlarmState(
                                hour       = parts[3].toIntOrNull() ?: 0,
                                minute     = parts[4].toIntOrNull() ?: 0,
                                enabled    = parts[5] == "1",
                                daysOfWeek = parts[6].toIntOrNull() ?: 0x7F,
                                useDate    = parts[7] == "1",
                                dateDay    = parts[8].toIntOrNull() ?: 1,
                                dateMonth  = if (parts.size >= 10) parts[9].toIntOrNull() ?: 1 else 1
                            )
                            alarms.postValue(arr)
                        }
                        return  // alarms already posted, skip watchState post below
                    }
                }
                watchState.postValue(state)
            }
        }
    }

    // ── Media state ───────────────────────────────────────────────────────────
    data class MediaState(
        val artist: String = "",
        val song: String = "",
        val playing: Boolean = false,
        val volume: Int = -1   // -1 = unknown
    )
    val mediaState = MutableLiveData(MediaState())

    // Called by MediaSessionManager when active session changes
    fun onMediaUpdate(artist: String, song: String, playing: Boolean, volume: Int) {
        mediaState.postValue(MediaState(artist, song, playing, volume))
        // Push to watch immediately — watch only redraws on new data, zero polling
        val volStr = if (volume >= 0) "|$volume" else ""
        bleManager.write("S|$artist|$song|${if (playing) 1 else 0}$volStr")
    }

    // ── BLE write helpers ─────────────────────────────────────────────────────
    fun sendConfig(key: String, value: Int) = bleManager.sendConfig(key, value)

    fun sendAlarm(idx: Int, alarm: AlarmState) {
        val packet = "X|ALARM|$idx|${alarm.hour}|${alarm.minute}" +
            "|${if (alarm.enabled) 1 else 0}|${alarm.daysOfWeek}" +
            "|${if (alarm.useDate) 1 else 0}|${alarm.dateDay}|${alarm.dateMonth}"
        bleManager.write(packet)
    }

    fun sendPwrCfg(mode: Int, bright: Int, timeout: Int) {
        bleManager.write("X|PWRCFG|$mode|$bright|$timeout")
    }

    // ── Find My Phone ─────────────────────────────────────────────────────────
    private fun triggerFindMyPhone() {
        // Vibrate in a long buzz pattern to locate the phone
        @Suppress("DEPRECATION")
        val vibrator: Vibrator? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val vm = getApplication<Application>().getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as? VibratorManager
            vm?.defaultVibrator
        } else {
            getApplication<Application>().getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            vibrator?.vibrate(VibrationEffect.createWaveform(longArrayOf(0, 500, 200, 500, 200, 500), -1))
        } else {
            @Suppress("DEPRECATION")
            vibrator?.vibrate(longArrayOf(0, 500, 200, 500, 200, 500), -1)
        }
        // Also play the default ringtone briefly
        try {
            val uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE)
            val ringtone = RingtoneManager.getRingtone(getApplication(), uri)
            ringtone?.play()
            // Stop after 3 seconds
            viewModelScope.launch {
                delay(3000)
                ringtone?.stop()
            }
        } catch (_: Exception) { }
        appendLog("FP: Find My Phone triggered")
    }

    // ── Weather fetch (wttr.in, no API key needed) ────────────────────────────
    fun fetchAndSendWeather() {
        viewModelScope.launch {
            val weather = withContext(Dispatchers.IO) { fetchWeatherBlocking() } ?: return@launch
            weatherState.postValue(weather)
            // Send to watch: "W|tempC|hi|lo|condition"
            val packet = "W|${weather.tempC}|${weather.hiC}|${weather.loC}|${weather.condition}"
            bleManager.write(packet)
            appendLog("Weather sent: $packet")
        }
    }

    private fun fetchWeatherBlocking(): WeatherState? {
        return try {
            // wttr.in format=j1 returns JSON with current + daily forecast
            val url = "https://wttr.in/?format=j1"
            val json = URL(url).readText(Charsets.UTF_8)
            parseWttrJson(json)
        } catch (e: Exception) {
            null
        }
    }

    private fun parseWttrJson(json: String): WeatherState? {
        return try {
            val obj = org.json.JSONObject(json)
            val cur = obj.getJSONArray("current_condition").getJSONObject(0)
            val day = obj.getJSONArray("weather").getJSONObject(0)
            val tempC = cur.getString("temp_C").toIntOrNull() ?: 0
            val hiC   = day.getString("maxtempC").toIntOrNull() ?: 0
            val loC   = day.getString("mintempC").toIntOrNull() ?: 0
            val code  = cur.getString("weatherCode").toIntOrNull() ?: 0
            val cond  = weatherCodeToLabel(code)
            WeatherState(tempC, hiC, loC, cond, System.currentTimeMillis())
        } catch (_: Exception) { null }
    }

    private fun weatherCodeToLabel(code: Int): String = when (code) {
        113                              -> "SUNNY"
        116                              -> "PCLOUD"
        119, 122                         -> "CLOUD"
        143, 248, 260                    -> "FOG"
        176, 263, 266, 293, 296,
        299, 302, 305, 308, 353, 356     -> "RAIN"
        179, 182, 185, 227, 230,
        255, 258, 261, 281, 284,
        311, 314, 317, 323, 326,
        329, 332, 335, 338, 350,
        362, 365, 368, 371, 374, 377     -> "SNOW"
        200, 386, 389, 392, 395          -> "STORM"
        else                             -> "CLOUD"
    }

    fun sendDnd(enabled: Boolean) = bleManager.write("X|DND|${if (enabled) 1 else 0}")

    override fun onCleared() {
        super.onCleared()
        viewModelScope.cancel()
    }

    // ── Logging ───────────────────────────────────────────────────────────────
    private fun appendLog(msg: String) {
        val ts = java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.getDefault())
            .format(java.util.Date())
        synchronized(_logLines) {
            _logLines.add("[$ts] $msg")
            if (_logLines.size > 50) _logLines.removeAt(0)
        }
        logLines.postValue(_logLines.toList())
    }
}
