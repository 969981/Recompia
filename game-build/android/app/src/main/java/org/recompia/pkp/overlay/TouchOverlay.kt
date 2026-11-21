// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.overlay

import android.app.Activity
import android.app.AlertDialog
import android.app.Application
import android.content.Intent
import android.graphics.Color
import android.hardware.input.InputManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Gravity
import android.view.InputDevice
import android.view.InputEvent
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.Window
import android.widget.Button
import android.widget.LinearLayout
import android.widget.RelativeLayout
import android.widget.Toast
import java.lang.reflect.Proxy
import org.libsdl.app.SDLActivity
import org.recompia.pkp.R

/**
 * The touch controls of GameActivity: an [InputOverlay] above SDL's surface feeding [SdlVirtualGamepad], the overlay
 * menu (show/hide, edit layout, exit), the edit bar, and auto-hide while a physical gamepad is connected or used.
 *
 * While a physical pad is present (and auto-hide is on) the virtual pad is also removed from SDL, so the runtime's pad
 * list holds only real pads ([pad] controller = 1 then means the real pad); it comes back on the next overlay touch.
 */
object TouchOverlay {
    private const val TAG = "Recompia"

    private lateinit var activity: SDLActivity
    private lateinit var overlay: InputOverlay
    private lateinit var editBar: LinearLayout
    private var padPresent = false
    private var editing = false
    private val main = Handler(Looper.getMainLooper())

    @JvmStatic
    fun attach(activity: SDLActivity) {
        this.activity = activity
        val container = SDLActivity.getContentView() as ViewGroup
        overlay = InputOverlay(activity, SdlVirtualGamepad).apply {
            onMenu = ::showMenu
        }
        container.addView(overlay, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        editBar = buildEditBar()
        container.addView(editBar, RelativeLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            addRule(RelativeLayout.CENTER_HORIZONTAL)
            addRule(RelativeLayout.ALIGN_PARENT_TOP)
            topMargin = (96 * activity.resources.displayMetrics.density).toInt()   // below the menu button
        })

        val input = activity.getSystemService(InputManager::class.java)
        input.registerInputDeviceListener(object : InputManager.InputDeviceListener {
            override fun onInputDeviceAdded(deviceId: Int) = updatePads()
            override fun onInputDeviceRemoved(deviceId: Int) = updatePads()
            override fun onInputDeviceChanged(deviceId: Int) = updatePads()
        }, main)
        watchPadInput(activity.window)

        activity.application.registerActivityLifecycleCallbacks(object : Application.ActivityLifecycleCallbacks {
            override fun onActivityResumed(a: Activity) { if (a === activity) applyPrefs() }
            override fun onActivityPaused(a: Activity) { if (a === activity) overlay.releaseAll() }
            override fun onActivityDestroyed(a: Activity) {
                if (a !== activity) return
                SdlVirtualGamepad.unregister()
                activity.application.unregisterActivityLifecycleCallbacks(this)
            }
            override fun onActivityCreated(a: Activity, b: Bundle?) {}
            override fun onActivityStarted(a: Activity) {}
            override fun onActivityStopped(a: Activity) {}
            override fun onActivitySaveInstanceState(a: Activity, b: Bundle) {}
        })
        updatePads()
    }

    /** Re-reads the preferences (the app's Settings page may have changed them) and rebuilds the controls. */
    private fun applyPrefs() {
        overlay.refreshControls()
        updateVisibility()
    }

    private fun updateVisibility() {
        val p = OverlayPrefs.of(activity)
        val hiddenForPad = padPresent && OverlayPrefs.autohide(p)
        overlay.controlsVisible = editing || (OverlayPrefs.enabled(p) && !hiddenForPad)
        if (hiddenForPad && !editing) SdlVirtualGamepad.unregister() else if (overlay.controlsVisible) registerSoon()
    }

