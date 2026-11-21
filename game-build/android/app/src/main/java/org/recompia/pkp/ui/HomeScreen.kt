// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import android.content.Context
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Build
import androidx.compose.material.icons.rounded.CheckCircle
import androidx.compose.material.icons.rounded.Description
import androidx.compose.material.icons.rounded.FolderOpen
import androidx.compose.material.icons.rounded.Info
import androidx.compose.material.icons.rounded.Lock
import androidx.compose.material.icons.rounded.Memory
import androidx.compose.material.icons.rounded.Palette
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material.icons.rounded.Save
import androidx.compose.material.icons.rounded.Settings
import androidx.compose.material.icons.rounded.SportsEsports
import androidx.compose.material.icons.rounded.Storage
import androidx.compose.material.icons.rounded.TouchApp
import androidx.compose.material.icons.rounded.Warning
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavHostController
import org.recompia.pkp.R
import org.recompia.pkp.Storage

/** Home: the game's state, a large Play button, controller status, quick settings and the way to everything else. */
@Composable
fun HomeScreen(vm: AppViewModel, nav: NavHostController) {
    val ui by vm.ui.collectAsStateWithLifecycle()
    val job by vm.job.collectAsStateWithLifecycle()
    val wide = isWide()
    ArtBackground(sharp = true) {
        val hero: @Composable ColumnScope.() -> Unit = { Hero(vm, ui, job, nav) }
        val more: @Composable ColumnScope.() -> Unit = {
            HeapNotice(ui)
            CoopCard(ui.status == GameStatus.READY || ui.status == GameStatus.SHADERS_MISSING)
            ControllerCard()
            QuickSettings()
            Menu(nav, ui)
        }
        if (wide) {
            Row(Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.safeDrawing).padding(horizontal = 32.dp, vertical = 16.dp),
                horizontalArrangement = Arrangement.spacedBy(32.dp)) {
                Column(Modifier.weight(0.5f).fillMaxSize().verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(20.dp, Alignment.CenterVertically), content = hero)
                Column(Modifier.weight(0.5f).fillMaxSize().verticalScroll(rememberScrollState()).padding(vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(16.dp), content = more)
            }
        } else {
            Column(
                Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.safeDrawing).verticalScroll(rememberScrollState()).padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                Spacer(Modifier.height(24.dp))
                hero()
                Spacer(Modifier.height(8.dp))
                more()
            }
        }
    }
}

