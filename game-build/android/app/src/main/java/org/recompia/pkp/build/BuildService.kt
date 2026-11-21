// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.build

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.Uri
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import org.json.JSONObject
import org.recompia.pkp.BuildConfig
import org.recompia.pkp.GameLauncher
import org.recompia.pkp.Paths
import org.recompia.pkp.R
import java.io.File
import java.io.IOException
import java.io.PrintWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.TimeUnit

enum class JobKind { BUILD, IMPORT_LIBRARY, IMPORT_SHADERS, COPY_DUMP, SHADERS }

enum class RunState { IDLE, RUNNING, CANCELLING, DONE, ERROR, CANCELLED, INTERRUPTED }

/** What the UI shows of a job. Lives in [BuildService.state]; the last finished state is kept in preferences (process death). */
data class BuildState(
    val kind: JobKind = JobKind.BUILD,
    val run: RunState = RunState.IDLE,
    val fake: Boolean = false,
    val step: String = "",
    val stepIndex: Int = 0,
    val stepCount: Int = 0,
    val done: Long = 0,
    val total: Long = 0,
    val unit: String = "",
    val message: String = "",
    val error: String = "",
    val logFile: String = "",
    val lines: List<String> = emptyList(),
    val startedAt: Long = 0,
    val etaMs: Long? = null,
    /** The build finished without a shader cache on the phone: the game needs one (import from a PC). */
    val shadersMissing: Boolean = false,
    /** Debug test of the real translator on a partial romfs (kept so Continue runs the same way). */
    val small: Boolean = false,
) {
    val active get() = run == RunState.RUNNING || run == RunState.CANCELLING
    val percent: Int? get() = if (total > 0) ((100 * done) / total).toInt().coerceIn(0, 100) else null
}

/**
 * The foreground service for every long job: the on-device build (the build driver, then the shader step), importing a game
 * library or a shader cache built on a PC, and copying a dump that cannot be read in place. One job at a time; a wake lock is held
 * while it runs; the notification shows step and percent and has Cancel. A build is resumable: the driver keeps finished work.
 */
