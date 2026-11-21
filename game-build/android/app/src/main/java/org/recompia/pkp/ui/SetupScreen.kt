// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import android.net.Uri
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.BatteryChargingFull
import androidx.compose.material.icons.rounded.CheckCircle
import androidx.compose.material.icons.rounded.CloudOff
import androidx.compose.material.icons.rounded.Error
import androidx.compose.material.icons.rounded.FolderOpen
import androidx.compose.material.icons.rounded.Info
import androidx.compose.material.icons.rounded.Lock
import androidx.compose.material.icons.rounded.Refresh
import androidx.compose.material.icons.rounded.SportsEsports
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.recompia.pkp.Paths
import org.recompia.pkp.R
import org.recompia.pkp.Storage
import org.recompia.pkp.build.JobKind
import java.io.File

/** The first-run wizard: welcome, storage access, game files, build. One glass panel that changes as you go (the launcher's setup page). */
@Composable
fun SetupScreen(vm: AppViewModel, onHelp: () -> Unit = {}, onDone: () -> Unit) {
    val ui by vm.ui.collectAsStateWithLifecycle()
    val step = ui.step
    val wide = isWide()
    val adreno = androidx.compose.runtime.remember { GpuDriver.isAdreno() }
    val steps = wizardSteps(adreno)
    BackHandler(enabled = step != SetupStep.WELCOME) { steps.getOrNull(steps.indexOf(step) - 1)?.let(vm::goTo) }
    // the next page; the shaders page is skipped when a usable cache is already there
    val advance = { from: SetupStep ->
        val next = steps.getOrNull(steps.indexOf(from) + 1)
        if (next == null || (next == SetupStep.SHADERS && org.recompia.pkp.build.BuildService.shaderCachePresent())) onDone() else vm.goTo(next)
    }
    ArtBackground(sharp = true) {
        val panel: @Composable ColumnScope.() -> Unit = {
            AnimatedContent(
                step,
                transitionSpec = {
                    val dir = if (targetState.ordinal > initialState.ordinal) 1 else -1
                    (slideInHorizontally(tween(320)) { it / 5 * dir } + fadeIn(tween(320))) togetherWith
                        (slideOutHorizontally(tween(240)) { -it / 5 * dir } + fadeOut(tween(200)))
                },
                label = "step",
            ) { s ->
                GlassCard(Modifier.fillMaxWidth()) {
                    when (s) {
                        SetupStep.WELCOME -> WelcomeStep(showButton = !wide, onNext = { vm.goTo(SetupStep.PERMISSION) })
                        SetupStep.PERMISSION -> PermissionStep(ui.hasPermission, onBack = { vm.goTo(SetupStep.WELCOME) }, onNext = { vm.goTo(SetupStep.DUMP) })
                        SetupStep.DUMP -> DumpStep(vm, ui, onBack = { vm.goTo(SetupStep.PERMISSION) }, onNext = { vm.goTo(SetupStep.BUILD) }, onHelp = onHelp)
                        SetupStep.BUILD -> BuildPanel(vm, inWizard = true, onFinish = { advance(SetupStep.BUILD) },
                            onChangeFiles = { vm.goTo(SetupStep.DUMP) })
                        SetupStep.DRIVER -> DriverStep(onBack = { vm.goTo(SetupStep.BUILD) }, onDone = { advance(SetupStep.DRIVER) })
                        SetupStep.SHADERS -> BuildPanel(vm, inWizard = true, onFinish = onDone, onChangeFiles = { vm.goTo(SetupStep.DUMP) }, shadersStep = true)
                    }
                }
            }
        }
        if (wide) {
            Row(Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.safeDrawing).padding(32.dp), horizontalArrangement = Arrangement.spacedBy(40.dp)) {
                // scrolls on short screens (a 411 dp tall landscape phone cut the step list after step 3)
                androidx.compose.foundation.layout.BoxWithConstraints(Modifier.weight(0.4f).fillMaxHeight()) {
                    Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).heightIn(min = maxHeight), verticalArrangement = Arrangement.Center) {
                        Image(painterResource(R.drawable.ui_logo), stringResource(R.string.ui_app_name), Modifier.fillMaxWidth().widthIn(max = 420.dp))
                        Spacer(Modifier.height(16.dp))
                        Text(stringResource(R.string.ui_tagline), style = MaterialTheme.typography.titleLarge, color = Rc.text2)
                        Spacer(Modifier.height(32.dp))
                        StepList(step, steps)
                    }
                }
                // the welcome's primary action is pinned under the scrolling panel, so it is visible on short screens (cover screen
                // in landscape) without scrolling
                Column(Modifier.weight(0.6f).fillMaxSize(), verticalArrangement = Arrangement.spacedBy(16.dp, Alignment.CenterVertically)) {
                    Column(Modifier.weight(1f, fill = false).verticalScroll(rememberScrollState()), content = panel)
                    if (step == SetupStep.WELCOME) RcButton(stringResource(R.string.ui_get_started), { vm.goTo(SetupStep.PERMISSION) },
                        Modifier.fillMaxWidth().initialFocus(), kind = Kind.PRIMARY, big = true)
                }
            }
        } else {
            androidx.compose.foundation.layout.BoxWithConstraints(Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.safeDrawing)) {
            Column(
                Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).heightIn(min = maxHeight).padding(16.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(16.dp, Alignment.CenterVertically),
            ) {
                Spacer(Modifier.height(8.dp))
                Image(painterResource(R.drawable.ui_logo), stringResource(R.string.ui_app_name), Modifier.width(260.dp))
                StepDots(step, steps)
                panel()
            }
            }
        }
    }
}

