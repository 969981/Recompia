// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.build

import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Is a file a Recompia game library for Android: ELF64, little endian, AArch64, and its dynamic symbol table defines SDL_main
 * (SDL calls it) and pkp_rt_api (the runtime's entry, version PKPGAME_1). Reads only the headers and .dynsym/.dynstr.
 */
object LibraryCheck {
    enum class Result { OK, NOT_ELF, NOT_ARM64, NO_EXPORTS }

    private val NEEDED = setOf("SDL_main", "pkp_rt_api")

    /** The 32-bit value of the defined data symbol [name] in [f]'s .dynsym (pkp_rt_api: the runtime API a file was built for), or null. */
    fun exportU32(f: File, name: String): Long? = runCatching {
        RandomAccessFile(f, "r").use { r ->
            fun read(off: Long, n: Int): ByteBuffer = ByteBuffer.wrap(ByteArray(n).also { r.seek(off); r.readFully(it) }).order(ByteOrder.LITTLE_ENDIAN)
            val h = read(0, 64)
            if (h.getInt(0) != 0x464c457f || h.get(4).toInt() != 2) return@use null
            val phoff = h.getLong(32); val phent = h.getShort(54).toInt() and 0xffff; val phnum = h.getShort(56).toInt() and 0xffff
            val shoff = h.getLong(40); val shent = h.getShort(58).toInt() and 0xffff; val shnum = h.getShort(60).toInt() and 0xffff
            val sh = read(shoff, shnum * shent)
            var value: Long? = null
            for (i in 0 until shnum) {
                val b = i * shent
                if (sh.getInt(b + 4) != 11) continue                         // SHT_DYNSYM
                val symOff = sh.getLong(b + 24); val symSize = sh.getLong(b + 32); val link = sh.getInt(b + 40)
                val strs = read(sh.getLong(link * shent + 24), sh.getLong(link * shent + 32).toInt()).array()
                val syms = read(symOff, symSize.toInt())
                for (e in 0 until (symSize / 24).toInt()) {
                    val n = syms.getInt(e * 24); if (n <= 0 || n >= strs.size || syms.getShort(e * 24 + 6).toInt() == 0) continue
                    var end = n; while (end < strs.size && strs[end] != 0.toByte()) end++
                    if (String(strs, n, end - n, Charsets.US_ASCII) == name) { value = syms.getLong(e * 24 + 8); break }
                }
            }
            val vaddr = value ?: return@use null
            val ph = read(phoff, phnum * phent)
            for (i in 0 until phnum) {
                val b = i * phent
                if (ph.getInt(b) != 1) continue                                // PT_LOAD
                val off = ph.getLong(b + 8); val va = ph.getLong(b + 16); val fsz = ph.getLong(b + 32)
                if (vaddr >= va && vaddr + 4 <= va + fsz) return@use read(off + (vaddr - va), 4).getInt(0).toLong() and 0xffffffffL
            }
            null
        }
    }.getOrNull()

    fun check(f: File): Result {
        try {
        RandomAccessFile(f, "r").use { r ->
            fun read(off: Long, n: Int): ByteBuffer = ByteBuffer.wrap(ByteArray(n).also { r.seek(off); r.readFully(it) }).order(ByteOrder.LITTLE_ENDIAN)
            val h = read(0, 64)
            if (h.getInt(0) != 0x464c457f) return Result.NOT_ELF
            if (h.get(4).toInt() != 2 || h.get(5).toInt() != 1 || h.getShort(18).toInt() != 183) return Result.NOT_ARM64
            val shoff = h.getLong(40)
            val shentsize = h.getShort(58).toInt() and 0xffff
            val shnum = h.getShort(60).toInt() and 0xffff
            if (shoff <= 0 || shentsize < 64 || shnum == 0 || shoff + shnum.toLong() * shentsize > r.length()) return Result.NO_EXPORTS
            val sh = read(shoff, shnum * shentsize)
            for (i in 0 until shnum) {
                val b = i * shentsize
                if (sh.getInt(b + 4) != 11) continue                     // SHT_DYNSYM
                val symOff = sh.getLong(b + 24); val symSize = sh.getLong(b + 32); val ent = sh.getLong(b + 56).takeIf { it >= 24 } ?: 24
                val link = sh.getInt(b + 40)
                if (link !in 0 until shnum || symSize > 64L shl 20) return Result.NO_EXPORTS
                val strOff = sh.getLong(link * shentsize + 24); val strSize = sh.getLong(link * shentsize + 32)
                if (strSize > 64L shl 20) return Result.NO_EXPORTS
                val syms = read(symOff, symSize.toInt())
                val strs = read(strOff, strSize.toInt()).array()
                val found = HashSet<String>()
                for (s in 0 until (symSize / ent).toInt()) {
                    val e = (s * ent).toInt()
                    val name = syms.getInt(e)
                    if (syms.getShort(e + 6).toInt() == 0 || name <= 0 || name >= strs.size) continue   // undefined
                    var end = name
                    while (end < strs.size && strs[end] != 0.toByte()) end++
                    String(strs, name, end - name, Charsets.US_ASCII).takeIf { it in NEEDED }?.let(found::add)
                }
                return if (found == NEEDED) Result.OK else Result.NO_EXPORTS
            }
            return Result.NO_EXPORTS
        }
        } catch (e: Exception) {
            return Result.NOT_ELF
        }
    }
}
