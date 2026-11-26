// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.focusable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ArrowBack
import androidx.compose.material.icons.rounded.Add
import androidx.compose.material.icons.rounded.Remove
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import org.recompia.pkp.R

/** True while the player navigates with keys or a gamepad: focus rings show only then, not under a finger. */
val LocalKeyNav = compositionLocalOf<MutableState<Boolean>> { mutableStateOf(false) }

/** Wide layouts: the unfolded inner screen and landscape get two panes. */
@Composable
fun isWide(): Boolean = LocalConfiguration.current.screenWidthDp >= 600

/** A clear ring around the focused control when navigating with a gamepad (the launcher's --color-focus). */
fun Modifier.focusRing(shape: Shape = RoundedCornerShape(Rc.radiusSm)): Modifier = composed {
    val keyNav by LocalKeyNav.current
    var focused by remember { mutableStateOf(false) }
    val ring by animateColorAsState(if (focused && keyNav) Rc.focus else Color.Transparent, label = "ring")
    this.onFocusChanged { focused = it.isFocused }.border(BorderStroke(3.dp, ring), shape)
}

/** A block of text a gamepad can step onto, so a long page scrolls with the D-pad (focus brings it into view). */
fun Modifier.readable(): Modifier = this.focusRing(RoundedCornerShape(Rc.radiusLg)).focusable()

/** Takes the focus when the screen appears, so a gamepad starts on the next thing to press. */
fun Modifier.initialFocus(key: Any? = Unit): Modifier = composed {
    val fr = remember { FocusRequester() }
    LaunchedEffect(key) { runCatching { fr.requestFocus() } }
    this.focusRequester(fr)
}

enum class Kind { PRIMARY, SECONDARY, QUIET, DANGER }

@Composable
fun RcButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    icon: ImageVector? = null,
    kind: Kind = Kind.SECONDARY,
    enabled: Boolean = true,
    big: Boolean = false,
) {
    val colors = when (kind) {
        Kind.PRIMARY -> ButtonDefaults.buttonColors(containerColor = Rc.accent, contentColor = Rc.onAccent, disabledContainerColor = Rc.surface2, disabledContentColor = Rc.text3)
        Kind.SECONDARY -> ButtonDefaults.buttonColors(containerColor = Rc.surface2, contentColor = Rc.text, disabledContainerColor = Rc.surface1, disabledContentColor = Rc.text3)
        Kind.QUIET -> ButtonDefaults.buttonColors(containerColor = Color.Transparent, contentColor = Rc.text2, disabledContainerColor = Color.Transparent, disabledContentColor = Rc.text3)
        Kind.DANGER -> ButtonDefaults.buttonColors(containerColor = Rc.dangerSolid, contentColor = Color.White, disabledContainerColor = Rc.surface1, disabledContentColor = Rc.text3)
    }
    val shape = RoundedCornerShape(if (big) Rc.radiusMd else Rc.radiusSm)
    Button(
        onClick = onClick,
        enabled = enabled,
        colors = colors,
        shape = shape,
        border = if (kind == Kind.SECONDARY) BorderStroke(1.dp, Rc.line) else null,
        contentPadding = PaddingValues(horizontal = if (big) 28.dp else 18.dp, vertical = if (big) 16.dp else 10.dp),
        modifier = modifier.heightIn(min = if (big) Rc.controlLg + 8.dp else Rc.control).focusRing(shape),
    ) {
        if (icon != null) {
            Icon(icon, contentDescription = null, modifier = Modifier.size(if (big) 24.dp else 20.dp))
            Spacer(Modifier.size(10.dp))
        }
        Text(text, style = if (big) MaterialTheme.typography.titleLarge else MaterialTheme.typography.labelLarge, maxLines = 2, overflow = TextOverflow.Ellipsis)
    }
}

