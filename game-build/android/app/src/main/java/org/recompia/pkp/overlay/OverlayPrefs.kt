// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.overlay

import android.content.Context
import android.content.SharedPreferences

/**
 * The overlay's settings, in the SharedPreferences file [FILE] (the app's Settings page writes them, the in-game menu writes
 * [ENABLED]), and the edited layouts in [LAYOUT_FILE] (only the game writes it; resetting = deleting that file). The game runs in
 * its own process: [FILE] is opened with MODE_MULTI_PROCESS so each process re-reads what the other wrote, and the layouts sit
 * in a file of their own so a settings write from the UI process never puts back a stale layout.
 */
object OverlayPrefs {
    const val FILE = "overlay"
    const val ENABLED = "overlay_enabled"        // boolean, default true
    const val OPACITY = "overlay_opacity"        // int 0..100 (percent), default 60
    const val SCALE = "overlay_scale"            // int 50..150 (percent), default 100
    const val HAPTICS = "overlay_haptics"        // boolean, default true
    const val AUTOHIDE = "overlay_autohide"      // boolean, default true: hide while a physical gamepad is connected
    const val CAMERA_DRAG = "overlay_camera_drag"                  // boolean, default true: drag on free screen space turns the camera
    const val CAMERA_SENSITIVITY = "overlay_camera_sensitivity"    // int 10..900 (percent), default 200 (players found 100 too slow)
    const val CAMERA_INVERT_Y = "overlay_camera_invert_y"          // boolean, default false
    const val CAMERA_INVERT_X = "overlay_camera_invert_x"          // boolean, default false
    const val LAYOUT_FILE = "overlay_layout"
    const val LAYOUT_COVER = "overlay_layout_cover"
    const val LAYOUT_INNER = "overlay_layout_inner"

    // MODE_MULTI_PROCESS is deprecated but still reloads the file when another process changed it (all we need here).
    @Suppress("DEPRECATION")
    fun of(context: Context): SharedPreferences = context.getSharedPreferences(FILE, Context.MODE_MULTI_PROCESS)

    fun layouts(context: Context): SharedPreferences = context.getSharedPreferences(LAYOUT_FILE, Context.MODE_PRIVATE)

    fun enabled(p: SharedPreferences) = p.getBoolean(ENABLED, true)
    fun opacity(p: SharedPreferences) = p.getInt(OPACITY, 60).coerceIn(0, 100)
    fun scale(p: SharedPreferences) = p.getInt(SCALE, 100).coerceIn(50, 150)
    fun haptics(p: SharedPreferences) = p.getBoolean(HAPTICS, true)
    fun autohide(p: SharedPreferences) = p.getBoolean(AUTOHIDE, true)
    fun cameraDrag(p: SharedPreferences) = p.getBoolean(CAMERA_DRAG, true)
    const val CAMERA_SENSITIVITY_MAX = 900
    fun cameraSensitivity(p: SharedPreferences) = p.getInt(CAMERA_SENSITIVITY, 200).coerceIn(10, CAMERA_SENSITIVITY_MAX)
    fun cameraInvertY(p: SharedPreferences) = p.getBoolean(CAMERA_INVERT_Y, false)
    fun cameraInvertX(p: SharedPreferences) = p.getBoolean(CAMERA_INVERT_X, false)

    /** The layout's controls: the saved ones ("id:x,y,scale;..."), every other control at its default. */
    fun loadLayout(p: SharedPreferences, layout: OverlayLayout, default: (OverlayControl) -> Pair<Double, Double>): List<OverlayControlData> {
        val saved = HashMap<OverlayControl, OverlayControlData>()
        p.getString(layout.prefKey, null)?.split(';')?.forEach { entry ->
            val (id, values) = entry.split(':').takeIf { it.size == 2 } ?: return@forEach
            val c = OverlayControl.from(id) ?: return@forEach
            val v = values.split(',').mapNotNull { it.toDoubleOrNull() }
            if (v.size == 3) saved[c] = OverlayControlData(c, v[0].coerceIn(0.0, 1.0), v[1].coerceIn(0.0, 1.0), v[2].toFloat().coerceIn(0.5f, 2f))
        }
        return OverlayControl.entries.map { c ->
            saved[c] ?: default(c).let { (x, y) -> OverlayControlData(c, x, y, 1f) }
        }
    }

    fun saveLayout(p: SharedPreferences, layout: OverlayLayout, controls: List<OverlayControlData>) {
        val s = controls.joinToString(";") { "${it.control.id}:%.4f,%.4f,%.2f".format(java.util.Locale.ROOT, it.x, it.y, it.individualScale) }
        p.edit().putString(layout.prefKey, s).apply()
    }

    fun resetLayout(p: SharedPreferences, layout: OverlayLayout) {
        p.edit().remove(layout.prefKey).apply()
    }
}
