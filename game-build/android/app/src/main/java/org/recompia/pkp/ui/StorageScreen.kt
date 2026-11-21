// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import android.content.Context
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Delete
import androidx.compose.material.icons.rounded.Download
import androidx.compose.material.icons.rounded.Error
import androidx.compose.material.icons.rounded.Upload
import androidx.compose.material.icons.rounded.Warning
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.recompia.pkp.Paths
import org.recompia.pkp.R
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/** A yes/no question; the safe answer has the focus. */
@Composable
fun Confirm(title: String, text: String, yes: String, onYes: () -> Unit, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = Rc.solidRaised,
        title = { Text(title, style = MaterialTheme.typography.headlineSmall) },
        text = { Text(text, style = MaterialTheme.typography.bodyLarge, color = Rc.text2) },
        confirmButton = { RcButton(yes, { onDismiss(); onYes() }, kind = Kind.DANGER) },
        dismissButton = { RcButton(stringResource(R.string.ui_cancel), onDismiss, Modifier.initialFocus()) },
    )
}

@Composable
private fun SizeRow(label: String, size: String, detail: String? = null, action: (@Composable () -> Unit)? = null) {
    Column(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Row {
            Text(label, style = MaterialTheme.typography.titleMedium, color = Rc.text, modifier = Modifier.weight(1f))
            Text(size, style = MaterialTheme.typography.titleMedium, color = Rc.text2)
        }
        if (detail != null) Text(detail, style = MaterialTheme.typography.bodySmall, color = Rc.text3)
        action?.invoke()
    }
}

@Composable
fun StorageScreen(vm: AppViewModel, onBack: () -> Unit) {
    val ui by vm.ui.collectAsStateWithLifecycle()
    val ctx = LocalContext.current
    val s = ui.sizes
    var ask by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(Unit) { vm.refreshSizes() }
    Page(stringResource(R.string.ui_storage), onBack, side = {
        GlassCard(Modifier.fillMaxWidth(), padding = 12.dp) {
            SectionTitle(stringResource(R.string.ui_storage_where))
            SizeRow(stringResource(R.string.ui_storage_free_label), ctx.size(s.free), shown(Paths.dataRoot.path))
            SizeRow(stringResource(R.string.ui_storage_dump), ctx.size(s.dump),
                if (s.dumpInPlace) stringResource(R.string.ui_storage_dump_in_place, shown(ui.report?.chosen?.path ?: "-")) else shown(Paths.dumpDir.path)) {
                if (!s.dumpInPlace && s.dump > 0) RcButton(stringResource(R.string.ui_storage_delete_dump), { ask = "dump" }, icon = Icons.Rounded.Delete)
            }
        }
    }) {
        GlassCard(Modifier.fillMaxWidth(), padding = 12.dp) {
            SectionTitle(stringResource(R.string.ui_storage_recompia))
            SizeRow(stringResource(R.string.ui_storage_build), ctx.size(maxOf(s.build, 0) + maxOf(s.library, 0)), stringResource(R.string.ui_storage_build_detail)) {
                RcButton(stringResource(R.string.ui_storage_delete_build), { ask = "build" }, icon = Icons.Rounded.Delete, enabled = s.build > 0 || s.library > 0)
            }
            SizeRow(stringResource(R.string.ui_storage_shaders), ctx.size(s.shaders), stringResource(R.string.ui_storage_shaders_detail)) {
                RcButton(stringResource(R.string.ui_storage_delete_shaders), { ask = "shaders" }, icon = Icons.Rounded.Delete, enabled = s.shaders > 0)
            }
            SizeRow(stringResource(R.string.ui_storage_logs), ctx.size(s.logs))
            SizeRow(stringResource(R.string.ui_saves), ctx.size(s.saves), stringResource(R.string.ui_storage_saves_detail)) {
                RcButton(stringResource(R.string.ui_storage_delete_saves), { ask = "saves" }, icon = Icons.Rounded.Warning, kind = Kind.QUIET, enabled = s.saves > 0)
            }
        }
    }
    when (ask) {
        "build" -> Confirm(stringResource(R.string.ui_storage_delete_build), stringResource(R.string.ui_confirm_build), stringResource(R.string.ui_delete),
            { vm.deleteBuild() }, { ask = null })
        "shaders" -> Confirm(stringResource(R.string.ui_storage_delete_shaders), stringResource(R.string.ui_confirm_shaders), stringResource(R.string.ui_delete),
            { vm.deleteShaders() }, { ask = null })
        "dump" -> Confirm(stringResource(R.string.ui_storage_delete_dump), stringResource(R.string.ui_confirm_dump), stringResource(R.string.ui_delete),
            { vm.deleteCopiedDump() }, { ask = null })
        // saves: two separate questions, the second one names what is lost
        "saves" -> Confirm(stringResource(R.string.ui_storage_delete_saves), stringResource(R.string.ui_confirm_saves_1), stringResource(R.string.ui_continue),
            { ask = "saves2" }, { if (ask == "saves") ask = null })
        "saves2" -> Confirm(stringResource(R.string.ui_confirm_saves_2_title), stringResource(R.string.ui_confirm_saves_2), stringResource(R.string.ui_confirm_saves_2_yes),
            { vm.deleteSaves() }, { ask = null })
    }
}

