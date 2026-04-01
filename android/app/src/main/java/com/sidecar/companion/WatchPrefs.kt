package com.sidecar.companion

import android.content.Context

object WatchPrefs {
    private const val PREFS = "sidecar_watch"
    fun save(ctx: Context, key: String, value: Int) =
        ctx.getSharedPreferences(PREFS, 0).edit().putInt(key, value).apply()
    fun load(ctx: Context, key: String, default: Int) =
        ctx.getSharedPreferences(PREFS, 0).getInt(key, default)
}
