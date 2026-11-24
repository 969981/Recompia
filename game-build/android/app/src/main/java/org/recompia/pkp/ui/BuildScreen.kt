// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import android.Manifest
import android.os.Build
import android.os.PowerManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.BugReport
import androidx.compose.material.icons.rounded.Build
import androidx.compose.material.icons.rounded.Check
import androidx.compose.material.icons.rounded.Close
import androidx.compose.material.icons.rounded.Error
import androidx.compose.material.icons.rounded.FolderOpen
import androidx.compose.material.icons.rounded.Info
import androidx.compose.material.icons.rounded.Memory
import androidx.compose.material.icons.rounded.Palette
import androidx.compose.material.icons.rounded.Pause
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material.icons.rounded.Refresh
import androidx.compose.material.icons.rounded.Terminal
import androidx.compose.material.icons.rounded.Thermostat
import androidx.compose.material.icons.rounded.Warning
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.recompia.pkp.R
import org.recompia.pkp.build.BuildService
import org.recompia.pkp.build.BuildState
import org.recompia.pkp.build.JobKind
import org.recompia.pkp.build.RunState
import org.recompia.pkp.build.ShaderCache
import org.recompia.pkp.build.Toolchain

/** The build as its own page (from Home: build, set up again, imports). */
@Composable
fun BuildPage(vm: AppViewModel, onBack: () -> Unit, onChangeFiles: () -> Unit) {
    Page(stringResource(R.string.ui_build_page), onBack) {
        GlassCard(Modifier.fillMaxWidth()) { BuildPanel(vm, inWizard = false, onFinish = onBack, onChangeFiles = onChangeFiles) }
    }
}

/** The steps the player sees, as on the launcher's setup page; the driver's stage names map onto them. */
private val STEPS = listOf(
    listOf("tools", "check") to R.string.ui_step_check,
    listOf("lift") to R.string.ui_step_lift,
    listOf("compile") to R.string.ui_step_compile,
    listOf("link", "finish") to R.string.ui_step_link,
    listOf("shaders") to R.string.ui_step_shaders,
)

/** Free space the on-device build driver demands for a fresh build. */
private const val BUILD_FREE = 2_791_728_742L // 2.6 GiB

/** The on-device shader translation: build_shaders.py's stages. */
private val SHADER_STEPS = listOf(
    listOf("tools", "extract") to R.string.ui_step_extract,
    listOf("translate") to R.string.ui_step_translate,
    listOf("finish") to R.string.ui_step_shaders_finish,
)

private fun stepsOf(s: BuildState) = if (s.kind == JobKind.SHADERS) SHADER_STEPS else STEPS

private fun stepIndex(s: BuildState) = stepsOf(s).let { st ->
    st.indexOfFirst { s.step in it.first }.takeIf { it >= 0 } ?: (s.stepIndex - 1).coerceIn(0, st.size - 1)
}