class BuildService : Service() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    @Volatile private var process: Process? = null
    @Volatile private var cancelled = false
    /** Debug test of the real translator on a partial romfs: no 12 GiB demand. */
    private var small = false
    private var wake: PowerManager.WakeLock? = null
    private var lastNotify = 0L
    private val eta = Eta()

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_CANCEL) {
            cancel()
            return START_NOT_STICKY
        }
        val kind = intent?.getStringExtra(EXTRA_KIND)?.let { runCatching { JobKind.valueOf(it) }.getOrNull() }
        if (kind == null || state.value.active) {
            if (!state.value.active) stopSelf(startId)
            return START_NOT_STICKY
        }
        val fake = intent.getBooleanExtra(EXTRA_FAKE, false)
        small = intent.getBooleanExtra(EXTRA_SMALL, false)
        cancelled = false
        eta.reset()
        val log = openLog(kind)
        set(BuildState(kind = kind, run = RunState.RUNNING, fake = fake, small = small, startedAt = System.currentTimeMillis(), logFile = log.second?.path.orEmpty()))
        startForegroundCompat()
        wake = (getSystemService(POWER_SERVICE) as PowerManager).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "Recompia:build").apply {
            setReferenceCounted(false)
            acquire(TimeUnit.HOURS.toMillis(12))
        }
        val uri = intent.getStringExtra(EXTRA_URI)?.let(Uri::parse)
        scope.launch {
            val out = log.first
            out.println("=== ${kind.name.lowercase()} started ${Date()} (app ${appStamp(this@BuildService)})")
            val end = try {
                when (kind) {
                    JobKind.BUILD -> build(fake, out)
                    JobKind.IMPORT_LIBRARY -> importLibrary(uri!!, out)
                    JobKind.IMPORT_SHADERS -> importShaders(uri!!, out)
                    JobKind.COPY_DUMP -> copyDump(uri!!, out)
                    JobKind.SHADERS -> translateShaders(fake, out)
                }
                RunState.DONE
            } catch (e: InterruptedException) {
                RunState.CANCELLED
            } catch (e: Exception) {
                if (cancelled) RunState.CANCELLED else {
                    Log.e(Paths.TAG, "job ${kind.name} failed", e)
                    out.println("ERROR: ${e.message}")
                    state.update { it.copy(error = it.error.ifEmpty { e.message ?: e.javaClass.simpleName }) }
                    RunState.ERROR
                }
            }
            out.println("=== ended: ${end.name.lowercase()} ${state.value.error}")
            out.close()
            finish(end)
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        process?.destroy()
        scope.cancel()
        wake?.takeIf { it.isHeld }?.release()
        super.onDestroy()
    }

    private fun cancel() {
        if (!state.value.active) return
        cancelled = true
        set(state.value.copy(run = RunState.CANCELLING))
        val p = process ?: return
        p.destroy() // SIGTERM: the driver stops its children and exits 2 (resumable)
        scope.launch {
            if (!p.waitFor(15, TimeUnit.SECONDS)) p.destroyForcibly()
            killStrays()
        }
    }

    /** A tool child the driver did not take down with it (seen: the shader translator after a pause): our own processes only. */
    private fun killStrays() {
        val me = android.os.Process.myPid()
        File("/proc").listFiles()?.forEach { d ->
            val pid = d.name.toIntOrNull() ?: return@forEach
            if (pid == me) return@forEach
            val cmd = runCatching { File(d, "cmdline").readText() }.getOrNull() ?: return@forEach
            if (STRAY_TOOLS.any { cmd.contains(it) }) runCatching { android.os.Process.killProcess(pid) }
        }
    }

    private fun finish(end: RunState) {
        set(state.value.copy(run = end, etaMs = null))
        persist(this, state.value)
        wake?.takeIf { it.isHeld }?.release()
        ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
        if (end == RunState.DONE || end == RunState.ERROR) notifyEnd(end)
        stopSelf()
    }

    // ---- jobs --------------------------------------------------------------------------------------------------------------

    private fun build(fake: Boolean, out: PrintWriter) {
        val cmd: List<String>
        val env = mutableMapOf<String, String>()
        val lib = Paths.gameLibrary(this)
        val work = Paths.buildDir
        if (fake) {
            cmd = FakeBuilder.command(this) ?: throw IOException("the fake builder exists only in debug builds")
        } else {
            val tc = tools(out)
            val dump = dumpForBuild() ?: throw IOException(getString(R.string.ui_build_err_no_dump))
            work.mkdirs()
            cmd = listOf(
                Toolchain.python(this).path, "-I", "-u", Toolchain.driver(this).path,
                "--dump", dump, "--work", work.path, "--out", lib.path, "--jobs", "0",
                "--native-lib-dir", applicationInfo.nativeLibraryDir, "--toolchain-dir", tc.path,
            )
            env.putAll(pythonEnv(tc))
        }
        env["HOME"] = filesDir.path
        env["TMPDIR"] = cacheDir.path
        lib.parentFile?.mkdirs()
        out.println("command: ${cmd.joinToString(" ")}")
        val done = runTool(cmd, env, out)
        if (!fake) {
            // a PC-built library left in build/ would replace this one at the next start (GameLauncher dev shortcut)
            if (Paths.devGameLibrary.delete()) out.println("removed the older dev library ${Paths.devGameLibrary}")
            if (File(done).path != lib.path) File(done).copyTo(lib, overwrite = true)
        }
        record(this, if (fake) "fake" else "built")
        shaderStep(out)
    }

    /** The on-device toolchain, unpacked when its version changed; the step "tools" while that runs. */
    private fun tools(out: PrintWriter): File {
        if (!Toolchain.present(this)) throw IOException(getString(R.string.ui_build_err_no_tools))
        if (Toolchain.needsUnpack(this)) {
            step("tools", 0, 0)
            out.println("unpacking the build tools into ${Toolchain.dir(this)}")
            Toolchain.unpack(this, { cancelled }) { done, total -> progress(done, total, "") }
        }
        return Toolchain.dir(this)
    }

    private fun pythonEnv(tc: File) = mapOf(
        "RECOMPIA_PYTHONHOME" to File(tc, "python").path, "PYTHONHOME" to File(tc, "python").path,
        "LD_LIBRARY_PATH" to File(tc, "python/lib").path, "HOME" to filesDir.path, "TMPDIR" to cacheDir.path,
    )

    /**
     * The on-device shader translation (tools/android/shaders): build_shaders.py with the on-device Python and the
     * translator libspirv_harness.so. Resumable (finished chunks are kept); needs 12 GiB free at the start, checked here first so the
     * player gets a clear message before anything runs.
     */
    private fun translateShaders(fake: Boolean, out: PrintWriter) {
        val cmd: List<String>
        val env = mutableMapOf<String, String>()
        if (fake) {
            cmd = FakeBuilder.command(this, "shaders") ?: throw IOException("the fake builder exists only in debug builds")
        } else {
            if (!File(applicationInfo.nativeLibraryDir, SHADER_TOOL).isFile) throw IOException(getString(R.string.ui_shaders_err_no_tool))
            if (!shaderCachePresent() && !small) {
                val free = Paths.dataRoot.also { it.mkdirs() }.usableSpace
                // a resumed run needs less: what is already in shader_cache counts
                val have = maxOf(0L, org.recompia.pkp.ui.AppViewModel.dirSize(Paths.shaderCacheDir))
                if (free + have < SHADER_FREE) throw IOException(getString(R.string.ui_shaders_err_space, gib(SHADER_FREE - have), gib(free)))
            }
            val tc = tools(out)
            val dir = File(filesDir, "shaders").apply { mkdirs() }
            for (n in listOf("build_shaders.py", "extract_corpus.py")) {
                assets.open("shaders/$n").use { i -> File(dir, n).outputStream().use { i.copyTo(it) } }
            }
            val script = File(dir, "build_shaders.py")
            val args = mutableListOf(
                Toolchain.python(this).path, "-I", "-u", script.path, "--data", Paths.dataRoot.path,
                "--native-lib-dir", applicationInfo.nativeLibraryDir, "--extractor", File(dir, "extract_corpus.py").path, "--jobs", "6",
            )
            if (small) args += listOf("--min-free-gib", "1")
            // a dump read in place lives outside <data>/dump: pass its paths when the script takes them
            val ini = org.recompia.pkp.ui.Ini.load(Paths.pkpIni)
            val romfs = ini.get("fs", "romfs")?.ifBlank { null }
            val exefs = ini.get("fs", "exefs")?.ifBlank { null }
            if (script.readText().contains("\"--romfs\"") && romfs != null && exefs != null) {
                args += listOf("--romfs", romfs, "--main", File(exefs, "main").path)
                ini.get("fs", "romfs_base")?.ifBlank { null }?.let { if (script.readText().contains("--romfs-base")) args += listOf("--romfs-base", it) }
            }
            cmd = args
            env.putAll(pythonEnv(tc))
        }
        out.println("command: ${cmd.joinToString(" ")}")
        runTool(cmd, env, out)
        state.update { it.copy(shadersMissing = !fake && !shaderCachePresent()) }
    }

    private fun dumpForBuild(): String? = exefsForBuild()


    /** Shaders: a PC cache already on the phone is used as is; on-device translation is not delivered yet. */
    private fun shaderStep(out: PrintWriter) {
        step("shaders", 0, 0)
        val ok = shaderCachePresent()
        out.println(if (ok) "shader cache found in ${Paths.shaderCacheDir}" else "no shader cache on the phone: import one from a PC install")
        state.update { it.copy(shadersMissing = !ok) }
    }

    private fun runTool(cmd: List<String>, env: Map<String, String>, out: PrintWriter): String {
        val pb = ProcessBuilder(cmd).redirectErrorStream(true)
        pb.environment().putAll(env)
        val p = pb.start()
        process = p
        var donePath: String? = null
        var error: String? = null
        p.inputStream.bufferedReader().useLines { lines ->
            for (line in lines) {
                out.println(line)
                if (line.startsWith("@@")) Log.i(Paths.TAG, line) // test harnesses read the protocol from logcat
                when (val e = BuildProtocol.parse(line)) {
                    is BuildEvent.Step -> step(e.name, e.index, e.count)
                    is BuildEvent.Progress -> progress(e.done, e.total, e.unit)
                    is BuildEvent.Log -> { state.update { it.copy(message = e.text) }; addLine(e.text) }
                    is BuildEvent.Done -> donePath = e.path
                    is BuildEvent.Error -> { error = e.text; state.update { it.copy(error = e.text) } }
                    is BuildEvent.Text -> addLine(e.text)
                }
            }
        }
        val code = p.waitFor()
        process = null
        out.println("exit code $code")
        if (cancelled || code == 2) throw InterruptedException()
        if (code != 0 || donePath == null) throw IOException(error ?: getString(R.string.ui_build_err_exit, code))
        return donePath!!
    }

    private fun importLibrary(uri: Uri, out: PrintWriter) {
        step("copy", 1, 1)
        val dst = Paths.devGameLibrary
        // the library found at the conventional place (<dataRoot>/build/libpkpgame.so): checked and installed where it is
        if (uri.scheme == "file" && uri.path?.let { File(it).canonicalPath } == dst.canonicalPath) {
            checkLibrary(dst)
            out.println("using ${dst.length()} bytes found at $dst")
            GameLauncher.syncDevLibrary(this) ?: throw IOException("cannot install the library into app storage")
            record(this, "imported")
            return
        }
        val total = runCatching { contentResolver.openFileDescriptor(uri, "r")?.use { it.statSize } }.getOrNull() ?: -1L
        dst.parentFile?.mkdirs()
        val part = File(dst.parentFile, dst.name + ".part")
        var moved = 0L
        (contentResolver.openInputStream(uri) ?: throw IOException("cannot open $uri")).use { input ->
            part.outputStream().use { o ->
                val buf = ByteArray(1 shl 20)
                while (true) {
                    if (cancelled) throw InterruptedException()
                    val n = input.read(buf)
                    if (n < 0) break
                    o.write(buf, 0, n)
                    moved += n
                    progress(moved, total, "")
                }
            }
        }
        try {
            checkLibrary(part)
        } catch (e: IOException) {
            part.delete()
            throw e
        }
        dst.delete()
        if (!part.renameTo(dst)) throw IOException("cannot rename $part")
        out.println("imported ${dst.length()} bytes to $dst")
        GameLauncher.syncDevLibrary(this) ?: throw IOException("cannot install the library into app storage")
        record(this, "imported")
    }

    private fun importShaders(uri: Uri, out: PrintWriter) {
        step("copy", 1, 1)
        val root = Copier.nodeOf(this, uri) ?: throw IOException("cannot read $uri")
        // the picked folder is shader_cache itself, or a PC install that holds it
        val cache = if (isShaderCache(root)) root else root.child("shader_cache")?.takeIf { isShaderCache(it) }
            ?: throw IOException(getString(R.string.ui_import_shaders_not_found))
        val order = root.child("shader_order.txt") ?: cache.child("shader_order.txt")
        val skip = { n: Node -> n.isDir && n.name.equals("corpus", true) || n.name.startsWith("pipeline_") } // corpus: never read at run time
        val total = Copier.size(cache, skip) + (order?.length ?: 0)
        checkFree(total)
        var moved = 0L
        out.println("copying ${cache.name} ($total bytes) to ${Paths.shaderCacheDir}")
        Copier.copy(cache, Paths.shaderCacheDir, { cancelled }, skip) { moved += it; progress(moved, total, "") }
        order?.let { Copier.copy(it, File(Paths.shaderCacheDir, "shader_order.txt"), { cancelled }) { n -> moved += n; progress(moved, total, "") } }
    }

    /** ELF64 AArch64 exporting SDL_main and pkp_rt_api ([LibraryCheck]), else an IOException with the player's message. */
    private fun checkLibrary(f: File) {
        when (LibraryCheck.check(f)) {
            LibraryCheck.Result.OK -> {}
            LibraryCheck.Result.NOT_ELF -> throw IOException(getString(R.string.ui_import_lib_not_elf))
            LibraryCheck.Result.NOT_ARM64 -> throw IOException(getString(R.string.ui_import_lib_not_arm64))
            LibraryCheck.Result.NO_EXPORTS -> throw IOException(getString(R.string.ui_import_lib_no_exports))
        }
    }

    private fun isShaderCache(n: Node) = n.isDir && (n.child("spirv.archive") != null || n.child("spirv.preload.pack") != null)

    /** Copies the parts of the dump the game needs (the newest valid update, else the base, plus the base romfs under an update) into <dataRoot>/dump, in the same layout so the scanner reads it as before. */
    private fun copyDump(uri: Uri, out: PrintWriter) {
        step("copy", 1, 1)
        val root = Copier.nodeOf(this, uri) ?: throw IOException("cannot read $uri")
        val parts = mutableListOf<Pair<Node, File>>()
        val dump = Paths.dumpDir
        val updates = root.child("updates")?.children()?.filter { it.isDir && it.child("exefs") != null }
            ?.sortedByDescending { it.name.toLongOrNull() ?: -1 }
        val base = root.child("base")
        when {
            root.child("exefs") != null -> { parts += root.child("exefs")!! to File(dump, "exefs"); root.child("romfs")?.let { parts += it to File(dump, "romfs") } }
            !updates.isNullOrEmpty() -> {
                val u = updates.first()
                u.children().filter { it.isDir }.forEach { parts += it to File(dump, "updates/${u.name}/${it.name}") }
                base?.child("romfs")?.let { parts += it to File(dump, "base/romfs") }
            }
            base != null -> base.children().filter { it.isDir }.forEach { parts += it to File(dump, "base/${it.name}") }
            else -> throw IOException(getString(R.string.ui_dump_err_no_layout))
        }
        val total = parts.sumOf { Copier.size(it.first) }
        checkFree(total)
        var moved = 0L
        for ((src, dst) in parts) {
            out.println("copying ${src.name} to $dst")
            Copier.copy(src, dst, { cancelled }) { moved += it; progress(moved, total, src.name) }
        }
    }

    private fun checkFree(need: Long) {
        val free = Paths.dataRoot.also { it.mkdirs() }.usableSpace
        if (need > free) throw IOException(getString(R.string.ui_err_no_space, gib(need), gib(free)))
    }

    // ---- state and notification ----------------------------------------------------------------------------------------------

    private fun step(name: String, index: Int, count: Int) {
        eta.reset()
        set(state.value.copy(step = name, stepIndex = index, stepCount = count, done = 0, total = 0, unit = "", etaMs = null))
        persist(this, state.value)
    }

    private fun progress(done: Long, total: Long, unit: String) {
        val s = state.value
        set(s.copy(done = done, total = total, unit = unit, etaMs = eta.update(s.step, done, total)))
    }

    private fun addLine(text: String) = state.update { it.copy(lines = (it.lines + text).takeLast(MAX_LINES)) }

    private fun set(s: BuildState) {
        state.value = s
        val now = System.currentTimeMillis()
        if (s.active && now - lastNotify > 1000) {
            lastNotify = now
            getSystemService(NotificationManager::class.java).notify(NOTIFY_ID, notification(s))
        }
    }

    private fun startForegroundCompat() {
        val type = if (Build.VERSION.SDK_INT >= 34) ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE else 0
        ServiceCompat.startForeground(this, NOTIFY_ID, notification(state.value), type)
    }

    private fun channel() {
        val nm = getSystemService(NotificationManager::class.java)
        if (nm.getNotificationChannel(CHANNEL) == null) {
            nm.createNotificationChannel(NotificationChannel(CHANNEL, getString(R.string.ui_notify_channel), NotificationManager.IMPORTANCE_LOW))
        }
    }

    private fun openApp(): PendingIntent = PendingIntent.getActivity(
        this, 0, packageManager.getLaunchIntentForPackage(packageName), PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)

    private fun notification(s: BuildState): Notification {
        channel()
        val cancel = PendingIntent.getService(this, 1, Intent(this, BuildService::class.java).setAction(ACTION_CANCEL), PendingIntent.FLAG_IMMUTABLE)
        val title = getString(when (s.kind) {
            JobKind.BUILD -> R.string.ui_notify_build
            JobKind.IMPORT_LIBRARY -> R.string.ui_notify_import_lib
            JobKind.IMPORT_SHADERS -> R.string.ui_notify_import_shaders
            JobKind.COPY_DUMP -> R.string.ui_notify_copy_dump
            JobKind.SHADERS -> R.string.ui_notify_shaders
        })
        val step = stepLabel(this, s)
        val pct = s.percent
        return NotificationCompat.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ui_ic_notification)
            .setContentTitle(title)
            .setContentText(if (pct != null) "$step, $pct%" else step)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setSilent(true)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            .setProgress(100, pct ?: 0, pct == null)
            .setContentIntent(openApp())
            .addAction(0, getString(R.string.ui_cancel), cancel)
            .build()
    }

    private fun notifyEnd(end: RunState) {
        val n = NotificationCompat.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ui_ic_notification)
            .setContentTitle(getString(if (end == RunState.DONE) R.string.ui_notify_done else R.string.ui_notify_failed))
            .setContentText(state.value.error.ifEmpty { stepLabel(this, state.value) })
            .setAutoCancel(true)
            .setContentIntent(openApp())
            .build()
        runCatching { getSystemService(NotificationManager::class.java).notify(NOTIFY_ID + 1, n) }
    }

    /** The log of this job: <dataRoot>/logs when it can be written, else app storage. */
    private fun openLog(kind: JobKind): Pair<PrintWriter, File?> {
        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(Date())
        for (dir in listOf(Paths.logsDir, File(filesDir, "logs"))) {
            val f = File(dir, "${kind.name.lowercase()}-$stamp.log")
            val w = runCatching { dir.mkdirs(); PrintWriter(f.outputStream().bufferedWriter(), true) }.getOrNull()
            if (w != null) return w to f
        }
        return PrintWriter(java.io.Writer.nullWriter()) to null
    }

    companion object {
        private const val CHANNEL = "build"
        private const val NOTIFY_ID = 4201
        private const val ACTION_CANCEL = "org.recompia.pkp.build.CANCEL"
        private const val EXTRA_KIND = "kind"
        private const val EXTRA_FAKE = "fake"
        private const val EXTRA_SMALL = "small"
        private const val EXTRA_URI = "uri"
        private const val MAX_LINES = 200
        private const val PREFS = "build"

        private val _state = MutableStateFlow(BuildState())
        private val state get() = _state
        val flow: StateFlow<BuildState> get() = _state

        fun start(ctx: Context, kind: JobKind, uri: Uri? = null, fake: Boolean = false, small: Boolean = false) {
            val i = Intent(ctx, BuildService::class.java).putExtra(EXTRA_KIND, kind.name).putExtra(EXTRA_FAKE, fake).putExtra(EXTRA_SMALL, small)
            uri?.let { i.putExtra(EXTRA_URI, it.toString()) }
            ContextCompat.startForegroundService(ctx, i)
        }

        /** A library copied to <dataRoot>/build/libpkpgame.so (the documented place for a PC build) that is not installed yet. */
        fun foundLibrary(ctx: Context): File? {
            val f = Paths.devGameLibrary
            val mine = Paths.gameLibrary(ctx)
            return f.takeIf { it.isFile && (it.length() != mine.length() || it.lastModified() != mine.lastModified()) }
        }

        fun cancel(ctx: Context) {
            ctx.startService(Intent(ctx, BuildService::class.java).setAction(ACTION_CANCEL))
        }

        /** The test entry points: debug and internalTest builds only (src/debug + TEST_ENTRY_POINTS); in release the constant
         *  lets R8 drop the buttons and the resource shrinker their strings. */
        fun fakeAvailable(): Boolean = BuildConfig.TEST_ENTRY_POINTS && FakeBuilder.available

        /** A usable shader cache (tools/android/shaders: the readiness rule): setup.json complete with translator >= 2, results.tsv, at least one pack. */
        fun shaderCachePresent(): Boolean {
            val c = Paths.shaderCacheDir
            val setup = runCatching { JSONObject(File(c, "setup.json").readText()) }.getOrNull() ?: return false
            return setup.optBoolean("complete") && setup.optInt("translator") >= 2 && File(c, "spirv/results.tsv").isFile &&
                (File(c, "spirv.archive").list()?.any { it.endsWith(".pack") } == true)
        }

        /** The folder holding exefs/main (the build driver's --dump), from pkp.ini [fs] exefs written by the dump step; null without one. */
        fun exefsForBuild(): String? {
            val exefs = org.recompia.pkp.ui.Ini.load(Paths.pkpIni).get("fs", "exefs")?.takeIf { it.isNotBlank() } ?: return null
            val f = File(exefs).let { if (it.isAbsolute) it else File(Paths.dataRoot, exefs) }
            return f.parentFile?.takeIf { File(f, "main").isFile }?.path
        }

        const val SHADER_TOOL = "libspirv_harness.so"
        private val STRAY_TOOLS = listOf(SHADER_TOOL, "libllvm_exec.so", "libpython_exec.so")
        /** Free space build_shaders.py demands at the start (it refuses below that). */
        const val SHADER_FREE = 12L shl 30

        /** The phone carries the shader translator and its scripts. */
        fun shaderToolPresent(ctx: Context) = File(ctx.applicationInfo.nativeLibraryDir, SHADER_TOOL).isFile &&
            runCatching { ctx.assets.open("shaders/build_shaders.py").close() }.isSuccess

        /**
         * A fresh process: the state of the last job from preferences. A job that was running when the process died is
         * INTERRUPTED (the driver keeps finished work, so starting again resumes it).
         */
        fun restore(ctx: Context) {
            if (_state.value.run != RunState.IDLE) return
            val j = runCatching { JSONObject(ctx.getSharedPreferences(PREFS, MODE_PRIVATE).getString("last", null) ?: return) }.getOrNull() ?: return
            val run = runCatching { RunState.valueOf(j.getString("run")) }.getOrDefault(RunState.IDLE)
            _state.value = BuildState(
                kind = runCatching { JobKind.valueOf(j.getString("kind")) }.getOrDefault(JobKind.BUILD),
                run = if (run == RunState.RUNNING || run == RunState.CANCELLING) RunState.INTERRUPTED else run,
                fake = j.optBoolean("fake"), step = j.optString("step"), stepIndex = j.optInt("index"), stepCount = j.optInt("count"),
                error = j.optString("error"), logFile = j.optString("log"), startedAt = j.optLong("started"),
                shadersMissing = j.optBoolean("shadersMissing"), small = j.optBoolean("small"),
            )
        }

        /** Forget a finished job (the UI dismissed its result). */
        fun clear(ctx: Context) {
            if (_state.value.active) return
            _state.value = BuildState()
            ctx.getSharedPreferences(PREFS, MODE_PRIVATE).edit().remove("last").apply()
        }

        private fun persist(ctx: Context, s: BuildState) {
            val j = JSONObject().put("kind", s.kind.name).put("run", s.run.name).put("fake", s.fake).put("step", s.step)
                .put("index", s.stepIndex).put("count", s.stepCount).put("error", s.error).put("log", s.logFile)
                .put("started", s.startedAt).put("shadersMissing", s.shadersMissing).put("small", s.small)
            ctx.getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString("last", j.toString()).apply()
        }

        /** The app and runtime a library was built against: a different stamp later means the game must be built again. */
        fun appStamp(ctx: Context): String {
            val pi = ctx.packageManager.getPackageInfo(ctx.packageName, 0)
            val rt = File(ctx.applicationInfo.nativeLibraryDir, "libpkp_rt.so")
            return "${pi.longVersionCode}/${pi.lastUpdateTime}/${rt.length()}"
        }

        /** A library built here must be built again only when (a) the runtime's exported pkp_rt_api differs from the one the library
         *  exports (the API it was built for), or (b) the build kit's compile inputs changed (lifter, flags, native headers, sdk:
         *  the kit version recorded at build time differs from this app's). Runtime changes inside the same API keep the library. */
        fun libraryStale(ctx: Context): Boolean {
            val p = ctx.getSharedPreferences(PREFS, MODE_PRIVATE)
            val built = LibraryCheck.exportU32(Paths.gameLibrary(ctx), "pkp_rt_api") ?: p.getLong("library_api", -1L).takeIf { it >= 0 }
            val runtime = LibraryCheck.exportU32(File(ctx.applicationInfo.nativeLibraryDir, "libpkp_rt.so"), "pkp_rt_api")
            if (built != null && runtime != null && built != runtime) return true
            val kit = p.getString("library_kit", null)
            return kit != null && kit != Toolchain.version(ctx)
        }

        private fun record(ctx: Context, how: String) {
            ctx.getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                .putString("library_kind", how).putString("library_stamp", appStamp(ctx)).putLong("library_time", System.currentTimeMillis())
                .putString("library_kit", if (how == "built") Toolchain.version(ctx) else null)
                .putLong("library_api", LibraryCheck.exportU32(Paths.gameLibrary(ctx), "pkp_rt_api") ?: -1L).apply()
        }

        /** How the installed library came to be: built, imported, fake, or null (pushed with adb or none). */
        fun libraryKind(ctx: Context): String? = ctx.getSharedPreferences(PREFS, MODE_PRIVATE).getString("library_kind", null)
        fun libraryStamp(ctx: Context): String? = ctx.getSharedPreferences(PREFS, MODE_PRIVATE).getString("library_stamp", null)
        fun forgetLibrary(ctx: Context) = ctx.getSharedPreferences(PREFS, MODE_PRIVATE).edit()
            .remove("library_kind").remove("library_stamp").remove("library_time").apply()

        fun stepLabel(ctx: Context, s: BuildState): String = ctx.getString(when (s.step) {
            "tools" -> R.string.ui_step_tools
            "check" -> R.string.ui_step_check
            "lift" -> R.string.ui_step_lift
            "compile" -> R.string.ui_step_compile
            "finish" -> if (s.kind == JobKind.SHADERS) R.string.ui_step_shaders_finish else R.string.ui_step_link
            "link" -> R.string.ui_step_link
            "shaders" -> R.string.ui_step_shaders
            "copy" -> R.string.ui_step_copy
            "extract" -> R.string.ui_step_extract
            "translate" -> R.string.ui_step_translate
            else -> R.string.ui_step_working
        })

        fun gib(b: Long) = String.format(Locale.getDefault(), "%.1f", b / 1073741824.0)
    }
}

/** Bridge to the debug-only fake builder (src/debug/.../FakeBuilderImpl.kt): release builds have no such class, so it cannot run there. */
object FakeBuilder {
    private val impl: Class<*>? = runCatching { Class.forName("org.recompia.pkp.build.FakeBuilderImpl") }.getOrNull()
    val available: Boolean get() = impl != null

    @Suppress("UNCHECKED_CAST")
    fun command(ctx: Context, what: String = "build"): List<String>? =
        impl?.getMethod("command", Context::class.java, String::class.java)?.invoke(null, ctx, what) as List<String>?
}
