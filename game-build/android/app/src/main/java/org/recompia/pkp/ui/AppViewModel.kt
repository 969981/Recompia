// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import android.app.Application
import android.content.Context
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import org.recompia.pkp.GameLauncher
import org.recompia.pkp.Paths
import org.recompia.pkp.R
import org.recompia.pkp.Storage
import org.recompia.pkp.build.BuildService
import org.recompia.pkp.build.Copier
import org.recompia.pkp.build.JobKind
import org.recompia.pkp.build.RunState
import org.recompia.pkp.build.ShaderCache
import java.io.File

/** Where the game stands, for Home's Play button. */
enum class GameStatus { NO_PERMISSION, DUMP_MISSING, NEEDS_BUILD, NEEDS_REBUILD, SHADERS_MISSING, BUSY, READY, READY_FAKE }

/** The wizard's pages, in order. DRIVER is optional and only shown on Adreno GPUs ([GpuDriver.isAdreno]); SHADERS comes last
 *  and can be left for later (Home then shows "Shader cache missing"), so a missing cache never blocks the library or driver steps. */
enum class SetupStep { WELCOME, PERMISSION, DUMP, BUILD, DRIVER, SHADERS }

/** How the dump is used: read where it is, or copied into <dataRoot>/dump (a picker tree that has no plain path). */
enum class DumpMode { IN_PLACE, COPIED }

data class Sizes(
    val dump: Long = -1, val dumpInPlace: Boolean = true, val build: Long = -1, val library: Long = -1,
    val shaders: Long = -1, val saves: Long = -1, val logs: Long = -1, val free: Long = -1,
)

data class UiState(
    val hasPermission: Boolean = false,
    val setupDone: Boolean = false,
    val step: SetupStep = SetupStep.WELCOME,
    val dumpPath: String = "",
    val dumpMode: DumpMode = DumpMode.IN_PLACE,
    val report: DumpScanner.Report? = null,
    val scanning: Boolean = false,
    /** A picked tree that cannot be read in place: it is copied (pending until the player confirms). */
    val pendingCopy: Uri? = null,
    val status: GameStatus = GameStatus.DUMP_MISSING,
    val lastPlayed: Long = 0,
    val sizes: Sizes = Sizes(),
    /** The last game session (length, first frame, driver fallback, failed starts with a custom driver). */
    val session: GameSession.Outcome? = null,
    val supported: DumpScanner.Supported = DumpScanner.Supported(null, null, null, null),
    /** "Check the cache" (a shader cache without setup.json): running, and its last result as a sentence. */
    val shaderChecking: Boolean = false,
    val shaderCheck: String? = null,
    /** The phone's facts for the defaults (null until the first refresh with storage access). */
    val facts: PhoneDefaults.Facts? = null,
)

class AppViewModel(app: Application) : AndroidViewModel(app) {
    private val ctx: Context get() = getApplication()
    private val prefs = app.getSharedPreferences("ui", Context.MODE_PRIVATE)
    private val _ui = MutableStateFlow(UiState())
    val ui: StateFlow<UiState> = _ui.asStateFlow()
    val job = BuildService.flow

    init {
        BuildService.restore(app)
        val supported = loadSupported(app)
        _ui.value = UiState(
            hasPermission = Storage.hasAllFilesAccess(),
            setupDone = prefs.getBoolean("setup_done", false),
            step = runCatching { SetupStep.valueOf(prefs.getString("step", null)!!) }.getOrDefault(SetupStep.WELCOME),
            dumpPath = prefs.getString("dump_path", "").orEmpty(),
            dumpMode = runCatching { DumpMode.valueOf(prefs.getString("dump_mode", null)!!) }.getOrDefault(DumpMode.IN_PLACE),
            lastPlayed = prefs.getLong("last_played", 0),
            supported = supported,
        )
        refresh()
        viewModelScope.launch {
            var was = job.value.run
            job.collect { s ->
                if (was != s.run && !s.active) {
                    if (s.kind == JobKind.COPY_DUMP && s.run == RunState.DONE) useDump(Paths.dumpDir.path, DumpMode.COPIED)
                    refresh()
                }
                was = s.run
            }
        }
    }

    private fun update(f: (UiState) -> UiState) { _ui.value = f(_ui.value) }

    /** Re-reads permission, the dump, the game library and sizes. Called on resume (the player may have changed things outside). */
    fun refresh() {
        update { it.copy(hasPermission = Storage.hasAllFilesAccess()) }
        viewModelScope.launch {
            val s = _ui.value
            if (s.hasPermission) withContext(Dispatchers.IO) { Storage.ensureLayout(); runCatching { ensurePhoneDefaults() } }
            val session = withContext(Dispatchers.IO) { runCatching { GameSession.last(ctx) }.getOrNull() }
            update { it.copy(session = session) }
            val report = if (s.hasPermission && s.dumpPath.isNotEmpty()) {
                update { it.copy(scanning = it.report == null) }
                withContext(Dispatchers.IO) { DumpScanner.scan(s.dumpPath, s.supported) }
            } else null
            update { it.copy(report = report, scanning = false) }
            update { it.copy(status = statusOf(_ui.value)) }
            refreshSizes()
        }
    }