@Composable
/** [shadersStep]: the wizard's last page (shaders only); the wizard's Build page never asks for shaders, it goes on to the next step. */
fun BuildPanel(vm: AppViewModel, inWizard: Boolean, onFinish: () -> Unit, onChangeFiles: () -> Unit, shadersStep: Boolean = false) {
    val job by vm.job.collectAsStateWithLifecycle()
    val ui by vm.ui.collectAsStateWithLifecycle()
    val ctx = LocalContext.current
    val notify = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { }
    val startKind = { kind: JobKind, fake: Boolean ->
        if (Build.VERSION.SDK_INT >= 33) notify.launch(Manifest.permission.POST_NOTIFICATIONS)
        if (kind == JobKind.SHADERS) vm.startShaders(fake) else vm.startBuild(fake)
    }
    val start = { fake: Boolean -> startKind(JobKind.BUILD, fake) }
    val pickLib = rememberLauncherForActivityResult(OpenDocumentInData()) { it?.let(vm::importLibrary) }
    val pickShaders = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { it?.let(vm::importShaders) }
    val tools = remember { Toolchain.present(ctx) }
    val fakeOk = BuildService.fakeAvailable()  // no remember: a constant false in release lets R8 drop the test buttons
    val build = job.kind == JobKind.BUILD || job.kind == JobKind.SHADERS
    val shaderChoices: @Composable () -> Unit = { ShaderChoices(vm, ui, fakeOk, { pickShaders.launch(DATA_ROOT_DOC) }, { f -> startKind(JobKind.SHADERS, f) }, { vm.startShaders(false, small = true) }) }

    when {
        job.active && build -> Running(vm, job)
        job.active -> JobProgress(vm, job, when (job.kind) {
            JobKind.IMPORT_LIBRARY -> R.string.ui_import_lib_title
            JobKind.IMPORT_SHADERS -> R.string.ui_import_shaders_title
            else -> R.string.ui_copy_title
        })
        job.run == RunState.ERROR -> {
            Heading(stringResource(if (build) R.string.ui_build_stopped else R.string.ui_job_failed))
            Banner(job.error.ifEmpty { stringResource(R.string.ui_build_err_unknown) }, Rc.danger, Rc.tintDanger, Icons.Rounded.Error, mono = true)
            if (job.logFile.isNotEmpty()) Mono(stringResource(R.string.ui_log_at, job.logFile))
            if (build) RcButton(stringResource(R.string.ui_try_again), { startKind(job.kind, job.fake) }, Modifier.fillMaxWidth().initialFocus(job.run), icon = Icons.Rounded.Refresh, kind = Kind.PRIMARY, big = true)
            LogLines(job)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                RcButton(stringResource(R.string.ui_change_files), onChangeFiles, icon = Icons.Rounded.FolderOpen)
                RcButton(stringResource(R.string.ui_dismiss), vm::dismissJob, kind = Kind.QUIET)
            }
        }
        build && (job.run == RunState.CANCELLED || job.run == RunState.INTERRUPTED) -> {
            val shaders = job.kind == JobKind.SHADERS
            Heading(stringResource(if (shaders) R.string.ui_shaders_paused else R.string.ui_build_paused))
            Body(stringResource(if (job.run == RunState.INTERRUPTED) R.string.ui_build_interrupted else R.string.ui_build_kept))
            RcButton(stringResource(if (shaders) R.string.ui_shaders_continue else R.string.ui_build_continue),
                { if (job.small) vm.startShaders(false, small = true) else startKind(job.kind, job.fake) }, Modifier.fillMaxWidth().initialFocus(job.run), icon = Icons.Rounded.PlayArrow, kind = Kind.PRIMARY, big = true)
            if (job.fake) FakeBadge()
            RcButton(stringResource(R.string.ui_dismiss), vm::dismissJob, kind = Kind.QUIET)
        }
        job.run == RunState.DONE && job.kind == JobKind.SHADERS -> {
            Heading(stringResource(R.string.ui_shaders_done))
            if (job.fake) FakeBadge()
            RcButton(stringResource(if (inWizard) R.string.ui_go_home else R.string.ui_close), { vm.dismissJob(); onFinish() },
                Modifier.fillMaxWidth().initialFocus(job.run), icon = Icons.Rounded.Check, kind = Kind.PRIMARY, big = true)
        }
        job.run == RunState.DONE && build -> {
            Heading(stringResource(when {
                job.fake -> R.string.ui_build_done_fake
                job.shadersMissing && inWizard -> R.string.ui_build_done_library
                job.shadersMissing -> R.string.ui_build_done_no_shaders
                else -> R.string.ui_build_done
            }))
            if (job.fake) FakeBadge()
            StepRow(if (job.shadersMissing) STEPS.size - 1 else STEPS.size, done = !job.shadersMissing)
            if (inWizard) {
                // the wizard goes on (graphics driver on Adreno, then shaders): never stuck on a missing shader cache here
                RcButton(stringResource(R.string.ui_continue), { vm.dismissJob(); onFinish() },
                    Modifier.fillMaxWidth().initialFocus(job.run), icon = Icons.Rounded.Check, kind = Kind.PRIMARY, big = true)
            } else if (job.shadersMissing) {
                shaderChoices()
                RcButton(stringResource(R.string.ui_close), { vm.dismissJob(); onFinish() }, kind = Kind.QUIET)
            } else {
                RcButton(stringResource(R.string.ui_close), { vm.dismissJob(); onFinish() },
                    Modifier.fillMaxWidth().initialFocus(job.run), icon = Icons.Rounded.Check, kind = Kind.PRIMARY, big = true)
            }
        }
        else -> {
            if (job.run == RunState.DONE) {
                Banner(stringResource(when (job.kind) {
                    JobKind.IMPORT_LIBRARY -> R.string.ui_import_lib_done
                    JobKind.IMPORT_SHADERS -> R.string.ui_import_shaders_done
                    else -> R.string.ui_copy_done
                }), Rc.accent, Rc.tintAccent, Icons.Rounded.Check)
            }
            if (shadersStep) {
                // keyed on the check's result and the job: read here so this scope recomposes when "Check the cache" finishes
                val ready = remember(ui.shaderCheck, ui.status, job.run) { BuildService.shaderCachePresent() }
                if (ready) {
                    Heading(stringResource(R.string.ui_shaders_done))
                    RcButton(stringResource(R.string.ui_go_home), onFinish, Modifier.fillMaxWidth().initialFocus(), icon = Icons.Rounded.Check, kind = Kind.PRIMARY, big = true)
                } else {
                    Heading(stringResource(R.string.ui_shaders_title))
                    shaderChoices()
                    RcButton(stringResource(R.string.ui_do_later), onFinish, kind = Kind.QUIET)
                }
                return
            }
            // a library copied to <dataRoot>/build (where a PC build goes): offered first, checked, no picker
            val found = remember(ui.status, job.run) { BuildService.foundLibrary(ctx) }
            if (found != null) {
                Heading(stringResource(R.string.ui_lib_found_title))
                Body(stringResource(R.string.ui_lib_found_detail, "%.0f MB".format(found.length() / 1e6),
                    java.text.DateFormat.getDateTimeInstance(java.text.DateFormat.MEDIUM, java.text.DateFormat.SHORT).format(java.util.Date(found.lastModified()))))
                RcButton(stringResource(R.string.ui_lib_found_use), { vm.importLibrary(android.net.Uri.fromFile(found)) },
                    Modifier.fillMaxWidth().initialFocus(), icon = Icons.Rounded.Memory, kind = Kind.PRIMARY, big = true)
                NavRow(Icons.Rounded.FolderOpen, stringResource(R.string.ui_import_lib_other), { pickLib.launch(arrayOf("*/*")) }, detail = stringResource(R.string.ui_import_lib_detail))
                if (inWizard) RcButton(stringResource(R.string.ui_finish_later), onFinish, kind = Kind.QUIET)
                return
            }
            if (inWizard && (ui.status == GameStatus.SHADERS_MISSING || ui.status == GameStatus.READY)) {
                // a library is in place (built, imported or pushed): the wizard goes on; building again stays on the Build page
                Heading(stringResource(R.string.ui_build_done_library))
                Banner(stringResource(R.string.ui_library_ready), Rc.accent, Rc.tintAccent, Icons.Rounded.Check)
                RcButton(stringResource(R.string.ui_continue), onFinish, Modifier.fillMaxWidth().initialFocus(), icon = Icons.Rounded.Check, kind = Kind.PRIMARY, big = true)
                return
            }
            if (ui.status == GameStatus.SHADERS_MISSING) {
                Heading(stringResource(R.string.ui_shaders_title))
                shaderChoices()
                return
            }
            val needs = ui.status == GameStatus.NEEDS_REBUILD
            Heading(stringResource(if (needs) R.string.ui_rebuild_title else R.string.ui_build_title))
            Body(stringResource(if (needs) R.string.ui_rebuild_body else R.string.ui_build_body))
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Icon(Icons.Rounded.Info, null, tint = Rc.info, modifier = Modifier.size(20.dp))
                Text(stringResource(R.string.ui_build_time_note), style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
            }
            HealthStrip()
            if (!tools) Banner(stringResource(R.string.ui_build_no_tools), Rc.warn, Rc.tintWarn, Icons.Rounded.Warning)
            // the build driver needs 2.6 GiB in the work area for a fresh build (less when resuming: what is there counts)
            val buildNeed = BUILD_FREE - maxOf(0L, ui.sizes.build)
            val buildRoom = ui.sizes.free < 0 || ui.sizes.free >= buildNeed
            if (tools && !buildRoom) Banner(stringResource(R.string.ui_err_no_space, BuildService.gib(buildNeed), BuildService.gib(ui.sizes.free)),
                Rc.warn, Rc.tintWarn, Icons.Rounded.Warning)
            if (ui.report?.chosen == null) Banner(stringResource(R.string.ui_build_needs_files), Rc.info, Rc.tintInfo, Icons.Rounded.Info)
            RcButton(stringResource(R.string.ui_build_start), { start(false) }, Modifier.fillMaxWidth().initialFocus(job.run),
                icon = Icons.Rounded.Build, kind = Kind.PRIMARY, big = true, enabled = tools && buildRoom && ui.report?.chosen != null)
            if (ui.report?.chosen == null) RcButton(stringResource(R.string.ui_change_files), onChangeFiles, icon = Icons.Rounded.FolderOpen)
            if (fakeOk) RcButton(stringResource(R.string.ui_build_fake), { start(true) }, Modifier.fillMaxWidth(), icon = Icons.Rounded.BugReport)
            if (fakeOk && BuildService.shaderToolPresent(ctx) && !BuildService.shaderCachePresent())
                RcButton(stringResource(R.string.ui_shaders_debug_small), { vm.startShaders(false, small = true) }, Modifier.fillMaxWidth(), icon = Icons.Rounded.BugReport)
            // DEBUG BUILDS ONLY (fakeOk): build from pkp.ini's exefs without the full dump check, for testing the real build on a
            // phone that has no room for the romfs. Release builds keep the full validation.
            if (fakeOk && tools && ui.report?.chosen == null && remember { BuildService.exefsForBuild() != null })
                RcButton(stringResource(R.string.ui_build_debug_exefs), { start(false) }, Modifier.fillMaxWidth(), icon = Icons.Rounded.BugReport)
            SectionTitle(stringResource(R.string.ui_build_faster))
            Text(stringResource(R.string.ui_build_faster_body), style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
            NavRow(Icons.Rounded.Memory, stringResource(R.string.ui_import_lib), { pickLib.launch(arrayOf("*/*")) }, detail = stringResource(R.string.ui_import_lib_detail))
            NavRow(Icons.Rounded.Palette, stringResource(R.string.ui_import_shaders), { pickShaders.launch(DATA_ROOT_DOC) }, detail = stringResource(R.string.ui_import_shaders_detail))
            if (inWizard) RcButton(stringResource(R.string.ui_finish_later), onFinish, kind = Kind.QUIET)
        }
    }
}

