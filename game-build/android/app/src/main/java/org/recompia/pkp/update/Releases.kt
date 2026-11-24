// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.update

import org.json.JSONArray
import org.json.JSONObject
import org.recompia.pkp.BuildConfig
import org.recompia.pkp.Paths
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/**
 * Which release the app offers, from the project's GitHub releases list (the launcher's releases.rs in short). Stable only: the
 * newest release that is not a draft or a prerelease, carries `Recompia-<v>-android-arm64.apk` and `SHA256SUMS.txt` (release.yml),
 * and is newer than this app. Prereleases (the launcher's Experimental channel) carry no APK, so the phone has no channel to pick.
 */
object Releases {
    const val REPO = "Dittobyte/Recompia"
    private const val API = "https://api.github.com/repos/$REPO/releases?per_page=30"
    private const val SUMS = "SHA256SUMS.txt"
    private const val TIMEOUT_MS = 20_000

    data class Release(
        val tag: String, val version: String, val name: String, val date: String, val notes: String, val page: String,
        val apkName: String, val apkUrl: String, val size: Long, val sumsUrl: String,
    ) {
        fun toJson(): JSONObject = JSONObject().put("tag", tag).put("version", version).put("name", name).put("date", date)
            .put("notes", notes).put("page", page).put("apkName", apkName).put("apkUrl", apkUrl).put("size", size).put("sumsUrl", sumsUrl)

        companion object {
            fun fromJson(j: JSONObject) = Release(j.getString("tag"), j.getString("version"), j.optString("name"), j.optString("date"),
                j.optString("notes"), j.optString("page"), j.getString("apkName"), j.getString("apkUrl"), j.optLong("size"), j.getString("sumsUrl"))
        }
    }

    /** "v0.17.5" or "0.16.0.2" as numbers; null for anything else (a prerelease or a co-op test tag). */
    fun version(s: String): List<Int>? {
        val parts = s.trim().removePrefix("v").removePrefix("V").split('.')
        if (parts.size !in 3..4 || parts.any { p -> p.isEmpty() || !p.all { it.isDigit() } }) return null
        return parts.map { it.toInt() }
    }

    /** a > b, missing parts read as 0 (0.17.0 = 0.17.0.0). */
    fun newer(a: List<Int>, b: List<Int>): Boolean {
        for (i in 0 until maxOf(a.size, b.size)) {
            val x = a.getOrElse(i) { 0 }; val y = b.getOrElse(i) { 0 }
            if (x != y) return x > y
        }
        return false
    }

    /** The release to offer from the API's answer, or null when none is newer than `current` (the app's version name). */
    fun pick(list: JSONArray, current: String): Release? {
        val have = version(current) ?: return null
        var best: Release? = null
        for (i in 0 until list.length()) {
            val r = list.optJSONObject(i) ?: continue
            if (r.optBoolean("draft") || r.optBoolean("prerelease")) continue
            val tag = r.optString("tag_name")
            val v = version(tag) ?: continue
            if (!newer(v, have) || (best != null && !newer(v, version(best.version)!!))) continue
            val bare = tag.removePrefix("v").removePrefix("V")
            val apkName = "Recompia-$bare-android-arm64.apk"
            val assets = r.optJSONArray("assets") ?: continue
            val byName = (0 until assets.length()).mapNotNull { assets.optJSONObject(it) }.associateBy { it.optString("name") }
            val apk = byName[apkName] ?: continue
            val sums = byName[SUMS] ?: continue   // never offered without a checksum to verify it against
            best = Release(tag, bare, r.optString("name").ifBlank { tag }, r.optString("published_at"), r.optString("body"),
                r.optString("html_url"), apkName, apk.optString("browser_download_url"), apk.optLong("size"), sums.optString("browser_download_url"))
        }
        return best
    }

    /** The SHA-256 SHA256SUMS.txt lists for `name` (`<hex> *<name>` or `<hex>  <name>`), lower case (update.rs sum_for). */
    fun sumFor(sums: String, name: String): String? = sums.removePrefix("﻿").lineSequence().firstNotNullOfOrNull { l ->
        val t = l.trim(); val sp = t.indexOfFirst { it.isWhitespace() }
        if (sp < 0) return@firstNotNullOfOrNull null
        val hex = t.substring(0, sp); val file = t.substring(sp).trim().removePrefix("*")
        hex.lowercase().takeIf { file == name && hex.length == 64 && hex.all { it in "0123456789abcdefABCDEF" } }
    }

    /**
     * The releases list. Test builds (debug, internalTest) read the list from the URL in `<data root>/update-feed.txt` when that
     * file exists, so the whole flow can be walked through against a local list; release builds always ask GitHub.
     */
    fun fetch(): JSONArray = JSONArray(text(feed()))

    /** A small text file (the list, SHA256SUMS.txt). */
    fun text(url: String): String = get(url).let { c -> try { c.inputStream.bufferedReader().readText() } finally { c.disconnect() } }

    private fun feed(): String {
        if (!BuildConfig.TEST_ENTRY_POINTS) return API
        return runCatching { File(Paths.dataRoot, "update-feed.txt").readText().trim() }.getOrNull()?.ifEmpty { null } ?: API
    }

    /** A GET that answered 2xx (a redirect to GitHub's download host is followed), else an exception with the HTTP code. */
    fun get(url: String, range: Long = 0): HttpURLConnection {
        val c = URL(url).openConnection() as HttpURLConnection
        c.connectTimeout = TIMEOUT_MS; c.readTimeout = TIMEOUT_MS
        c.setRequestProperty("User-Agent", "Recompia-Android/${BuildConfig.VERSION_NAME}")
        c.setRequestProperty("Accept", if (url.startsWith("https://api.github.com/")) "application/vnd.github+json" else "*/*")
        if (range > 0) c.setRequestProperty("Range", "bytes=$range-")
        val code = c.responseCode
        if (code !in 200..299) { c.disconnect(); throw HttpError(code) }
        return c
    }

    class HttpError(val code: Int) : java.io.IOException("HTTP $code")
}
