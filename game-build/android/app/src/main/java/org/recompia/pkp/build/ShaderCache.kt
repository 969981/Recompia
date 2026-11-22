// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.build

import org.json.JSONObject
import org.recompia.pkp.Paths
import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * What the shader cache under `<dataRoot>/shader_cache` is, and "Check the cache" for one that came without its setup.json.
 * Ready = the readiness rule (BuildService.shaderCachePresent). The check reads the archive exactly as the launcher's
 * shader_archive.rs read_index does and compares it with results.tsv; it writes setup.json only when every segment is sound,
 * every archived shader has its results row and results.tsv lists every block of the supported version.
 */
object ShaderCache {
    enum class State { READY, MISSING, NO_SETUP, INCOMPLETE }

    /** Shader blocks the extractor finds in update 655360 (romfs + exefs/main), the same count on the PC and on a phone. */
    const val BLOCKS_655360 = 99_012
    /** TRANSLATOR_REV of launcher/src-tauri/src/shaders.rs and build_shaders.py. */
    const val TRANSLATOR_REV = 2

    private val dir get() = Paths.shaderCacheDir
    private fun packs(): List<File> = File(dir, "spirv.archive").listFiles { f -> f.name.endsWith(".pack") }?.sortedBy { it.name }.orEmpty()
    private val results get() = File(dir, "spirv/results.tsv")

    fun state(): State = when {
        BuildService.shaderCachePresent() -> State.READY
        packs().isEmpty() && !results.isFile -> State.MISSING
        packs().isNotEmpty() && results.isFile && !File(dir, "setup.json").exists() -> State.NO_SETUP
        else -> State.INCOMPLETE
    }

    /** rows = distinct blocks in results.tsv, entries = archived shaders, orphans = archived shaders with no results row. */
    data class Check(val ok: Boolean, val rows: Int, val entries: Int, val orphans: Int, val badSegment: String?, val error: String? = null)

    /** Slow (reads results.tsv, about 100 MB, and every segment index): call off the main thread. */
    fun check(): Check {
        val rows = HashSet<String>(1 shl 17)
        try {
            results.bufferedReader().useLines { lines ->
                lines.drop(1).forEach { l -> l.substringBefore('\t').takeIf { it.isNotEmpty() }?.let { rows.add(it.take(16)) } }
            }
        } catch (e: Exception) {
            return Check(false, 0, 0, 0, null, e.message ?: e.javaClass.simpleName)
        }
        var entries = 0
        var orphans = 0
        for (p in packs()) {
            val ok = runCatching {
                RandomAccessFile(p, "r").use { f ->
                    val h = ByteArray(32).also { f.readFully(it) }
                    val hb = ByteBuffer.wrap(h).order(ByteOrder.LITTLE_ENDIAN)
                    val count = hb.getInt(12).toLong() and 0xffffffffL
                    val index = hb.getLong(16)
                    if (String(h, 0, 8, Charsets.US_ASCII) != "NXSPACK1" || hb.getInt(8) != 1 || count > 1_000_000 || index < 32 ||
                        index + count * 40 != f.length()) return@use false
                    val idx = ByteArray((count * 40).toInt()).also { f.seek(index); f.readFully(it) }
                    val ib = ByteBuffer.wrap(idx).order(ByteOrder.LITTLE_ENDIAN)
                    for (i in 0 until count.toInt()) {
                        val e = i * 40
                        val off = ib.getLong(e + 24)
                        val csize = ib.getInt(e + 32).toLong() and 0xffffffffL
                        val usize = ib.getInt(e + 36).toLong() and 0xffffffffL
                        if (off < 32 || csize == 0L || off + csize > index || usize == 0L || usize % 4 != 0L) return@use false
                        entries++
                        if (String(idx, e, 16, Charsets.US_ASCII) !in rows) orphans++
                    }
                    true
                }
            }.getOrDefault(false)
            if (!ok) return Check(false, rows.size, entries, orphans, p.name)
        }
        val ok = entries > 0 && orphans == 0 && rows.size >= BLOCKS_655360
        if (ok) {
            File(dir, "setup.json").writeText(JSONObject()
                .put("romfs", "").put("extracted", true).put("blocks", rows.size).put("complete", true)
                .put("translator", TRANSLATOR_REV).put("storage", "compact")
                .put("checked_on_phone", "$entries archived shaders, ${rows.size} results rows, ${packs().size} segments")
                .toString(2))
        }
        return Check(ok, rows.size, entries, orphans, null)
    }
}