@Composable
private fun FakeBadge() = Pill(stringResource(R.string.ui_fake_badge), Rc.warn, Rc.tintWarn, Icons.Rounded.BugReport)

@Composable
private fun Running(vm: AppViewModel, job: BuildState) {
    val ctx = LocalContext.current
    Heading(stringResource(if (job.kind == JobKind.SHADERS) R.string.ui_shaders_running else R.string.ui_build_running))
    if (job.fake) FakeBadge()
    StepRow(stepIndex(job), done = false, steps = stepsOf(job))
    val label = BuildService.stepLabel(ctx, job)
    Meter(job.percent, label, job.etaMs?.let { stringResource(R.string.ui_eta_left, ctx.roughly(it)) } ?: "")
    val line = when {
        job.step == "compile" && job.total > 0 -> stringResource(R.string.ui_build_units, job.done, job.total)
        job.step == "translate" && job.total > 0 -> stringResource(R.string.ui_shaders_units, job.done, job.total)
        job.message.isNotEmpty() -> job.message
        else -> label
    }
    Text(line, style = MaterialTheme.typography.bodyMedium, color = Rc.text2, modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite })
    if (job.unit.isNotEmpty()) Mono(job.unit)
    if (job.step == "compile" || job.step == "lift" || job.step == "translate") Text(stringResource(R.string.ui_build_long), style = MaterialTheme.typography.bodySmall, color = Rc.text3)
    HealthStrip()
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
        RcButton(stringResource(if (job.run == RunState.CANCELLING) R.string.ui_stopping else R.string.ui_pause),
            vm::cancelJob, Modifier.initialFocus(), icon = Icons.Rounded.Pause, enabled = job.run == RunState.RUNNING)
    }
    Text(stringResource(R.string.ui_build_background), style = MaterialTheme.typography.bodySmall, color = Rc.text3)
    LogLines(job)
}

