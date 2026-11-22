// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontVariation
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import androidx.compose.foundation.shape.RoundedCornerShape
import org.recompia.pkp.R

/** The PC launcher's design tokens (launcher/src/theme.css, the default "Dusk" theme), so both feel like one product. */
object Rc {
    val bg = Color(0xFF0B0F22)
    val solid = Color(0xFF1A1C36)
    val solidRaised = Color(0xFF24274A)
    val text = Color(0xFFF6F7FC)
    val text2 = Color(0xC2EEF0FB)
    val text3 = Color(0x8FEEF0FB)
    val accent = Color(0xFF7EE8C1)
    val accentPress = Color(0xFF69D9B0)
    val onAccent = Color(0xFF06231A)
    val warn = Color(0xFFFFC47E)
    val danger = Color(0xFFFF9095)
    val dangerSolid = Color(0xFFD8434C)
    val info = Color(0xFFB3C9FF)
    val focus = Color(0xFFE4FFF4)
    val tintAccent = Color(0x247EE8C1)
    val tintWarn = Color(0x24FFC47E)
    val tintDanger = Color(0x24FF9095)
    val tintInfo = Color(0x24B3C9FF)
    val surface1 = Color(0x0DFFFFFF)
    val surface2 = Color(0x17FFFFFF)
    val surface3 = Color(0x24FFFFFF)
    val sunken = Color(0x57060818)
    val line = Color(0x17FFFFFF)
    val lineStrong = Color(0x2EFFFFFF)
    val glass = Color(0x9E0D0D26)        // --glass-tint-strong over the art
    val glassLight = Color(0x6B0D0D26)   // --glass-tint
    val controlOff = Color(0x29FFFFFF)

    val radiusSm = 14.dp
    val radiusMd = 20.dp
    val radiusLg = 28.dp
    val controlLg = 56.dp
    val control = 48.dp // touch target floor
}

@OptIn(androidx.compose.ui.text.ExperimentalTextApi::class)
private fun inter(weight: Int) = Font(R.font.ui_inter, FontWeight(weight), variationSettings = FontVariation.Settings(FontVariation.weight(weight)))

val InterFamily = FontFamily(inter(400), inter(500), inter(600), inter(700), inter(800))

private fun style(size: Int, weight: Int, tracking: Double = -0.006, line: Double = 1.45) =
    TextStyle(fontFamily = InterFamily, fontWeight = FontWeight(weight), fontSize = size.sp, letterSpacing = tracking.em, lineHeight = (size * line).sp)

private val type = Typography(
    displayLarge = style(48, 800, -0.035, 1.1),
    displayMedium = style(40, 800, -0.035, 1.1),
    displaySmall = style(32, 700, -0.03, 1.15),
    headlineLarge = style(28, 700, -0.022, 1.2),
    headlineMedium = style(24, 700, -0.022, 1.2),
    headlineSmall = style(20, 650, -0.018, 1.25),
    titleLarge = style(18, 650, -0.012, 1.3),
    titleMedium = style(16, 600, -0.01),
    titleSmall = style(14, 600),
    bodyLarge = style(16, 400),
    bodyMedium = style(14, 400),
    bodySmall = style(13, 400),
    labelLarge = style(15, 600),
    labelMedium = style(13, 600),
    labelSmall = style(12, 600, 0.01),
)

private val colors = darkColorScheme(
    primary = Rc.accent, onPrimary = Rc.onAccent, primaryContainer = Rc.tintAccent, onPrimaryContainer = Rc.accent,
    secondary = Rc.info, onSecondary = Rc.bg, tertiary = Rc.warn,
    background = Rc.bg, onBackground = Rc.text, surface = Rc.solid, onSurface = Rc.text,
    surfaceVariant = Rc.solidRaised, onSurfaceVariant = Rc.text2, surfaceContainer = Rc.solid, surfaceContainerHigh = Rc.solidRaised,
    surfaceContainerHighest = Rc.solidRaised, outline = Rc.lineStrong, outlineVariant = Rc.line,
    error = Rc.danger, onError = Rc.bg, errorContainer = Rc.tintDanger,
)

@Composable
fun RecompiaTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = colors,
        typography = type,
        shapes = Shapes(small = RoundedCornerShape(Rc.radiusSm), medium = RoundedCornerShape(Rc.radiusMd), large = RoundedCornerShape(Rc.radiusLg)),
        content = content,
    )
}
