// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted for Recompia from Eden's overlay/InputOverlay.kt: input goes to an OverlayInputSink instead of
// NativeInput/NativeLibrary, settings come from OverlayPrefs (SharedPreferences) instead of the emulator's settings
// classes, no player index, no IME, no colour themes or preset previews, layouts per form factor (cover / inner
// screen), sliding between face buttons, a menu button, edit mode with selection and resize through TouchOverlay.

package org.recompia.pkp.overlay

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.RectF
import android.util.Log
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import kotlin.math.hypot
import org.libsdl.app.SDLActivity
import android.view.View
import android.view.WindowInsets
import androidx.core.content.ContextCompat
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import org.recompia.pkp.BuildConfig
import org.recompia.pkp.R

/**
 * Draws the interactive input overlay on top of the game's surface.
 */
@SuppressLint("ViewConstructor")
class InputOverlay(context: Context, private val sink: OverlayInputSink) : View(context), View.OnTouchListener {
    private val overlayButtons = ArrayList<InputOverlayDrawableButton>()
    private val overlayDpads = ArrayList<InputOverlayDrawableDpad>()
    private val overlayJoysticks = ArrayList<InputOverlayDrawableJoystick>()
    private var menuButton: InputOverlayDrawableButton? = null

    private val prefs get() = OverlayPrefs.of(context)
    private val layoutPrefs = OverlayPrefs.layouts(context)
    var layout = OverlayLayout.Cover
        private set
    private var controlData: List<OverlayControlData> = emptyList()
    private val safe = Rect()

    /** Draw and handle the game controls; the menu button is always there. */
    var controlsVisible = true
        set(value) {
            if (field == value) return
            field = value
            if (!value) releaseAll()
            invalidate()
        }

    var onMenu: (() -> Unit)? = null

    private val cameraDrag = CameraDrag(sink, { resources.displayMetrics.density }) {
        overlayJoysticks.any { it.joystick == OverlayStick.RIGHT && it.trackId != -1 }
    }

    private var inEditMode = false
    private var selected: OverlayControlData? = null
    private var dragging: Any? = null
    private var touchStartX = 0f
    private var touchStartY = 0f
    private var hasMoved = false
    private val moveThreshold = 20f

    private val selectionPaint = Paint().apply {
        color = Color.argb(200, 255, 200, 0)
        strokeWidth = 4f
        style = Paint.Style.STROKE
    }
    private val safePaint = Paint().apply {
        color = Color.argb(60, 255, 255, 255)
        strokeWidth = 2f
        style = Paint.Style.STROKE
    }

    init {
        setOnTouchListener(this)
        setWillNotDraw(false)
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        refreshControls()
    }

    override fun onApplyWindowInsets(insets: WindowInsets): WindowInsets {
        post { refreshControls() }
        return super.onApplyWindowInsets(insets)
    }

    override fun draw(canvas: Canvas) {
        super.draw(canvas)
        if (inEditMode) canvas.drawRect(safe, safePaint)
        if (controlsVisible || inEditMode) {
            for (button in overlayButtons) button.draw(canvas)
            for (dpad in overlayDpads) dpad.draw(canvas)
            for (joystick in overlayJoysticks) joystick.draw(canvas)
        }
        menuButton?.draw(canvas)
        if (inEditMode) selectedBounds()?.let { canvas.drawRect(RectF(it).apply { inset(-6f, -6f) }, selectionPaint) }
    }

    private fun selectedBounds(): Rect? {
        val sel = selected ?: return null
        overlayButtons.firstOrNull { it.overlayControlData === sel }?.let { return it.bounds }
        overlayDpads.firstOrNull { it.overlayControlData === sel }?.let { return it.bounds }
        overlayJoysticks.firstOrNull { it.overlayControlData === sel }?.let { return it.bounds }
        menuButton?.takeIf { it.overlayControlData === sel }?.let { return it.bounds }
        return null
    }

