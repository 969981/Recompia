// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import android.content.Context
import android.content.Intent
import android.widget.Toast
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.relocation.BringIntoViewRequester
import androidx.compose.foundation.relocation.bringIntoViewRequester
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.Login
import androidx.compose.material.icons.automirrored.rounded.Send
import androidx.compose.material.icons.rounded.Add
import androidx.compose.material.icons.rounded.Delete
import androidx.compose.material.icons.rounded.ContentCopy
import androidx.compose.material.icons.rounded.ExpandLess
import androidx.compose.material.icons.rounded.ExpandMore
import androidx.compose.material.icons.rounded.Group
import androidx.compose.material.icons.rounded.Info
import androidx.compose.material.icons.rounded.Person
import androidx.compose.material.icons.rounded.PersonAdd
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material.icons.rounded.Share
import androidx.compose.material.icons.rounded.Warning
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.ime
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import org.recompia.pkp.Coop
import org.recompia.pkp.GameLauncher
import org.recompia.pkp.R
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/*
 * Recompia: Co-op, laid out as the PC launcher's Co-op page (launcher/src/pages/coop.js): each co-op save on this phone with its players,
 * who hosts next (Coop.hostsByDefault, coop_save.rs choose_host), Start as (Auto / Host / Join, per start, never stored) and Play; the
 * creator's players (Coop.addMember) and invite code (Coop.invite, Coop.shorten); Join a friend (the long invite or its short code,
 * Coop.resolve / Coop.accept) and New co-op save (Coop.create); How co-op works. Home only shows CoopEntry and PlayCoopButton.
 * Network calls run off the main thread.
 */

/** The co-op saves on this phone; empty when the folder cannot be read. */
fun coopSaves(): List<Coop.Profile> = runCatching { Coop.list() }.getOrDefault(emptyList())

private fun hostsNext(p: Coop.Profile) = runCatching { Coop.hostsByDefault(p) }.getOrDefault(false)

private enum class Form { JOIN, NEW }

/** Home's way in: one row saying where co-op stands (no save, the one save and who hosts, or how many). */
@Composable
fun CoopEntry(saves: List<Coop.Profile>, onOpen: () -> Unit) {
    val one = saves.singleOrNull()
    val hosts = remember(one) { one != null && hostsNext(one) }
    val detail = when {
        one != null -> stringResource(R.string.ui_coop_home_one, one.name, one.invite.members.joinToString { it.name },
            stringResource(if (hosts) R.string.ui_coop_you_host else R.string.ui_coop_you_visit))
        saves.isEmpty() -> stringResource(R.string.ui_coop_home_none)
        else -> stringResource(R.string.ui_coop_home_many, saves.size)
    }
    NavRow(Icons.Rounded.Group, stringResource(R.string.ui_coop_group), onOpen, detail = detail)
}

