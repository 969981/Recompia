// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import java.io.File
import java.io.RandomAccessFile

/**
 * Validates a decrypted game dump folder. Read-only. A port of the PC launcher's launcher/src-tauri/src/dump.rs, so the phone
 * accepts the same layouts: `<dump>/base/{exefs,romfs}` and `<dump>/updates/<version>/{exefs,romfs}` (the highest valid update
 * wins, else the base), a folder holding `exefs/` and `romfs/` directly, or `exefs/` itself. An update dumped without the base
 * game's files is completed from `base/romfs` (`romfs_base`). The exefs hash check stays with the build script.
 * Texts are not here: the UI turns [Check] and [Problem] into string resources.
 */
object DumpScanner {
    val EXEFS_FILES = listOf("main", "sdk", "subsdk0", "rtld", "main.npdm")
    private val NSO_FILES = listOf("main", "sdk", "subsdk0", "rtld")
    private const val OLDER_MAIN_BUILD = "816c5358e8178d802fcec4a7ce5a741a"
    private val ROMFS_MARKER = listOf("motor_asset_re", "KIDS_PLATFORM_OUNCE")

    /** What this release supports (dump_hashes.json); null fields skip that check, as in a launcher dev checkout. */
    data class Supported(val mainBuildId: String?, val versionText: String?, val romfsFiles: Long?, val romfsBytes: Long?)

    enum class CheckKind { FILE, ROMFS_DIR, ROMFS_NOT_EMPTY, ROMFS_GAME_DATA, ROMFS_COMPLETE, ROMFS_COMPLETED_FROM, TEXT_MODE, VERSION, OLDER }

    /** [arg]: a file name, a path, or the numbers of an incomplete romfs ("22 of 36 files, 1.20 of 7.25 GB"). */
    data class Check(val kind: CheckKind, val ok: Boolean, val arg: String = "")

    data class Candidate(
        val kind: String, // base, update, folder
        val version: String,
        val path: String,
        val exefs: String,
        val romfs: String,
        val romfsBase: String,
        val romfsFiles: Long,
        val romfsBytes: Long,
        val exefsBytes: Long,
        /** romfs files of the supported version that are absent or shorter here (relative paths), when the romfs is incomplete. */
        val romfsMissing: List<String>,
        val ok: Boolean,
        val checks: List<Check>,
    )

    sealed interface Problem {
        data object NoneChosen : Problem
        data object NotFound : Problem
        data object NoLayout : Problem
        data class Incomplete(val romfs: String, val files: Long, val needFiles: Long, val gb: String, val needGb: String, val missing: List<String>) : Problem
        /** [files]: the damaged files with the bytes they lack. */
        data class TextMode(val files: List<String>) : Problem
        data class Older(val version: String) : Problem
        data class BaseOnly(val version: String) : Problem
        data class Different(val versionText: String) : Problem
        /** [missing]: the failed checks, "<folder>: <check>". */
        data class RomfsBad(val missing: List<String>) : Problem
        data class NothingUsable(val missing: List<String>) : Problem
    }

    data class Report(
        val path: String,
        val exists: Boolean,
        val problems: List<Problem>,
        val candidates: List<Candidate>,
        val chosen: Candidate?,
        val aocDir: String,
        val aoc: List<String>,
    )

    private fun gb(b: Long) = String.format(java.util.Locale.ROOT, "%.2f", b / 1e9)

    private fun romfsFiles(root: File): MutableMap<String, Long> {
        val out = sortedMapOf<String, Long>()
        fun walk(dir: File, prefix: String, depth: Int) {
            for (e in dir.listFiles() ?: return) {
                if (e.isDirectory && depth < 32) walk(e, "$prefix${e.name}/", depth + 1)
                else if (e.isFile) out["$prefix${e.name}"] = e.length()
            }
        }
        walk(root, "", 0)
        return out
    }

    fun mainBuildId(main: File): String? = runCatching {
        RandomAccessFile(main, "r").use { f ->
            val h = ByteArray(0x54)
            f.readFully(h)
            if (String(h, 0, 4, Charsets.ISO_8859_1) != "NSO0") null
            else h.copyOfRange(0x40, 0x54).joinToString("") { "%02x".format(it) }
        }
    }.getOrNull()