/** The five steps in a row, done ones ticked. */
@Composable
private fun StepRow(current: Int, done: Boolean, steps: List<Pair<List<String>, Int>> = STEPS) {
    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        steps.forEachIndexed { i, (_, name) ->
            val state = when { done || i < current -> 2; i == current -> 1; else -> 0 }
            val desc = stringResource(when (state) { 2 -> R.string.ui_step_state_done; 1 -> R.string.ui_step_state_now; else -> R.string.ui_step_state_next })
            Row(
                Modifier.clip(CircleShape).background(when (state) { 2 -> Rc.tintAccent; 1 -> Rc.surface3; else -> Rc.surface1 })
                    .padding(horizontal = 12.dp, vertical = 8.dp).semantics { stateDescription = desc },
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Box(Modifier.size(20.dp).clip(CircleShape).background(if (state == 2) Rc.accent else Rc.surface2), contentAlignment = Alignment.Center) {
                    if (state == 2) Icon(Icons.Rounded.Check, null, tint = Rc.onAccent, modifier = Modifier.size(14.dp))
                    else Text("${i + 1}", style = MaterialTheme.typography.labelSmall, color = if (state == 1) Rc.accent else Rc.text3)
                }
                Spacer(Modifier.width(8.dp))
                Text(stringResource(name), style = MaterialTheme.typography.labelLarge, color = if (state == 0) Rc.text3 else Rc.text)
            }
        }
    }
}

