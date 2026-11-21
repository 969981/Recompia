// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import android.content.Context
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.RestartAlt
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.unit.dp
import org.recompia.pkp.Paths
import org.recompia.pkp.R
import org.recompia.pkp.overlay.OverlayPrefs
import java.util.concurrent.Executors

private val writer = Executors.newSingleThreadExecutor()

/** Writes one ini key off the main thread, in order (the Ini line editor keeps every other line of the file). */
fun writeSetting(row: SettingsModel.Row, value: String) = writer.execute { runCatching { SettingsModel.write(row, value) } }

@Composable
fun SettingsScreen(onBack: () -> Unit) {
    val ctx = LocalContext.current
    val groups = remember { SettingsModel.groups(ctx) }
    var values by remember { mutableStateOf(runCatching { SettingsModel.read(groups) }.getOrDefault(emptyMap())) }
    val set = { row: SettingsModel.Row, v: String -> writeSetting(row, v); values = values + (row.id to v) }
    val readable = values.isNotEmpty()
    Page(
        stringResource(R.string.ui_settings), onBack,
        side = {
            if (!readable) Banner(stringResource(R.string.ui_set_unreadable), Rc.warn, Rc.tintWarn, Icons.Rounded.RestartAlt)
            groups.take(2).forEach { GroupCard(it, values, set) }
            TouchCard()
            CameraCard()
            LinkPlayCard()
            GpuDriverCard()
        },
    ) {
        groups.drop(2).forEach { GroupCard(it, values, set) }
    }
}

@Composable
private fun GroupCard(g: SettingsModel.Group, values: Map<String, String>, set: (SettingsModel.Row, String) -> Unit) {
    GlassCard(Modifier.fillMaxWidth(), padding = 12.dp) {
        SectionTitle(stringResource(g.title))
        if (g.note != 0) Text(stringResource(g.note), style = MaterialTheme.typography.bodySmall, color = Rc.text3, modifier = Modifier.padding(horizontal = 12.dp))
        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            for (r in g.rows) {
                val parent = when (r) { is SettingsModel.Toggle -> r.parent; is SettingsModel.Choice -> r.parent }
                if (parent != null && values[parent] != "1") continue
                val indent = if (parent != null) Modifier.padding(start = 20.dp) else Modifier
                when (r) {
                    is SettingsModel.Toggle -> {
                        SwitchRow(if (r.label != 0) stringResource(r.label) else r.text, values[r.id] == "1", { set(r, if (it) "1" else "0") }, indent, enabled = values.isNotEmpty())
                        if (r.note != 0 && values[r.id] == "1") Text(stringResource(r.note), style = MaterialTheme.typography.bodySmall, color = Rc.text3, modifier = indent.padding(horizontal = 12.dp))
                    }
                    is SettingsModel.Choice -> {
                        ChoiceRow(if (r.label != 0) stringResource(r.label) else r.text,
                            choiceOptions(r, values[r.id] ?: r.def), values[r.id] ?: r.def, { set(r, it) }, indent)
                        if (r.note != 0) Text(stringResource(r.note), style = MaterialTheme.typography.bodySmall, color = Rc.text3, modifier = indent.padding(horizontal = 12.dp))
                    }
                }
            }
        }
    }
}

/**
 * The touch overlay's options (OverlayPrefs). The game runs in its own process and re-reads this file (MODE_MULTI_PROCESS)
 * each time it resumes, so changes apply when the player returns to the game.
 */
