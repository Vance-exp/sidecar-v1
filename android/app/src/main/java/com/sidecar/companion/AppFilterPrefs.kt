package com.sidecar.companion

import android.content.Context

object AppFilterPrefs {
    private const val PREFS = "sidecar_filter"
    private const val KEY = "blocked"

    fun isBlocked(ctx: Context, pkg: String): Boolean =
        (prefs(ctx).getStringSet(KEY, emptySet()) ?: emptySet()).contains(pkg)

    fun setBlocked(ctx: Context, pkg: String, blocked: Boolean) {
        val set = (prefs(ctx).getStringSet(KEY, emptySet()) ?: emptySet()).toMutableSet()
        if (blocked) set.add(pkg) else set.remove(pkg)
        prefs(ctx).edit().putStringSet(KEY, set).apply()
    }

    private fun prefs(ctx: Context) = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
}
