// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.update

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentSender
import android.content.pm.PackageInstaller
import android.os.Build

/** The install session's status ([Updater.install]), and MY_PACKAGE_REPLACED, which opens the app again after its update. */
class InstallReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        when (intent.action) {
            Intent.ACTION_MY_PACKAGE_REPLACED -> Updater.reopen(ctx)
            ACTION_STATUS -> {
                val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)
                if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                    // Android's own confirmation (the first update, or before Android 12); the UI is in front, so it may open it
                    val confirm = if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java)
                        else @Suppress("DEPRECATION") intent.getParcelableExtra(Intent.EXTRA_INTENT)
                    if (confirm != null) runCatching { ctx.startActivity(confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
                } else Updater.installResult(ctx, status, intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE))
            }
        }
    }

    companion object {
        private const val ACTION_STATUS = "org.recompia.pkp.update.STATUS"

        /** Mutable: the installer fills in the status extras. */
        fun sender(ctx: Context, session: Int): IntentSender = PendingIntent.getBroadcast(ctx, session,
            Intent(ctx, InstallReceiver::class.java).setAction(ACTION_STATUS),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE).intentSender
    }
}