    private fun statusOf(s: UiState): GameStatus {
        if (!s.hasPermission) return GameStatus.NO_PERMISSION
        if (job.value.active) return GameStatus.BUSY
        if (s.report?.chosen == null) return GameStatus.DUMP_MISSING
        val kind = BuildService.libraryKind(ctx)
        if (kind == "fake") return GameStatus.READY_FAKE
        if (!GameLauncher.hasGameLibrary(ctx)) return GameStatus.NEEDS_BUILD
        // only a library this app built carries a stamp; one imported or pushed with adb is taken as it is
        if (kind == "built" && BuildService.libraryStale(ctx)) return GameStatus.NEEDS_REBUILD
        if (!BuildService.shaderCachePresent()) return GameStatus.SHADERS_MISSING
        return GameStatus.READY
    }

    fun goTo(step: SetupStep) {
        prefs.edit().putString("step", step.name).apply()
        update { it.copy(step = step) }
    }

    fun finishSetup() {
        prefs.edit().putBoolean("setup_done", true).apply()
        update { it.copy(setupDone = true) }
        refresh()
    }

    /** The system picker returned a tree: read it in place when it has a plain path, else offer to copy it. */
    fun pickedTree(uri: Uri) {
        val file = Copier.pathOf(uri)
        if (file != null && _ui.value.hasPermission) useDump(file.path, DumpMode.IN_PLACE)
        else update { it.copy(pendingCopy = uri) }
    }

    fun cancelCopy() = update { it.copy(pendingCopy = null) }

    fun startCopy(uri: Uri) {
        update { it.copy(pendingCopy = null) }
        BuildService.start(ctx, JobKind.COPY_DUMP, uri)
    }

    /** Copies a dump that is readable in place into <dataRoot>/dump anyway (for a card that may be removed). */
    fun copyInPlaceDump() {
        val path = _ui.value.report?.path ?: return
        BuildService.start(ctx, JobKind.COPY_DUMP, Uri.fromFile(File(path)))
    }

    fun useDump(path: String, mode: DumpMode) {
        update { it.copy(dumpPath = path, dumpMode = mode, scanning = true, report = null) }
        viewModelScope.launch {
            val report = withContext(Dispatchers.IO) { DumpScanner.scan(path, _ui.value.supported) }
            report.chosen?.let { c -> withContext(Dispatchers.IO) { writePlayPaths(c, report) } }
            prefs.edit().putString("dump_path", path).putString("dump_mode", mode.name).apply()
            update { it.copy(report = report, scanning = false) }
            update { it.copy(status = statusOf(_ui.value)) }
            refreshSizes()
        }
    }

    /**
     * pkp.ini [fs] paths of the chosen dump, plus the phone's platform keys when they are missing (the same values as
     * a test setup that pushes them). Unknown keys and the player's own values are kept (Ini line editor).
     */
    private fun writePlayPaths(c: DumpScanner.Candidate, r: DumpScanner.Report) {
        val ini = Ini.load(Paths.pkpIni)
        ini.set("fs", "exefs", c.exefs)
        ini.set("fs", "romfs", c.romfs)
        ini.set("fs", "romfs_base", c.romfsBase)
        if (r.aocDir.isNotEmpty() && ini.get("fs", "aoc_dir") == null) ini.set("fs", "aoc_dir", r.aocDir)
        ini.save(Paths.pkpIni)
        ensurePhoneDefaults()
    }

    /** [PhoneDefaults] on the two ini files. */
    private fun ensurePhoneDefaults() {
        val ini = Ini.load(Paths.pkpIni)
        val controls = Ini.load(Paths.controlsIni)
        val facts = DeviceFacts.read(ctx)
        update { it.copy(facts = facts) }
        ensurePromptIcons()
        val changed = PhoneDefaults.apply(ini, controls, Paths.dataRoot.path, facts)
        ini.save(Paths.pkpIni)
        if (changed) controls.save(Paths.controlsIni)
    }

    /** The button prompt icon pack ([prompts] dir, set by PhoneDefaults) in <data root>/assets/prompts: copied when missing or of
     *  another size (an app update with a newer pack replaces it). Without it a PlayStation or Xbox pad shows the game's own glyphs. */
    private fun ensurePromptIcons() {
        runCatching {
            val dst = File(Paths.assetsDir, "prompts/prompt_icons.pak")
            ctx.assets.openFd("prompts/prompt_icons.pak").use { fd ->
                if (dst.length() != fd.length) {
                    dst.parentFile?.mkdirs()
                    fd.createInputStream().use { i -> dst.outputStream().use { i.copyTo(it) } }
                }
            }
        }
    }

    fun startBuild(fake: Boolean) = BuildService.start(ctx, JobKind.BUILD, fake = fake)
    fun startShaders(fake: Boolean, small: Boolean = false) = BuildService.start(ctx, JobKind.SHADERS, fake = fake, small = small)
    fun cancelJob() = BuildService.cancel(ctx)

