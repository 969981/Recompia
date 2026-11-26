// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.annotation.DrawableRes
import android.content.Context
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.BiasAlignment
import androidx.compose.ui.platform.LocalContext
import kotlinx.coroutines.delay
import java.time.LocalTime
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

/**
 * One of the PC launcher's three looks (launcher/src/theme.css: Dusk is :root, Sunrise and Meadow its [data-theme] blocks). All three are
 * dark: they share the text and line colours and differ in these, the art and the scrim's hue.
 */
class Palette(
    val bg: Color, val solid: Color, val solidRaised: Color,
    val accent: Color, val accentPress: Color, val onAccent: Color, val warn: Color, val danger: Color, val info: Color, val focus: Color,
    val tintAccent: Color, val tintWarn: Color, val tintDanger: Color, val tintInfo: Color,
    val sunken: Color, val glass: Color, val glassLight: Color,
    val scrim: Color,   // the hero scrim's colour (--scrim-hero), alpha 1: ArtBackground sets the alphas
    val scrimMid: Float = 0.4f,   // the sharp scrim's alpha halfway down (Meadow's bright sky needs more for the pills over it)
    @DrawableRes val art: Int, @DrawableRes val artSoft: Int, val artAlign: BiasAlignment,   // --art-position x% y% = bias 2x-1, 2y-1
)

object Palettes {
    val dusk = Palette(
        bg = Color(0xFF0B0F22), solid = Color(0xFF1A1C36), solidRaised = Color(0xFF24274A),
        accent = Color(0xFF7EE8C1), accentPress = Color(0xFF69D9B0), onAccent = Color(0xFF06231A), warn = Color(0xFFFFC47E),
        danger = Color(0xFFFF9095), info = Color(0xFFB3C9FF), focus = Color(0xFFE4FFF4),
        tintAccent = Color(0x247EE8C1), tintWarn = Color(0x24FFC47E), tintDanger = Color(0x24FF9095), tintInfo = Color(0x24B3C9FF),
        sunken = Color(0x57060818), glass = Color(0x9E0D0D26), glassLight = Color(0x6B0D0D26), scrim = Color(0xFF070A1E),
        art = R.drawable.ui_art_island, artSoft = R.drawable.ui_art_island_soft, artAlign = BiasAlignment(0.24f, 0.16f),
    )
    val sunrise = Palette(
        bg = Color(0xFF1C1006), solid = Color(0xFF2A190C), solidRaised = Color(0xFF3A2414),
        accent = Color(0xFFFFC23D), accentPress = Color(0xFFF0AE1F), onAccent = Color(0xFF2A1700), warn = Color(0xFFFFB35C),
        danger = Color(0xFFFF7A6B), info = Color(0xFF9FD8FF), focus = Color(0xFFFFF1C9),
        tintAccent = Color(0x29FFC23D), tintWarn = Color(0x29FFB35C), tintDanger = Color(0x29FF7A6B), tintInfo = Color(0x249FD8FF),
        sunken = Color(0x5C180C04), glass = Color(0x9E241206), glassLight = Color(0x70241206), scrim = Color(0xFF1A0C04),
        art = R.drawable.ui_art_island_sunrise, artSoft = R.drawable.ui_art_island_sunrise_soft, artAlign = BiasAlignment(0.24f, 0.16f),
    )
    // Meadow keeps Dusk's accent and status colours; its glass is a little darker (a bright picture, text keeps 4.5:1)
    val meadow = Palette(
        bg = Color(0xFF08201F), solid = Color(0xFF10302C), solidRaised = Color(0xFF1A423D),
        accent = dusk.accent, accentPress = dusk.accentPress, onAccent = dusk.onAccent, warn = dusk.warn,
        danger = dusk.danger, info = dusk.info, focus = dusk.focus,
        tintAccent = dusk.tintAccent, tintWarn = dusk.tintWarn, tintDanger = dusk.tintDanger, tintInfo = dusk.tintInfo,
        sunken = Color(0x66041614), glass = Color(0xAD061E1C), glassLight = Color(0x80061E1C), scrim = Color(0xFF041614), scrimMid = 0.6f,
        art = R.drawable.ui_art_meadow, artSoft = R.drawable.ui_art_meadow_soft, artAlign = BiasAlignment(0f, 0.12f),
    )

