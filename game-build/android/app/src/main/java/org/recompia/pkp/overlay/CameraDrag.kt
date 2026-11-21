// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.overlay

import android.view.Choreographer
import android.view.MotionEvent
import kotlin.math.sqrt

/**
 * Camera drag: a finger that lands on no control turns the camera, like the mouse on desktop. Its movement becomes a right-stick
 * deflection on the virtual pad, proportional to the finger's speed (dp per second), smoothed over frames, and back to exactly 0
 * when the finger stops or lifts. So the runtime needs no new input path, the prompts stay the pad's, and the result merges with a
 * real pad's right stick like any other pad input.
 *
 * Why not SDL relative mouse motion (the desktop mouse camera, 1:1 in radians): it needs the runtime's relative mouse mode, which on
 * Android is pointer capture (meant for a physical mouse), it would switch the button prompts to keyboard and mouse, and it would
 * show the menu pointer in menus. Rate control through the stick has none of that.
 *
 * Only one finger drags at a time; while the touch right stick is held, the drag leaves the stick to it.
 */
class CameraDrag(
    private val sink: OverlayInputSink,
    /** Read at every sample: the density changes when a foldable is opened or closed and this activity is not recreated (configChanges). */
    private val density: () -> Float,
    private val rightStickHeld: () -> Boolean,
) : Choreographer.FrameCallback {
    var enabled = true
    var sensitivity = 1f      // OverlayPrefs.CAMERA_SENSITIVITY / 100
    var invertX = false
    var invertY = false

    private var pointerId = -1
    private var lastX = 0f
    private var lastY = 0f
    private var accX = 0f      // dp moved since the last frame
    private var accY = 0f
    private var outX = 0f
    private var outY = 0f
    private var lastFrameNs = 0L
    private var scheduled = false

    val active get() = pointerId != -1

    /**
     * Feeds one touch event. [owned] says whether a pointer belongs to a control (or the menu button). Returns true when the
     * event started or ended a drag.
     */
    fun onTouch(event: MotionEvent, owned: (Int) -> Boolean): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = event.actionIndex
                val id = event.getPointerId(i)
                if (!enabled || pointerId != -1 || owned(id)) return false
                pointerId = id
                lastX = event.getX(i)
                lastY = event.getY(i)
                accX = 0f
                accY = 0f
                lastFrameNs = 0L
                schedule()
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                val i = event.findPointerIndex(pointerId)
                if (i < 0) return false
                // every sample, also the batched history, so a fast flick is not lost between frames
                for (h in 0 until event.historySize) add(event.getHistoricalX(i, h), event.getHistoricalY(i, h))
                add(event.getX(i), event.getY(i))
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                if (event.getPointerId(event.actionIndex) != pointerId) return false
                release()
                return true
            }
            MotionEvent.ACTION_CANCEL -> release()
        }
        return false
    }

    private fun add(x: Float, y: Float) {
        val d = density()
        accX += (x - lastX) / d
        accY += (y - lastY) / d
        lastX = x
        lastY = y
    }

    /** Ends the drag and centres the stick at once. */
    fun release() {
        val wasOutput = outX != 0f || outY != 0f
        pointerId = -1
        accX = 0f
        accY = 0f
        outX = 0f
        outY = 0f
        if (wasOutput && !rightStickHeld()) sink.stick(OverlayStick.RIGHT, 0f, 0f)
    }

    private fun schedule() {
        if (scheduled) return
        scheduled = true
        Choreographer.getInstance().postFrameCallback(this)
    }

    override fun doFrame(frameTimeNanos: Long) {
        scheduled = false
        if (pointerId == -1) return
        val dt = if (lastFrameNs == 0L) 1f / 60f else ((frameTimeNanos - lastFrameNs) / 1e9f).coerceIn(1f / 240f, 0.1f)
        lastFrameNs = frameTimeNanos
        // finger speed in dp/s, FULL_SPEED_DP_S (times the sensitivity) = full deflection
        // direction = the desktop mouse camera's (pkp_mouse_camera: mouse right = stick right, mouse up = stick up): finger right
        // turns right, finger up = stick up; each axis can be inverted in Settings
        var tx = accX / dt / FULL_SPEED_DP_S * sensitivity * (if (invertX) -1f else 1f)
        var ty = accY / dt / FULL_SPEED_DP_S * sensitivity * (if (invertY) -1f else 1f)
        accX = 0f
        accY = 0f
        var len = sqrt(tx * tx + ty * ty)
        if (len > 1f) { tx /= len; ty /= len; len = 1f }
        // smoothing: half of the way to the new value each frame (a finger that stops reaches 0 within a few frames)
        outX += (tx - outX) * SMOOTHING
        outY += (ty - outY) * SMOOTHING
        val r = sqrt(outX * outX + outY * outY)
        val (sx, sy) = when {
            r < DEAD_ZONE -> { outX = 0f; outY = 0f; 0f to 0f }
            // lift the smallest deflection past the runtime's right-stick dead zone ([pad] right_deadzone, 0.10 by default), so a
            // slow drag still turns the camera; full speed stays full deflection
            else -> { val k = (PAD_DEAD_ZONE + (1f - PAD_DEAD_ZONE) * r) / r; outX * k to outY * k }
        }
        if (!rightStickHeld()) sink.stick(OverlayStick.RIGHT, sx.coerceIn(-1f, 1f), sy.coerceIn(-1f, 1f))
        schedule()
    }

    companion object {
        // 480 dp/s of finger speed = full deflection (1200 felt too slow in play). Per dp,
        // so the same physical swipe turns the same on a foldable's inner and cover screens.
        const val FULL_SPEED_DP_S = 480f
        const val SMOOTHING = 0.5f
        const val DEAD_ZONE = 0.02f
        const val PAD_DEAD_ZONE = 0.12f
    }
}
