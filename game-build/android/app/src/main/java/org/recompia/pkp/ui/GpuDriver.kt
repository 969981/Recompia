// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import android.content.Context
import android.net.Uri
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.annotation.StringRes
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.CheckCircle
import androidx.compose.material.icons.rounded.Error
import androidx.compose.material.icons.rounded.FolderOpen
import androidx.compose.material.icons.rounded.Memory
import androidx.compose.material.icons.rounded.RestartAlt
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import org.recompia.pkp.Paths
import org.recompia.pkp.R
import java.io.File
import java.io.InputStream
import java.util.zip.ZipInputStream

/**
 * A Vulkan driver the player installs ([gfx] vulkan_driver_dir in pkp.ini, read by runtime/src/os/os_android_vkdriver.cpp).
 * A driver package is a zip with meta.json and the library its "libraryName" names: the layout of the Switch emulators' driver
 * packages, so the zips players already have work here. The app downloads no driver and ships no Qualcomm package; it may carry
 * the project's own Mesa Turnip build (MIT) as the asset [INCLUDED]. The picked zip is unpacked into app-private storage (the
 * linker refuses libraries on shared storage) and the key points at it; empty = the system driver.
 */
object GpuDriver {
    data class Meta(val name: String, val version: String, val library: String)

    /** Why a zip was refused, as the player reads it. */
    class Refused(@StringRes val reason: Int, val arg: String = "") : Exception(arg)

    private const val KEY_SECTION = "gfx"
    private const val KEY = "vulkan_driver_dir"
    private const val MAX_LIB = 256L shl 20
    /** The project's own Turnip build, when its zip is packed into the APK (not yet: then the option is hidden). */
    const val INCLUDED = "gpu/turnip.zip"

    private fun slot(ctx: Context) = File(ctx.filesDir, "gpu_driver")

    /** Custom drivers exist for Qualcomm Adreno GPUs only (Turnip, Qualcomm's own packages). */
    fun isAdreno(): Boolean {
        val soc = if (Build.VERSION.SDK_INT >= 31) Build.SOC_MANUFACTURER else ""
        return soc.equals("QTI", true) || soc.contains("qualcomm", true) || Build.HARDWARE.contains("qcom", true)
    }

    fun hasIncluded(ctx: Context) = runCatching { ctx.assets.open(INCLUDED).close() }.isSuccess

    fun meta(dir: File): Meta? = runCatching {
        val j = JSONObject(File(dir, "meta.json").readText())
        Meta(j.optString("name"), j.optString("driverVersion"), j.getString("libraryName"))
    }.getOrNull()

    /** The installed driver the game will load, or null for the system driver. */
    fun current(): Meta? = Ini.load(Paths.pkpIni).get(KEY_SECTION, KEY)?.ifBlank { null }?.let { meta(File(it)) }

    fun install(ctx: Context, uri: Uri): Meta =
        install(ctx) { ctx.contentResolver.openInputStream(uri) ?: throw Refused(R.string.ui_gpu_err_unreadable) }

    fun installIncluded(ctx: Context): Meta = install(ctx) { ctx.assets.open(INCLUDED) }

