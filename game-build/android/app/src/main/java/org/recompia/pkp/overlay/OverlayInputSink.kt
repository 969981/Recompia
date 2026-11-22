// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.overlay

/** Switch pad buttons the touch overlay can press (Switch layout: A is the right face button). */
enum class OverlayButton { A, B, X, Y, L, R, ZL, ZR, PLUS, MINUS, L3, R3, DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT }

enum class OverlayStick { LEFT, RIGHT }

/** Where the overlay's input goes. The overlay sends only changes; every pressed button gets a release. */
interface OverlayInputSink {
    fun button(button: OverlayButton, pressed: Boolean)

    /** x, y in -1..1, y down = + (Android and SDL convention). */
    fun stick(stick: OverlayStick, x: Float, y: Float)
}