/** The wizard's pages on this phone: the driver step exists only on Adreno phones. */
private fun wizardSteps(adreno: Boolean) = SetupStep.entries.filter { it != SetupStep.DRIVER || adreno }

private fun stepName(s: SetupStep) = when (s) {
    SetupStep.WELCOME -> R.string.ui_step_name_welcome
    SetupStep.PERMISSION -> R.string.ui_step_name_storage
    SetupStep.DUMP -> R.string.ui_step_name_files
    SetupStep.BUILD -> R.string.ui_step_name_build
    SetupStep.DRIVER -> R.string.ui_step_name_driver
    SetupStep.SHADERS -> R.string.ui_step_name_shaders
}

@Composable
private fun StepList(current: SetupStep, steps: List<SetupStep>) {
    val at = steps.indexOf(current)
    Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
        steps.map(::stepName).forEachIndexed { i, name ->
            val done = i < at
            val now = i == at
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    Modifier.size(32.dp).clip(CircleShape).background(if (done) Rc.accent else if (now) Rc.tintAccent else Rc.surface2),
                    contentAlignment = Alignment.Center,
                ) {
                    if (done) Icon(Icons.Rounded.CheckCircle, null, tint = Rc.onAccent, modifier = Modifier.size(20.dp))
                    else Text("${i + 1}", style = MaterialTheme.typography.labelLarge, color = if (now) Rc.accent else Rc.text3)
                }
                Spacer(Modifier.width(14.dp))
                Text(stringResource(name), style = MaterialTheme.typography.titleMedium, color = if (now) Rc.text else Rc.text3)
            }
        }
    }
}

@Composable
private fun StepDots(current: SetupStep, steps: List<SetupStep>) {
    val at = steps.indexOf(current)
    val label = stringResource(R.string.ui_step_of, at + 1, steps.size, stringResource(stepName(current)))
    Row(Modifier.semantics { contentDescription = label }, horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
        steps.indices.forEach { i ->
            Box(Modifier.height(8.dp).width(if (i == at) 28.dp else 8.dp).clip(CircleShape)
                .background(if (i <= at) Rc.accent else Rc.surface3))
        }
    }
}

@Composable
private fun Fact(icon: androidx.compose.ui.graphics.vector.ImageVector, text: String, color: Color = Rc.accent) {
    Row(horizontalArrangement = Arrangement.spacedBy(14.dp)) {
        Box(Modifier.size(36.dp).clip(CircleShape).background(Rc.surface2), contentAlignment = Alignment.Center) {
            Icon(icon, null, tint = color, modifier = Modifier.size(20.dp))
        }
        Text(text, style = MaterialTheme.typography.bodyLarge, color = Rc.text2, modifier = Modifier.padding(top = 6.dp))
    }
}

/**
 * Adreno only: the Mesa Turnip driver is the supported path there (the stock driver shows a black picture in this game), so the
 * step recommends it and skipping it asks first.
 */