    /** Registers the virtual pad as soon as SDL is ready (polled: SDL has no Java-side signal), so no first press is lost. */
    private fun registerSoon() {
        main.removeCallbacks(registerTick)
        main.post(registerTick)
    }

    private val registerTick: Runnable = object : Runnable {
        override fun run() {
            if (!overlay.controlsVisible || SdlVirtualGamepad.ensureRegistered()) return
            main.postDelayed(this, 250)
        }
    }

    private fun isPhysicalPad(d: InputDevice?): Boolean {
        if (d == null || d.isVirtual || d.id < 0) return false
        val s = d.sources
        return (s and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
            (s and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
    }

    private fun updatePads() {
        val pads = InputDevice.getDeviceIds().toList().mapNotNull { InputDevice.getDevice(it) }.filter(::isPhysicalPad)
        val present = pads.isNotEmpty()
        if (present != padPresent) {
            Log.i(TAG, "overlay: physical gamepad ${if (present) "connected: ${pads.joinToString { it.name }}" else "gone"}")
        }
        padPresent = present
        updateVisibility()
    }

    /**
     * Sees every key and motion event of the window before SDL does, to notice a pad that is used (the device list may
     * lag, or a pad was connected before the game started). Wraps the window callback; everything is passed on.
     */
    private fun watchPadInput(window: Window) {
        val original = window.callback
        window.callback = Proxy.newProxyInstance(Window.Callback::class.java.classLoader, arrayOf(Window.Callback::class.java)) { _, method, args ->
            if (!padPresent && (method.name == "dispatchKeyEvent" || method.name == "dispatchGenericMotionEvent")) {
                val e = args?.getOrNull(0) as? InputEvent
                if (e != null && isPhysicalPad(e.device)) main.post(::updatePads)
            }
            if (args == null) method.invoke(original) else method.invoke(original, *args)
        } as Window.Callback
    }

    private fun showMenu() {
        val p = OverlayPrefs.of(activity)
        val enabled = OverlayPrefs.enabled(p)
        val items = arrayOf(
            activity.getString(if (enabled) R.string.overlay_menu_hide else R.string.overlay_menu_show),
            activity.getString(R.string.overlay_menu_edit),
            activity.getString(R.string.overlay_menu_exit),
            activity.getString(R.string.overlay_menu_close),
        )
        overlay.releaseAll()
        AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle(R.string.overlay_menu_title)
            .setItems(items) { _, which ->
                when (which) {
                    0 -> {
                        p.edit().putBoolean(OverlayPrefs.ENABLED, !enabled).apply()
                        updateVisibility()
                    }
                    1 -> setEditing(true)
                    2 -> exitToApp()
                }
            }
            .show()
    }

    private fun setEditing(on: Boolean) {
        editing = on
        editBar.visibility = if (on) View.VISIBLE else View.GONE
        overlay.setEditMode(on)
        updateVisibility()
        if (on) Toast.makeText(activity, R.string.overlay_edit_hint, Toast.LENGTH_LONG).show()
    }

    private fun exitToApp() {
        overlay.releaseAll()
        activity.startActivity(Intent().setClassName(activity, "org.recompia.pkp.MainActivity")
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP))
        activity.finish()
    }

    private fun buildEditBar(): LinearLayout = LinearLayout(activity).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER
        setBackgroundColor(Color.argb(170, 0, 0, 0))
        visibility = View.GONE
        fun add(text: Int, action: () -> Unit) = addView(Button(activity).apply {
            setText(text)
            isAllCaps = false
            setOnClickListener { action() }
        })
        add(R.string.overlay_edit_smaller) { TouchOverlay.overlay.scaleSelected(1f / 1.1f) }
        add(R.string.overlay_edit_bigger) { TouchOverlay.overlay.scaleSelected(1.1f) }
        add(R.string.overlay_edit_reset) { TouchOverlay.overlay.resetLayout() }
        add(R.string.overlay_edit_done) { setEditing(false) }
    }
}