    override fun onTouch(v: View, event: MotionEvent): Boolean {
        if (inEditMode) return onTouchWhileEditing(event)

        val menu = menuButton
        if (!controlsVisible) {
            // Only the menu button takes touches; everything else falls through to SDL's surface.
            if (menu == null) return false
            if (event.actionMasked == MotionEvent.ACTION_DOWN && !menu.bounds.contains(event.x.toInt(), event.y.toInt())) return false
            return handleMenu(menu, event)
        }

        if (event.actionMasked == MotionEvent.ACTION_CANCEL) {
            releaseAll()
            return true
        }

        var shouldUpdateView = false
        var pressed = false
        for (button in overlayButtons) {
            if (!button.updateStatus(event)) continue
            sink.button(button.button!!, button.pressedState)
            pressed = pressed || button.pressedState
            shouldUpdateView = true
        }
        for (dpad in overlayDpads) {
            if (!dpad.updateStatus(event, true)) continue
            sink.button(dpad.up, dpad.upStatus)
            sink.button(dpad.down, dpad.downStatus)
            sink.button(dpad.left, dpad.leftStatus)
            sink.button(dpad.right, dpad.rightStatus)
            pressed = pressed || isDown(event)
            shouldUpdateView = true
        }
        for (joystick in overlayJoysticks) {
            if (!joystick.updateStatus(event)) continue
            sink.stick(joystick.joystick, joystick.xAxis, joystick.yAxis)
            pressed = pressed || isDown(event)
            shouldUpdateView = true
        }
        if (event.actionMasked == MotionEvent.ACTION_MOVE && slideFaceButtons(event)) {
            pressed = true
            shouldUpdateView = true
        }
        if (menu != null && (menu.trackId != -1 || isDown(event))) shouldUpdateView = handleMenu(menu, event) || shouldUpdateView
        // a finger on no control (and not on the menu button) drags the camera; a short tap there goes to the game instead
        cameraDrag.onTouch(event, ::ownedByControl)
        forwardTap(event)

        if (pressed) playHaptics()
        if (shouldUpdateView) invalidate()
        return true
    }

    // Free-area taps reach the game: the runtime's own prompts (the quit prompt, the on-screen keyboard,
    // the F1 menu) take SDL's touch-made mouse events, and the overlay above SDL's surface would otherwise eat every finger.
    // Rule: down and up within TAP_MS and TAP_DP on no control = a tap, forwarded to SDL as one finger down then up at that spot;
    // anything longer or farther is the camera drag's and is never forwarded.
    private var tapId = -1
    private var tapAt = 0L
    private var tapX = 0f
    private var tapY = 0f
    private var tapFar = false