/** The launcher's glass panel over the art. */
@Composable
fun GlassCard(modifier: Modifier = Modifier, strong: Boolean = true, padding: Dp = 24.dp, content: @Composable ColumnScope.() -> Unit) {
    Column(
        modifier
            .clip(RoundedCornerShape(Rc.radiusLg))
            .background(if (strong) Rc.glass else Rc.glassLight)
            .border(1.dp, Rc.line, RoundedCornerShape(Rc.radiusLg))
            .padding(padding),
        verticalArrangement = Arrangement.spacedBy(16.dp),
        content = content,
    )
}

/** The theme's art behind every screen (Rc.palette): sharp with the hero scrim on Home and the wizard, soft (blurred) elsewhere. */
@Composable
fun ArtBackground(sharp: Boolean, content: @Composable BoxScope.() -> Unit) {
    Box(Modifier.fillMaxSize().background(Rc.bg)) {
        val p = Rc.palette
        Image(
            painterResource(if (sharp) p.art else p.artSoft),
            contentDescription = null,
            contentScale = ContentScale.Crop,
            alignment = p.artAlign,
            modifier = Modifier.fillMaxSize(),
        )
        Box(Modifier.fillMaxSize().background(
            if (sharp) Brush.verticalGradient(0f to p.scrim.copy(alpha = 0.16f), 0.5f to p.scrim.copy(alpha = p.scrimMid), 1f to p.scrim.copy(alpha = 0.9f))
            else Brush.verticalGradient(0f to p.scrim.copy(alpha = 0.6f), 1f to p.scrim.copy(alpha = 0.85f))
        ))
        content()
    }
}

@Composable
fun Pill(text: String, color: Color, tint: Color, icon: ImageVector? = null) {
    Row(
        Modifier.clip(CircleShape).background(tint).padding(horizontal = 12.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (icon != null) { Icon(icon, null, tint = color, modifier = Modifier.size(16.dp)); Spacer(Modifier.size(6.dp)) }
        Text(text, color = color, style = MaterialTheme.typography.labelMedium)
    }
}

/** A coloured note box (the launcher's .banner). */
@Composable
fun Banner(text: String, color: Color, tint: Color, icon: ImageVector, modifier: Modifier = Modifier, mono: Boolean = false) {
    Row(
        modifier.fillMaxWidth().clip(RoundedCornerShape(Rc.radiusSm)).background(tint).padding(14.dp),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Icon(icon, null, tint = color, modifier = Modifier.size(22.dp))
        Text(text, color = Rc.text, style = if (mono) MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace) else MaterialTheme.typography.bodyMedium)
    }
}

@Composable
fun Heading(text: String, modifier: Modifier = Modifier, big: Boolean = false) {
    Text(text, style = if (big) MaterialTheme.typography.displaySmall else MaterialTheme.typography.headlineMedium,
        color = Rc.text, modifier = modifier.semantics { heading() })
}

@Composable
fun SectionTitle(text: String) {
    Text(text.uppercase(), style = MaterialTheme.typography.labelSmall, color = Rc.text3,
        modifier = Modifier.padding(top = 8.dp).semantics { heading() })
}

@Composable
fun Body(text: String, color: Color = Rc.text2, modifier: Modifier = Modifier) {
    Text(text, style = MaterialTheme.typography.bodyLarge, color = color, modifier = modifier)
}

@Composable
fun Mono(text: String, modifier: Modifier = Modifier) {
    Text(shown(text), style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace), color = Rc.text3, modifier = modifier)
}