    /** Unpacks meta.json and the library it names (base names only, nothing else), checks it, then selects it. */
    private fun install(ctx: Context, open: () -> InputStream): Meta {
        val tmp = File(ctx.filesDir, "gpu_driver.tmp").apply { deleteRecursively(); mkdirs() }
        try {
            var entries = 0
            open().use { raw ->
                ZipInputStream(raw.buffered()).use { zip ->
                    while (true) {
                        val e = runCatching { zip.nextEntry }.getOrElse { throw Refused(R.string.ui_gpu_err_not_zip) } ?: break
                        entries++
                        val base = e.name.substringAfterLast('/')
                        if (e.isDirectory || !(base == "meta.json" || base.endsWith(".so"))) continue
                        File(tmp, base).outputStream().use { out ->
                            val buf = ByteArray(1 shl 16)
                            var total = 0L
                            while (true) {
                                val n = zip.read(buf)
                                if (n < 0) break
                                total += n
                                if (total > MAX_LIB) throw Refused(R.string.ui_gpu_err_too_large, base)
                                out.write(buf, 0, n)
                            }
                        }
                    }
                }
            }
            if (entries == 0) throw Refused(R.string.ui_gpu_err_not_zip)
            val m = meta(tmp) ?: throw Refused(R.string.ui_gpu_err_no_meta)
            if (m.library.isEmpty() || '/' in m.library || !m.library.endsWith(".so")) throw Refused(R.string.ui_gpu_err_no_meta)
            val lib = File(tmp, m.library)
            if (!lib.isFile) throw Refused(R.string.ui_gpu_err_missing_lib, m.library)
            val h = ByteArray(20).also { b -> lib.inputStream().use { it.read(b) } }
            // ELF, 64-bit, e_machine 183 (AArch64): the app runs arm64 only
            val elf = h[0] == 0x7f.toByte() && h[1] == 'E'.code.toByte() && h[2] == 'L'.code.toByte() && h[3] == 'F'.code.toByte()
            if (!elf || h[4] != 2.toByte() || h[18] != 183.toByte() || h[19] != 0.toByte()) throw Refused(R.string.ui_gpu_err_arch, m.library)
            val dst = slot(ctx)
            dst.deleteRecursively()
            if (!tmp.renameTo(dst)) throw Refused(R.string.ui_gpu_err_unreadable)
            setKey(dst.absolutePath)
            return m
        } finally {
            tmp.deleteRecursively()
        }
    }

    fun useSystem(ctx: Context) {
        setKey("")
        slot(ctx).deleteRecursively()
    }

    private fun setKey(v: String) {
        val ini = Ini.load(Paths.pkpIni)
        ini.set(KEY_SECTION, KEY, v)
        ini.save(Paths.pkpIni)
    }
}

/** The driver's name for display: "Turnip 26.3.1" or "System driver". */
@Composable
fun driverName(m: GpuDriver.Meta?): String =
    if (m == null) stringResource(R.string.ui_gpu_system) else listOf(m.name, m.version).filter { it.isNotBlank() }.joinToString(" ").ifEmpty { m.library }

/**
 * Install, switch and explain the GPU driver. Used by Settings and by the wizard's optional driver step ([wizard] shows the
 * recommendation and hides the long note). On a phone without an Adreno GPU it explains why there is nothing to choose.
 */
