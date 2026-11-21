// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import android.app.ActivityManager
import android.content.Context
import org.json.JSONObject
import org.recompia.pkp.Paths
import java.io.File

/**
 * What happened in the last game session, for Home: its length, whether it reached the first frame, and whether the runtime fell back
 * to the system driver. The game runs in the `:game` process, so the two processes share a small file: GameActivity writes the
 * start and the end, the UI reads it together with the runtime log once the game process is gone.
 * A custom GPU driver that ends two starts in a row before the first frame is reported as a crash loop (Home then offers the
 * system driver).
 */
object GameSession {
    /** Runtime log lines (runtime/src/gfx/nvn/nvn_replay.cpp, runtime/src/os/os_android_vkdriver.cpp). */
    private const val FIRST_FRAME = "first frame with"
    private const val FALLBACK = "using the system driver"

    data class Outcome(val lengthMs: Long, val firstFrame: Boolean, val fallback: String?, val driverActive: Boolean, val failedStarts: Int)

    private fun file(ctx: Context) = File(ctx.filesDir, "session.json")

    private fun read(ctx: Context) = runCatching { JSONObject(file(ctx).readText()) }.getOrNull()
    private fun write(ctx: Context, j: JSONObject) = runCatching { file(ctx).writeText(j.toString()) }

    /** GameActivity.onCreate (game process). */
    fun started(ctx: Context) {
        val old = read(ctx)
        write(ctx, JSONObject().put("start", System.currentTimeMillis()).put("failed", old?.optInt("failed") ?: 0)
            .put("driver", runCatching { GpuDriver.current() != null }.getOrDefault(false)))
    }

    /** GameActivity.onDestroy when finishing (game process). */
    fun ended(ctx: Context) {
        read(ctx)?.let { write(ctx, it.put("end", System.currentTimeMillis())) }
    }

    private fun gameRunning(ctx: Context): Boolean = ctx.getSystemService(ActivityManager::class.java).runningAppProcesses
        ?.any { it.processName == ctx.packageName + ":game" } == true

    /**
     * The last session once the game process is gone, evaluated once (later calls return the same result from the file).
     * Null when the game never ran or still runs.
     */
    fun last(ctx: Context): Outcome? {
        val j = read(ctx) ?: return null
        if (gameRunning(ctx)) return null
        if (!j.has("evaluated")) {
            val log = logFile()
            val text = log?.let { headOf(it) }.orEmpty()
            val first = text.contains(FIRST_FRAME)
            val fallback = text.lineSequence().lastOrNull { it.contains(FALLBACK) }?.substringAfter("custom Vulkan driver:")?.trim()
            val end = if (j.has("end")) j.getLong("end") else (log?.lastModified() ?: j.getLong("start"))
            val failed = if (!first && j.optBoolean("driver")) j.optInt("failed") + 1 else 0
            j.put("evaluated", true).put("first", first).put("fallback", fallback ?: "").put("end", end).put("failed", failed)
            write(ctx, j)
        }
        return Outcome(
            (j.optLong("end") - j.optLong("start")).coerceAtLeast(0), j.optBoolean("first"), j.optString("fallback").ifEmpty { null },
            j.optBoolean("driver"), j.optInt("failed"),
        )
    }

    /** After "Try again" or a driver change: count failed starts from zero. */
    fun resetFailures(ctx: Context) { read(ctx)?.let { write(ctx, it.put("failed", 0)) } }

    /** The runtime log of the last session: pkp.ini [log] path (the app sets <dataRoot>/logs/pkp.log), relative to the data root. */
    private fun logFile(): File? {
        val p = Ini.load(Paths.pkpIni).get("log", "path")?.trim()?.ifEmpty { null } ?: "logs/pkp.log"
        return File(p).let { if (it.isAbsolute) it else File(Paths.dataRoot, p) }.takeIf { it.isFile }
    }

    private fun headOf(f: File): String = runCatching {
        f.inputStream().use { s ->
            // the first frame and the driver choice are logged early: the first 2 MiB hold them
            String(s.readNBytes(2 shl 20), Charsets.UTF_8)
        }
    }.getOrDefault("")
}