/** The progress bar with percent and time left, as on the launcher's setup page. */
@Composable
fun Meter(percent: Int?, label: String, eta: String, modifier: Modifier = Modifier) {
    Column(modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Row(verticalAlignment = Alignment.Bottom) {
            Text(percent?.let { "$it%" } ?: label, style = MaterialTheme.typography.headlineLarge, color = Rc.text)
            Spacer(Modifier.weight(1f))
            Text(eta, style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
        }
        val shape = RoundedCornerShape(50)
        if (percent == null) LinearProgressIndicator(Modifier.fillMaxWidth().height(10.dp).clip(shape), color = Rc.accent, trackColor = Rc.surface2)
        else {
            val p by animateFloatAsState(percent / 100f, label = "p")
            LinearProgressIndicator({ p }, Modifier.fillMaxWidth().height(10.dp).clip(shape).semantics { contentDescription = label },
                color = Rc.accent, trackColor = Rc.surface2, gapSize = 0.dp, drawStopIndicator = {})
        }
    }
}

/** A setting with a switch: the whole row is one target (gamepad A toggles it). */
@Composable
fun SwitchRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit, modifier: Modifier = Modifier, sub: String? = null, enabled: Boolean = true,
              overline: String? = null, icon: androidx.compose.ui.graphics.vector.ImageVector? = null) {
    val shape = RoundedCornerShape(Rc.radiusSm)
    Row(
        modifier.fillMaxWidth().heightIn(min = 56.dp).clip(shape)
            .focusRing(shape)
            .toggleable(checked, enabled = enabled, role = Role.Switch, onValueChange = onChange)
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (icon != null) { Icon(icon, null, tint = Rc.accent, modifier = Modifier.size(28.dp)); Spacer(Modifier.size(14.dp)) }
        Column(Modifier.weight(1f)) {
            if (overline != null) Text(overline, style = MaterialTheme.typography.labelMedium, color = Rc.text3)
            Text(label, style = MaterialTheme.typography.titleMedium, color = if (enabled) Rc.text else Rc.text3)
            if (sub != null) Text(sub, style = MaterialTheme.typography.bodySmall, color = Rc.text3)
        }
        Spacer(Modifier.size(12.dp))
        Switch(checked, onCheckedChange = null, enabled = enabled, colors = SwitchDefaults.colors(
            checkedTrackColor = Rc.accent, checkedThumbColor = Rc.onAccent, uncheckedTrackColor = Rc.controlOff,
            uncheckedThumbColor = Color.White, uncheckedBorderColor = Color.Transparent))
    }
}

/** A choice as a row of chips (the launcher's segmented control); wraps on narrow screens. */
@Composable
fun ChoiceRow(label: String, options: List<Pair<String, String>>, selected: String, onSelect: (String) -> Unit, modifier: Modifier = Modifier) {
    if (options.size > 8) return StepperRow(label, options, selected, onSelect, modifier)
    Column(modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Text(label, style = MaterialTheme.typography.titleMedium, color = Rc.text)
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            for ((value, text) in options) {
                val on = value == selected
                val shape = RoundedCornerShape(50)
                Box(
                    Modifier.heightIn(min = 44.dp).widthIn(min = 56.dp).clip(shape)
                        .background(if (on) Rc.accent else Rc.surface2)
                        .focusRing(shape)
                        .selectable(on, role = Role.RadioButton) { onSelect(value) }
                        .padding(horizontal = 16.dp, vertical = 10.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(text, style = MaterialTheme.typography.labelLarge, color = if (on) Rc.onAccent else Rc.text)
                }
            }
        }
    }
}

/** A tappable list row with icon, title, detail and an optional trailing text. */
@Composable
fun NavRow(icon: ImageVector, title: String, onClick: () -> Unit, modifier: Modifier = Modifier, detail: String? = null, trailing: String? = null,
           background: Color = Rc.surface1) {
    val shape = RoundedCornerShape(Rc.radiusMd)
    Row(
        modifier.fillMaxWidth().heightIn(min = 64.dp).clip(shape).background(background)
            .focusRing(shape).clickable(role = Role.Button, onClick = onClick).padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        Box(Modifier.size(40.dp).clip(RoundedCornerShape(12.dp)).background(Rc.tintAccent), contentAlignment = Alignment.Center) {
            Icon(icon, null, tint = Rc.accent, modifier = Modifier.size(22.dp))
        }
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.titleMedium, color = Rc.text)
            if (detail != null) Text(detail, style = MaterialTheme.typography.bodySmall, color = Rc.text3, maxLines = 2, overflow = TextOverflow.Ellipsis)
        }
        if (trailing != null) Text(trailing, style = MaterialTheme.typography.labelLarge, color = Rc.text2)
    }
}

