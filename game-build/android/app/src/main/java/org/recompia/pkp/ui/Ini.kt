// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import java.io.File

/**
 * A careful line editor for the runtime's ini files (pkp.ini, controls.ini), the same rules as the launcher's
 * launcher/src-tauri/src/ini.rs: lines are kept as they are, `set` changes only the value text of one `key = value` line
 * (keeping key spelling, spacing and a trailing comment) or inserts a new line. Comments, blank lines, unknown keys and the
 * line-ending style survive. Section and key names are case-insensitive, as in the runtime.
 */
class Ini private constructor(private val lines: MutableList<String>, private val eol: String, private val finalEol: Boolean) {

    private fun find(section: String, key: String): Int? {
        val s = section.lowercase()
        val k = key.lowercase()
        var cur = ""
        var hit: Int? = null
        lines.forEachIndexed { i, line ->
            val name = sectionName(line)
            if (name != null) cur = name
            else if (cur == s) {
                val eq = keyEq(line)
                if (eq != null && line.substring(0, eq).trim().lowercase() == k) hit = i
            }
        }
        return hit
    }

    fun get(section: String, key: String): String? {
        val i = find(section, key) ?: return null
        val line = lines[i]
        val eq = keyEq(line) ?: return null
        return unquote(splitValue(line.substring(eq + 1).trimStart()).first)
    }

    fun set(section: String, key: String, value: String) {
        val v = formatValue(value)
        val i = find(section, key)
        if (i != null) {
            val line = lines[i]
            val eq = keyEq(line)!!
            val after = line.substring(eq + 1)
            val blanks = after.length - after.trimStart().length
            val tail = splitValue(after.substring(blanks)).second
            lines[i] = line.substring(0, eq + 1) + after.substring(0, blanks) + v + tail
            return
        }
        val newLine = "$key = $v"
        val target = section.lowercase()
        var cur = ""
        var last: Int? = null
        lines.forEachIndexed { idx, line ->
            val name = sectionName(line)
            if (name != null) {
                cur = name
                if (cur == target) last = idx
            } else if (cur == target && line.isNotBlank()) last = idx
        }
        val at = last
        if (at != null) lines.add(at + 1, newLine)
        else {
            if (lines.isNotEmpty() && lines.last().isNotBlank()) lines.add("")
            lines.add("[$section]")
            lines.add(newLine)
        }
    }

    /** Removes the key's line (the last one, as [get] reads it); nothing else changes. */
    fun remove(section: String, key: String) {
        find(section, key)?.let { lines.removeAt(it) }
    }

    fun text(): String = lines.joinToString(eol) + if (finalEol && lines.isNotEmpty()) eol else ""

    /** Written next to the file and renamed over it, so a crash never leaves half a file. */
    fun save(file: File) {
        file.parentFile?.mkdirs()
        val tmp = File(file.parentFile, file.name + ".tmp")
        tmp.writeText(text())
        if (!tmp.renameTo(file)) {
            file.writeText(text())
            tmp.delete()
        }
    }

    companion object {
        /** Throws if the editor loses a line it must keep. The debug fake builder runs it before every fake build. */
        fun selfCheck() {
            val i = parse("; c\r\n[Display]\r\nVsync = 1 ; keep\r\nunknown=x\r\n")
            i.set("display", "vsync", "0"); i.set("display", "fps_limit", "60"); i.set("pkp", "camera_aim", "1")
            check(i.text() == "; c\r\n[Display]\r\nVsync = 0 ; keep\r\nunknown=x\r\nfps_limit = 60\r\n\r\n[pkp]\r\ncamera_aim = 1\r\n") { i.text() }
            check(parse(i.text()).get("DISPLAY", "VSYNC") == "0")
        }

        fun parse(text: String): Ini {
            val eol = if (text.contains("\r\n")) "\r\n" else "\n"
            return Ini(text.lines().let { if (text.endsWith("\n")) it.dropLast(1) else it }
                .map { it.removeSuffix("\r") }.toMutableList(), eol, text.endsWith("\n"))
        }

        /** A missing or unreadable file is an empty ini (the runtime then uses its defaults). */
        fun load(file: File): Ini = runCatching { parse(file.readText()) }.getOrElse { Ini(mutableListOf(), "\n", true) }

        private fun sectionName(line: String): String? {
            val t = line.trim()
            if (!t.startsWith("[")) return null
            val end = t.indexOf(']')
            return if (end > 0) t.substring(1, end).trim().lowercase() else null
        }

        private fun keyEq(line: String): Int? {
            val t = line.trimStart()
            if (t.isEmpty() || t[0] == ';' || t[0] == '#' || t[0] == '[') return null
            return line.indexOf('=').takeIf { it >= 0 }
        }

        private fun splitValue(rest: String): Pair<String, String> {
            if (rest.startsWith("\"")) {
                val close = rest.indexOf('"', 1)
                if (close > 0) return rest.substring(0, close + 1) to rest.substring(close + 1)
            }
            for (i in 1 until rest.length) {
                if ((rest[i] == ';' || rest[i] == '#') && (rest[i - 1] == ' ' || rest[i - 1] == '\t')) {
                    var s = i
                    while (s > 0 && (rest[s - 1] == ' ' || rest[s - 1] == '\t')) s--
                    return rest.substring(0, s) to rest.substring(s)
                }
            }
            val trimmed = rest.trimEnd()
            return trimmed to rest.substring(trimmed.length)
        }

        private fun unquote(v: String): String {
            val t = v.trim()
            return if (t.length >= 2 && t.startsWith("\"") && t.endsWith("\"")) t.substring(1, t.length - 1) else t
        }

        private fun formatValue(value: String): String {
            val v = value.filter { it != '\r' && it != '\n' }
            val needs = v != v.trim() || v.contains(" ;") || v.contains(" #") || v.contains("\t;") || v.contains("\t#") || v.startsWith("\"")
            return if (needs && !v.contains('"')) "\"$v\"" else v
        }
    }
}
