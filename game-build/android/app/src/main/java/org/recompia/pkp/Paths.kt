package org.recompia.pkp

import android.content.Context
import android.os.Environment
import java.io.File

/** Where everything lives (contract: data root on shared storage, game library in app-private storage). */
object Paths {
    const val TAG = "Recompia"

    /** `/sdcard/Recompia` (resolved through the primary external storage, same directory). */
    val dataRoot: File get() = File(Environment.getExternalStorageDirectory(), "Recompia")
    val dumpDir: File get() = File(dataRoot, "dump")
    val exefsDir: File get() = File(dumpDir, "exefs")
    val romfsDir: File get() = File(dumpDir, "romfs")
    val shaderCacheDir: File get() = File(dataRoot, "shader_cache")
    val assetsDir: File get() = File(dataRoot, "assets")
    val saveDir: File get() = File(dataRoot, "save")
    val logsDir: File get() = File(dataRoot, "logs")
    /** Work area of the on-device build, and the dev shortcut location of a PC-built `libpkpgame.so`. */
    val buildDir: File get() = File(dataRoot, "build")
    val pkpIni: File get() = File(dataRoot, "pkp.ini")
    val controlsIni: File get() = File(dataRoot, "controls.ini")
    val devGameLibrary: File get() = File(buildDir, GAME_LIBRARY)

    const val GAME_LIBRARY = "libpkpgame.so"

    /** `<filesDir>/game`: the only place the game library is loaded from (dlopen refuses shared storage). */
    fun gameDir(context: Context): File = File(context.filesDir, "game")
    fun gameLibrary(context: Context): File = File(gameDir(context), GAME_LIBRARY)

    /** Extracted APK native libs (libSDL3.so, libpkp_rt.so, on-device tools named lib*.so). */
    fun nativeLibraryDir(context: Context): File = File(context.applicationInfo.nativeLibraryDir)
}
