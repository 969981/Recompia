// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.Settings
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.OpenInNew
import androidx.compose.material.icons.rounded.CheckCircle
import androidx.compose.material.icons.rounded.Download
import androidx.compose.material.icons.rounded.Info
import androidx.compose.material.icons.rounded.NewReleases
import androidx.compose.material.icons.rounded.Refresh
import androidx.compose.material.icons.rounded.Security
import androidx.compose.material.icons.rounded.Warning
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.recompia.pkp.BuildConfig
import org.recompia.pkp.R
import org.recompia.pkp.update.Releases
import org.recompia.pkp.update.Updater

/** Home: the update on offer (or the one just installed), with Update and What's new. Nothing when there is neither. */
@Composable
fun UpdateCard(onOpen: () -> Unit) {
    val s by Updater.state.collectAsStateWithLifecycle()
    s.updatedTo?.let { v ->
        GlassCard(Modifier.fillMaxWidth(), padding = 16.dp) {
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Pill(stringResource(R.string.ui_upd_updated, v), Rc.accent, Rc.tintAccent, Icons.Rounded.CheckCircle)
            }
            Text(stringResource(R.string.ui_upd_updated_body), style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
            RcButton(stringResource(R.string.ui_dismiss), { Updater.dismissUpdated() }, kind = Kind.QUIET)
        }
    }
    val r = s.release ?: return
    GlassCard(Modifier.fillMaxWidth(), padding = 16.dp) {
        Pill(stringResource(R.string.ui_upd_available), Rc.accent, Rc.tintAccent, Icons.Rounded.NewReleases)
        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(stringResource(R.string.ui_upd_title, r.version), style = MaterialTheme.typography.headlineSmall, color = Rc.text)
            Text(meta(LocalContext.current, r), style = MaterialTheme.typography.bodySmall, color = Rc.text3)
        }
        // the release's own "Update:" line says what it asks of the player (a rebuild of the game library, for one)
        notesIntro(r.notes)?.let { Text(inline(it), style = MaterialTheme.typography.bodyMedium, color = Rc.text2, maxLines = 5, overflow = TextOverflow.Ellipsis) }
        UpdateControls(s, r)
        if (!s.busy) RcButton(stringResource(R.string.ui_upd_whats_new), onOpen, Modifier.fillMaxWidth(), icon = Icons.Rounded.Info)
    }
}

/** The update's page: the controls beside the full release notes, Android's part first. */
@Composable
fun UpdatePage(onBack: () -> Unit) {
    val s by Updater.state.collectAsStateWithLifecycle()
    val r = s.release
    Page(stringResource(R.string.ui_upd_whats_new), onBack, side = {
        GlassCard(Modifier.fillMaxWidth(), padding = 16.dp) {
            if (r == null) Banner(stringResource(R.string.ui_upd_up_to_date_short, BuildConfig.VERSION_NAME), Rc.accent, Rc.tintAccent, Icons.Rounded.CheckCircle)
            else UpdateDetails(s, r)
        }
    }) {
        if (r != null) ReleaseNotes(r.notes)
    }
}

@Composable
private fun UpdateDetails(s: Updater.State, r: Releases.Release) {
    val ctx = LocalContext.current
    Pill(stringResource(R.string.ui_upd_available), Rc.accent, Rc.tintAccent, Icons.Rounded.NewReleases)
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(stringResource(R.string.ui_upd_title, r.version), style = MaterialTheme.typography.headlineSmall, color = Rc.text)
        Text(r.name, style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
        Text(meta(ctx, r), style = MaterialTheme.typography.bodySmall, color = Rc.text3)
    }
    UpdateControls(s, r, focus = true)
    Text(stringResource(R.string.ui_upd_first_note), style = MaterialTheme.typography.bodySmall, color = Rc.text3)
    RcButton(stringResource(R.string.ui_upd_release_page), { runCatching { ctx.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(r.page))) } },
        Modifier.fillMaxWidth(), icon = Icons.AutoMirrored.Rounded.OpenInNew, kind = Kind.QUIET)
}

