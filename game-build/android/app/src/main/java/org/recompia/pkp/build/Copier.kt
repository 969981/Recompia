// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.build

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import androidx.documentfile.provider.DocumentFile
import java.io.File
import java.io.IOException
import java.io.InputStream

/** A file or folder to copy from: a plain path (all-files access) or a document from the system picker (any provider). */
interface Node {
    val name: String
    val isDir: Boolean
    val length: Long
    fun children(): List<Node>
    fun child(name: String): Node? = children().firstOrNull { it.name.equals(name, ignoreCase = true) }
    fun open(): InputStream
}

class FileNode(val file: File) : Node {
    override val name: String get() = file.name
    override val isDir: Boolean get() = file.isDirectory
    override val length: Long get() = file.length()
    override fun children() = file.listFiles()?.map { FileNode(it) }.orEmpty()
    override fun child(name: String) = File(file, name).takeIf { it.exists() }?.let { FileNode(it) } ?: super.child(name)
    override fun open(): InputStream = file.inputStream()
}

class DocNode(private val ctx: Context, val doc: DocumentFile) : Node {
    override val name: String get() = doc.name.orEmpty()
    override val isDir: Boolean get() = doc.isDirectory
    override val length: Long get() = doc.length()
    override fun children() = doc.listFiles().map { DocNode(ctx, it) }
    override fun open(): InputStream = ctx.contentResolver.openInputStream(doc.uri) ?: throw IOException("cannot open ${doc.uri}")
}

object Copier {
    /**
     * A picker tree on local storage as a plain path (`primary:Games/Dump` -> /storage/emulated/0/Games/Dump, an SD card id ->
     * /storage/<id>/...), so it can be read in place with all-files access. Null for other providers (cloud, USB through a
     * provider) or a path that cannot be read: those are copied.
     */
    fun pathOf(uri: Uri): File? {
        if (uri.authority != "com.android.externalstorage.documents") return null
        val id = runCatching { DocumentsContract.getTreeDocumentId(uri) }.getOrNull() ?: return null
        val vol = id.substringBefore(':')
        val rel = id.substringAfter(':', "")
        val base = if (vol.equals("primary", ignoreCase = true)) File("/storage/emulated/0") else File("/storage/$vol")
        return File(base, rel).takeIf { it.isDirectory && it.canRead() }
    }

    fun nodeOf(ctx: Context, uri: Uri): Node? {
        if (uri.scheme == "file") return uri.path?.let { File(it) }?.takeIf { it.isDirectory }?.let { FileNode(it) }
        pathOf(uri)?.let { return FileNode(it) }
        return DocumentFile.fromTreeUri(ctx, uri)?.takeIf { it.isDirectory }?.let { DocNode(ctx, it) }
    }

    fun size(n: Node, skip: (Node) -> Boolean = { false }): Long =
        if (skip(n)) 0 else if (n.isDir) n.children().sumOf { size(it, skip) } else n.length

    /** Copies [src] into [dst] (a folder is merged into it); files are written as .part and renamed, so a stop never leaves a whole-looking half file. */
    fun copy(src: Node, dst: File, cancelled: () -> Boolean, skip: (Node) -> Boolean = { false }, moved: (Long) -> Unit) {
        if (cancelled()) throw InterruptedException()
        if (skip(src)) return
        if (src.isDir) {
            if (!dst.isDirectory && !dst.mkdirs()) throw IOException("cannot create $dst")
            for (c in src.children()) copy(c, File(dst, c.name), cancelled, skip, moved)
            return
        }
        if (dst.isFile && dst.length() == src.length) { moved(src.length); return } // resumed: already there
        val part = File(dst.parentFile, dst.name + ".part")
        src.open().use { input ->
            part.outputStream().use { out ->
                val buf = ByteArray(1 shl 20)
                while (true) {
                    if (cancelled()) throw InterruptedException()
                    val n = input.read(buf)
                    if (n < 0) break
                    out.write(buf, 0, n)
                    moved(n.toLong())
                }
            }
        }
        dst.delete()
        if (!part.renameTo(dst)) throw IOException("cannot rename $part")
    }
}
