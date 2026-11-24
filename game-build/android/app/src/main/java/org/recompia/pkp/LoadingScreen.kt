// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp

import android.content.res.ColorStateList
import android.os.Handler
import android.os.Looper
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import androidx.core.content.res.ResourcesCompat
import org.libsdl.app.SDLActivity

/**
 * What GameActivity shows from Play until the game's first picture: the screen is otherwise black for 10 to 30 s or more (the
 * runtime starts, the game boots, the shaders load, and the game's first frames are black). An opaque view above SDL's surface
 * (below the touch controls, so the overlay menu can still quit), removed when the runtime reports the first present after a
 * blended draw into a presented texture (runtime/src/os/os_app_phase.h, sent with SDL_SendAndroidMessage), at the latest [CAP_MS]
 * after Play.
 */
class LoadingScreen(private val activity: SDLActivity) {
    private val main = Handler(Looper.getMainLooper())
    private val status: TextView
    private var lastPhase = 0
    private val view: View

    init {
        val res = activity.resources
        val dp = res.displayMetrics.density
        val font = runCatching { ResourcesCompat.getFont(activity, R.font.ui_inter) }.getOrNull()
        fun text(size: Float, color: Int) = TextView(activity).apply {
            typeface = font
            setTextSize(TypedValue.COMPLEX_UNIT_SP, size)
            setTextColor(color)
            gravity = Gravity.CENTER
        }
        status = text(15f, 0xC2EEF0FB.toInt()).apply { setText(R.string.ui_loading_starting) }
        view = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setBackgroundColor(0xFF0B0F22.toInt())   // the app's background (Theme.kt Palette.bg)
            isClickable = true                       // touches during loading do not reach the game
            addView(ImageView(activity).apply {   // the logo Home shows (HomeScreen.kt Hero)
                setImageResource(R.drawable.ui_logo)
                contentDescription = activity.getString(R.string.ui_app_name)
                adjustViewBounds = true
            }, LinearLayout.LayoutParams((280 * dp).toInt(), ViewGroup.LayoutParams.WRAP_CONTENT))
            addView(ProgressBar(activity).apply {
                isIndeterminate = true
                indeterminateTintList = ColorStateList.valueOf(0xFF7EE8C1.toInt())   // Palette.accent
            }, LinearLayout.LayoutParams((40 * dp).toInt(), (40 * dp).toInt()).apply { setMargins(0, (24 * dp).toInt(), 0, (16 * dp).toInt()) })
            addView(status)
        }
        // index 1: above SDL's surface (child 0), below the touch controls added after it
        (SDLActivity.getContentView() as ViewGroup).addView(view, 1,
            ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        // ponytail: a fixed cap, should a game never draw geometry or a phone take longer; raise CAP_MS if a slow phone needs it
        main.postDelayed(::hide, CAP_MS)
    }

    /** A phase from the runtime (UI thread). */
    fun phase(p: Int) {
        if (view.parent == null || p <= lastPhase) return   // the phases come from two runtime threads: never step back
        lastPhase = p
        when (p) {
            PHASE_SHADERS -> status.setText(R.string.ui_loading_shaders)
            PHASE_FIRST_PRESENT -> status.setText(R.string.ui_loading_world)
            PHASE_PICTURE -> hide()
        }
    }

    /** A short fade into the game's picture, which keeps rendering underneath, then the view is gone. */
    private fun hide() {
        main.removeCallbacksAndMessages(null)
        view.isClickable = false
        view.animate().alpha(0f).setDuration(300).withEndAction { (view.parent as? ViewGroup)?.removeView(view) }
    }

    companion object {
        /** SDL_SendAndroidMessage command of the runtime's start-up phases (SDLActivity.COMMAND_USER + 1). */
        const val COMMAND = 0x8001
        const val PHASE_SHADERS = 1
        const val PHASE_FIRST_PRESENT = 2
        const val PHASE_PICTURE = 3
        private const val CAP_MS = 120_000L
    }
}