// ---- saves ------------------------------------------------------------------------------------------------------------------

@Composable
fun SavesScreen(vm: AppViewModel, onBack: () -> Unit) {
    val ctx = LocalContext.current
    val ui by vm.ui.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()
    var result by remember { mutableStateOf<Pair<Boolean, String>?>(null) }
    var pendingImport by remember { mutableStateOf<Uri?>(null) }
    val stamp = remember { SimpleDateFormat("yyyyMMdd-HHmm", Locale.ROOT).format(Date()) }
    val export = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/zip")) { uri ->
        uri ?: return@rememberLauncherForActivityResult
        scope.launch {
            result = withContext(Dispatchers.IO) {
                runCatching { exportSaves(ctx, uri) }.fold({ true to ctx.getString(R.string.ui_saves_exported, it) }, { false to (it.message ?: "") })
            }
        }
    }
    val import = rememberLauncherForActivityResult(OpenDocumentInData()) { pendingImport = it }
    LaunchedEffect(Unit) { vm.refreshSizes() }
    Page(stringResource(R.string.ui_saves), onBack) {
        GlassCard(Modifier.fillMaxWidth()) {
            Text(stringResource(R.string.ui_saves_body), style = MaterialTheme.typography.bodyLarge, color = Rc.text2)
            Mono(Paths.saveDir.path + "  " + ctx.size(ui.sizes.saves))
            RcButton(stringResource(R.string.ui_saves_export), { export.launch("recompia-saves-$stamp.zip") }, Modifier.fillMaxWidth().initialFocus(),
                icon = Icons.Rounded.Upload, kind = Kind.PRIMARY, big = true, enabled = ui.sizes.saves > 0)
            RcButton(stringResource(R.string.ui_saves_import), { import.launch(arrayOf("application/zip", "application/x-zip-compressed", "application/octet-stream")) },
                Modifier.fillMaxWidth(), icon = Icons.Rounded.Download, big = true)
            Text(stringResource(R.string.ui_saves_import_facts), style = MaterialTheme.typography.bodySmall, color = Rc.text3)
            result?.let { (ok, msg) -> Banner(msg, if (ok) Rc.accent else Rc.danger, if (ok) Rc.tintAccent else Rc.tintDanger, if (ok) Icons.Rounded.Download else Icons.Rounded.Error) }
        }
    }
    pendingImport?.let { uri ->
        Confirm(stringResource(R.string.ui_saves_import), stringResource(R.string.ui_saves_import_facts), stringResource(R.string.ui_saves_import_yes), {
            scope.launch {
                result = withContext(Dispatchers.IO) {
                    runCatching { importSaves(ctx, uri) }.fold({ true to ctx.getString(R.string.ui_saves_imported, it) }, { false to (it.message ?: "") })
                }
                vm.refreshSizes()
            }
        }, { pendingImport = null })
    }
}

/** Zips the save folder; returns the number of files. */
private fun exportSaves(ctx: Context, uri: Uri): Int {
    val root = Paths.saveDir
    var n = 0
    (ctx.contentResolver.openOutputStream(uri) ?: error("cannot write $uri")).use { os ->
        ZipOutputStream(os.buffered()).use { zip ->
            root.walkTopDown().filter { it.isFile }.forEach { f ->
                zip.putNextEntry(ZipEntry("save/" + f.relativeTo(root).invariantSeparatorsPath).apply { time = f.lastModified() })
                f.inputStream().use { it.copyTo(zip) }
                zip.closeEntry()
                n++
            }
        }
    }
    return n
}

/**
 * Unpacks a save zip (from this app or a PC's save folder) into the save folder. The current save first moves to
 * <dataRoot>/save-backup-<time>, so nothing is lost. A leading "save/" folder in the zip is dropped; entries that would land
 * outside the folder are refused. Returns the backup folder.
 */
private fun importSaves(ctx: Context, uri: Uri): String {
    val tmp = File(Paths.dataRoot, "save-import.tmp").apply { deleteRecursively(); mkdirs() }
    var files = 0
    (ctx.contentResolver.openInputStream(uri) ?: error("cannot read $uri")).use { input ->
        ZipInputStream(input.buffered()).use { zip ->
            while (true) {
                val e = zip.nextEntry ?: break
                val out = File(tmp, e.name)
                if (!out.canonicalPath.startsWith(tmp.canonicalPath + File.separator)) error(ctx.getString(R.string.ui_saves_bad_zip))
                if (e.isDirectory) out.mkdirs() else { out.parentFile?.mkdirs(); out.outputStream().use { zip.copyTo(it) }; files++ }
            }
        }
    }
    if (files == 0) { tmp.deleteRecursively(); error(ctx.getString(R.string.ui_saves_bad_zip)) }
    val top = tmp.listFiles().orEmpty()
    val content = if (top.size == 1 && top[0].isDirectory && top[0].name.equals("save", true)) top[0] else tmp
    val backup = File(Paths.dataRoot, "save-backup-" + SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(Date()))
    if (Paths.saveDir.exists() && !Paths.saveDir.renameTo(backup)) error("cannot move ${Paths.saveDir} to $backup")
    if (!content.renameTo(Paths.saveDir)) { backup.renameTo(Paths.saveDir); error("cannot move the imported save into place") }
    tmp.deleteRecursively()
    return backup.path
}
