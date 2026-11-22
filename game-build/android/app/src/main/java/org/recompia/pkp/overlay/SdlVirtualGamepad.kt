// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.overlay

import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.KeyEvent
import java.lang.reflect.Method
import org.libsdl.app.SDLActivity
import org.libsdl.app.SDLControllerManager
import org.recompia.pkp.BuildConfig

/**
 * The touch overlay as an Android joystick in SDL: the same JNI entry points SDL's own Java glue uses for real
 * controllers (SDLControllerManager.nativeAddJoystick, onNativePadDown/Up, onNativeJoy, nativeRemoveJoystick), so the
 * runtime sees an ordinary gamepad and needs no touch code.
 *
 * Identity: vendor 0x057E product 0x2009 (Switch Pro Controller), button mask 0x1FFFF, axis mask 0x0F. SDL builds the
 * GUID 05000000 7e050000 09200000 ffff0f00 from that, which SDL's built-in database maps for Android
 * (SDL_gamepad_db.h "Nintendo Switch Pro Controller", sdk>=29: a:b0 b:b1 x:b2 y:b3 lefttrigger:b15 righttrigger:b16,
 * positional, Nintendo face labels). So SDL reports type NINTENDO_SWITCH_PRO with labels B A Y X on
 * SOUTH EAST WEST NORTH: the runtime maps Switch A from EAST in both [pad] label_abxy modes and its prompts show the
 * game's own (Switch) icons. ZL/ZR are digital (L2/R2 keycodes = buttons 15/16 = full trigger axis).
 * A plain "standard" Android pad would instead get Xbox labels, swapping A/B under label_abxy = 1.
 *
 * The native methods are package-private in SDL's glue, hence reflection (they are this app's own classes).
 */
object SdlVirtualGamepad : OverlayInputSink {
    private const val TAG = "Recompia"

    /** Negative: never an Android InputDevice id, and SDL's own device polling ignores ids < 0. */
    const val DEVICE_ID = -0x5243

    // ponytail: SDL gives Java no "joysticks initialised" signal; registering before SDL_Init would cache a failed
    // mapping, so wait until SDL's main thread has run this long. Upgrade: a hook from GameActivity.
    private const val SDL_SETTLE_MS = 1500L

    /** After registering, the runtime needs a frame or two to open the pad; earlier presses would become key events. */
    private const val OPEN_DELAY_MS = 300L

    private val main = Handler(Looper.getMainLooper())
    private var registered = false
    private var liveAt = Long.MAX_VALUE
    private var sdlSeenAt = 0L

    private val want = BooleanArray(OverlayButton.entries.size)
    private val sent = BooleanArray(OverlayButton.entries.size)
    private val wantAxis = FloatArray(4)
    private val sentAxis = FloatArray(4)

    private val keycode = mapOf(
        OverlayButton.A to KeyEvent.KEYCODE_BUTTON_B,          // EAST
        OverlayButton.B to KeyEvent.KEYCODE_BUTTON_A,          // SOUTH
        OverlayButton.X to KeyEvent.KEYCODE_BUTTON_Y,          // NORTH
        OverlayButton.Y to KeyEvent.KEYCODE_BUTTON_X,          // WEST
        OverlayButton.L to KeyEvent.KEYCODE_BUTTON_L1,
        OverlayButton.R to KeyEvent.KEYCODE_BUTTON_R1,
        OverlayButton.ZL to KeyEvent.KEYCODE_BUTTON_L2,        // b15 = lefttrigger
        OverlayButton.ZR to KeyEvent.KEYCODE_BUTTON_R2,        // b16 = righttrigger
        OverlayButton.PLUS to KeyEvent.KEYCODE_BUTTON_START,
        OverlayButton.MINUS to KeyEvent.KEYCODE_BUTTON_SELECT,
        OverlayButton.L3 to KeyEvent.KEYCODE_BUTTON_THUMBL,
        OverlayButton.R3 to KeyEvent.KEYCODE_BUTTON_THUMBR,
        OverlayButton.DPAD_UP to KeyEvent.KEYCODE_DPAD_UP,
        OverlayButton.DPAD_DOWN to KeyEvent.KEYCODE_DPAD_DOWN,
        OverlayButton.DPAD_LEFT to KeyEvent.KEYCODE_DPAD_LEFT,
        OverlayButton.DPAD_RIGHT to KeyEvent.KEYCODE_DPAD_RIGHT,
    )

    private val methods: Map<String, Method> by lazy {
        SDLControllerManager::class.java.declaredMethods
            .filter { it.name in setOf("nativeAddJoystick", "nativeRemoveJoystick", "onNativePadDown", "onNativePadUp", "onNativeJoy") }
            .onEach { it.isAccessible = true }
            .associateBy { it.name }
    }