    const val AUTO = "auto"
    private val byId = mapOf("sunrise" to sunrise, "meadow" to meadow, "dusk" to dusk)

    /** Automatic: Sunrise 04:00-09:59, Meadow 10:00-17:59, Dusk otherwise. */
    fun forHour(hour: Int) = when (hour) { in 4..9 -> sunrise; in 10..17 -> meadow; else -> dusk }

    /** The palette for a saved choice ("auto", "sunrise", "meadow", "dusk"; anything else is Automatic). */
    fun pick(choice: String, hour: Int = LocalTime.now().hour) = byId[choice] ?: forHour(hour)

    private fun prefs(ctx: Context) = ctx.getSharedPreferences("launcher", Context.MODE_PRIVATE)
    fun choice(ctx: Context): String = prefs(ctx).getString("theme", AUTO) ?: AUTO
    fun setChoice(ctx: Context, v: String) { prefs(ctx).edit().putString("theme", v).apply(); Rc.palette = pick(v) }
}

/** The launcher's design tokens (launcher/src/theme.css), so both feel like one product; the themed ones follow [palette]. */
object Rc {
    var palette by mutableStateOf(Palettes.dusk)
    val bg get() = palette.bg
    val solid get() = palette.solid
    val solidRaised get() = palette.solidRaised
    val text = Color(0xFFF6F7FC)
    val text2 = Color(0xC2EEF0FB)
    val text3 = Color(0x8FEEF0FB)
    val accent get() = palette.accent
    val accentPress get() = palette.accentPress
    val onAccent get() = palette.onAccent
    val warn get() = palette.warn
    val danger get() = palette.danger
    val dangerSolid = Color(0xFFD8434C)
    val info get() = palette.info
    val focus get() = palette.focus
    val tintAccent get() = palette.tintAccent
    val tintWarn get() = palette.tintWarn
    val tintDanger get() = palette.tintDanger
    val tintInfo get() = palette.tintInfo
    val surface1 = Color(0x0DFFFFFF)
    val surface2 = Color(0x17FFFFFF)
    val surface3 = Color(0x24FFFFFF)
    val sunken get() = palette.sunken
    val line = Color(0x17FFFFFF)
    val lineStrong = Color(0x2EFFFFFF)
    val glass get() = palette.glass
    val glassLight get() = palette.glassLight
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

private fun colors() = darkColorScheme(
    primary = Rc.accent, onPrimary = Rc.onAccent, primaryContainer = Rc.tintAccent, onPrimaryContainer = Rc.accent,
    secondary = Rc.info, onSecondary = Rc.bg, tertiary = Rc.warn,
    background = Rc.bg, onBackground = Rc.text, surface = Rc.solid, onSurface = Rc.text,
    surfaceVariant = Rc.solidRaised, onSurfaceVariant = Rc.text2, surfaceContainer = Rc.solid, surfaceContainerHigh = Rc.solidRaised,
    surfaceContainerHighest = Rc.solidRaised, outline = Rc.lineStrong, outlineVariant = Rc.line,
    error = Rc.danger, onError = Rc.bg, errorContainer = Rc.tintDanger,
)

@Composable
fun RecompiaTheme(content: @Composable () -> Unit) {
    val ctx = LocalContext.current
    remember { Rc.palette = Palettes.pick(Palettes.choice(ctx)) }   // before the first frame: no flash of Dusk
    // follow the saved choice; Automatic re-checks the hour every minute while the launcher is open
    LaunchedEffect(Unit) { while (true) { Rc.palette = Palettes.pick(Palettes.choice(ctx)); delay(60_000) } }
    MaterialTheme(
        colorScheme = remember(Rc.palette) { colors() },
        typography = type,
        shapes = Shapes(small = RoundedCornerShape(Rc.radiusSm), medium = RoundedCornerShape(Rc.radiusMd), large = RoundedCornerShape(Rc.radiusLg)),
        content = content,
    )
}
