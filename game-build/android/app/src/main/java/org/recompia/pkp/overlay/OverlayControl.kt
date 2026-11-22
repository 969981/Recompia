// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted for Recompia from Eden's overlay/model/OverlayControl.kt, OverlayControlData.kt and OverlayLayout.kt:
// default positions as corner anchors instead of integer resources per layout, no settings classes.

package org.recompia.pkp.overlay

/** The two screens of a foldable in landscape, each with its own saved layout: the narrow cover screen (very wide) and the inner one (nearly square). */
enum class OverlayLayout(val prefKey: String) {
    Cover(OverlayPrefs.LAYOUT_COVER),
    Inner(OverlayPrefs.LAYOUT_INNER);

    companion object {
        /** Wider than 1.6:1 counts as the cover screen (a phone in landscape); the inner screen is about 1.16:1. */
        fun forSize(width: Int, height: Int): OverlayLayout =
            if (height > 0 && width.toFloat() / height >= 1.6f) Cover else Inner
    }
}

/** Which edge of the safe area a default position is measured from. */
enum class Edge { Start, Center, End }

/**
 * Every control: its size (fraction of the base dimension, as upstream) and its default centre, measured from an edge
 * of the safe area in base units (the base is the screen's short side, capped at 400 dp: InputOverlay.refreshControls).
 * Everything but the menu hangs from the bottom corners, so the same preset fits the very wide cover screen (where it
 * spans the full height) and the nearly square inner screen (lower corners, within thumb reach, middle left free).
 */
enum class OverlayControl(
    val id: String,
    val size: Float,
    val h: Edge,
    val dx: Double,
    val fromTop: Boolean,
    val dy: Double,
) {
    BUTTON_A("button_a", 0.11f, Edge.End, 0.26, false, 0.34),
    BUTTON_B("button_b", 0.11f, Edge.End, 0.365, false, 0.24),
    BUTTON_X("button_x", 0.11f, Edge.End, 0.365, false, 0.44),
    BUTTON_Y("button_y", 0.11f, Edge.End, 0.47, false, 0.34),
    BUTTON_PLUS("button_plus", 0.07f, Edge.Center, 0.12, false, 0.07),
    BUTTON_MINUS("button_minus", 0.07f, Edge.Center, -0.12, false, 0.07),
    BUTTON_L("button_l", 0.26f, Edge.Start, 0.18, false, 0.78),
    BUTTON_R("button_r", 0.26f, Edge.End, 0.18, false, 0.78),
    BUTTON_ZL("button_zl", 0.26f, Edge.Start, 0.18, false, 0.92),
    BUTTON_ZR("button_zr", 0.26f, Edge.End, 0.18, false, 0.92),
    BUTTON_STICK_L("button_stick_l", 0.155f, Edge.Start, 0.75, false, 0.50),
    BUTTON_STICK_R("button_stick_r", 0.155f, Edge.End, 0.75, false, 0.50),
    STICK_L("stick_l", 0.3f, Edge.Start, 0.31, false, 0.45),
    STICK_R("stick_r", 0.3f, Edge.End, 0.85, false, 0.24),
    COMBINED_DPAD("combined_dpad", 0.25f, Edge.Start, 0.70, false, 0.20),
    MENU("menu", 0.07f, Edge.Center, 0.0, true, 0.06);

    /** The default centre as fractions of a safe area of [w] x [h] pixels, with [base] pixels per base unit. */
    fun defaultPosition(w: Int, h: Int, base: Float): Pair<Double, Double> {
        val px = when (this.h) {
            Edge.Start -> dx * base
            Edge.Center -> w / 2.0 + dx * base
            Edge.End -> w - dx * base
        }
        val py = if (fromTop) dy * base else h - dy * base
        return (px / w).coerceIn(0.0, 1.0) to (py / h).coerceIn(0.0, 1.0)
    }

    companion object {
        fun from(id: String): OverlayControl? = entries.firstOrNull { it.id == id }
    }
}

/** Where one control sits in one layout: centre as fractions of the safe area, and its own size factor. */
data class OverlayControlData(val control: OverlayControl, var x: Double, var y: Double, var individualScale: Float)