    private val sdlThread: Thread?
        get() = try {
            SDLActivity::class.java.getDeclaredField("mSDLThread").apply { isAccessible = true }.get(null) as Thread?
        } catch (e: ReflectiveOperationException) {
            null
        }

    /** Registers the device once SDL's thread has run for [SDL_SETTLE_MS] (polled by TouchOverlay). Returns whether it is registered. */
    fun ensureRegistered(): Boolean {
        if (registered) return true
        val now = SystemClock.uptimeMillis()
        if (sdlThread?.isAlive != true) {
            sdlSeenAt = 0
            return false
        }
        if (sdlSeenAt == 0L) sdlSeenAt = now
        if (now - sdlSeenAt < SDL_SETTLE_MS) return false
        val add = methods["nativeAddJoystick"] ?: return false.also { Log.e(TAG, "overlay: SDL has no nativeAddJoystick") }
        val args = mutableListOf<Any>(DEVICE_ID, "Recompia Touch Controls", "recompia-touch-controls", 0x057E, 0x2009,
            0x1FFFF, 4, 0x0F, 0, false)
        while (args.size < add.parameterCount) args.add(false)   // SDL 3.2+: has_rgb_led, has_accelerometer, has_gyroscope
        try {
            add.invoke(null, *args.toTypedArray())
        } catch (e: Exception) {
            Log.e(TAG, "overlay: nativeAddJoystick failed", e)
            return false
        }
        registered = true
        sent.fill(false)
        sentAxis.fill(0f)
        liveAt = now + OPEN_DELAY_MS
        main.postDelayed(::flush, OPEN_DELAY_MS)
        Log.i(TAG, "overlay: virtual gamepad registered (device $DEVICE_ID, 057e:2009)")
        return true
    }

    /** Releases everything and removes the device from SDL (a real pad took over, or the game is closing). */
    fun unregister() {
        releaseAll()
        if (!registered) return
        registered = false
        liveAt = Long.MAX_VALUE
        try {
            methods["nativeRemoveJoystick"]?.invoke(null, DEVICE_ID)
        } catch (e: Exception) {
            Log.e(TAG, "overlay: nativeRemoveJoystick failed", e)
        }
        Log.i(TAG, "overlay: virtual gamepad removed")
    }

    fun releaseAll() {
        want.fill(false)
        wantAxis.fill(0f)
        flush()
    }

    override fun button(button: OverlayButton, pressed: Boolean) {
        want[button.ordinal] = pressed
        flush()
    }

    override fun stick(stick: OverlayStick, x: Float, y: Float) {
        val base = if (stick == OverlayStick.LEFT) 0 else 2
        wantAxis[base] = x.coerceIn(-1f, 1f)
        wantAxis[base + 1] = y.coerceIn(-1f, 1f)
        flush()
    }

    private fun flush() {
        if (!registered || SystemClock.uptimeMillis() < liveAt) return
        for (b in OverlayButton.entries) {
            val i = b.ordinal
            if (want[i] == sent[i]) continue
            sent[i] = want[i]
            val m = methods[if (want[i]) "onNativePadDown" else "onNativePadUp"] ?: continue
            val args = mutableListOf<Any>(DEVICE_ID, keycode.getValue(b))
            if (m.parameterCount == 3) args.add(0)                 // SDL 3.2+: scancode
            m.invoke(null, *args.toTypedArray())
            if (BuildConfig.DEBUG) Log.d(TAG, "overlay: button ${b.name} ${if (want[i]) "down" else "up"}")
        }
        val joy = methods["onNativeJoy"] ?: return
        for (a in 0 until 4) {
            if (wantAxis[a] == sentAxis[a]) continue
            sentAxis[a] = wantAxis[a]
            joy.invoke(null, DEVICE_ID, a, wantAxis[a])
        }
        if (BuildConfig.DEBUG && !wantAxis.contentEquals(lastLogged)) logAxes()
    }

    private val lastLogged = FloatArray(4)
    private var lastLogAt = 0L

    /** Debug builds: stick values at most 10 times a second, and always the return to centre. */
    private fun logAxes() {
        val now = SystemClock.uptimeMillis()
        val centred = wantAxis.all { it == 0f }
        if (!centred && now - lastLogAt < 100) return
        lastLogAt = now
        wantAxis.copyInto(lastLogged)
        Log.d(TAG, "overlay: axes lx=%.2f ly=%.2f rx=%.2f ry=%.2f".format(wantAxis[0], wantAxis[1], wantAxis[2], wantAxis[3]))
    }
}
