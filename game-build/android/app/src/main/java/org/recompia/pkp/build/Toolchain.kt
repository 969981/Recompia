// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.build

import android.content.Context
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.util.zip.GZIPInputStream

/**
 * The on-device build tools (tools/android/ondevice): executables in nativeLibraryDir, and an asset archive with
 * Python's stdlib, the sysroot and the driver, unpacked once into `<filesDir>/toolchain` (app-private: libraries are loaded from
 * it). The archive is re-unpacked when its version line changes.
 */
object Toolchain {
    const val PYTHON = "libpython_exec.so"
    private const val VERSION_ASSET = "recompia-toolchain.version"
    // AAPT stores a .gz asset decompressed and without the .gz, so the APK may hold either name
    private val ARCHIVES = listOf("recompia-toolchain.tar.gz", "recompia-toolchain.tar")

    fun dir(ctx: Context) = File(ctx.filesDir, "toolchain")
    fun python(ctx: Context) = File(ctx.applicationInfo.nativeLibraryDir, PYTHON)
    fun driver(ctx: Context) = File(dir(ctx), "driver/recompia_build.py")

    /** The APK carries the real builder (the Python launcher and the tools archive). */
    fun present(ctx: Context): Boolean = python(ctx).isFile && assetVersion(ctx) != null

    /** The build kit in this APK (sha256 of the archive, from package.sh). */
    fun version(ctx: Context): String? = assetVersion(ctx)

    private fun assetVersion(ctx: Context): String? =
        runCatching { ctx.assets.open(VERSION_ASSET).bufferedReader().use { it.readLine()?.trim() } }.getOrNull()

    fun needsUnpack(ctx: Context): Boolean {
        val want = assetVersion(ctx) ?: return false
        return runCatching { File(dir(ctx), ".version").readText().trim() }.getOrNull() != want
    }

    /** Unpacks the archive; [progress] gets (bytes read, archive size or -1). Throws with a reason the log can show. */
    fun unpack(ctx: Context, cancelled: () -> Boolean, progress: (Long, Long) -> Unit) {
        val want = assetVersion(ctx) ?: throw IOException("the build tools are not in this app")
        val name = ARCHIVES.firstOrNull { n -> runCatching { ctx.assets.open(n).close() }.isSuccess }
            ?: throw IOException("the build tools archive is missing")
        val size = runCatching { ctx.assets.openFd(name).use { it.length } }.getOrDefault(-1L)
        val target = dir(ctx)
        val tmp = File(target.path + ".tmp")
        tmp.deleteRecursively()
        target.deleteRecursively()
        val counter = CountingStream(ctx.assets.open(name))
        val stream = if (name.endsWith(".gz")) GZIPInputStream(counter, 1 shl 16) else counter.buffered(1 shl 16)
        stream.use { untar(it, tmp, cancelled) { progress(counter.count, size) } }
        File(tmp, ".version").writeText(want + "\n")
        if (!tmp.renameTo(target)) throw IOException("cannot rename $tmp to $target")
    }

    private class CountingStream(private val s: InputStream) : InputStream() {
        var count = 0L
        override fun read(): Int = s.read().also { if (it >= 0) count++ }
        override fun read(b: ByteArray, off: Int, len: Int): Int = s.read(b, off, len).also { if (it > 0) count += it }
        override fun close() = s.close()
    }

    /** A plain ustar/GNU tar reader: files, folders, symlinks (the driver calls clang through symlinks), long names. */
    private fun untar(input: InputStream, into: File, cancelled: () -> Boolean, tick: () -> Unit) {
        val header = ByteArray(512)
        var longName: String? = null
        var longLink: String? = null
        val root = into.canonicalPath
        while (true) {
            if (cancelled()) throw InterruptedException()
            if (!readFully(input, header)) break
            if (header.all { it.toInt() == 0 }) break
            fun str(off: Int, len: Int) = String(header, off, len, Charsets.UTF_8).substringBefore('\u0000')
            val size = str(124, 12).trim().ifEmpty { "0" }.toLong(8)
            val type = header[156].toInt().toChar()
            val prefix = if (str(257, 6).startsWith("ustar")) str(345, 155) else ""
            val name = longName ?: (if (prefix.isNotEmpty()) "$prefix/" else "") + str(0, 100)
            val link = longLink ?: str(157, 100)
            if (type == 'L' || type == 'K') {
                val data = ByteArray(size.toInt()).also { if (!readFully(input, it)) throw IOException("truncated archive") }
                skip(input, pad(size))
                val text = String(data, Charsets.UTF_8).substringBefore('\u0000')
                if (type == 'L') longName = text else longLink = text
                continue
            }
            longName = null; longLink = null
            val out = File(into, name)
            if (!out.canonicalPath.startsWith(root)) throw IOException("archive entry outside the target: $name")
            val mode = str(100, 8).trim().ifEmpty { "0" }.toInt(8)
            when (type) {
                '5' -> out.mkdirs()
                '2' -> {
                    out.parentFile?.mkdirs()
                    android.system.Os.symlink(link, out.path)
                }
                '0', '\u0000', '7' -> {
                    out.parentFile?.mkdirs()
                    out.outputStream().use { o ->
                        var left = size
                        val buf = ByteArray(1 shl 16)
                        while (left > 0) {
                            val n = input.read(buf, 0, minOf(buf.size.toLong(), left).toInt())
                            if (n < 0) throw IOException("truncated archive")
                            o.write(buf, 0, n)
                            left -= n
                        }
                    }
                    if (mode and 0b001_001_001 != 0) out.setExecutable(true, false)
                    skip(input, pad(size))
                    tick()
                    continue
                }
                else -> {}
            }
            skip(input, size + pad(size))
        }
    }

    private fun pad(size: Long) = (512 - size % 512) % 512

    private fun readFully(input: InputStream, b: ByteArray): Boolean {
        var off = 0
        while (off < b.size) {
            val n = input.read(b, off, b.size - off)
            if (n < 0) { if (off == 0) return false else throw IOException("truncated archive") }
            off += n
        }
        return true
    }

    private fun skip(input: InputStream, n: Long) {
        var left = n
        val buf = ByteArray(8192)
        while (left > 0) {
            val r = input.read(buf, 0, minOf(buf.size.toLong(), left).toInt())
            if (r < 0) throw IOException("truncated archive")
            left -= r
        }
    }
}