/** A sub-page: back button, title, scrolling content inside the safe area; two columns on wide screens when [side] is given. */
@Composable
fun Page(
    title: String,
    onBack: () -> Unit,
    side: (@Composable ColumnScope.() -> Unit)? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    ArtBackground(sharp = false) {
        Column(Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.safeDrawing)) {
            Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                IconButton(onClick = onBack, modifier = Modifier.size(56.dp).focusRing(CircleShape)) {
                    Icon(Icons.AutoMirrored.Rounded.ArrowBack, contentDescription = stringResource(R.string.ui_back), tint = Rc.text)
                }
                Spacer(Modifier.size(4.dp))
                Heading(title)
            }
            val wide = isWide()
            if (wide && side != null) {
                Row(Modifier.fillMaxSize().padding(horizontal = 24.dp), horizontalArrangement = Arrangement.spacedBy(24.dp)) {
                    Column(Modifier.weight(0.42f).verticalScroll(rememberScrollState()).padding(bottom = 24.dp), verticalArrangement = Arrangement.spacedBy(16.dp), content = side)
                    Column(Modifier.weight(0.58f).verticalScroll(rememberScrollState()).padding(bottom = 24.dp), verticalArrangement = Arrangement.spacedBy(16.dp), content = content)
                }
            } else {
                Column(
                    Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 16.dp).padding(bottom = 24.dp)
                        .let { if (wide) it.widthIn(max = 760.dp).align(Alignment.CenterHorizontally) else it },
                    verticalArrangement = Arrangement.spacedBy(16.dp),
                ) {
                    if (side != null) side()
                    content()
                }
            }
        }
    }
}

@Composable
fun ProvideKeyNav(state: MutableState<Boolean>, content: @Composable () -> Unit) =
    CompositionLocalProvider(LocalKeyNav provides state, content = content)

/** A long list of numbers as "-  value  +" (a 26-chip grid is a wall on a phone). */
@Composable
private fun StepperRow(label: String, options: List<Pair<String, String>>, selected: String, onSelect: (String) -> Unit, modifier: Modifier) {
    val i = options.indexOfFirst { it.first == selected }.coerceAtLeast(0)
    Row(modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(label, style = MaterialTheme.typography.titleMedium, color = Rc.text, modifier = Modifier.weight(1f))
        val shape = CircleShape
        IconButton({ onSelect(options[i - 1].first) }, enabled = i > 0, modifier = Modifier.size(48.dp).clip(shape).background(Rc.surface2).focusRing(shape)) {
            Icon(androidx.compose.material.icons.Icons.Rounded.Remove, stringResource(R.string.ui_less, label), tint = Rc.text)
        }
        Text(options[i].second, style = MaterialTheme.typography.titleLarge, color = Rc.text, modifier = Modifier.widthIn(min = 64.dp).padding(horizontal = 8.dp),
            textAlign = androidx.compose.ui.text.style.TextAlign.Center)
        IconButton({ onSelect(options[i + 1].first) }, enabled = i < options.size - 1, modifier = Modifier.size(48.dp).clip(shape).background(Rc.surface2).focusRing(shape)) {
            Icon(androidx.compose.material.icons.Icons.Rounded.Add, stringResource(R.string.ui_more, label), tint = Rc.text)
        }
    }
}

/** Recompia's folder in the system picker (primary storage), so a picker opens there and not in a random place. */
val DATA_ROOT_DOC: android.net.Uri get() = android.provider.DocumentsContract.buildDocumentUri(
    "com.android.externalstorage.documents", "primary:" + org.recompia.pkp.Paths.dataRoot.name)

/** OpenDocument that starts in [DATA_ROOT_DOC]. */
class OpenDocumentInData : androidx.activity.result.contract.ActivityResultContracts.OpenDocument() {
    override fun createIntent(context: android.content.Context, input: Array<String>): android.content.Intent =
        super.createIntent(context, input).putExtra(android.provider.DocumentsContract.EXTRA_INITIAL_URI, DATA_ROOT_DOC)
}