    /** Crash-loop answers (Home): back to the system driver, or start counting again. */
    fun useSystemDriver() = viewModelScope.launch {
        withContext(Dispatchers.IO) { GpuDriver.useSystem(ctx); GameSession.resetFailures(ctx) }
        refresh()
    }
    fun resetDriverFailures() = viewModelScope.launch { withContext(Dispatchers.IO) { GameSession.resetFailures(ctx) }; refresh() }
    fun checkShaderCache() = viewModelScope.launch {
        update { it.copy(shaderChecking = true, shaderCheck = null) }
        val r = withContext(Dispatchers.IO) { ShaderCache.check() }
        val msg = when {
            r.ok -> ctx.getString(R.string.ui_shaders_check_ok, r.entries, r.rows)
            r.error != null -> ctx.getString(R.string.ui_shaders_check_unreadable, r.error)
            r.badSegment != null -> ctx.getString(R.string.ui_shaders_check_bad_segment, r.badSegment)
            r.orphans > 0 -> ctx.getString(R.string.ui_shaders_check_orphans, r.orphans)
            else -> ctx.getString(R.string.ui_shaders_check_short, r.rows, ShaderCache.BLOCKS_655360)
        }
        update { it.copy(shaderChecking = false, shaderCheck = msg) }
        refresh()
    }
    fun dismissJob() { BuildService.clear(ctx); refresh() }
    fun importLibrary(uri: Uri) = BuildService.start(ctx, JobKind.IMPORT_LIBRARY, uri)
    fun importShaders(uri: Uri) = BuildService.start(ctx, JobKind.IMPORT_SHADERS, uri)

    fun played() {
        val now = System.currentTimeMillis()
        prefs.edit().putLong("last_played", now).apply()
        update { it.copy(lastPlayed = now) }
    }

    fun refreshSizes() {
        viewModelScope.launch {
            val s = _ui.value
            val sizes = withContext(Dispatchers.IO) {
                val c = s.report?.chosen
                Sizes(
                    dump = c?.let { it.romfsBytes + it.exefsBytes } ?: -1,
                    dumpInPlace = s.dumpMode == DumpMode.IN_PLACE,
                    build = dirSize(Paths.buildDir),
                    library = Paths.gameLibrary(ctx).takeIf { it.isFile }?.length() ?: -1,
                    shaders = dirSize(Paths.shaderCacheDir),
                    saves = dirSize(Paths.saveDir),
                    logs = dirSize(Paths.logsDir),
                    free = runCatching { Storage.freeBytes() }.getOrDefault(-1),
                )
            }
            update { it.copy(sizes = sizes) }
        }
    }

    /** Deletes the build work area and the installed library (the dump, shaders and saves stay). */
    fun deleteBuild() = viewModelScope.launch {
        withContext(Dispatchers.IO) {
            Paths.buildDir.deleteRecursively(); Paths.buildDir.mkdirs()
            Paths.gameDir(ctx).deleteRecursively()
            BuildService.forgetLibrary(ctx)
        }
        refresh()
    }

    fun deleteShaders() = viewModelScope.launch {
        withContext(Dispatchers.IO) { Paths.shaderCacheDir.deleteRecursively(); Paths.shaderCacheDir.mkdirs() }
        refresh()
    }

    fun deleteSaves() = viewModelScope.launch {
        withContext(Dispatchers.IO) { Paths.saveDir.deleteRecursively(); Paths.saveDir.mkdirs() }
        refresh()
    }

    fun deleteCopiedDump() = viewModelScope.launch {
        if (_ui.value.dumpMode != DumpMode.COPIED) return@launch
        withContext(Dispatchers.IO) { Paths.dumpDir.deleteRecursively() }
        prefs.edit().remove("dump_path").apply()
        update { it.copy(dumpPath = "", report = null) }
        refresh()
    }

    companion object {
        fun dirSize(f: File): Long = if (!f.exists()) -1 else f.walkBottomUp().filter { it.isFile }.sumOf { it.length() }

        /** A repo file packed into the APK's assets at build time (app/build.gradle.kts uiAssets); "" when missing. */
        fun readText(ctx: Context, asset: String): String =
            runCatching { ctx.assets.open(asset).bufferedReader().use { it.readText() } }.getOrDefault("")

        /** dump_hashes.json: what this release supports. */
        fun loadSupported(ctx: Context): DumpScanner.Supported = runCatching {
            val j = JSONObject(readText(ctx, "dump_hashes.json"))
            val r = j.optJSONObject("romfs")
            DumpScanner.Supported(
                j.optString("main_build_id").lowercase().ifEmpty { null }, j.optString("version").ifEmpty { null },
                r?.optLong("files")?.takeIf { it > 0 }, r?.optLong("bytes")?.takeIf { it > 0 },
            )
        }.getOrDefault(DumpScanner.Supported(null, null, null, null))
    }
}