    /** Bytes an NSO lacks against the segments its header lists (0 = complete). A text-mode FTP copy drops one per CR LF. */
    private fun nsoShort(p: File): Long? = runCatching {
        RandomAccessFile(p, "r").use { f ->
            val h = ByteArray(0x70)
            f.readFully(h)
            if (String(h, 0, 4, Charsets.ISO_8859_1) != "NSO0") return null
            fun u32(o: Int) = (h[o].toLong() and 0xff) or ((h[o + 1].toLong() and 0xff) shl 8) or
                ((h[o + 2].toLong() and 0xff) shl 16) or ((h[o + 3].toLong() and 0xff) shl 24)
            val flags = u32(0xC)
            val end = (0 until 3).maxOf { i ->
                u32(0x10 + 0x10 * i) + if (flags and (1L shl i) != 0L) u32(0x60 + 4 * i) else u32(0x18 + 0x10 * i)
            }
            (end - f.length()).coerceAtLeast(0)
        }
    }.getOrNull()

    private fun hasEntries(p: File) = (p.list()?.isNotEmpty()) == true

    private fun checkCandidate(kind: String, version: String, dir: File, want: Supported, baseRomfs: File?): Candidate {
        val exefs = File(dir, "exefs")
        val romfs = File(dir, "romfs")
        val checks = mutableListOf<Check>()
        var romfsBase = ""
        var nFiles = 0L
        var nBytes = 0L
        var missing = emptyList<String>()
        for (f in EXEFS_FILES) checks += Check(CheckKind.FILE, File(exefs, f).isFile, "exefs/$f")
        checks += Check(CheckKind.ROMFS_DIR, romfs.isDirectory)
        if (romfs.isDirectory) {
            val filled = hasEntries(romfs)
            checks += Check(CheckKind.ROMFS_NOT_EMPTY, filled)
            if (filled) checks += Check(CheckKind.ROMFS_GAME_DATA, hasEntries(ROMFS_MARKER.fold(romfs) { a, b -> File(a, b) }))
            val files = romfsFiles(romfs)
            nFiles = files.size.toLong(); nBytes = files.values.sum()
            if (want.romfsFiles != null && want.romfsBytes != null) {
                val complete = { f: Long, b: Long -> f >= want.romfsFiles && b >= want.romfsBytes }
                var ok = complete(nFiles, nBytes)
                if (!ok && baseRomfs != null && baseRomfs.isDirectory) {
                    for ((rel, size) in romfsFiles(baseRomfs)) files.putIfAbsent(rel, size)
                    val f = files.size.toLong(); val b = files.values.sum()
                    if (complete(f, b)) {
                        ok = true
                        romfsBase = baseRomfs.path
                        nFiles = f; nBytes = b
                        checks += Check(CheckKind.ROMFS_COMPLETED_FROM, true, baseRomfs.path)
                    }
                }
                // only the totals of dump_hashes.json are known; what can be named is the player's own empty files (a copy that stopped)
                if (!ok) missing = files.filterValues { it == 0L }.keys.sorted()
                checks += Check(CheckKind.ROMFS_COMPLETE, ok,
                    if (ok) "" else "$nFiles / ${want.romfsFiles}, ${gb(nBytes)} / ${gb(want.romfsBytes)} GB")
            }
        }
        for (f in NSO_FILES) {
            val short = nsoShort(File(exefs, f))
            if (short != null && short > 0) checks += Check(CheckKind.TEXT_MODE, false, "exefs/$f ($short)")
        }
        if (want.mainBuildId != null && File(exefs, "main").isFile) {
            val got = mainBuildId(File(exefs, "main"))
            val ok = got == want.mainBuildId
            checks += Check(CheckKind.VERSION, ok, want.versionText ?: "")
            if (!ok && got?.startsWith(OLDER_MAIN_BUILD) == true) checks += Check(CheckKind.OLDER, false)
        }
        val exefsBytes = EXEFS_FILES.sumOf { File(exefs, it).length() }
        return Candidate(kind, version, dir.path, exefs.path, romfs.path, romfsBase, nFiles, nBytes, exefsBytes, missing, checks.all { it.ok }, checks)
    }

    private val versionOrder = Comparator<String> { a, b ->
        val x = a.toLongOrNull(); val y = b.toLongOrNull()
        when {
            x != null && y != null -> y.compareTo(x)
            x != null -> -1
            y != null -> 1
            else -> b.compareTo(a)
        }
    }