@Composable
private fun DriverStep(onBack: () -> Unit, onDone: () -> Unit) {
    var active by remember { mutableStateOf(runCatching { GpuDriver.current() }.getOrNull() != null) }
    var askSkip by remember { mutableStateOf(false) }
    Heading(stringResource(R.string.ui_gpu_step_title))
    GpuDriverPanel(wizard = true, onChanged = { active = it != null })
    if (active) RcButton(stringResource(R.string.ui_continue), onDone, Modifier.fillMaxWidth().initialFocus(active), icon = Icons.Rounded.CheckCircle, kind = Kind.PRIMARY, big = true)
    else RcButton(stringResource(R.string.ui_gpu_skip), { askSkip = true }, kind = Kind.QUIET)
    RcButton(stringResource(R.string.ui_back), onBack, kind = Kind.QUIET)
    if (askSkip) Confirm(stringResource(R.string.ui_gpu_skip_title), stringResource(R.string.ui_gpu_skip_text), stringResource(R.string.ui_gpu_skip),
        onDone, { askSkip = false })
}

@Composable
private fun WelcomeStep(showButton: Boolean, onNext: () -> Unit) {
    Heading(stringResource(R.string.ui_welcome_title))
    Body(stringResource(R.string.ui_welcome_body))
    Fact(Icons.Rounded.FolderOpen, stringResource(R.string.ui_welcome_dump))
    Fact(Icons.Rounded.CloudOff, stringResource(R.string.ui_welcome_no_download))
    Fact(Icons.Rounded.BatteryChargingFull, stringResource(R.string.ui_welcome_time), Rc.warn)
    Text(stringResource(R.string.ui_supported), style = MaterialTheme.typography.bodySmall, color = Rc.text3)
    if (showButton) RcButton(stringResource(R.string.ui_get_started), onNext, Modifier.fillMaxWidth().initialFocus(), kind = Kind.PRIMARY, big = true)
}

@Composable
private fun PermissionStep(granted: Boolean, onBack: () -> Unit, onNext: () -> Unit) {
    val act = activity()
    Heading(stringResource(R.string.ui_perm_title))
    Body(stringResource(R.string.ui_perm_body, shown(Paths.dataRoot.path)))
    if (granted) Pill(stringResource(R.string.ui_perm_granted), Rc.accent, Rc.tintAccent, Icons.Rounded.CheckCircle)
    else Pill(stringResource(R.string.ui_perm_missing), Rc.warn, Rc.tintWarn, Icons.Rounded.Lock)
    if (granted) RcButton(stringResource(R.string.ui_continue), onNext, Modifier.fillMaxWidth().initialFocus(granted), kind = Kind.PRIMARY, big = true)
    else RcButton(stringResource(R.string.ui_perm_allow), { Storage.requestAllFilesAccess(act) }, Modifier.fillMaxWidth().initialFocus(granted), kind = Kind.PRIMARY, big = true)
    RcButton(stringResource(R.string.ui_back), onBack, kind = Kind.QUIET)
}

// ---- game files --------------------------------------------------------------------------------------------------------------

@Composable
private fun DumpStep(vm: AppViewModel, ui: UiState, onBack: () -> Unit, onNext: () -> Unit, onHelp: () -> Unit) {
    val job by vm.job.collectAsStateWithLifecycle()
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri: Uri? -> uri?.let(vm::pickedTree) }
    var changing by rememberSaveable { mutableStateOf(false) }
    val report = ui.report
    when {
        job.kind == JobKind.COPY_DUMP && job.active -> JobProgress(vm, job, R.string.ui_copy_title)
        ui.pendingCopy != null -> CopyOffer(vm, ui.pendingCopy)
        ui.scanning -> {
            Heading(stringResource(R.string.ui_dump_checking))
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(14.dp)) {
                CircularProgressIndicator(Modifier.size(24.dp), color = Rc.accent, strokeWidth = 3.dp)
                Mono(ui.dumpPath)
            }
        }
        report?.chosen != null && !changing -> DumpGood(vm, ui, report, onNext = onNext, onChange = { changing = true })
        else -> {
            Heading(stringResource(if (report != null && report.chosen == null) R.string.ui_dump_bad_title else R.string.ui_dump_title))
            if (report != null && report.chosen == null) {
                report.problems.forEach { ProblemCard(problemView(it)) }
                Mono(report.path)
                RcButton(stringResource(R.string.ui_dump_check_again), { vm.useDump(report.path, ui.dumpMode) }, icon = Icons.Rounded.Refresh)
            } else Body(stringResource(R.string.ui_dump_body))
            if (job.kind == JobKind.COPY_DUMP && job.error.isNotEmpty()) Banner(job.error, Rc.danger, Rc.tintDanger, Icons.Rounded.Error, mono = true)
            RcButton(stringResource(R.string.ui_dump_choose), { picker.launch(null) }, Modifier.fillMaxWidth().initialFocus(), icon = Icons.Rounded.FolderOpen, kind = Kind.PRIMARY, big = true)
            FoundDumps(ui) { changing = false; vm.useDump(it, DumpMode.IN_PLACE) }
            RcButton(stringResource(R.string.ui_help_title), onHelp, icon = Icons.Rounded.Info, kind = Kind.QUIET)
            if (changing) RcButton(stringResource(R.string.ui_cancel), { changing = false }, kind = Kind.QUIET)
            DumpDetails(report)
            Text(stringResource(R.string.ui_dump_legal), style = MaterialTheme.typography.bodySmall, color = Rc.text3)
            RcButton(stringResource(R.string.ui_back), onBack, kind = Kind.QUIET)
        }
    }
}