@Composable
private fun Hero(vm: AppViewModel, ui: UiState, job: org.recompia.pkp.build.BuildState, nav: NavHostController) {
    val act = activity()
    val pickShaders = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { it?.let(vm::importShaders) }
    Image(painterResource(R.drawable.ui_logo), stringResource(R.string.ui_app_name), Modifier.widthIn(max = 380.dp).fillMaxWidth())
    val adreno = remember { GpuDriver.isAdreno() }
    var driverRead by remember { mutableStateOf(false) }
    val driver = androidx.compose.runtime.produceState<GpuDriver.Meta?>(null, ui.status, ui.session) {
        value = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) { runCatching { GpuDriver.current() }.getOrNull() }
        driverRead = true
    }.value
    val (text, color, tint, icon) = statusLine(LocalContext.current, ui.status, job)
    androidx.compose.foundation.layout.FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Pill(text, color, tint, icon)
        // the driver the game will start with (Adreno phones choose one in Settings)
        if (adreno) {
            if (driver != null) Pill(stringResource(R.string.ui_gpu_home, driverName(driver)), Rc.text2, Rc.surface2, Icons.Rounded.Memory)
            else if (driverRead) Pill(stringResource(R.string.ui_gpu_none_pill), Rc.warn, Rc.tintWarn, Icons.Rounded.Warning)
        }
    }
    // Adreno without a custom driver: the stock driver shows a black picture in this game (Mesa Turnip is the supported path)
    if (adreno && driverRead && driver == null) {
        Banner(stringResource(R.string.ui_gpu_home_missing), Rc.warn, Rc.tintWarn, Icons.Rounded.Warning)
        RcButton(stringResource(R.string.ui_gpu_home_fix), { nav.navigate(Routes.SETTINGS) }, icon = Icons.Rounded.Memory)
    }
    when (ui.status) {
        GameStatus.READY -> PlayButton(stringResource(R.string.ui_play), Icons.Rounded.PlayArrow) { play(act, vm) }
        GameStatus.READY_FAKE -> {
            PlayButton(stringResource(R.string.ui_play), Icons.Rounded.PlayArrow, enabled = false) {}
            Text(stringResource(R.string.ui_fake_cannot_play), style = MaterialTheme.typography.bodyMedium, color = Rc.warn)
            RcButton(stringResource(R.string.ui_build_page), { nav.navigate(Routes.BUILD) }, icon = Icons.Rounded.Build)
        }
        GameStatus.NEEDS_BUILD -> PlayButton(stringResource(R.string.ui_build_title), Icons.Rounded.Build) { nav.navigate(Routes.BUILD) }
        GameStatus.NEEDS_REBUILD -> {
            PlayButton(stringResource(R.string.ui_rebuild_title), Icons.Rounded.Build) { nav.navigate(Routes.BUILD) }
            Text(stringResource(R.string.ui_rebuild_body), style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
        }
        GameStatus.DUMP_MISSING -> PlayButton(stringResource(R.string.ui_dump_choose_files), Icons.Rounded.FolderOpen) { openSetup(vm, nav, SetupStep.DUMP) }
        GameStatus.NO_PERMISSION -> PlayButton(stringResource(R.string.ui_perm_allow), Icons.Rounded.Lock) { Storage.requestAllFilesAccess(act) }
        GameStatus.SHADERS_MISSING -> {
            PlayButton(stringResource(R.string.ui_shaders_title), Icons.Rounded.Palette) { nav.navigate(Routes.BUILD) }
            Text(stringResource(R.string.ui_shaders_needed), style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
            RcButton(stringResource(R.string.ui_play_anyway), { play(act, vm) }, icon = Icons.Rounded.PlayArrow, kind = Kind.QUIET)
        }
        GameStatus.BUSY -> {
            Meter(job.percent, org.recompia.pkp.build.BuildService.stepLabel(LocalContext.current, job), "")
            PlayButton(stringResource(R.string.ui_view_progress), Icons.Rounded.Build) { nav.navigate(Routes.BUILD) }
        }
    }
    val ctx = LocalContext.current
    val session = ui.session
    if (ui.lastPlayed > 0) Text(
        if (session != null && session.lengthMs >= 60_000) stringResource(R.string.ui_last_played_for, ago(ui.lastPlayed), ctx.duration(session.lengthMs))
        else stringResource(R.string.ui_last_played, ago(ui.lastPlayed)),
        style = MaterialTheme.typography.bodyMedium, color = Rc.text3)
    // the runtime could not load the chosen driver and used the phone's own (runtime/src/os/os_android_vkdriver.cpp)
    if (session?.fallback != null && driver != null) {
        Banner(stringResource(R.string.ui_gpu_fell_back, session.fallback), Rc.warn, Rc.tintWarn, Icons.Rounded.Warning)
        RcButton(stringResource(R.string.ui_gpu_home_fix), { nav.navigate(Routes.SETTINGS) }, icon = Icons.Rounded.Memory)
    }
    // two starts in a row ended before the first frame with a custom driver: offer the way out before the next start
    var loopDismissed by remember { mutableStateOf(false) }
    if (session != null && session.driverActive && session.failedStarts >= 2 && driver != null && !loopDismissed) {
        androidx.compose.material3.AlertDialog(
            onDismissRequest = { loopDismissed = true },
            containerColor = Rc.solidRaised,
            title = { Text(stringResource(R.string.ui_gpu_loop_title), style = MaterialTheme.typography.headlineSmall) },
            text = { Text(stringResource(R.string.ui_gpu_loop_text, driverName(driver)), style = MaterialTheme.typography.bodyLarge, color = Rc.text2) },
            confirmButton = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    RcButton(stringResource(R.string.ui_gpu_use_system), { loopDismissed = true; vm.useSystemDriver() }, Modifier.fillMaxWidth().initialFocus(), kind = Kind.PRIMARY)
                    RcButton(stringResource(R.string.ui_try_again), { loopDismissed = true; vm.resetDriverFailures(); play(act, vm) }, Modifier.fillMaxWidth())
                    RcButton(stringResource(R.string.ui_gpu_install_other), { loopDismissed = true; vm.resetDriverFailures(); nav.navigate(Routes.SETTINGS) }, Modifier.fillMaxWidth())
                }
            },
        )
    }
}

