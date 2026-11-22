package org.recompia.pkp

import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import android.os.Environment
import android.provider.Settings
import android.util.Log
import java.io.File

/** All-files access (MANAGE_EXTERNAL_STORAGE) and the data root layout. */
object Storage {
    fun hasAllFilesAccess(): Boolean = Environment.isExternalStorageManager()

    /** Opens the system screen where the user grants all-files access to this app. */
    fun requestAllFilesAccess(activity: Activity) {
        val perApp = Intent(
            Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
            Uri.parse("package:" + activity.packageName),
        )
        try {
            activity.startActivity(perApp)
        } catch (e: ActivityNotFoundException) {
            activity.startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
        }
    }

    /** Creates the data root folders. Returns false without all-files access or on an I/O failure. */
    fun ensureLayout(): Boolean {
        if (!hasAllFilesAccess()) return false
        val dirs = listOf(
            Paths.exefsDir, Paths.romfsDir, Paths.shaderCacheDir, Paths.assetsDir,
            Paths.saveDir, Paths.logsDir, Paths.buildDir,
        )
        return dirs.all { it.isDirectory || it.mkdirs() }.also {
            if (!it) Log.e(Paths.TAG, "could not create the layout under ${Paths.dataRoot}")
        }
    }

    fun freeBytes(dir: File = Paths.dataRoot): Long =
        (if (dir.exists()) dir else Environment.getExternalStorageDirectory()).usableSpace
}