/** Home's second button under Play: starts the only co-op save (Start as Auto), or opens Co-op to choose one of several. */
@Composable
fun PlayCoopButton(saves: List<Coop.Profile>, onOpen: () -> Unit, modifier: Modifier = Modifier) {
    val ctx = LocalContext.current
    val one = saves.singleOrNull()
    RcButton(if (one != null) stringResource(R.string.ui_coop_play_named, one.name) else stringResource(R.string.ui_coop_play),
        { if (one != null) GameLauncher.launchCoop(ctx, one.id, "auto") else onOpen() }, modifier, icon = Icons.Rounded.Group)
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun CoopScreen(canPlay: Boolean, onBack: () -> Unit, load: () -> List<Coop.Profile> = ::coopSaves) {
    val ctx = LocalContext.current
    val clip = LocalClipboardManager.current
    val scope = rememberCoroutineScope()
    var saves by remember { mutableStateOf(load()) }
    var form by remember { mutableStateOf<Form?>(null) }
    var startAs by remember { mutableStateOf(mapOf<String, String>()) }      // save id -> auto | host | visit, for this visit only
    // the creator's invite panel (one save at a time) and add-player field
    var open by remember { mutableStateOf<String?>(null) }
    var short by remember { mutableStateOf<String?>(null) }
    var shortening by remember { mutableStateOf(false) }
    var inviteReq by remember { mutableStateOf(0) }
    var adding by remember { mutableStateOf<String?>(null) }
    var addName by remember { mutableStateOf("") }
    var addError by remember { mutableStateOf<String?>(null) }
    var fresh by remember { mutableStateOf<String?>(null) }   // the save just made: its card takes the focus, the keyboard closes
    val keyboard = LocalSoftwareKeyboardController.current
    // Join a friend
    var text by remember { mutableStateOf("") }
    var joinError by remember { mutableStateOf<String?>(null) }
    var lookingUp by remember { mutableStateOf(false) }
    val invite = remember(text) { Coop.parseInvite(text) }
    // New co-op save
    var newName by remember { mutableStateOf("") }
    var myName by remember { mutableStateOf("") }
    var createError by remember { mutableStateOf<String?>(null) }
    var removing by remember { mutableStateOf<Coop.Profile?>(null) }   // the save the confirm dialog asks about

    fun refresh() { saves = load() }
    fun showInvite(p: Coop.Profile) {
        val req = ++inviteReq   // only the latest request's code is shown (an older one may lack a player added since)
        open = p.id; short = null; shortening = true
        scope.launch {
            val code = withContext(Dispatchers.IO) { Coop.shorten(p) }
            if (req == inviteReq) { short = code; shortening = false }
        }
    }
    fun add(p: Coop.Profile) {
        addError = Coop.addMember(p.id, addName)
        if (addError == null) { addName = ""; adding = null; refresh(); saves.firstOrNull { it.id == p.id }?.let(::showInvite) }   // a new invite: share it again
    }
    fun create() {
        val before = saves.map { it.id }.toSet()
        createError = Coop.create(newName, myName)
        if (createError == null) {
            newName = ""; form = null; refresh()
            keyboard?.hide()
            fresh = saves.firstOrNull { it.id !in before }?.id   // next: its card (Play, else Invite code); Add a player is one press away
        }
    }
    val canCreate = newName.isNotBlank() && myName.isNotBlank()

    val intro: @Composable ColumnScope.() -> Unit = {
        Text(stringResource(R.string.ui_coop_lead), style = MaterialTheme.typography.bodyLarge, color = Rc.text2)
        Banner(stringResource(R.string.ui_coop_experimental), Rc.info, Rc.tintInfo, Icons.Rounded.Info)
    }

    val saveCards: @Composable ColumnScope.() -> Unit = {
        intro()
        SectionTitle(stringResource(R.string.ui_coop_saves))
        saves.forEachIndexed { i, p ->
            val creator = Coop.isCreator(p)
            val choice = startAs[p.id] ?: "auto"
            val hosts = remember(p) { hostsNext(p) }
            GlassCard(Modifier.fillMaxWidth(), padding = 16.dp) {
                Column {
                    Text(p.name, style = MaterialTheme.typography.titleLarge, color = Rc.text)
                    Text(if (creator) stringResource(R.string.ui_coop_made_by_you) else stringResource(R.string.ui_coop_made_by, p.invite.members.firstOrNull()?.name ?: ""),
                        style = MaterialTheme.typography.bodySmall, color = Rc.text3)
                }
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp), itemVerticalAlignment = Alignment.CenterVertically) {
                    for (m in p.invite.members) {
                        if (m.id == p.me) Pill(stringResource(R.string.ui_coop_member_you, m.name), Rc.accent, Rc.tintAccent, Icons.Rounded.Person)
                        else Pill(m.name, Rc.text2, Rc.surface2)
                    }
                    if (creator && p.invite.members.size < 4 && adding != p.id)
                        RcButton(stringResource(R.string.ui_coop_add_player), { adding = p.id; addName = ""; addError = null }, icon = Icons.Rounded.PersonAdd, kind = Kind.QUIET)
                }
                if (adding == p.id) {
                    OutlinedTextField(addName, { addName = it; addError = null }, label = { Text(stringResource(R.string.ui_coop_add_name)) }, singleLine = true,
                        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done), keyboardActions = KeyboardActions(onDone = { if (addName.isNotBlank()) add(p) }),
                        modifier = Modifier.fillMaxWidth().initialFocus())
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        RcButton(stringResource(R.string.ui_coop_add), { add(p) }, icon = Icons.Rounded.PersonAdd, enabled = addName.isNotBlank())
                        RcButton(stringResource(R.string.ui_cancel), { adding = null; addError = null }, kind = Kind.QUIET)
                    }
                    addError?.let { Banner(it, Rc.warn, Rc.tintWarn, Icons.Rounded.Warning) }
                }
                if (creator && p.invite.members.size < 2) Banner(stringResource(R.string.ui_coop_lonely), Rc.warn, Rc.tintWarn, Icons.Rounded.Info)
                ChoiceRow(stringResource(R.string.ui_coop_start_as), listOf("auto" to stringResource(R.string.ui_coop_auto),
                    "host" to stringResource(R.string.ui_coop_host), "visit" to stringResource(R.string.ui_coop_join)), choice,
                    { startAs = startAs + (p.id to it) })
                Text(stringResource(when (choice) {
                    "host" -> R.string.ui_coop_host_now
                    "visit" -> R.string.ui_coop_visit_now
                    else -> if (hosts) R.string.ui_coop_you_host else R.string.ui_coop_you_visit
                }), style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
                // a gamepad walks the card's last part in visual order, whatever the FlowRows wrap to on a narrow screen: DOWN / UP go
                // Play, Invite code, Copy code, Share, Copy long invite, Remove; LEFT / RIGHT stay in a row (Play, Invite code) or
                // (Copy code, Share, Copy long invite). A disabled target cannot take focus, so it is skipped here (Default: geometric).
                val playF = remember { FocusRequester() }
                val inviteF = remember { FocusRequester() }
                val copyF = remember { FocusRequester() }
                val shareF = remember { FocusRequester() }
                val longF = remember { FocusRequester() }
                val removeF = remember { FocusRequester() }
                val panel = open == p.id && !shortening   // the panel's buttons are enabled
                val hasLong = panel && short != null
                val playOrDefault = if (canPlay) playF else FocusRequester.Default
                val aboveRemove = when { hasLong -> longF; panel -> shareF; creator -> inviteF; else -> playOrDefault }
                val belowTop = if (panel) copyF else removeF
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    RcButton(stringResource(R.string.ui_coop_play), { GameLauncher.launchCoop(ctx, p.id, choice) },
                        Modifier.focusRequester(playF).focusProperties { down = if (creator) inviteF else belowTop; left = FocusRequester.Cancel; right = if (creator) inviteF else FocusRequester.Cancel }
                            .let { if (fresh == p.id) it.initialFocus(fresh) else if (i == 0 && fresh == null) it.initialFocus() else it },
                        icon = Icons.Rounded.PlayArrow, kind = Kind.PRIMARY, enabled = canPlay)
                    if (creator) RcButton(stringResource(R.string.ui_coop_invite_others), { if (open == p.id) open = null else showInvite(p) },
                        Modifier.focusRequester(inviteF).focusProperties { up = playOrDefault; down = belowTop; left = if (canPlay) playF else FocusRequester.Cancel; right = FocusRequester.Cancel }
                            .let { if (fresh == p.id && !canPlay) it.initialFocus(fresh) else it },
                        icon = Icons.AutoMirrored.Rounded.Send)
                }
                if (open == p.id) {
                    val long = remember(p) { Coop.invite(p) }
                    val code = short
                    val names = p.invite.members.filter { it.id != p.me }.joinToString { it.name }
                    if (names.isNotEmpty()) Text(stringResource(R.string.ui_coop_invite_to, names), style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
                    when {
                        code != null -> {
                            Text(code, style = MaterialTheme.typography.displaySmall.copy(fontFamily = FontFamily.Monospace), color = Rc.text)
                            Text(stringResource(R.string.ui_coop_short_note), style = MaterialTheme.typography.bodySmall, color = Rc.text3)
                        }
                        shortening -> Meter(null, "...", "")
                        else -> Text(stringResource(R.string.ui_coop_no_short), style = MaterialTheme.typography.bodySmall, color = Rc.text3)
                    }
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        val send = code ?: long
                        RcButton(stringResource(R.string.ui_coop_copy), { clip.setText(AnnotatedString(send)) },
                            Modifier.focusRequester(copyF).focusProperties { up = if (creator) inviteF else playOrDefault; down = shareF; left = FocusRequester.Cancel; right = shareF }.initialFocus(shortening),
                            icon = Icons.Rounded.ContentCopy, enabled = !shortening)
                        RcButton(stringResource(R.string.ui_coop_share_code), { share(ctx, send) },
                            Modifier.focusRequester(shareF).focusProperties { up = copyF; down = if (hasLong) longF else removeF; left = copyF; right = if (hasLong) longF else FocusRequester.Cancel },
                            icon = Icons.Rounded.Share, enabled = !shortening)
                        if (hasLong) RcButton(stringResource(R.string.ui_coop_copy_long), { clip.setText(AnnotatedString(long)) },
                            Modifier.focusRequester(longF).focusProperties { up = shareF; down = removeF; left = shareF; right = FocusRequester.Cancel }, kind = Kind.QUIET)
                    }
                }
                RcButton(stringResource(R.string.ui_coop_remove), { removing = p },
                    Modifier.focusRequester(removeF).focusProperties { up = aboveRemove; left = FocusRequester.Cancel; right = FocusRequester.Cancel },
                    icon = Icons.Rounded.Delete, kind = Kind.QUIET)
            }
        }
    }

    Page(stringResource(R.string.ui_coop_group), onBack, side = if (saves.isEmpty()) null else saveCards) {
        if (saves.isEmpty()) intro()
        NavRow(Icons.AutoMirrored.Rounded.Login, stringResource(R.string.ui_coop_join_title), { form = if (form == Form.JOIN) null else Form.JOIN },
            if (saves.isEmpty()) Modifier.initialFocus() else Modifier, detail = stringResource(R.string.ui_coop_join_detail))
        if (form == Form.JOIN) GlassCard(Modifier.fillMaxWidth(), padding = 16.dp) {
            OutlinedTextField(text, { text = it; joinError = null }, label = { Text(stringResource(R.string.ui_coop_invite)) }, maxLines = 3,
                isError = text.isNotBlank() && invite == null && Coop.shortCode(text) == null, modifier = Modifier.fillMaxWidth())
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                if (text.isBlank()) RcButton(stringResource(R.string.ui_coop_paste), { text = clip.getText()?.text.orEmpty() }, Modifier.initialFocus(),
                    icon = Icons.Rounded.ContentCopy)
                if (Coop.shortCode(text) != null) RcButton(stringResource(R.string.ui_coop_look_up), {
                    lookingUp = true
                    scope.launch {
                        val r = withContext(Dispatchers.IO) { Coop.resolve(text) }
                        lookingUp = false
                        r.onSuccess { text = it; joinError = null }.onFailure { joinError = it.message }
                    }
                }, Modifier.initialFocus(text), kind = Kind.PRIMARY, enabled = !lookingUp)
            }
            if (invite != null) {
                Text(stringResource(R.string.ui_coop_who, invite.name), style = MaterialTheme.typography.titleMedium, color = Rc.text)
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    invite.members.drop(1).forEachIndexed { k, m ->   // the creator (first) is never joined: coop.rs accept
                        RcButton(m.name, {
                            joinError = Coop.accept(text, m.id)
                            if (joinError == null) {
                                text = ""; form = null; refresh()
                                Toast.makeText(ctx, R.string.ui_coop_joined, Toast.LENGTH_LONG).show()
                            }
                        }, if (k == 0) Modifier.initialFocus(invite.id) else Modifier, icon = Icons.Rounded.Person)
                    }
                }
            }
            joinError?.let { Banner(it, Rc.warn, Rc.tintWarn, Icons.Rounded.Warning) }
        }
        NavRow(Icons.Rounded.Add, stringResource(R.string.ui_coop_new), { form = if (form == Form.NEW) null else Form.NEW },
            detail = stringResource(R.string.ui_coop_new_detail))
        if (form == Form.NEW) {
            // the keyboard must not cover Create: a focused field brings the whole card into view once the keyboard has opened
            // (Page pads by WindowInsets.safeDrawing, which includes the IME; the activity is adjustResize)
            val bring = remember { BringIntoViewRequester() }
            var nameFocused by remember { mutableStateOf(false) }
            var meFocused by remember { mutableStateOf(false) }
            val typing = nameFocused || meFocused
            val ime = WindowInsets.ime.getBottom(LocalDensity.current)
            LaunchedEffect(typing, ime) { if (typing) { delay(80); bring.bringIntoView() } }   // restarts on every keyboard step: the last one wins

            GlassCard(Modifier.fillMaxWidth().bringIntoViewRequester(bring), padding = 16.dp) {
                Text(stringResource(R.string.ui_coop_new_note), style = MaterialTheme.typography.bodySmall, color = Rc.text3)
                OutlinedTextField(newName, { newName = it; createError = null }, label = { Text(stringResource(R.string.ui_coop_new_name)) }, singleLine = true,
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Next), modifier = Modifier.fillMaxWidth().onFocusChanged { nameFocused = it.isFocused }.initialFocus())
                OutlinedTextField(myName, { myName = it; createError = null }, label = { Text(stringResource(R.string.ui_coop_my_name)) }, singleLine = true,
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done), keyboardActions = KeyboardActions(onDone = { if (canCreate) create() }),
                    modifier = Modifier.fillMaxWidth().onFocusChanged { meFocused = it.isFocused })
                RcButton(stringResource(R.string.ui_coop_create), ::create, icon = Icons.Rounded.Add, kind = Kind.PRIMARY, enabled = canCreate)
                createError?.let { Banner(it, Rc.warn, Rc.tintWarn, Icons.Rounded.Warning) }
            }
        }
        // the launcher's "How co-op works": open while there is no save, folded away under the saves
        var how by remember { mutableStateOf(false) }
        if (saves.isEmpty() || how) HowCoopWorks()
        if (saves.isNotEmpty()) RcButton(stringResource(R.string.ui_coop_how), { how = !how },
            icon = if (how) Icons.Rounded.ExpandLess else Icons.Rounded.ExpandMore, kind = Kind.QUIET)
    }

    // the launcher's "Delete <name> from this PC?": this phone's folder only, no relay call; the other players keep their copies
    removing?.let { p ->
        val rest = p.invite.members.filter { it.id != p.me }.joinToString { it.name }
        var removeError by remember(p) { mutableStateOf<String?>(null) }
        androidx.compose.material3.AlertDialog(
            onDismissRequest = { removing = null },
            containerColor = Rc.solidRaised,
            title = { Text(stringResource(R.string.ui_coop_remove_title, p.name), style = MaterialTheme.typography.headlineSmall) },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Text(if (rest.isNotEmpty()) stringResource(R.string.ui_coop_remove_text, rest) else stringResource(R.string.ui_coop_remove_text_alone),
                        style = MaterialTheme.typography.bodyLarge, color = Rc.text2)
                    removeError?.let { Banner(it, Rc.warn, Rc.tintWarn, Icons.Rounded.Warning) }
                }
            },
            confirmButton = {
                RcButton(stringResource(R.string.ui_coop_remove), {
                    removeError = Coop.delete(p.id)
                    if (removeError == null) {
                        if (open == p.id) open = null
                        if (adding == p.id) adding = null
                        startAs = startAs - p.id
                        removing = null; refresh()
                    }
                }, icon = Icons.Rounded.Delete, kind = Kind.DANGER)
            },
            dismissButton = { RcButton(stringResource(R.string.ui_cancel), { removing = null }, Modifier.initialFocus(), kind = Kind.QUIET) },
        )
    }
}