private data class Line(val text: String, val color: androidx.compose.ui.graphics.Color, val tint: androidx.compose.ui.graphics.Color, val icon: androidx.compose.ui.graphics.vector.ImageVector)

private fun statusLine(ctx: Context, s: GameStatus, job: org.recompia.pkp.build.BuildState): Line = when (s) {
    GameStatus.READY -> Line(ctx.getString(R.string.ui_status_ready), Rc.accent, Rc.tintAccent, Icons.Rounded.CheckCircle)
    GameStatus.READY_FAKE -> Line(ctx.getString(R.string.ui_fake_badge), Rc.warn, Rc.tintWarn, Icons.Rounded.Warning)
    GameStatus.NEEDS_BUILD -> Line(ctx.getString(R.string.ui_status_needs_build), Rc.info, Rc.tintInfo, Icons.Rounded.Info)
    GameStatus.NEEDS_REBUILD -> Line(ctx.getString(R.string.ui_status_needs_rebuild), Rc.warn, Rc.tintWarn, Icons.Rounded.Warning)
    GameStatus.DUMP_MISSING -> Line(ctx.getString(R.string.ui_status_dump_missing), Rc.warn, Rc.tintWarn, Icons.Rounded.Warning)
    GameStatus.NO_PERMISSION -> Line(ctx.getString(R.string.ui_perm_missing), Rc.warn, Rc.tintWarn, Icons.Rounded.Lock)
    GameStatus.SHADERS_MISSING -> Line(ctx.getString(R.string.ui_status_no_shaders), Rc.warn, Rc.tintWarn, Icons.Rounded.Palette)
    GameStatus.BUSY -> Line(ctx.getString(R.string.ui_status_busy, org.recompia.pkp.build.BuildService.stepLabel(ctx, job)), Rc.info, Rc.tintInfo, Icons.Rounded.Build)
}

@Composable
private fun PlayButton(text: String, icon: androidx.compose.ui.graphics.vector.ImageVector, enabled: Boolean = true, onClick: () -> Unit) {
    RcButton(text, onClick, Modifier.fillMaxWidth().widthIn(max = 480.dp).height(76.dp).initialFocus(text), icon = icon, kind = Kind.PRIMARY, big = true, enabled = enabled)
}

@Composable
private fun ControllerCard() {
    val pads = rememberControllers()
    GlassCard(Modifier.fillMaxWidth(), strong = false, padding = 18.dp) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(14.dp)) {
            Icon(if (pads.isEmpty()) Icons.Rounded.TouchApp else Icons.Rounded.SportsEsports, null,
                tint = Rc.accent, modifier = Modifier.size(28.dp))
            Column(Modifier.weight(1f)) {
                Text(stringResource(R.string.ui_controls_label), style = MaterialTheme.typography.labelMedium, color = Rc.text3)
                Text(if (pads.isEmpty()) stringResource(R.string.ui_touch_controls) else pads.joinToString(", "),
                    style = MaterialTheme.typography.titleMedium, color = Rc.text)
            }
        }
    }
}

