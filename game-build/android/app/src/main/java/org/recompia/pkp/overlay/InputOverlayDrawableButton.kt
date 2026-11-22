// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Adapted for Recompia: OverlayButton instead of NativeButton, a Boolean state, no snap-to-grid setting, and
// pressed state settable from outside (sliding between face buttons).

package org.recompia.pkp.overlay

import android.content.res.Resources
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Rect
import android.graphics.drawable.BitmapDrawable
import android.view.MotionEvent

/**
 * Custom [BitmapDrawable] that is capable
 * of storing it's own ID.
 *
 * @param res                [Resources] instance.
 * @param defaultStateBitmap [Bitmap] to use with the default state Drawable.
 * @param pressedStateBitmap [Bitmap] to use with the pressed state Drawable.
 * @param button             [OverlayButton] for this type of button, null for the menu button.
 */
class InputOverlayDrawableButton(
    res: Resources,
    defaultStateBitmap: Bitmap,
    pressedStateBitmap: Bitmap,
    val button: OverlayButton?,
    val overlayControlData: OverlayControlData
) {
    // The ID value what motion event is tracking
    var trackId: Int

    // The drawable position on the screen
    private var buttonPositionX = 0
    private var buttonPositionY = 0

    val width: Int
    val height: Int

    private val defaultStateBitmap: BitmapDrawable
    private val pressedStateBitmap: BitmapDrawable
    var pressedState = false
        private set

    private var previousTouchX = 0
    private var previousTouchY = 0
    var controlPositionX = 0
    var controlPositionY = 0

    init {
        this.defaultStateBitmap = BitmapDrawable(res, defaultStateBitmap)
        this.pressedStateBitmap = BitmapDrawable(res, pressedStateBitmap)
        trackId = -1
        width = this.defaultStateBitmap.intrinsicWidth
        height = this.defaultStateBitmap.intrinsicHeight
    }

    /**
     * Updates button status based on the motion event.
     *
     * @return true if value was changed
     */
    fun updateStatus(event: MotionEvent): Boolean {
        val pointerIndex = event.actionIndex
        val xPosition = event.getX(pointerIndex).toInt()
        val yPosition = event.getY(pointerIndex).toInt()
        val pointerId = event.getPointerId(pointerIndex)
        val motionEvent = event.action and MotionEvent.ACTION_MASK
        val isActionDown =
            motionEvent == MotionEvent.ACTION_DOWN || motionEvent == MotionEvent.ACTION_POINTER_DOWN
        val isActionUp =
            motionEvent == MotionEvent.ACTION_UP || motionEvent == MotionEvent.ACTION_POINTER_UP

        if (isActionDown) {
            if (!bounds.contains(xPosition, yPosition)) {
                return false
            }
            pressedState = true
            trackId = pointerId
            return true
        }

        if (isActionUp) {
            if (trackId != pointerId) {
                return false
            }
            pressedState = false
            trackId = -1
            return true
        }

        return false
    }

    /** Press for [pointerId], or release with -1. */
    fun setTracked(pointerId: Int) {
        trackId = pointerId
        pressedState = pointerId != -1
    }

    fun setPosition(x: Int, y: Int) {
        buttonPositionX = x
        buttonPositionY = y
    }

    fun draw(canvas: Canvas?) {
        currentStateBitmapDrawable.draw(canvas!!)
    }

    private val currentStateBitmapDrawable: BitmapDrawable
        get() = if (pressedState) pressedStateBitmap else defaultStateBitmap

    fun onConfigureTouch(event: MotionEvent): Boolean {
        val pointerIndex = event.actionIndex
        val fingerPositionX = event.getX(pointerIndex).toInt()
        val fingerPositionY = event.getY(pointerIndex).toInt()

        when (event.action) {
            MotionEvent.ACTION_DOWN -> {
                previousTouchX = fingerPositionX
                previousTouchY = fingerPositionY
                controlPositionX = bounds.left
                controlPositionY = bounds.top
            }

            MotionEvent.ACTION_MOVE -> {
                controlPositionX += fingerPositionX - previousTouchX
                controlPositionY += fingerPositionY - previousTouchY
                setBounds(
                    controlPositionX,
                    controlPositionY,
                    width + controlPositionX,
                    height + controlPositionY
                )
                previousTouchX = fingerPositionX
                previousTouchY = fingerPositionY
            }
        }
        return true
    }

    fun setBounds(left: Int, top: Int, right: Int, bottom: Int) {
        defaultStateBitmap.setBounds(left, top, right, bottom)
        pressedStateBitmap.setBounds(left, top, right, bottom)
    }

    fun setOpacity(value: Int) {
        defaultStateBitmap.alpha = value
        pressedStateBitmap.alpha = value
    }

    val bounds: Rect
        get() = defaultStateBitmap.bounds
}