/** Shares an invite code through the system share sheet (a chat app, mail). */
private fun share(ctx: Context, code: String) {
    val send = Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, code)
    runCatching { ctx.startActivity(Intent.createChooser(send, null)) }
}

@Composable
private fun HowCoopWorks() {
    val steps = listOf(
        R.string.ui_coop_step1_title to R.string.ui_coop_step1,
        R.string.ui_coop_step2_title to R.string.ui_coop_step2,
        R.string.ui_coop_step3_title to R.string.ui_coop_step3,
    )
    GlassCard(Modifier.fillMaxWidth().readable(), strong = false, padding = 18.dp) {
        SectionTitle(stringResource(R.string.ui_coop_how))
        steps.forEachIndexed { i, (title, body) ->
            Row(horizontalArrangement = Arrangement.spacedBy(14.dp)) {
                Box(Modifier.size(32.dp).clip(CircleShape).background(Rc.tintAccent), contentAlignment = Alignment.Center) {
                    Text("${i + 1}", style = MaterialTheme.typography.labelLarge, color = Rc.accent)
                }
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(stringResource(title), style = MaterialTheme.typography.titleMedium, color = Rc.text)
                    Text(stringResource(body), style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
                }
            }
        }
    }
}

// ---- previews (no files are read: the saves are made up) ------------------------------------------------------------------------------

internal val previewSaves = listOf(
    Coop.Profile(Coop.Invite("a1b2".repeat(4), "Our island", "0".repeat(32), "ABCD-EFGH-JKMN-PQRS", Coop.PUBLIC_RELAY,
        listOf(Coop.Member("m1", "Ana"), Coop.Member("m2", "Bo"))), "m1"),
)

@Preview(name = "Co-op, foldable inner, one save", widthDp = 882, heightDp = 640)
@Composable
private fun CoopScreenWidePreview() = RecompiaTheme { CoopScreen(true, {}, { previewSaves }) }

@Preview(name = "Co-op, foldable cover, no save", widthDp = 360, heightDp = 780)
@Composable
private fun CoopScreenEmptyPreview() = RecompiaTheme { CoopScreen(true, {}, { emptyList() }) }

@Preview(name = "Co-op, foldable cover, one save", widthDp = 360, heightDp = 1400)
@Composable
private fun CoopScreenNarrowPreview() = RecompiaTheme { CoopScreen(true, {}, { previewSaves }) }
