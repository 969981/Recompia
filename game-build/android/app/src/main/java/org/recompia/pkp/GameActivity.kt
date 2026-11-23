package org.recompia.pkp

import android.content.Intent
import android.os.Bundle
import android.os.Process
import android.util.Log
import android.view.WindowManager
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import org.libsdl.app.SDLActivity
import org.recompia.pkp.overlay.TouchOverlay

/**
 * Hosts the game: SDL loads libSDL3.so and libpkp_rt.so from the APK, then dlopens
 * `<filesDir>/game/libpkpgame.so` and calls its `SDL_main`. Runs in its own process (":game")
 * so SDL's static state and the runtime's address-space reservation start fresh on every launch.
 */
class GameActivity : SDLActivity() {
    /** Why the game cannot start, shown by SDL's error dialog instead of a crash. */
    private var problem: String? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        problem = checkGame()
        // Environment for this start, set in ONE place below (NXRT_<SECTION>_<KEY> overrides pkp.ini; the runtime reads it at SDL_main;
        // the ":game" process is new for every start, so nothing carries over between starts).
        val env = linkedMapOf<String, String>()
        // the file-backed heap ([memory] section_dir) must never fill the storage: below the free space it needs, this start uses the
        // zram heap instead and says so
        if (org.recompia.pkp.ui.DeviceFacts.heapFileShort()) {
            env["NXRT_MEMORY_HEAP_SECTION"] = "auto"; env["NXRT_MEMORY_SECTION_DIR"] = ""
            Log.i(Paths.TAG, "game start: too little free storage for the file-backed heap: the zram heap is used (may be less stable)")
            android.widget.Toast.makeText(this, R.string.ui_heap_file_short_toast, android.widget.Toast.LENGTH_LONG).show()
        }
        // The network service account id (acc.cpp network_account_id): on a PC a hash of the machine id and the uid; a phone has no
        // /etc/machine-id, so every phone fell back to uid hi ^ lo, the same for all phones of a co-op group (they share the save's
        // account uid) and for every phone on the default uid: the game's session layer then refuses the second phone (error 9005).
        // One random id per install (app-private), unless the player set [account] network_account_id.
        if (org.recompia.pkp.ui.Ini.load(Paths.pkpIni).get("account", "network_account_id").isNullOrBlank())
            runCatching { env["NXRT_ACCOUNT_NETWORK_ACCOUNT_ID"] = installId() }
        // Recompia: Co-op: the launcher's environment for this start (Coop.env, coop_launch.rs), from the group the app's Home picked. The
        // activity is not exported in release builds, so only this app starts it with the extra.
        intent?.getStringExtra(Coop.EXTRA_GROUP)?.let { id ->
            val p = Coop.get(id)
            if (p == null) Log.w(Paths.TAG, "co-op start: no co-op save $id: singleplayer") else {
                val coop = Coop.env(p, intent?.getStringExtra(Coop.EXTRA_START_AS) ?: "auto")
                env.putAll(coop)
                Log.i(Paths.TAG, "co-op start: ${p.name} (${p.id}) as ${coop["NXRT_PKP_COOP_ROLE"]}, relay ${p.invite.relay}")
            }
        }
        runCatching { env.forEach { (k, v) -> android.system.Os.setenv(k, v, true) } }
        // the one start decision line that release builds keep too (a locked phone pauses a release build at once: it is not shown over the lock screen)
        Log.i(Paths.TAG, "game start: " + (if (problem == null) "library ready" else "no game library, SDL shows the reason") +
            (if (getSystemService(android.app.KeyguardManager::class.java).isKeyguardLocked)
                "; the lock screen is showing: a release build waits behind it until the phone is unlocked" else ""))
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.attributes = window.attributes.apply {
            layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
        }
        hideSystemBars()
        if (!mBrokenLibraries) {
            loading = LoadingScreen(this)
            TouchOverlay.attach(this)
            org.recompia.pkp.ui.GameSession.started(this)
            sessionStarted = true
        }
    }

    private var loading: LoadingScreen? = null

    /** The runtime's start-up phases (runtime/src/os/os_app_phase.h), on the UI thread. */
    override fun onUnhandledMessage(command: Int, param: Any?): Boolean {
        if (command != LoadingScreen.COMMAND) return false
        loading?.phase(param as? Int ?: 0)
        return true
    }

    /** 16 hex digits, nonzero, made once from the OS random source and kept in the app's private files (never shared or logged). */
    private fun installId(): String {
        val f = java.io.File(filesDir, "network-account-id")
        f.takeIf { it.isFile }?.readText()?.trim()?.takeIf { it.length == 16 && it.all { c -> c in '0'..'9' || c in 'a'..'f' } && it.any { c -> c != '0' } }?.let { return it }
        var id: String
        do { id = "%016x".format(java.security.SecureRandom().nextLong()) } while (id.all { it == '0' })
        f.writeText(id)
        return id
    }

    private fun checkGame(): String? {
        if (!Storage.hasAllFilesAccess()) {
            Log.w(Paths.TAG, "no all-files access: ${Paths.dataRoot} is not readable")
        }
        val lib = GameLauncher.syncDevLibrary(this)
        val msg = when {
            lib == null -> "Game library not found.\n\nExpected ${Paths.gameLibrary(this)}.\n" +
                "Build the game in Recompia, or push a PC-built libpkpgame.so to ${Paths.devGameLibrary}" +
                (if (Storage.hasAllFilesAccess()) "." else " and grant all-files access.")
            else -> null
        }
        if (msg != null) Log.e(Paths.TAG, msg.replace('\n', ' '))
        return msg
    }

    override fun loadLibraries() {
        problem?.let { throw UnsatisfiedLinkError(it) }
        super.loadLibraries()
    }

    override fun getLibraries(): Array<String> = arrayOf("SDL3", "pkp_rt")  // libc++_shared.so resolves as NEEDED from the APK lib dir

    override fun getMainSharedObject(): String = Paths.gameLibrary(this).absolutePath

    override fun getMainFunction(): String = "SDL_main"

    override fun getArguments(): Array<String> {
        // extra runtime arguments from the intent only in test builds (adb harness); a release build passes nothing from an intent
        val extra = if (BuildConfig.TEST_ENTRY_POINTS) intent?.getStringArrayExtra(GameLauncher.EXTRA_ARGS) ?: emptyArray() else emptyArray()
        return (arrayOf("--game", Paths.dataRoot.absolutePath) + extra).also {
            Log.i(Paths.TAG, "game arguments: ${it.joinToString(" ")}")
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    /** Immersive sticky: bars come back on a swipe and hide again by themselves. */
    private fun hideSystemBars() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            hide(WindowInsetsCompat.Type.systemBars())
        }
    }

    override fun onPause() {
        super.onPause()
        // The game ended (quit from the title, the overlay's exit, SDL_main returned): back to Recompia's Home, which reads the
        // session (length, first frame, driver fallback) from GameSession. Started here while our window is still visible, which
        // background-start rules allow; not when the system pauses us for other reasons (isFinishing false).
        if (isFinishing && !homeStarted) {
            homeStarted = true
            if (sessionStarted) org.recompia.pkp.ui.GameSession.ended(this)
            runCatching {
                startActivity(Intent(this, org.recompia.pkp.ui.MainActivity::class.java)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_REORDER_TO_FRONT))
            }
        }
    }

    private var homeStarted = false
    /** Only a game that really started records its end (a missing library must not stretch the last session). */
    private var sessionStarted = false

    override fun onDestroy() {
        super.onDestroy()
        if (isFinishing) {
            if (!homeStarted && sessionStarted) org.recompia.pkp.ui.GameSession.ended(this)
            // ponytail: the ":game" process dies with the activity; SDL cannot re-create itself in a used process.
            Process.killProcess(Process.myPid())
        }
    }
}
