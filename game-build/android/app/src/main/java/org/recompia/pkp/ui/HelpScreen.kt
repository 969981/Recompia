// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import org.recompia.pkp.R

/** "How to get your game files onto the phone": the player's own dump, copied over, then picked in the wizard. */
@Composable
fun HelpScreen(onBack: () -> Unit) {
    val steps = listOf(
        R.string.ui_help_1_title to R.string.ui_help_1,
        R.string.ui_help_2_title to R.string.ui_help_2,
        R.string.ui_help_3_title to R.string.ui_help_3,
        R.string.ui_help_4_title to R.string.ui_help_4,
        R.string.ui_help_5_title to R.string.ui_help_5,
        R.string.ui_help_6_title to R.string.ui_help_6,
    )
    Page(stringResource(R.string.ui_help_title), onBack) {
        GlassCard(Modifier.fillMaxWidth().readable()) {
            Text(stringResource(R.string.ui_help_intro), style = MaterialTheme.typography.bodyLarge, color = Rc.text2)
        }
        steps.forEachIndexed { i, (title, body) ->
            GlassCard(Modifier.fillMaxWidth().readable(), strong = false, padding = 18.dp) {
                Row(horizontalArrangement = Arrangement.spacedBy(14.dp)) {
                    Box(Modifier.size(32.dp).clip(CircleShape).background(Rc.tintAccent), contentAlignment = Alignment.Center) {
                        Text("${i + 1}", style = MaterialTheme.typography.labelLarge, color = Rc.accent)
                    }
                    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        Text(stringResource(title), style = MaterialTheme.typography.titleMedium, color = Rc.text)
                        Text(stringResource(body), style = MaterialTheme.typography.bodyMedium, color = Rc.text2)
                    }
                }
            }
        }
        Text(stringResource(R.string.ui_dump_legal), style = MaterialTheme.typography.bodySmall, color = Rc.text3)
    }
}