    /** Item names for the "missing or incomplete" list: only file and folder names, which need no translation. */
    private fun checkLabel(k: Check) = when (k.kind) {
        CheckKind.FILE -> k.arg
        CheckKind.ROMFS_DIR, CheckKind.ROMFS_NOT_EMPTY -> "romfs/"
        CheckKind.ROMFS_GAME_DATA -> "romfs/" + ROMFS_MARKER.joinToString("/") + "/"
        CheckKind.ROMFS_COMPLETE -> "romfs/ (${k.arg})"
        else -> k.arg
    }

    fun scan(path: String, want: Supported): Report {
        val root = File(path.trim())
        val exists = root.isDirectory
        fun bad(p: Problem) = Report(root.path, exists, listOf(p), emptyList(), null, "", emptyList())
        if (path.isBlank()) return bad(Problem.NoneChosen)
        if (!exists) return bad(Problem.NotFound)
        val base = File(root, "base")
        val updates = File(root, "updates")
        val direct = when {
            File(root, "exefs").isDirectory -> root
            File(root, "main").isFile && File(root, "main.npdm").isFile -> root.parentFile?.takeIf { File(it, "romfs").isDirectory }
            else -> null
        }
        if (!base.isDirectory && !updates.isDirectory && direct == null) return bad(Problem.NoLayout)

        val candidates = mutableListOf<Candidate>()
        if (direct != null) {
            val up = direct.parentFile?.takeIf { it.name.equals("updates", ignoreCase = true) }
            candidates += checkCandidate("folder", "", direct, want, up?.parentFile?.let { File(File(it, "base"), "romfs") })
        }
        if (updates.isDirectory) {
            val versions = updates.listFiles()?.filter { it.isDirectory }?.map { it.name }?.sortedWith(versionOrder).orEmpty()
            for (v in versions) candidates += checkCandidate("update", v, File(updates, v), want, File(base, "romfs"))
        }
        if (base.isDirectory) candidates += checkCandidate("base", "", base, want, null)
        val chosen = candidates.firstOrNull { it.ok }

        val aocRoot = File(root, "aoc")
        val aoc = aocRoot.listFiles()?.map { it.name }
            ?.filter { n -> n.length == 16 && n.all { it.isDigit() || it.lowercaseChar() in 'a'..'f' } && File(File(aocRoot, n), "romfs").isDirectory }
            ?.map { it.uppercase() }?.sorted().orEmpty()

        val problems = mutableListOf<Problem>()
        if (chosen == null) {
            fun failed(k: CheckKind) = candidates.any { c -> c.checks.any { it.kind == k && !it.ok } }
            fun bad(c: Candidate, k: CheckKind) = c.checks.any { it.kind == k && !it.ok }
            val incomplete = candidates.firstOrNull {
                bad(it, CheckKind.ROMFS_COMPLETE) && !bad(it, CheckKind.VERSION) && !bad(it, CheckKind.TEXT_MODE)
            }
            val version = want.versionText?.substringBefore(' ') ?: ""
            fun rel(c: Candidate) = c.path.removePrefix(root.path).trimStart('/').ifEmpty { root.name }
            val failedChecks = candidates.flatMap { c -> c.checks.filter { !it.ok && it.kind != CheckKind.VERSION && it.kind != CheckKind.OLDER }.map { k -> "${rel(c)}/${checkLabel(k)}" } }
            val p: Problem = when {
                incomplete != null && want.romfsFiles != null && want.romfsBytes != null -> Problem.Incomplete(
                    incomplete.romfs, incomplete.romfsFiles, want.romfsFiles, gb(incomplete.romfsBytes), gb(want.romfsBytes), incomplete.romfsMissing)
                failed(CheckKind.TEXT_MODE) -> Problem.TextMode(candidates.flatMap { c -> c.checks.filter { it.kind == CheckKind.TEXT_MODE }.map { "${rel(c)}/${it.arg}" } })
                want.versionText != null && failed(CheckKind.OLDER) -> Problem.Older(version)
                want.versionText != null && failed(CheckKind.VERSION) && candidates.none { it.kind == "update" } && direct == null -> Problem.BaseOnly(version)
                want.versionText != null && failed(CheckKind.VERSION) -> Problem.Different(want.versionText)
                failed(CheckKind.ROMFS_DIR) || failed(CheckKind.ROMFS_NOT_EMPTY) || failed(CheckKind.ROMFS_GAME_DATA) || failed(CheckKind.ROMFS_COMPLETE) -> Problem.RomfsBad(failedChecks)
                else -> Problem.NothingUsable(failedChecks)
            }
            problems += p
        }
        return Report(root.path, true, problems, candidates, chosen, if (aoc.isEmpty()) "" else aocRoot.path, aoc)
    }
}