/** Update / progress and Cancel / the error with Try again / the "Install unknown apps" step. */
@Composable
private fun UpdateControls(s: Updater.State, r: Releases.Release, focus: Boolean = false) {
    val ctx = LocalContext.current
    when {
        s.phase == Updater.Phase.DOWNLOADING -> {
            val pct = if (s.total > 0) (s.done * 100 / s.total).toInt().coerceIn(0, 100) else null
            Meter(pct, stringResource(R.string.ui_upd_downloading), stringResource(R.string.ui_upd_progress, ctx.size(s.done), ctx.size(s.total)))
            Text(stringResource(R.string.ui_upd_downloading), style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
            RcButton(stringResource(R.string.ui_cancel), { Updater.cancel() }, Modifier.fillMaxWidth())
        }
        s.busy -> {
            Meter(null, "", "")
            Text(stringResource(if (s.phase == Updater.Phase.VERIFYING) R.string.ui_upd_verifying else R.string.ui_upd_installing),
                style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
        }
        s.needsPermission -> {
            Banner(stringResource(R.string.ui_upd_perm_text), Rc.info, Rc.tintInfo, Icons.Rounded.Security)
            RcButton(stringResource(R.string.ui_upd_perm_button), {
                runCatching { ctx.startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${ctx.packageName}"))) }
            }, Modifier.fillMaxWidth().let { if (focus) it.initialFocus() else it }, icon = Icons.Rounded.Security, kind = Kind.PRIMARY)
        }
        else -> {
            s.error?.let { Banner(it, Rc.warn, Rc.tintWarn, Icons.Rounded.Warning) }
            RcButton(stringResource(if (s.error != null) R.string.ui_try_again else R.string.ui_upd_update_to, r.version), { Updater.update(ctx) },
                Modifier.fillMaxWidth().let { if (focus) it.initialFocus() else it }, icon = Icons.Rounded.Download, kind = Kind.PRIMARY)
        }
    }
}

/** Settings: the automatic check switch, Check now and where the check stands. */
@Composable
fun UpdateSettingsCard(onOpen: () -> Unit) {
    val ctx = LocalContext.current
    val s by Updater.state.collectAsStateWithLifecycle()
    var auto by remember { mutableStateOf(Updater.auto(ctx)) }
    GlassCard(Modifier.fillMaxWidth(), padding = 12.dp) {
        SectionTitle(stringResource(R.string.ui_upd_group))
        SwitchRow(stringResource(R.string.ui_upd_auto), auto, { auto = it; Updater.setAuto(ctx, it) })
        Text(stringResource(R.string.ui_upd_auto_note), style = MaterialTheme.typography.bodySmall, color = Rc.text3, modifier = Modifier.padding(horizontal = 12.dp))
        val status = when {
            s.checking -> stringResource(R.string.ui_upd_checking)
            s.release != null -> stringResource(R.string.ui_upd_found, s.release!!.version)
            s.checkedAt == 0L -> stringResource(R.string.ui_upd_never, BuildConfig.VERSION_NAME)
            s.checkFailed -> stringResource(R.string.ui_upd_check_failed, checkedAgo(s.checkedAt))
            else -> stringResource(R.string.ui_upd_up_to_date, BuildConfig.VERSION_NAME, checkedAgo(s.checkedAt))
        }
        Text(status, style = MaterialTheme.typography.bodyMedium, color = if (s.release != null) Rc.accent else Rc.text2, modifier = Modifier.padding(horizontal = 12.dp))
        Row(Modifier.padding(horizontal = 12.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            RcButton(stringResource(R.string.ui_upd_check_now), { Updater.check(ctx) }, icon = Icons.Rounded.Refresh, enabled = !s.checking && !s.busy)
            if (s.release != null) RcButton(stringResource(R.string.ui_upd_whats_new), onOpen, icon = Icons.Rounded.NewReleases, kind = Kind.PRIMARY)
        }
    }
}

/** "just now" for the last minute (the relative time span says "0 minutes ago"). */
@Composable
private fun checkedAgo(t: Long) = if (System.currentTimeMillis() - t < 60_000) stringResource(R.string.ui_upd_just_now) else ago(t)

private fun meta(ctx: Context, r: Releases.Release): String {
    val date = runCatching { java.text.DateFormat.getDateInstance(java.text.DateFormat.MEDIUM).format(java.time.Instant.parse(r.date).let { java.util.Date.from(it) }) }.getOrDefault("")
    return ctx.getString(R.string.ui_upd_meta, date, ctx.size(r.size)).trim()
}

// ---- release notes: the markdown GitHub shows (release.yml takes it from the tag's CHANGELOG.md section) ----------------------------

/** The notes' first paragraph before the first heading (the "**Update:**" line), from its "On Android" sentence when it has one;
 *  null when there is none. */
private fun notesIntro(md: String): String? {
    val intro = md.lineSequence().takeWhile { !it.startsWith("#") }.joinToString(" ") { it.trim() }.trim()
    val android = intro.indexOf("On Android")
    return (if (android >= 0) intro.substring(android) else intro).ifEmpty { null }
}

private data class Section(val title: String, val lines: List<String>)

private fun sections(md: String): List<Section> {
    val out = mutableListOf<Section>()
    var title = ""; var lines = mutableListOf<String>()
    for (l in unwrapMarkdown(md).lines()) {
        if (l.startsWith("#")) { if (title.isNotEmpty() || lines.any { it.isNotBlank() }) out += Section(title, lines); title = l.trimStart('#').trim(); lines = mutableListOf() }
        else lines += l
    }
    if (title.isNotEmpty() || lines.any { it.isNotBlank() }) out += Section(title, lines)
    return out
}

/** The intro and Highlights, then Android's section, then every other section behind "Show all changes". */
@Composable
private fun ReleaseNotes(md: String) {
    val all = remember(md) { sections(md) }
    val first = all.filter { it.title.isEmpty() || it.title.equals("Highlights", true) || it.title.equals("Android", true) }
        .sortedBy { if (it.title.isEmpty()) 0 else if (it.title.equals("Android", true)) 1 else 2 }
    val rest = all - first.toSet()
    var more by remember { mutableStateOf(false) }
    first.forEach { NotesCard(it) }
    if (rest.isNotEmpty()) {
        RcButton(stringResource(if (more) R.string.ui_upd_fewer_changes else R.string.ui_upd_all_changes, rest.size), { more = !more }, Modifier.fillMaxWidth())
        if (more) rest.forEach { NotesCard(it) }
    }
}

@Composable
private fun NotesCard(s: Section) {
    GlassCard(Modifier.fillMaxWidth().readable(), padding = 16.dp) {
        if (s.title.isNotEmpty()) SectionTitle(s.title)
        Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
            for (l in s.lines) {
                if (l.isBlank()) continue
                val bullet = Regex("""^(\s*)[-*] (.*)""").matchEntire(l)
                if (bullet != null) Row(Modifier.padding(start = (bullet.groupValues[1].length / 2 * 16).dp)) {
                    Text("•", style = MaterialTheme.typography.bodyMedium, color = Rc.accent, modifier = Modifier.width(16.dp))
                    Text(inline(bullet.groupValues[2]), style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
                } else Text(inline(l.trim()), style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
            }
        }
    }
}

/** **bold**, `code` and [links](url) (as their text). */
private fun inline(s: String): AnnotatedString = buildAnnotatedString {
    var i = 0
    for (m in Regex("""\*\*(.+?)\*\*|`([^`]+)`|\[([^\]]+)]\([^)]*\)""").findAll(s)) {
        append(s.substring(i, m.range.first))
        when {
            m.groups[1] != null -> withStyle(SpanStyle(fontWeight = FontWeight.SemiBold, color = Rc.text)) { append(inline(m.groupValues[1])) }
            m.groups[2] != null -> withStyle(SpanStyle(fontFamily = FontFamily.Monospace, background = Rc.surface2)) { append(m.groupValues[2]) }
            else -> append(m.groupValues[3])
        }
        i = m.range.last + 1
    }
    append(s.substring(i))
}