/** Battery, charger and heat, so the player can see why a build slows down. */
@Composable
fun HealthStrip() {
    val h = rememberHealth() ?: return
    // Android's thermal status decides (it is what slows the build down); the battery temperature is only extra detail
    val heat = when {
        h.thermal >= PowerManager.THERMAL_STATUS_SEVERE -> R.string.ui_heat_hot
        h.thermal == PowerManager.THERMAL_STATUS_MODERATE -> R.string.ui_heat_warm
        else -> null
    }
    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        if (heat != null) Pill(stringResource(heat), if (heat == R.string.ui_heat_hot) Rc.danger else Rc.warn,
            if (heat == R.string.ui_heat_hot) Rc.tintDanger else Rc.tintWarn, Icons.Rounded.Warning)
        if (h.batteryPct >= 0) Pill(
            stringResource(if (h.charging) R.string.ui_battery_charging else R.string.ui_battery, h.batteryPct),
            if (h.charging) Rc.accent else Rc.warn, if (h.charging) Rc.tintAccent else Rc.tintWarn,
        )
        h.batteryTempC?.let { Pill(stringResource(R.string.ui_battery_temp, it), Rc.text2, Rc.surface2, Icons.Rounded.Thermostat) }
    }
    if (!h.charging) Text(stringResource(R.string.ui_plug_in), style = MaterialTheme.typography.bodySmall, color = Rc.warn)
}

/** The tool's own output, last lines, behind a toggle. */
@Composable
private fun LogLines(job: BuildState) {
    var open by rememberSaveable { mutableStateOf(false) }
    if (job.lines.isEmpty()) return
    RcButton(stringResource(if (open) R.string.ui_hide_log else R.string.ui_show_log), { open = !open }, icon = Icons.Rounded.Terminal, kind = Kind.QUIET)
    if (!open) return
    val list = rememberLazyListState()
    LaunchedEffect(job.lines.size) { list.scrollToItem((job.lines.size - 1).coerceAtLeast(0)) }
    LazyColumn(
        Modifier.fillMaxWidth().heightIn(max = 260.dp).clip(RoundedCornerShape(Rc.radiusSm)).background(Rc.sunken).padding(12.dp),
        state = list,
    ) {
        items(job.lines) { Text(it, style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace), color = Rc.text2) }
    }
}

