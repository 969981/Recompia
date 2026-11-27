// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.build

import android.content.Context

/**
 * The player's build options on the Build screen, the same two as the PC launcher's Set up page (pkp_build.py use_options, through
 * the build driver): "Optimized build" (profile-guided: a test build, a short automatic play-test on a copy of the save, then the
 * build with the profile; the profile stays in the build folder and is reused while the dump is the same) and "Tune for this CPU"
 * (the library then runs on this phone's processor only). Both off unless the player switches them on.
 */
object BuildOptions {
    private const val PREFS = "build_options"

    fun optimize(ctx: Context) = prefs(ctx).getBoolean("optimize", false)
    fun tuneCpu(ctx: Context) = prefs(ctx).getBoolean("tune_cpu", false)
    fun setOptimize(ctx: Context, on: Boolean) = prefs(ctx).edit().putBoolean("optimize", on).apply()
    fun setTuneCpu(ctx: Context, on: Boolean) = prefs(ctx).edit().putBoolean("tune_cpu", on).apply()

    /** The driver's arguments for them (tools/android/ondevice/driver/recompia_build.py). */
    fun driverArgs(ctx: Context): List<String> =
        (if (optimize(ctx)) listOf("--optimize", "--save-dir", org.recompia.pkp.Paths.saveDir.path) else emptyList()) +
            (if (tuneCpu(ctx)) listOf("--tune-cpu") else emptyList())

    private fun prefs(ctx: Context) = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
}