/** The settings a player reaches for most, the same keys as Settings. */
@Composable
private fun QuickSettings() {
    val ctx = LocalContext.current
    val groups = remember { SettingsModel.groups(ctx) }
    var values by remember { mutableStateOf(runCatching { SettingsModel.read(groups) }.getOrDefault(emptyMap())) }
    val overlay = remember { org.recompia.pkp.overlay.OverlayPrefs.of(ctx) }
    var touch by remember { mutableStateOf(org.recompia.pkp.overlay.OverlayPrefs.enabled(overlay)) }
    val scale = groups.flatMap { it.rows }.firstOrNull { it.id == "render_scale" } as? SettingsModel.Choice
    GlassCard(Modifier.fillMaxWidth(), strong = false, padding = 12.dp) {
        SectionTitle(stringResource(R.string.ui_quick_settings))
        if (scale != null && values.isNotEmpty()) { val v = values[scale.id] ?: scale.def
            ChoiceRow(stringResource(scale.label), choiceOptions(scale, v), v, { n -> writeSetting(scale, n); values = values + (scale.id to n) }) }
        Text(stringResource(R.string.ui_set_render_note), style = MaterialTheme.typography.bodySmall, color = Rc.text3, modifier = Modifier.padding(horizontal = 12.dp))
        SwitchRow(stringResource(R.string.ui_set_touch), touch, { touch = it; overlay.edit().putBoolean(org.recompia.pkp.overlay.OverlayPrefs.ENABLED, it).apply() })
    }
}

@Composable
fun optionText(o: SettingsModel.Option) = if (o.label != 0) stringResource(o.label) else o.text

/** A choice's options plus the current value as one more chip when the ini holds a value the list does not have ("62%"), so the
 *  current value is always highlighted. */
@Composable
fun choiceOptions(c: SettingsModel.Choice, current: String): List<Pair<String, String>> {
    val base = c.options.map { it.value to optionText(it) }
    return if (current.isBlank() || base.any { it.first.equals(current, true) }) base
    else base + (current to (if (c.key == "render_scale" && current.toIntOrNull() != null) "$current%" else current))
}

@Composable
private fun Menu(nav: NavHostController, ui: UiState) {
    val ctx = LocalContext.current
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        NavRow(Icons.Rounded.Settings, stringResource(R.string.ui_settings), { nav.navigate(Routes.SETTINGS) }, detail = stringResource(R.string.ui_settings_detail))
        NavRow(Icons.Rounded.Build, stringResource(R.string.ui_build_page), { nav.navigate(Routes.BUILD) }, detail = stringResource(R.string.ui_build_page_detail))
        NavRow(Icons.Rounded.Storage, stringResource(R.string.ui_storage), { nav.navigate(Routes.STORAGE) },
            detail = stringResource(R.string.ui_storage_free, ctx.size(ui.sizes.free)))
        NavRow(Icons.Rounded.Save, stringResource(R.string.ui_saves), { nav.navigate(Routes.SAVES) }, detail = stringResource(R.string.ui_saves_detail))
        NavRow(Icons.Rounded.Description, stringResource(R.string.ui_logs), { nav.navigate(Routes.LOGS) }, detail = stringResource(R.string.ui_logs_detail))
        NavRow(Icons.Rounded.FolderOpen, stringResource(R.string.ui_help_title), { nav.navigate(Routes.HELP) }, detail = stringResource(R.string.ui_help_detail))
        NavRow(Icons.Rounded.Info, stringResource(R.string.ui_about), { nav.navigate(Routes.ABOUT) }, detail = stringResource(R.string.ui_about_detail))
    }
}


/** The file-backed game heap ([memory] section_dir, 8 GB phones) needs free storage; below it the game starts with the zram heap. */
@Composable
private fun HeapNotice(ui: UiState) {
    val short = androidx.compose.runtime.remember(ui.sizes) { DeviceFacts.heapFileShort() }
    if (short) Banner(stringResource(R.string.ui_heap_file_short, PhoneDefaults.HEAP_FILE_FREE_BYTES shr 30), Rc.warn, Rc.tintWarn, Icons.Rounded.Warning)
    val f = ui.facts ?: return
    if (PhoneDefaults.untestedGpu(f)) Text(stringResource(R.string.ui_gpu_untested, f.gpu), style = MaterialTheme.typography.bodyMedium, color = Rc.warn)
    if (!PhoneDefaults.foldClass(f)) Text(stringResource(R.string.ui_defaults_cautious), style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
}
