package org.recompia.pkp

import android.content.Context
import android.content.Intent
import android.util.Log
import java.io.File

/** Starts the game and answers "is everything there to play". */
object GameLauncher {
    /** String array extra: extra runtime arguments appended after `--game <dataRoot>`. */
    const val EXTRA_ARGS = "args"

    fun launch(context: Context, args: Array<String> = emptyArray()) {
        val intent = Intent(context, GameActivity::class.java).putExtra(EXTRA_ARGS, args)
        if (context !is android.app.Activity) intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        context.startActivity(intent)
    }

    /** A Recompia: Co-op start of co-op save `group` (Coop): GameActivity sets the launcher's environment. startAs: auto | host | visit. */
    fun launchCoop(context: Context, group: String, startAs: String) {
        val intent = Intent(context, GameActivity::class.java).putExtra(Coop.EXTRA_GROUP, group).putExtra(Coop.EXTRA_START_AS, startAs)
        if (context !is android.app.Activity) intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        context.startActivity(intent)
    }

    /** String extra: the optimized build's play-test session (BuildState.session, the driver's @@SESSION JSON). */
    const val EXTRA_SESSION = "pgo_session"

    /**
     * The optimized build's play-test: the test version (built in the shared build folder, which Android does not load code from) is
     * copied read-only into app storage as [Paths.sessionLibrary], then the game plays the session script with it (GameActivity).
     * The session ends the game by itself; the next build takes its counts (pgo/pkp_pgo.py Store.session_done).
     */
    fun launchSession(context: Context, session: String) {
        // the library is hundreds of MB: copied off the UI thread, the game started from the main thread once it is in place
        val main = android.os.Handler(android.os.Looper.getMainLooper())
        Thread {
            try {
                val src = File(org.json.JSONObject(session).getString("library"))
                val dst = Paths.sessionLibrary(context)
                dst.parentFile!!.mkdirs()
                dst.delete()
                src.copyTo(dst, overwrite = true)
                dst.setReadOnly()
            } catch (e: Exception) {
                Log.e(Paths.TAG, "play-test: cannot install the test version", e)
                main.post { android.widget.Toast.makeText(context, e.message ?: e.javaClass.simpleName, android.widget.Toast.LENGTH_LONG).show() }
                return@Thread
            }
            main.post {
                val intent = Intent(context, GameActivity::class.java).putExtra(EXTRA_SESSION, session)
                if (context !is android.app.Activity) intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                context.startActivity(intent)
            }
        }.start()
    }

    /** A game library is installed privately, or a dev copy waits in `<dataRoot>/build/`. */
    fun hasGameLibrary(context: Context): Boolean =
        Paths.gameLibrary(context).isFile || Paths.devGameLibrary.isFile

    /**
     * The data the runtime needs at minimum: all-files access, pkp.ini and the dump it names ([fs] exefs / romfs: the UI may use a
     * dump in place elsewhere on shared storage; a relative path is relative to the data root, the game's working directory).
     */
    fun hasGameData(): Boolean {
        if (!Storage.hasAllFilesAccess() || !Paths.pkpIni.isFile) return false
        val ini = org.recompia.pkp.ui.Ini.load(Paths.pkpIni)
        fun dir(key: String, fallback: File) = ini.get("fs", key)?.trim()?.ifEmpty { null }
            ?.let { File(it).let { f -> if (f.isAbsolute) f else File(Paths.dataRoot, it) } } ?: fallback
        return dir("exefs", Paths.exefsDir).isDirectory && dir("romfs", Paths.romfsDir).isDirectory
    }

    /**
     * Dev shortcut (contract): copy `<dataRoot>/build/libpkpgame.so` into `<filesDir>/game/` when size or
     * mtime differ, then make the copy read-only. Returns the private library, or null if there is none.
     */
    fun syncDevLibrary(context: Context): File? {
        val dst = Paths.gameLibrary(context)
        val src = Paths.devGameLibrary
        try {
            if (src.isFile && (src.length() != dst.length() || src.lastModified() != dst.lastModified())) {
                // never replace a working library with something that is not one (an x86 build, a wrong file)
                val ok = org.recompia.pkp.build.LibraryCheck.check(src)
                if (ok != org.recompia.pkp.build.LibraryCheck.Result.OK) {
                    Log.w(Paths.TAG, "dev shortcut: $src is not a game library for this phone ($ok): not copied")
                    return dst.takeIf { it.isFile }
                }
                Log.i(Paths.TAG, "dev shortcut: copying $src (${src.length()} B) to $dst")
                dst.parentFile!!.mkdirs()
                val tmp = File(dst.parentFile, "$GAME_TMP")
                tmp.delete()
                src.copyTo(tmp, overwrite = true)
                tmp.setLastModified(src.lastModified())
                tmp.setReadOnly()
                dst.delete()
                if (!tmp.renameTo(dst)) throw java.io.IOException("rename $tmp -> $dst failed")
            }
        } catch (e: Exception) {
            Log.e(Paths.TAG, "dev shortcut failed", e)
        }
        return dst.takeIf { it.isFile }
    }

    private const val GAME_TMP = "libpkpgame.so.tmp"
}