/** A copy job (import or dump copy): bytes, time left, cancel. */
@Composable
fun JobProgress(vm: AppViewModel, job: BuildState, title: Int) {
    val ctx = LocalContext.current
    Heading(stringResource(title))
    Meter(job.percent, stringResource(R.string.ui_step_working), job.etaMs?.let { stringResource(R.string.ui_eta_left, ctx.roughly(it)) } ?: "")
    if (job.total > 0) Text("${ctx.size(job.done)} / ${ctx.size(job.total)}", style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
    if (job.unit.isNotEmpty()) Mono(job.unit)
    RcButton(stringResource(if (job.run == RunState.CANCELLING) R.string.ui_stopping else R.string.ui_cancel), vm::cancelJob,
        Modifier.initialFocus(), icon = Icons.Rounded.Close, enabled = job.run == RunState.RUNNING)
}


/**
 * The game's shader cache, two ways: copy it from a PC install (fast), or translate it here with the on-device tools (1.5 to 2 hours,
 * 12 GB free at the start, resumable). The translate path says up front what it needs, so nothing starts that cannot finish.
 */
@Composable
private fun ShaderChoices(vm: AppViewModel, ui: UiState, fakeOk: Boolean, onImport: () -> Unit, onTranslate: (Boolean) -> Unit, onSmall: () -> Unit = {}) {
    val ctx = LocalContext.current
    val tool = remember { BuildService.shaderToolPresent(ctx) }
    val free = ui.sizes.free
    val need = BuildService.SHADER_FREE - maxOf(0L, ui.sizes.shaders)
    val room = free < 0 || free >= need
    // an archive and its list without setup.json (a cache copied without it): say exactly that and offer the check (ShaderCache)
    val noSetup = remember(ui.status, ui.shaderCheck) { ShaderCache.state() == ShaderCache.State.NO_SETUP }
    if (noSetup) {
        Banner(stringResource(R.string.ui_shaders_no_setup), Rc.warn, Rc.tintWarn, Icons.Rounded.Palette)
        RcButton(stringResource(R.string.ui_shaders_check), vm::checkShaderCache, Modifier.fillMaxWidth().initialFocus(), icon = Icons.Rounded.Check,
            kind = Kind.PRIMARY, big = true, enabled = !ui.shaderChecking)
    } else {
        Banner(stringResource(R.string.ui_shaders_needed), Rc.warn, Rc.tintWarn, Icons.Rounded.Palette)
    }
    if (ui.shaderChecking) Text(stringResource(R.string.ui_shaders_checking), style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
    ui.shaderCheck?.let { Text(it, style = MaterialTheme.typography.bodyMedium, color = Rc.text2) }
    RcButton(stringResource(R.string.ui_import_shaders), onImport, if (noSetup) Modifier.fillMaxWidth() else Modifier.fillMaxWidth().initialFocus(),
        icon = Icons.Rounded.Palette, kind = if (noSetup) Kind.SECONDARY else Kind.PRIMARY, big = !noSetup)
    Text(stringResource(R.string.ui_import_shaders_detail), style = MaterialTheme.typography.bodySmall, color = Rc.text3)
    SectionTitle(stringResource(R.string.ui_shaders_or))
    RcButton(stringResource(R.string.ui_shaders_translate), { onTranslate(false) }, Modifier.fillMaxWidth(), icon = Icons.Rounded.Build,
        enabled = tool && room && ui.report?.chosen != null)
    Text(stringResource(R.string.ui_shaders_translate_note, BuildService.gib(free) + " GB"), style = MaterialTheme.typography.bodySmall, color = Rc.text3)
    if (!tool) Banner(stringResource(R.string.ui_shaders_err_no_tool), Rc.info, Rc.tintInfo, Icons.Rounded.Info)
    else if (!room) Banner(stringResource(R.string.ui_shaders_err_space, BuildService.gib(need), BuildService.gib(free)), Rc.warn, Rc.tintWarn, Icons.Rounded.Warning)
    if (fakeOk) RcButton(stringResource(R.string.ui_shaders_fake), { onTranslate(true) }, Modifier.fillMaxWidth(), icon = Icons.Rounded.BugReport)
    // DEBUG BUILDS ONLY: the real translator on whatever romfs pkp.ini names (a partial one for a test), without the 12 GB demand
    if (fakeOk && tool) RcButton(stringResource(R.string.ui_shaders_debug_small), { onSmall() }, Modifier.fillMaxWidth(), icon = Icons.Rounded.BugReport)
}