/** Folders on this phone that look like a dump (a quick look in the usual places), so most players need no picker at all. */
@Composable
private fun FoundDumps(ui: UiState, onUse: (String) -> Unit) {
    if (!ui.hasPermission) return
    val found by androidx.compose.runtime.produceState(emptyList<File>()) { value = withContext(Dispatchers.IO) { findDumps() } }
    if (found.isEmpty()) return
    SectionTitle(stringResource(R.string.ui_dump_found))
    found.forEach { f ->
        NavRow(Icons.Rounded.SportsEsports, f.name, { onUse(f.path) }, detail = shown(f.path))
    }
}

private fun looksLikeDump(f: File) = File(f, "exefs").isDirectory || File(f, "base").isDirectory || File(f, "updates").isDirectory

private fun findDumps(): List<File> {
    val root = File("/storage/emulated/0")
    val out = mutableListOf<File>()
    fun look(dir: File, depth: Int) {
        if (out.size >= 6) return
        for (c in dir.listFiles()?.filter { it.isDirectory && !it.name.startsWith(".") && it.name != "Android" }.orEmpty()) {
            if (looksLikeDump(c)) out += c else if (depth > 0) look(c, depth - 1)
        }
    }
    if (looksLikeDump(Paths.dumpDir)) out += Paths.dumpDir
    look(root, 2)
    return out.distinctBy { it.path }
}

@Composable
private fun DumpGood(vm: AppViewModel, ui: UiState, report: DumpScanner.Report, onNext: () -> Unit, onChange: () -> Unit) {
    val ctx = LocalContext.current
    val c = report.chosen!!
    Heading(stringResource(R.string.ui_dump_ok_title))
    Row(horizontalArrangement = Arrangement.spacedBy(14.dp)) {
        Icon(Icons.Rounded.CheckCircle, null, tint = Rc.accent, modifier = Modifier.size(28.dp))
        Column {
            Text(candidateName(c) + aocSuffix(report), style = MaterialTheme.typography.titleMedium, color = Rc.text)
            Mono(c.path)
        }
    }
    val size = c.exefsBytes + c.romfsBytes
    val free = Storage.freeBytes()
    val inPlace = ui.dumpMode == DumpMode.IN_PLACE
    Banner(
        if (inPlace) stringResource(R.string.ui_dump_in_place, ctx.size(size)) else stringResource(R.string.ui_dump_copied, shown(Paths.dumpDir.path)),
        Rc.info, Rc.tintInfo, Icons.Rounded.FolderOpen,
    )
    Text(stringResource(R.string.ui_dump_sizes, ctx.size(size), ctx.size(free)), style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
    RcButton(stringResource(R.string.ui_continue), onNext, Modifier.fillMaxWidth().initialFocus(), kind = Kind.PRIMARY, big = true)
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        RcButton(stringResource(R.string.ui_change), onChange, icon = Icons.Rounded.FolderOpen)
        if (inPlace && !c.path.startsWith(Paths.dataRoot.path)) RcButton(stringResource(R.string.ui_dump_copy_instead), vm::copyInPlaceDump, kind = Kind.QUIET)
    }
    DumpDetails(report)
}

