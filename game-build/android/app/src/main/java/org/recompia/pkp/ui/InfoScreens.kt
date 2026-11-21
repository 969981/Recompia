// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import android.content.Intent
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.OpenInNew
import androidx.compose.material.icons.rounded.Description
import androidx.compose.material.icons.rounded.Download
import androidx.compose.material.icons.rounded.Share
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.recompia.pkp.Paths
import org.recompia.pkp.R
import java.io.File
import java.io.RandomAccessFile

/** Every log of the runtime and of the app's jobs, newest first. */
private fun logFiles(ctx: android.content.Context): List<File> =
    listOf(Paths.logsDir, File(ctx.filesDir, "logs"), Paths.dataRoot)
        .flatMap { d -> d.listFiles()?.filter { it.isFile && (it.name.endsWith(".log") || it.name.endsWith(".txt") && it.name.contains("crash")) }.orEmpty() }
        .sortedByDescending { it.lastModified() }

/** The last [max] bytes of a file, whole lines. */
private fun tail(f: File, max: Long = 96 * 1024): String = runCatching {
    RandomAccessFile(f, "r").use { r ->
        val start = (r.length() - max).coerceAtLeast(0)
        r.seek(start)
        val b = ByteArray((r.length() - start).toInt())
        r.readFully(b)
        String(b, Charsets.UTF_8).let { if (start > 0) it.substringAfter('\n') else it }
    }
}.getOrDefault("")

@Composable
fun LogsScreen(onBack: () -> Unit) {
    val ctx = LocalContext.current
    val files by produceState(emptyList<File>()) { value = withContext(Dispatchers.IO) { logFiles(ctx) } }
    var chosen by remember { mutableStateOf<File?>(null) }
    val file = chosen ?: files.firstOrNull()
    val text by produceState("", file) { value = file?.let { withContext(Dispatchers.IO) { tail(it) } } ?: "" }
    val save = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("text/plain")) { uri: Uri? ->
        val f = file ?: return@rememberLauncherForActivityResult
        uri?.let { u -> Thread { runCatching { ctx.contentResolver.openOutputStream(u)?.use { o -> f.inputStream().use { it.copyTo(o) } } } }.start() }
    }
    Page(stringResource(R.string.ui_logs), onBack, side = {
        if (files.isEmpty()) Text(stringResource(R.string.ui_logs_none), style = MaterialTheme.typography.bodyLarge, color = Rc.text2)
        files.take(12).forEach { f ->
            NavRow(Icons.Rounded.Description, f.name, { chosen = f }, detail = ago(f.lastModified()) + "  " + ctx.size(f.length()),
                trailing = if (f == file) "•" else null)
        }
    }) {
        if (file != null) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                RcButton(stringResource(R.string.ui_share), {
                    val send = Intent(Intent.ACTION_SEND).setType("text/plain")
                        .putExtra(Intent.EXTRA_SUBJECT, file.name)
                        .putExtra(Intent.EXTRA_TEXT, text.takeLast(60_000))
                    ctx.startActivity(Intent.createChooser(send, ctx.getString(R.string.ui_share)))
                }, Modifier.initialFocus(), icon = Icons.Rounded.Share, kind = Kind.PRIMARY)
                RcButton(stringResource(R.string.ui_save_copy), { save.launch(file.name) }, icon = Icons.Rounded.Download)
            }
            Mono(file.path)
            Text(
                text.ifEmpty { stringResource(R.string.ui_logs_empty) },
                style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace), color = Rc.text2,
                modifier = Modifier.fillMaxWidth().heightIn(max = 2000.dp).clip(RoundedCornerShape(Rc.radiusSm)).background(Rc.sunken)
                    .horizontalScroll(rememberScrollState()).padding(12.dp),
            )
        }
    }
}

/** Markdown links read as their text in plain text. */
private fun plain(md: String) = md.replace(Regex("""\[([^\]]+)]\([^)]+\)"""), "$1").replace("**", "").replace("`", "")
    .replace(Regex("""(?m)^#+\s*"""), "").replace(Regex("""(?m)^- """), "\u2022 ")

@Composable
fun AboutScreen(onBack: () -> Unit) {
    val ctx = LocalContext.current
    val version = remember {
        val pi = ctx.packageManager.getPackageInfo(ctx.packageName, 0)
        "${pi.versionName} (${pi.longVersionCode})"
    }
    val credits = remember { plain(AppViewModel.readText(ctx, "CREDITS.md").trim().removePrefix("# Credits")).trim() }
    val legal = remember { plain(AppViewModel.readText(ctx, "LEGAL.md").trim().removePrefix("# Legal")).trim() }
    val open = { url: String -> runCatching { ctx.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url))) } }
    Page(stringResource(R.string.ui_about), onBack, side = {
        GlassCard(Modifier.fillMaxWidth()) {
            Image(painterResource(R.drawable.ui_logo), stringResource(R.string.ui_app_name), Modifier.fillMaxWidth().widthIn(max = 360.dp))
            Text(stringResource(R.string.ui_about_version, version), style = MaterialTheme.typography.titleMedium, color = Rc.text)
            Text(stringResource(R.string.ui_about_body), style = MaterialTheme.typography.bodyLarge, color = Rc.text2)
            Text(stringResource(R.string.ui_about_license), style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
            Text(stringResource(R.string.ui_supported), style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
            RcButton(stringResource(R.string.ui_about_source), { open(SOURCE_URL) }, Modifier.fillMaxWidth().initialFocus(), icon = Icons.AutoMirrored.Rounded.OpenInNew)
            RcButton(stringResource(R.string.ui_about_privacy), { open(PRIVACY_URL) }, Modifier.fillMaxWidth(), icon = Icons.AutoMirrored.Rounded.OpenInNew)
        }
    }) {
        GlassCard(Modifier.fillMaxWidth().readable()) {
            SectionTitle(stringResource(R.string.ui_about_legal))
            Text(legal, style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
        }
        GlassCard(Modifier.fillMaxWidth().readable()) {
            SectionTitle(stringResource(R.string.ui_about_credits))
            Text(credits, style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
            Text(stringResource(R.string.ui_about_android_credits), style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
        }
        // the licence texts of what this APK carries (assets/licenses, packed at build time) and the included Turnip's notice
        val notices = remember {
            val names = runCatching { ctx.assets.list("licenses")?.sorted() }.getOrNull().orEmpty().map { "licenses/$it" } + "gpu/NOTICE.txt"
            names.mapNotNull { n -> AppViewModel.readText(ctx, n).takeIf { it.isNotBlank() }?.let { n.substringAfterLast('/').removeSuffix(".txt") to it } }
        }
        if (notices.isNotEmpty()) GlassCard(Modifier.fillMaxWidth().readable()) {
            SectionTitle(stringResource(R.string.ui_about_licences))
            notices.forEach { (name, text) ->
                var open by remember { mutableStateOf(false) }
                NavRow(Icons.Rounded.Description, name, { open = !open })
                if (open) Text(text, style = MaterialTheme.typography.bodySmall, color = Rc.text2)
            }
        }
    }
}

private const val SOURCE_URL = "https://github.com/Dittobyte/Recompia"
private const val PRIVACY_URL = "https://github.com/Dittobyte/Recompia/blob/main/PRIVACY.md"