@Composable
private fun TouchCard() {
    val ctx = LocalContext.current
    val p = remember { OverlayPrefs.of(ctx) }
    var enabled by remember { mutableStateOf(OverlayPrefs.enabled(p)) }
    var opacity by remember { mutableIntStateOf(OverlayPrefs.opacity(p)) }
    var scale by remember { mutableIntStateOf(OverlayPrefs.scale(p)) }
    var haptics by remember { mutableStateOf(OverlayPrefs.haptics(p)) }
    var autohide by remember { mutableStateOf(OverlayPrefs.autohide(p)) }
    var reset by remember { mutableStateOf(false) }
    GlassCard(Modifier.fillMaxWidth(), padding = 12.dp) {
        SectionTitle(stringResource(R.string.ui_set_touch_group))
        SwitchRow(stringResource(R.string.ui_set_touch), enabled, { enabled = it; p.edit().putBoolean(OverlayPrefs.ENABLED, it).apply() })
        SliderRow(stringResource(R.string.ui_set_opacity), opacity, 0, 100, enabled) { opacity = it; p.edit().putInt(OverlayPrefs.OPACITY, it).apply() }
        SliderRow(stringResource(R.string.ui_set_size), scale, 50, 150, enabled) { scale = it; p.edit().putInt(OverlayPrefs.SCALE, it).apply() }
        SwitchRow(stringResource(R.string.ui_set_haptics), haptics, { haptics = it; p.edit().putBoolean(OverlayPrefs.HAPTICS, it).apply() }, enabled = enabled)
        SwitchRow(stringResource(R.string.ui_set_autohide), autohide, { autohide = it; p.edit().putBoolean(OverlayPrefs.AUTOHIDE, it).apply() }, enabled = enabled)
        RcButton(stringResource(if (reset) R.string.ui_set_layout_reset_done else R.string.ui_set_layout_reset), {
            OverlayPrefs.layouts(ctx).edit().clear().apply() // the edited layouts live in their own file
            reset = true
        }, Modifier.padding(horizontal = 12.dp), icon = Icons.Rounded.RestartAlt, enabled = !reset)
        Text(stringResource(R.string.ui_set_layout_note), style = MaterialTheme.typography.bodySmall, color = Rc.text3, modifier = Modifier.padding(horizontal = 12.dp))
    }
}

/** Turning the camera by dragging a finger on free screen space (CameraDrag). */
@Composable
private fun CameraCard() {
    val ctx = LocalContext.current
    val p = remember { OverlayPrefs.of(ctx) }
    var drag by remember { mutableStateOf(OverlayPrefs.cameraDrag(p)) }
    var speed by remember { mutableIntStateOf(OverlayPrefs.cameraSensitivity(p)) }
    var invert by remember { mutableStateOf(p.getBoolean(OverlayPrefs.CAMERA_INVERT_Y, false)) }
    var invertX by remember { mutableStateOf(OverlayPrefs.cameraInvertX(p)) }
    GlassCard(Modifier.fillMaxWidth(), padding = 12.dp) {
        SectionTitle(stringResource(R.string.ui_set_camera_group))
        SwitchRow(stringResource(R.string.ui_set_camera_drag), drag, { drag = it; p.edit().putBoolean(OverlayPrefs.CAMERA_DRAG, it).apply() })
        SliderRow(stringResource(R.string.ui_set_camera_speed), speed, 10, OverlayPrefs.CAMERA_SENSITIVITY_MAX, drag) { speed = it; p.edit().putInt(OverlayPrefs.CAMERA_SENSITIVITY, it).apply() }
        SwitchRow(stringResource(R.string.ui_set_camera_invert_x), invertX, { invertX = it; p.edit().putBoolean(OverlayPrefs.CAMERA_INVERT_X, it).apply() }, enabled = drag)
        SwitchRow(stringResource(R.string.ui_set_camera_invert), invert, { invert = it; p.edit().putBoolean(OverlayPrefs.CAMERA_INVERT_Y, it).apply() }, enabled = drag)
        Text(stringResource(R.string.ui_set_overlay_applies), style = MaterialTheme.typography.bodySmall, color = Rc.text3, modifier = Modifier.padding(horizontal = 12.dp))
    }
}

/**
 * Link Play ([net] in pkp.ini, read at the game's next start), the launcher's Link Play page in short (launcher/src/linkplay-model.js):
 * Off = mode off; Same network = internet false, mode auto (host, or find a host) or join:<IPv4> (Join directly); Internet = internet true,
 * mode auto, relay (the public relay unless another is typed). A phone finds no LAN host by itself (the co-op module has no DNS-SD or LAN
 * sweep on Android: coop_disc.c), so a visitor on the same network types the host's address; a host needs nothing.
 */