    private fun forwardTap(event: MotionEvent) {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = event.actionIndex
                val id = event.getPointerId(i)
                if (tapId != -1 || ownedByControl(id)) return
                tapId = id; tapAt = event.eventTime; tapX = event.getX(i); tapY = event.getY(i); tapFar = false
            }
            MotionEvent.ACTION_MOVE -> {
                val i = event.findPointerIndex(tapId)
                if (i >= 0 && hypot(event.getX(i) - tapX, event.getY(i) - tapY) > TAP_DP * resources.displayMetrics.density) tapFar = true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                if (event.getPointerId(event.actionIndex) != tapId) return
                tapId = -1
                if (tapFar || event.eventTime - tapAt > TAP_MS || width <= 1 || height <= 1) return
                val x = tapX / (width - 1)
                val y = tapY / (height - 1)
                runCatching {
                    SDLActivity.onNativeTouch(event.deviceId, TAP_FINGER, MotionEvent.ACTION_DOWN, x, y, 1f)
                    SDLActivity.onNativeTouch(event.deviceId, TAP_FINGER, MotionEvent.ACTION_UP, x, y, 0f)
                }
            }
            MotionEvent.ACTION_CANCEL -> tapId = -1
        }
    }

    private fun ownedByControl(pointerId: Int): Boolean =
        overlayButtons.any { it.trackId == pointerId } || overlayDpads.any { it.trackId == pointerId } ||
            overlayJoysticks.any { it.trackId == pointerId } || menuButton?.trackId == pointerId

    private fun isDown(event: MotionEvent) =
        event.actionMasked == MotionEvent.ACTION_DOWN || event.actionMasked == MotionEvent.ACTION_POINTER_DOWN

    /** The menu button opens the overlay menu when the finger that pressed it lifts on it. */
    private fun handleMenu(menu: InputOverlayDrawableButton, event: MotionEvent): Boolean {
        val wasTracked = menu.trackId
        if (!menu.updateStatus(event)) return false
        if (menu.trackId == -1 && wasTracked != -1) {
            val i = event.actionIndex
            if (menu.bounds.contains(event.getX(i).toInt(), event.getY(i).toInt())) onMenu?.invoke()
        }
        invalidate()
        return true
    }

    /**
     * A finger on a face button that slides onto another face button moves the press there (A B X Y only).
     * Returns whether a button got newly pressed.
     */
    private fun slideFaceButtons(event: MotionEvent): Boolean {
        var newPress = false
        for (i in 0 until event.pointerCount) {
            val id = event.getPointerId(i)
            val x = event.getX(i).toInt()
            val y = event.getY(i).toInt()
            val face = overlayButtons.filter { it.button in FACE }
            val owner = face.firstOrNull { it.trackId == id } ?: continue
            if (owner.bounds.contains(x, y)) continue
            val target = face.firstOrNull { it.trackId == -1 && it.bounds.contains(x, y) } ?: continue
            owner.setTracked(-1)
            sink.button(owner.button!!, false)
            target.setTracked(id)
            sink.button(target.button!!, true)
            newPress = true
        }
        return newPress
    }

    private fun playHaptics() {
        if (OverlayPrefs.haptics(prefs)) performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
    }

    /** Every control released and centred; the sink gets a release for everything (no stuck input). */
    fun releaseAll() {
        for (button in overlayButtons) {
            if (button.pressedState) sink.button(button.button!!, false)
            button.setTracked(-1)
        }
        for (dpad in overlayDpads) {
            dpad.reset()
            for (b in listOf(dpad.up, dpad.down, dpad.left, dpad.right)) sink.button(b, false)
        }
        for (joystick in overlayJoysticks) {
            joystick.reset()
            sink.stick(joystick.joystick, 0f, 0f)
        }
        menuButton?.setTracked(-1)
        cameraDrag.release()
        invalidate()
    }

    private fun onTouchWhileEditing(event: MotionEvent): Boolean {
        val pointerIndex = event.actionIndex
        val x = event.getX(pointerIndex).toInt()
        val y = event.getY(pointerIndex).toInt()
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                dragging = (overlayJoysticks.firstOrNull { it.bounds.contains(x, y) }
                    ?: overlayDpads.firstOrNull { it.bounds.contains(x, y) }
                    ?: overlayButtons.firstOrNull { it.bounds.contains(x, y) }
                    ?: menuButton?.takeIf { it.bounds.contains(x, y) })
                selected = dataOf(dragging)
                touchStartX = event.x
                touchStartY = event.y
                hasMoved = false
                configure(dragging, event)
                invalidate()
            }

            MotionEvent.ACTION_MOVE -> if (dragging != null) {
                val dx = event.x - touchStartX
                val dy = event.y - touchStartY
                if (hasMoved || dx * dx + dy * dy > moveThreshold * moveThreshold) {
                    hasMoved = true
                    configure(dragging, event)
                    invalidate()
                }
            }

            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                val d = dragging
                dragging = null
                if (d != null && hasMoved) {
                    val b = boundsOf(d)
                    dataOf(d)?.let {
                        it.x = ((b.exactCenterX() - safe.left) / max(1, safe.width())).toDouble().coerceIn(0.0, 1.0)
                        it.y = ((b.exactCenterY() - safe.top) / max(1, safe.height())).toDouble().coerceIn(0.0, 1.0)
                    }
                    OverlayPrefs.saveLayout(layoutPrefs, layout, controlData)
                    refreshControls()
                }
            }
        }
        return true
    }

    private fun configure(d: Any?, event: MotionEvent) {
        when (d) {
            is InputOverlayDrawableButton -> d.onConfigureTouch(event)
            is InputOverlayDrawableDpad -> d.onConfigureTouch(event)
            is InputOverlayDrawableJoystick -> d.onConfigureTouch(event)
        }
    }

    private fun dataOf(d: Any?): OverlayControlData? = when (d) {
        is InputOverlayDrawableButton -> d.overlayControlData
        is InputOverlayDrawableDpad -> d.overlayControlData
        is InputOverlayDrawableJoystick -> d.overlayControlData
        else -> null
    }

    private fun boundsOf(d: Any): Rect = when (d) {
        is InputOverlayDrawableButton -> d.bounds
        is InputOverlayDrawableDpad -> d.bounds
        is InputOverlayDrawableJoystick -> d.bounds
        else -> Rect()
    }

    fun setEditMode(editMode: Boolean) {
        releaseAll()
        inEditMode = editMode
        selected = null
        dragging = null
        refreshControls()
    }

    /** Edit mode: the selected control's own size times [factor] (0.5..2). Returns false when nothing is selected. */
    fun scaleSelected(factor: Float): Boolean {
        val sel = selected ?: return false
        sel.individualScale = (sel.individualScale * factor).coerceIn(0.5f, 2f)
        OverlayPrefs.saveLayout(layoutPrefs, layout, controlData)
        refreshControls()
        return true
    }

    /** Edit mode: every control of the current layout back to its default place and size. */
    fun resetLayout() {
        OverlayPrefs.resetLayout(layoutPrefs, layout)
        selected = null
        refreshControls()
    }

    /** Rebuilds every control from the preferences (size, opacity, layout) and the current size and insets. */
    fun refreshControls() {
        if (width == 0 || height == 0) return
        releaseAll()
        layout = OverlayLayout.forSize(width, height)

        val insets = rootWindowInsets?.getInsets(WindowInsets.Type.displayCutout() or WindowInsets.Type.systemGestures())
        safe.set(insets?.left ?: 0, insets?.top ?: 0, width - (insets?.right ?: 0), height - (insets?.bottom ?: 0))

        // Upstream sizes controls from the screen's short side. Capped at 400 dp, so the large, nearly square inner
        // screen of a foldable gets controls of phone size instead of twice that.
        val density = resources.displayMetrics.density
        val unit = min(min(safe.width(), safe.height()).toFloat(), 400f * density)
        val base = unit * OverlayPrefs.scale(prefs) / 100f
        val selId = selected?.control
        controlData = OverlayPrefs.loadLayout(layoutPrefs, layout) { it.defaultPosition(safe.width(), safe.height(), unit) }
        selected = controlData.firstOrNull { it.control == selId }
        cameraDrag.enabled = OverlayPrefs.cameraDrag(prefs)
        cameraDrag.sensitivity = OverlayPrefs.cameraSensitivity(prefs) / 100f
        cameraDrag.invertY = OverlayPrefs.cameraInvertY(prefs)
        cameraDrag.invertX = OverlayPrefs.cameraInvertX(prefs)
        val alpha = if (inEditMode) max(OverlayPrefs.opacity(prefs), 80) * 255 / 100 else OverlayPrefs.opacity(prefs) * 255 / 100

        overlayButtons.clear()
        overlayDpads.clear()
        overlayJoysticks.clear()
        menuButton = null
        for (data in controlData) {
            val c = data.control
            val size = c.size * base * data.individualScale
            val cx = (safe.left + data.x * safe.width()).roundToInt()
            val cy = (safe.top + data.y * safe.height()).roundToInt()
            when (c) {
                OverlayControl.STICK_L, OverlayControl.STICK_R -> {
                    val outer = getBitmap(R.drawable.overlay_joystick_range, size)
                    val innerSize = size / 1.66f
                    val rectOuter = Rect(cx - outer.width / 2, cy - outer.height / 2, cx + outer.width / 2, cy + outer.height / 2)
                    val stick = InputOverlayDrawableJoystick(
                        resources, outer,
                        getBitmap(R.drawable.overlay_joystick, innerSize),
                        getBitmap(R.drawable.overlay_joystick_depressed, innerSize),
                        rectOuter, Rect(0, 0, innerSize.toInt(), innerSize.toInt()),
                        if (c == OverlayControl.STICK_L) OverlayStick.LEFT else OverlayStick.RIGHT,
                        true, data
                    )
                    stick.setPosition(cx, cy)
                    stick.setOpacity(alpha)
                    overlayJoysticks.add(stick)
                }

                OverlayControl.COMBINED_DPAD -> {
                    val dpad = InputOverlayDrawableDpad(
                        resources,
                        getBitmap(R.drawable.overlay_dpad_standard, size),
                        getBitmap(R.drawable.overlay_dpad_standard_cardinal_depressed, size),
                        getBitmap(R.drawable.overlay_dpad_standard_diagonal_depressed, size),
                        data
                    )
                    dpad.setBounds(cx - dpad.width / 2, cy - dpad.height / 2, cx + dpad.width / 2, cy + dpad.height / 2)
                    dpad.setPosition(cx - dpad.width / 2, cy - dpad.height / 2)
                    dpad.setOpacity(alpha)
                    overlayDpads.add(dpad)
                }

                else -> {
                    val (res, resPressed, button) = BUTTON_ART.getValue(c)
                    val b = InputOverlayDrawableButton(resources, getBitmap(res, size), getBitmap(resPressed, size), button, data)
                    b.setBounds(cx - b.width / 2, cy - b.height / 2, cx + b.width / 2, cy + b.height / 2)
                    b.setPosition(cx - b.width / 2, cy - b.height / 2)
                    b.setOpacity(if (button == null) max(alpha, 140) else alpha)
                    if (button == null) menuButton = b else overlayButtons.add(b)
                }
            }
        }
        if (BuildConfig.DEBUG) {
            val centres = overlayButtons.map { "${it.overlayControlData.control.id}=${it.bounds.centerX()},${it.bounds.centerY()}" } +
                overlayDpads.map { "combined_dpad=${it.bounds.centerX()},${it.bounds.centerY()},${it.bounds.width()}" } +
                overlayJoysticks.map { "${it.overlayControlData.control.id}=${it.bounds.centerX()},${it.bounds.centerY()},${it.bounds.width()}" } +
                listOfNotNull(menuButton?.let { "menu=${it.bounds.centerX()},${it.bounds.centerY()}" })
            Log.d("Recompia", "overlay: layout $layout ${width}x$height safe=$safe ${centres.joinToString(" ")}")
        }
        invalidate()
    }

    /** Renders a vector drawable so that its longer side is [maxSide] pixels (upstream getBitmap without tinting). */
    private fun getBitmap(drawableId: Int, maxSide: Float): Bitmap {
        val drawable = ContextCompat.getDrawable(context, drawableId)!!
        val k = maxSide / max(drawable.intrinsicWidth, drawable.intrinsicHeight)
        val bitmap = Bitmap.createBitmap(
            max(1, (drawable.intrinsicWidth * k).roundToInt()),
            max(1, (drawable.intrinsicHeight * k).roundToInt()),
            Bitmap.Config.ARGB_8888
        )
        val canvas = Canvas(bitmap)
        drawable.setBounds(0, 0, canvas.width, canvas.height)
        drawable.draw(canvas)
        return bitmap
    }

    companion object {
        private val FACE = setOf(OverlayButton.A, OverlayButton.B, OverlayButton.X, OverlayButton.Y)

        private val BUTTON_ART: Map<OverlayControl, Triple<Int, Int, OverlayButton?>> = mapOf(
            OverlayControl.BUTTON_A to Triple(R.drawable.overlay_facebutton_a, R.drawable.overlay_facebutton_a_depressed, OverlayButton.A),
            OverlayControl.BUTTON_B to Triple(R.drawable.overlay_facebutton_b, R.drawable.overlay_facebutton_b_depressed, OverlayButton.B),
            OverlayControl.BUTTON_X to Triple(R.drawable.overlay_facebutton_x, R.drawable.overlay_facebutton_x_depressed, OverlayButton.X),
            OverlayControl.BUTTON_Y to Triple(R.drawable.overlay_facebutton_y, R.drawable.overlay_facebutton_y_depressed, OverlayButton.Y),
            OverlayControl.BUTTON_PLUS to Triple(R.drawable.overlay_facebutton_plus, R.drawable.overlay_facebutton_plus_depressed, OverlayButton.PLUS),
            OverlayControl.BUTTON_MINUS to Triple(R.drawable.overlay_facebutton_minus, R.drawable.overlay_facebutton_minus_depressed, OverlayButton.MINUS),
            OverlayControl.BUTTON_L to Triple(R.drawable.overlay_l_shoulder, R.drawable.overlay_l_shoulder_depressed, OverlayButton.L),
            OverlayControl.BUTTON_R to Triple(R.drawable.overlay_r_shoulder, R.drawable.overlay_r_shoulder_depressed, OverlayButton.R),
            OverlayControl.BUTTON_ZL to Triple(R.drawable.overlay_zl_trigger, R.drawable.overlay_zl_trigger_depressed, OverlayButton.ZL),
            OverlayControl.BUTTON_ZR to Triple(R.drawable.overlay_zr_trigger, R.drawable.overlay_zr_trigger_depressed, OverlayButton.ZR),
            OverlayControl.BUTTON_STICK_L to Triple(R.drawable.overlay_button_l3, R.drawable.overlay_button_l3_depressed, OverlayButton.L3),
            OverlayControl.BUTTON_STICK_R to Triple(R.drawable.overlay_button_r3, R.drawable.overlay_button_r3_depressed, OverlayButton.R3),
            OverlayControl.MENU to Triple(R.drawable.overlay_facebutton_home, R.drawable.overlay_facebutton_home_depressed, null),
        )
    }
}

private const val TAP_MS = 250L
private const val TAP_DP = 10f
/** A finger id SDL never gets from the overlay otherwise (the overlay forwards no other fingers). */
private const val TAP_FINGER = 0x7a9
