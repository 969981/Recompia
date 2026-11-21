// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Group
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material.icons.rounded.Warning
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import org.recompia.pkp.Coop
import org.recompia.pkp.GameLauncher
import org.recompia.pkp.R

/**
 * Recompia: Co-op on Home: the co-op saves on this phone with Play (the launcher's automatic host choice), Host and Join, and joining a
 * co-op save from the invite text the creator's launcher copies (Coop.accept). Making a co-op save and adding players stay on the PC.
 */
@Composable
fun CoopCard(canPlay: Boolean) {
    val act = activity()
    var saves by remember { mutableStateOf(runCatching { Coop.list() }.getOrDefault(emptyList())) }
    var text by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    val clip = LocalClipboardManager.current
    val invite = remember(text) { Coop.parseInvite(text) }
    GlassCard(Modifier.fillMaxWidth(), padding = 12.dp) {
        SectionTitle(stringResource(R.string.ui_coop_group))
        for (p in saves) {
            val host = remember(p) { runCatching { Coop.hostsByDefault(p) }.getOrDefault(false) }
            Text(p.name, style = MaterialTheme.typography.titleMedium, color = Rc.text, modifier = Modifier.padding(horizontal = 12.dp))
            Text(stringResource(R.string.ui_coop_players, p.invite.members.joinToString { it.name }, p.member(p.me)?.name ?: "") + "  " +
                stringResource(if (host) R.string.ui_coop_you_host else R.string.ui_coop_you_visit),
                style = MaterialTheme.typography.bodySmall, color = Rc.text3, modifier = Modifier.padding(horizontal = 12.dp))
            FlowRow(Modifier.padding(horizontal = 12.dp), horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                RcButton(stringResource(R.string.ui_coop_play), { GameLauncher.launchCoop(act, p.id, "auto") }, icon = Icons.Rounded.PlayArrow, kind = Kind.PRIMARY, enabled = canPlay)
                RcButton(stringResource(R.string.ui_coop_host), { GameLauncher.launchCoop(act, p.id, "host") }, kind = Kind.QUIET, enabled = canPlay)
                RcButton(stringResource(R.string.ui_coop_join), { GameLauncher.launchCoop(act, p.id, "visit") }, kind = Kind.QUIET, enabled = canPlay)
            }
        }
        OutlinedTextField(text, { text = it; error = null }, label = { Text(stringResource(R.string.ui_coop_invite)) }, maxLines = 3,
            isError = text.isNotBlank() && invite == null, modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp))
        if (text.isBlank()) RcButton(stringResource(R.string.ui_coop_paste), { text = clip.getText()?.text.orEmpty() }, Modifier.padding(horizontal = 12.dp), kind = Kind.QUIET)
        if (invite != null) {
            Text(stringResource(R.string.ui_coop_who, invite.name), style = MaterialTheme.typography.bodyMedium, color = Rc.text2, modifier = Modifier.padding(horizontal = 12.dp))
            FlowRow(Modifier.padding(horizontal = 12.dp), horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                for (m in invite.members.drop(1)) RcButton(m.name, {   // the creator (first) is never joined: coop.rs accept
                    error = Coop.accept(text, m.id)
                    if (error == null) { text = ""; saves = Coop.list() }
                }, icon = Icons.Rounded.Group)
            }
        }
        error?.let { Banner(it, Rc.warn, Rc.tintWarn, Icons.Rounded.Warning) }
        Text(stringResource(R.string.ui_coop_note), style = MaterialTheme.typography.bodySmall, color = Rc.text3, modifier = Modifier.padding(horizontal = 12.dp))
    }
}