@Composable
private fun CopyOffer(vm: AppViewModel, uri: Uri) {
    val ctx = LocalContext.current
    Heading(stringResource(R.string.ui_copy_offer_title))
    Body(stringResource(R.string.ui_copy_offer_body, shown(Paths.dumpDir.path), ctx.size(Storage.freeBytes())))
    RcButton(stringResource(R.string.ui_copy_start), { vm.startCopy(uri) }, Modifier.fillMaxWidth().initialFocus(), kind = Kind.PRIMARY, big = true)
    RcButton(stringResource(R.string.ui_cancel), vm::cancelCopy, kind = Kind.QUIET)
}

@Composable
private fun DumpDetails(report: DumpScanner.Report?) {
    var open by rememberSaveable { mutableStateOf(false) }
    RcButton(stringResource(if (open) R.string.ui_hide_details else R.string.ui_show_details), { open = !open }, kind = Kind.QUIET)
    if (!open) return
    Column(Modifier.fillMaxWidth().clip(androidx.compose.foundation.shape.RoundedCornerShape(Rc.radiusSm)).background(Rc.sunken).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp)) {
        report?.problems?.map { problemView(it) }?.forEach { v ->
            if (v.items.isNotEmpty()) {
                Text(stringResource(R.string.ui_dump_missing_title), style = MaterialTheme.typography.titleSmall, color = Rc.text)
                v.items.take(12).forEach { Mono("\u2022 $it") }
                if (v.items.size > 12) Mono(stringResource(R.string.ui_more_items, v.items.size - 12))
            }
            if (v.detail.isNotEmpty()) Text(v.detail, style = MaterialTheme.typography.bodySmall, color = Rc.text2)
        }
        report?.candidates?.forEach { c ->
            val used = report.chosen?.path == c.path
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(candidateName(c), style = MaterialTheme.typography.titleSmall, color = Rc.text, modifier = Modifier.weight(1f))
                when {
                    used -> Pill(stringResource(R.string.ui_dump_will_use), Rc.accent, Rc.tintAccent)
                    c.ok -> Pill(stringResource(R.string.ui_dump_valid_older), Rc.text2, Rc.surface2)
                    report.chosen != null -> Pill(stringResource(R.string.ui_dump_not_needed), Rc.text2, Rc.surface2)
                    else -> Pill(stringResource(R.string.ui_dump_incomplete), Rc.danger, Rc.tintDanger)
                }
            }
            Mono(c.path)
            if (report.chosen == null || used) c.checks.forEach { k ->
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Icon(if (k.ok) Icons.Rounded.CheckCircle else Icons.Rounded.Error, null, tint = if (k.ok) Rc.accent else Rc.danger, modifier = Modifier.size(16.dp))
                    Text(checkText(k), style = MaterialTheme.typography.bodySmall, color = Rc.text2)
                }
            }
        }
        Text(LAYOUT, style = MaterialTheme.typography.bodySmall.copy(fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace), color = Rc.text3,
            softWrap = false, modifier = Modifier.horizontalScroll(rememberScrollState()))
        Text(stringResource(R.string.ui_dump_detail_decrypted), style = MaterialTheme.typography.bodySmall, color = Rc.text2)
        Text(stringResource(R.string.ui_dump_detail_readonly), style = MaterialTheme.typography.bodySmall, color = Rc.text2)
    }
}

private const val LAYOUT = """Game/
  base/
    exefs/    main, sdk, subsdk0, rtld, main.npdm
    romfs/
  updates/<version>/
    exefs/
    romfs/
  aoc/<add-on id>/romfs/    optional"""

@Composable
fun candidateName(c: DumpScanner.Candidate): String = when (c.kind) {
    "base" -> stringResource(R.string.ui_dump_base)
    "update" -> stringResource(R.string.ui_dump_update, c.version)
    else -> stringResource(R.string.ui_dump_folder)
}

@Composable
private fun aocSuffix(r: DumpScanner.Report): String = when (r.aoc.size) {
    0 -> ""
    1 -> ", " + stringResource(R.string.ui_dump_one_addon)
    else -> ", " + stringResource(R.string.ui_dump_addons, r.aoc.size)
}