@Composable
private fun LinkPlayCard() {
    val ini = remember { Ini.load(Paths.pkpIni) }
    val mode0 = ini.get("net", "mode")?.trim()?.lowercase() ?: "auto"
    var choice by remember { mutableStateOf(when {
        mode0 == "off" || mode0 == "0" || mode0.isEmpty() -> "off"
        SettingsModel.truthy(ini.get("net", "internet")) -> "internet"
        else -> "lan"
    }) }
    var host by remember { mutableStateOf(if (mode0.startsWith("join:")) mode0.removePrefix("join:") else "") }
    var relay by remember { mutableStateOf(ini.get("net", "relay")?.trim().orEmpty().ifEmpty { PUBLIC_RELAY }) }
    val save = {
        val c = choice; val h = host.trim(); val r = relay.trim()
        writer.execute { runCatching {
            val f = Ini.load(Paths.pkpIni)
            f.set("net", "protocol", "native")
            when (c) {
                "off" -> f.set("net", "mode", "off")
                "lan" -> { f.set("net", "internet", "false"); f.set("net", "mode", if (validAddr(h)) "join:$h" else "auto") }
                else -> { f.set("net", "internet", "true"); f.set("net", "mode", "auto"); f.set("net", "relay", if (validAddr(r)) r else PUBLIC_RELAY) }
            }
            f.save(Paths.pkpIni)
        } }
    }
    GlassCard(Modifier.fillMaxWidth(), padding = 12.dp) {
        SectionTitle(stringResource(R.string.ui_set_linkplay_group))
        ChoiceRow(stringResource(R.string.ui_set_linkplay), listOf("off" to stringResource(R.string.ui_off),
            "lan" to stringResource(R.string.ui_set_linkplay_lan), "internet" to stringResource(R.string.ui_set_linkplay_internet)),
            choice, { choice = it; save() })
        if (choice != "off") {
            val lan = choice == "lan"
            val value = if (lan) host else relay
            OutlinedTextField(value, { if (lan) host = it else relay = it; save() },
                label = { Text(stringResource(if (lan) R.string.ui_set_linkplay_host else R.string.ui_set_linkplay_relay)) },
                singleLine = true, isError = value.isNotBlank() && !validAddr(value),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp))
            Text(stringResource(if (lan) R.string.ui_set_linkplay_lan_note else R.string.ui_set_linkplay_internet_note),
                style = MaterialTheme.typography.bodySmall, color = Rc.text3, modifier = Modifier.padding(horizontal = 12.dp))
        }
        Text(stringResource(R.string.ui_set_linkplay_applies), style = MaterialTheme.typography.bodySmall, color = Rc.text3, modifier = Modifier.padding(horizontal = 12.dp))
    }
}

/** The launcher's public relay (launcher/src/linkplay-model.js PUBLIC_RELAY). */
private const val PUBLIC_RELAY = "204.216.111.91:47610"

/** IPv4[:port] as the game's [net] takes it (coop_cfg_parse_ex: no host names; not 0.x, not multicast or above, port 1..65535). */
internal fun validAddr(s: String): Boolean {
    val m = Regex("""^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?::(\d{1,5}))?$""").matchEntire(s.trim()) ?: return false
    val ip = (1..4).map { m.groupValues[it].toInt() }
    val port = m.groupValues[5].toIntOrNull()
    return ip.all { it <= 255 } && ip[0] != 0 && ip[0] < 224 && (port == null || port in 1..65535)
}

@Composable
private fun SliderRow(label: String, value: Int, min: Int, max: Int, enabled: Boolean, onChange: (Int) -> Unit) {
    Column(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 4.dp)) {
        Text("$label: $value%", style = MaterialTheme.typography.titleMedium, color = if (enabled) Rc.text else Rc.text3)
        Slider(
            value.toFloat(), { onChange(it.toInt()) }, valueRange = min.toFloat()..max.toFloat(), steps = (max - min) / 5 - 1, enabled = enabled,
            colors = SliderDefaults.colors(thumbColor = Rc.accent, activeTrackColor = Rc.accent, inactiveTrackColor = Rc.surface3),
            modifier = Modifier.fillMaxWidth().focusRing().semantics { contentDescription = label },
        )
    }
}