@Composable
fun GpuDriverPanel(wizard: Boolean = false, onChanged: (GpuDriver.Meta?) -> Unit = {}) {
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    val adreno = remember { GpuDriver.isAdreno() }
    val included = remember { GpuDriver.hasIncluded(ctx) }
    var cur by remember { mutableStateOf(runCatching { GpuDriver.current() }.getOrNull()) }
    var err by remember { mutableStateOf<GpuDriver.Refused?>(null) }
    var busy by remember { mutableStateOf(false) }
    val run = { work: () -> GpuDriver.Meta? ->
        busy = true
        scope.launch {
            val r = withContext(Dispatchers.IO) { runCatching(work) }
            busy = false
            r.onSuccess { cur = it; err = null; onChanged(it) }
                .onFailure { err = it as? GpuDriver.Refused ?: GpuDriver.Refused(R.string.ui_gpu_err_unreadable, it.message ?: "") }
        }
    }
    // first run on Adreno: the included Turnip is installed at once (once only, so "Use the system driver" stays undone)
    var auto by remember { mutableStateOf(false) }
    androidx.compose.runtime.LaunchedEffect(Unit) {
        val prefs = ctx.getSharedPreferences("ui", Context.MODE_PRIVATE)
        if (wizard && adreno && included && cur == null && !prefs.getBoolean("turnip_auto", false)) {
            prefs.edit().putBoolean("turnip_auto", true).apply()
            auto = true
            run { GpuDriver.installIncluded(ctx) }
        }
    }
    val pick = rememberLauncherForActivityResult(OpenDocumentInData()) { uri ->
        if (uri != null) run { GpuDriver.install(ctx, uri) }
    }
    Text(stringResource(R.string.ui_gpu_what), style = MaterialTheme.typography.bodyMedium, color = Rc.text2, modifier = Modifier.padding(horizontal = 12.dp))
    if (!adreno) {
        Banner(stringResource(R.string.ui_gpu_not_adreno), Rc.info, Rc.tintInfo, Icons.Rounded.Memory)
        return
    }
    Row(Modifier.padding(horizontal = 12.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        androidx.compose.material3.Icon(if (cur == null) Icons.Rounded.Memory else Icons.Rounded.CheckCircle, null, tint = Rc.accent)
        Text(stringResource(R.string.ui_gpu_active, driverName(cur)), style = MaterialTheme.typography.titleMedium, color = Rc.text)
    }
    err?.let { e ->
        val reason = if (e.arg.isEmpty()) stringResource(e.reason) else stringResource(e.reason, e.arg)
        Banner(reason, Rc.danger, Rc.tintDanger, Icons.Rounded.Error)
    }
    if (auto && cur != null) Text(stringResource(R.string.ui_gpu_auto_done), style = MaterialTheme.typography.bodyMedium, color = Rc.accent,
        modifier = Modifier.padding(horizontal = 12.dp))
    if (cur == null) Text(stringResource(R.string.ui_gpu_recommend), style = MaterialTheme.typography.bodyMedium, color = Rc.text2,
        modifier = Modifier.padding(horizontal = 12.dp))
    // the project's own Turnip is the recommended choice: the primary action, focused first in the wizard
    if (included) RcButton(stringResource(R.string.ui_gpu_use_included), { run { GpuDriver.installIncluded(ctx) } },
        Modifier.fillMaxWidth().padding(horizontal = 12.dp).let { if (wizard && cur == null) it.initialFocus() else it },
        icon = Icons.Rounded.Memory, kind = if (cur == null) Kind.PRIMARY else Kind.SECONDARY, enabled = !busy)
    // driver zips copied to <dataRoot>/drivers: offered by name before the picker
    val zips = remember { java.io.File(org.recompia.pkp.Paths.dataRoot, "drivers").listFiles { f -> f.name.endsWith(".zip", true) }?.sortedBy { it.name }.orEmpty() }
    zips.forEach { z ->
        RcButton(stringResource(R.string.ui_gpu_install_found, z.name), { run { GpuDriver.install(ctx, android.net.Uri.fromFile(z)) } },
            Modifier.fillMaxWidth().padding(horizontal = 12.dp), icon = Icons.Rounded.Memory, enabled = !busy)
    }
    RcButton(stringResource(R.string.ui_gpu_install), { pick.launch(arrayOf("application/zip", "application/octet-stream", "*/*")) },
        Modifier.fillMaxWidth().padding(horizontal = 12.dp), icon = Icons.Rounded.FolderOpen, kind = if (included || cur != null) Kind.SECONDARY else Kind.PRIMARY, enabled = !busy)
    if (cur != null) RcButton(stringResource(R.string.ui_gpu_use_system), {
        run { GpuDriver.useSystem(ctx); null }
    }, Modifier.padding(horizontal = 12.dp), icon = Icons.Rounded.RestartAlt, enabled = !busy)
    Text(stringResource(R.string.ui_gpu_own_files), style = MaterialTheme.typography.bodySmall, color = Rc.text3, modifier = Modifier.padding(horizontal = 12.dp))
    if (!wizard) Text(stringResource(R.string.ui_gpu_note), style = MaterialTheme.typography.bodySmall, color = Rc.text3, modifier = Modifier.padding(horizontal = 12.dp))
}

/** Settings card around [GpuDriverPanel]. */
@Composable
fun GpuDriverCard() {
    GlassCard(Modifier.fillMaxWidth(), padding = 12.dp) {
        SectionTitle(stringResource(R.string.ui_gpu_group))
        GpuDriverPanel()
    }
}