@Composable
private fun checkText(k: DumpScanner.Check): String = when (k.kind) {
    DumpScanner.CheckKind.FILE -> k.arg
    DumpScanner.CheckKind.ROMFS_DIR -> "romfs/"
    DumpScanner.CheckKind.ROMFS_NOT_EMPTY -> stringResource(R.string.ui_check_romfs_not_empty)
    DumpScanner.CheckKind.ROMFS_GAME_DATA -> stringResource(R.string.ui_check_romfs_game_data)
    DumpScanner.CheckKind.ROMFS_COMPLETE -> stringResource(R.string.ui_check_romfs_complete) + if (k.arg.isNotEmpty()) " (${k.arg})" else ""
    DumpScanner.CheckKind.ROMFS_COMPLETED_FROM -> stringResource(R.string.ui_check_romfs_from, shown(k.arg))
    DumpScanner.CheckKind.TEXT_MODE -> stringResource(R.string.ui_check_text_mode, k.arg)
    DumpScanner.CheckKind.VERSION -> stringResource(R.string.ui_check_version, k.arg)
    DumpScanner.CheckKind.OLDER -> stringResource(R.string.ui_check_older)
}

/** A dump problem as the player reads it: one short line, one short line per fix, the named items and the long text behind Show details. */
data class ProblemView(val title: String, val fixes: List<String>, val items: List<String>, val detail: String)

@Composable
private fun ProblemCard(v: ProblemView) {
    Banner(v.title, Rc.danger, Rc.tintDanger, Icons.Rounded.Error)
    if (v.fixes.isNotEmpty()) {
        Text(stringResource(R.string.ui_dump_fix_title), style = MaterialTheme.typography.titleSmall, color = Rc.text)
        v.fixes.forEach { f ->
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Text("\u2022", style = MaterialTheme.typography.bodyMedium, color = Rc.accent, modifier = Modifier.clearAndSetSemantics { })
                Text(f, style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
            }
        }
    }
}

@Composable
fun problemView(p: DumpScanner.Problem): ProblemView = when (p) {
    DumpScanner.Problem.NoneChosen -> ProblemView(stringResource(R.string.ui_dump_err_none), emptyList(), emptyList(), "")
    DumpScanner.Problem.NotFound -> ProblemView(stringResource(R.string.ui_dump_err_not_found), listOf(stringResource(R.string.ui_dump_fix_pick_again)), emptyList(), "")
    DumpScanner.Problem.NoLayout -> ProblemView(stringResource(R.string.ui_dump_err_no_layout_short), listOf(stringResource(R.string.ui_dump_fix_layout)), emptyList(), stringResource(R.string.ui_dump_err_no_layout))
    is DumpScanner.Problem.Incomplete -> {
        val missing = p.needFiles - p.files   // p.missing lists only the player's own empty files, not the count
        ProblemView(
            if (missing > 0) stringResource(R.string.ui_dump_err_files_missing, missing, p.needFiles) else stringResource(R.string.ui_dump_err_bytes_short, p.gb, p.needGb),
            listOf(stringResource(R.string.ui_dump_fix_copy_missing), stringResource(R.string.ui_dump_fix_base_next), stringResource(R.string.ui_dump_fix_merged)),
            p.missing.map { "romfs/$it" },
            stringResource(R.string.ui_dump_err_incomplete, shown(p.romfs), p.files, p.needFiles, p.gb, p.needGb),
        )
    }
    is DumpScanner.Problem.TextMode -> ProblemView(stringResource(R.string.ui_dump_err_text_mode_short), listOf(stringResource(R.string.ui_dump_fix_binary)), p.files, stringResource(R.string.ui_dump_err_text_mode))
    is DumpScanner.Problem.Older -> ProblemView(stringResource(R.string.ui_dump_err_older_short), listOf(stringResource(R.string.ui_dump_fix_update, p.version)), emptyList(), "")
    is DumpScanner.Problem.BaseOnly -> ProblemView(stringResource(R.string.ui_dump_err_base_only_short), listOf(stringResource(R.string.ui_dump_fix_update, p.version)), emptyList(), "")
    is DumpScanner.Problem.Different -> ProblemView(stringResource(R.string.ui_dump_err_different_short), listOf(stringResource(R.string.ui_dump_fix_needs, p.versionText)), emptyList(), "")
    is DumpScanner.Problem.RomfsBad -> ProblemView(stringResource(R.string.ui_dump_err_romfs_short), listOf(stringResource(R.string.ui_dump_fix_dump_again), stringResource(R.string.ui_dump_fix_binary)), p.missing, "")
    is DumpScanner.Problem.NothingUsable -> ProblemView(stringResource(R.string.ui_dump_err_nothing_short), listOf(stringResource(R.string.ui_dump_fix_layout)), p.missing, stringResource(R.string.ui_dump_err_nothing))
}
