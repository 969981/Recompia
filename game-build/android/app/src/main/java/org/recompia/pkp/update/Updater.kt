// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.update

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.content.pm.PackageManager
import android.os.Build
import android.util.Log
import androidx.core.app.NotificationCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import org.json.JSONObject
import org.recompia.pkp.BuildConfig
import org.recompia.pkp.Paths
import org.recompia.pkp.R
import java.io.File
import java.io.FileOutputStream
import java.security.MessageDigest
import kotlin.coroutines.cancellation.CancellationException

/**
 * The app's own update, the launcher's Update button on a phone: the release [Releases] picked is downloaded to
 * `<cacheDir>/update` (resumed after a cancel or a lost connection), checked against the release's SHA256SUMS.txt and against the
 * installed app's signing key, then handed to a PackageInstaller session. On Android 12 and later the session asks for no
 * confirmation when this app installed the running version itself (the first update from a browser- or adb-installed APK shows
 * Android's prompt once). Android stops the app to replace it; [InstallReceiver] opens it again. Only the APK is downloaded,
 * never anything of the game.
 */
object Updater {
    enum class Phase { IDLE, DOWNLOADING, VERIFYING, INSTALLING }

    data class State(
        /** The update on offer (null: up to date, or never checked). */
        val release: Releases.Release? = null,
        val checking: Boolean = false,
        val checkedAt: Long = 0,
        /** The last check could not reach GitHub (offline, rate limited): the cached answer stays. */
        val checkFailed: Boolean = false,
        val phase: Phase = Phase.IDLE,
        val done: Long = 0,
        val total: Long = 0,
        val error: String? = null,
        /** Android needs "Install unknown apps" for Recompia before the session can be committed. */
        val needsPermission: Boolean = false,
        /** The version this app was just updated to by itself (shown once on Home). */
        val updatedTo: String? = null,
    ) {
        val busy get() = phase != Phase.IDLE
    }

    /** At most a few automatic checks a day; Check now ignores it. */
    private const val INTERVAL_MS = 6 * 3600_000L
    private const val PREFS = "update"
    private const val CHANNEL = "updates"
    private const val NOTIFY_ID = 7301
    private const val TAG = "${Paths.TAG}.update"

    private val _state = MutableStateFlow(State())
    val state: StateFlow<State> = _state.asStateFlow()
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var job: Job? = null
    private var loaded = false

    private fun prefs(ctx: Context) = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    private fun set(f: (State) -> State) { _state.value = f(_state.value) }

    fun auto(ctx: Context): Boolean = prefs(ctx).getBoolean("auto", true)
    fun setAuto(ctx: Context, on: Boolean) { prefs(ctx).edit().putBoolean("auto", on).apply(); if (on) onResume(ctx) }

    /** Each time the UI comes to the front: the cached answer once, an automatic check when one is due, and the install that waited
     *  for "Install unknown apps". */
    fun onResume(ctx: Context) {
        val app = ctx.applicationContext
        load(app)
        val s = _state.value
        if (s.needsPermission && app.packageManager.canRequestPackageInstalls()) update(app)
        else if (auto(app) && !s.checking && !s.busy && System.currentTimeMillis() - s.checkedAt > INTERVAL_MS) check(app)
    }

    private fun load(ctx: Context) {
        if (loaded) return
        loaded = true
        val p = prefs(ctx)
        val cached = runCatching { Releases.Release.fromJson(JSONObject(p.getString("release", null)!!)) }.getOrNull()
            ?.takeIf { stillNewer(it) }
        val updating = p.getString("updating_to", null)
        p.edit().remove("updating_to").apply()
        _state.value = State(release = cached, checkedAt = p.getLong("checked_at", 0),
            updatedTo = updating?.takeIf { it == BuildConfig.VERSION_NAME })
        if (cached == null) File(ctx.cacheDir, "update").deleteRecursively()
        ctx.getSystemService(NotificationManager::class.java)?.cancel(NOTIFY_ID)
    }

    private fun stillNewer(r: Releases.Release): Boolean {
        val have = Releases.version(BuildConfig.VERSION_NAME) ?: return false
        return Releases.version(r.version)?.let { Releases.newer(it, have) } == true
    }

    fun dismissUpdated() = set { it.copy(updatedTo = null) }

    /** Asks GitHub for the releases list. Every failure is silent but for [State.checkFailed]; the attempt counts toward the interval. */
    fun check(ctx: Context) {
        val app = ctx.applicationContext
        load(app)
        if (_state.value.checking) return
        set { it.copy(checking = true) }
        scope.launch {
            val now = System.currentTimeMillis()
            prefs(app).edit().putLong("checked_at", now).apply()
            val found = runCatching { Releases.pick(Releases.fetch(), BuildConfig.VERSION_NAME) }
            found.exceptionOrNull()?.let { Log.i(TAG, "check failed: $it") }
            found.onSuccess { r -> prefs(app).edit().putString("release", r?.toJson()?.toString()).apply() }
            set { it.copy(checking = false, checkedAt = now, checkFailed = found.isFailure, release = if (found.isSuccess) found.getOrNull() else it.release) }
        }
    }

    /** Update: download, verify, install. Also the way to try again after an error or a dismissed prompt (a finished download is kept). */
    fun update(ctx: Context) {
        val app = ctx.applicationContext
        val r = _state.value.release ?: return
        if (job?.isActive == true) return
        set { it.copy(needsPermission = false, error = null, phase = Phase.DOWNLOADING, done = 0, total = r.size) }
        job = scope.launch {
            try {
                val apk = fetchVerified(app, r)
                set { it.copy(phase = Phase.VERIFYING) }
                checkPackage(app, apk)
                // asked only now, so the download runs while the player is in Android's settings; coming back runs update() again,
                // which finds the verified APK and installs it (onResume)
                if (!app.packageManager.canRequestPackageInstalls()) { set { it.copy(phase = Phase.IDLE, needsPermission = true) }; return@launch }
                set { it.copy(phase = Phase.INSTALLING) }
                prefs(app).edit().putString("updating_to", r.version).commit()
                install(app, apk)
            } catch (e: CancellationException) {
                set { it.copy(phase = Phase.IDLE) }
            } catch (e: Exception) {
                Log.w(TAG, "update failed", e)
                val msg = (e as? Problem)?.message ?: app.getString(R.string.ui_upd_err_network, e.message ?: e.javaClass.simpleName)
                set { it.copy(phase = Phase.IDLE, error = msg) }
            }
        }
    }

    fun cancel() { job?.cancel() }

    /** A message for the player (the rest are network or disk errors, shown with their own text). */
    private class Problem(message: String) : Exception(message)

    /** The APK in `<cacheDir>/update`, its SHA-256 equal to the release's SHA256SUMS.txt line; a partial download is resumed. */
    private suspend fun fetchVerified(ctx: Context, r: Releases.Release): File {
        val dir = File(ctx.cacheDir, "update").apply { mkdirs() }
        dir.listFiles()?.filter { !it.name.startsWith(r.apkName) }?.forEach { it.deleteRecursively() }   // an older release's download
        val sums = Releases.text(r.sumsUrl)
        val want = Releases.sumFor(sums, r.apkName) ?: throw Problem(ctx.getString(R.string.ui_upd_err_no_sum))
        val apk = File(dir, r.apkName)
        if (apk.isFile && sha256(apk) == want) return apk
        apk.delete()
        val part = File(dir, r.apkName + ".part")
        if (r.size <= 0 || part.length() < r.size) download(r.apkUrl, part, r.size)
        set { it.copy(phase = Phase.VERIFYING) }
        if (sha256(part) != want) { part.delete(); throw Problem(ctx.getString(R.string.ui_upd_err_sum)) }
        if (!part.renameTo(apk)) throw Problem(ctx.getString(R.string.ui_upd_err_network, "rename"))
        return apk
    }

    private suspend fun download(url: String, part: File, size: Long) {
        var have = part.length()
        val c = Releases.get(url, have)
        try {
            if (have > 0 && c.responseCode != 206) have = 0   // the server ignored the range: start over
            val total = if (c.contentLengthLong > 0) have + c.contentLengthLong else size
            FileOutputStream(part, have > 0).use { out ->
                c.inputStream.use { input ->
                    val buf = ByteArray(1 shl 16)
                    var shown = 0L
                    while (true) {
                        kotlin.coroutines.coroutineContext.ensureActive()
                        val n = input.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        have += n
                        if (have - shown >= 256 * 1024) { shown = have; set { it.copy(done = have, total = total) } }
                    }
                }
            }
            set { it.copy(done = have, total = total) }
        } finally { c.disconnect() }
    }

    private fun sha256(f: File): String {
        val md = MessageDigest.getInstance("SHA-256")
        f.inputStream().use { i -> val b = ByteArray(1 shl 16); while (true) { val n = i.read(b); if (n < 0) break; md.update(b, 0, n) } }
        return md.digest().joinToString("") { "%02x".format(it) }
    }

    /** The APK is this app, newer, and signed with the installed app's key. Android refuses another key anyway; saying it here
     *  explains why (a test build or one signed by someone else) instead of a bare install failure. */
    @Suppress("DEPRECATION")
    private fun checkPackage(ctx: Context, apk: File) {
        val pm = ctx.packageManager
        val info = pm.getPackageArchiveInfo(apk.path, PackageManager.GET_SIGNING_CERTIFICATES)
        if (info == null || info.packageName != ctx.packageName) { apk.delete(); throw Problem(ctx.getString(R.string.ui_upd_err_sum)) }
        val signers = info.signingInfo?.apkContentsSigners.orEmpty()
        if (signers.none { pm.hasSigningCertificate(ctx.packageName, it.toByteArray(), PackageManager.CERT_INPUT_RAW_X509) })
            throw Problem(ctx.getString(R.string.ui_upd_err_signature))
        val installed = pm.getPackageInfo(ctx.packageName, 0).longVersionCode
        if (info.longVersionCode <= installed) throw Problem(ctx.getString(R.string.ui_upd_err_not_newer, info.versionName ?: ""))
    }

    private fun install(ctx: Context, apk: File) {
        val pi = ctx.packageManager.packageInstaller
        pi.mySessions.forEach { runCatching { pi.abandonSession(it.sessionId) } }   // one left by a dismissed prompt
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
            setAppPackageName(ctx.packageName)
            setSize(apk.length())
            setInstallReason(PackageManager.INSTALL_REASON_USER)
            if (Build.VERSION.SDK_INT >= 31) setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED)
        }
        val id = pi.createSession(params)
        pi.openSession(id).use { s ->
            s.openWrite("base.apk", 0, apk.length()).use { out -> apk.inputStream().use { it.copyTo(out) }; s.fsync(out) }
            s.commit(InstallReceiver.sender(ctx, id))
        }
    }

    /** The session's answer ([InstallReceiver]); success never arrives here for this app, Android stops it first. */
    internal fun installResult(ctx: Context, status: Int, message: String?) {
        prefs(ctx).edit().remove("updating_to").apply()
        val err = when (status) {
            PackageInstaller.STATUS_SUCCESS -> null
            PackageInstaller.STATUS_FAILURE_ABORTED -> ctx.getString(R.string.ui_upd_err_aborted)
            PackageInstaller.STATUS_FAILURE_CONFLICT, PackageInstaller.STATUS_FAILURE_INCOMPATIBLE -> ctx.getString(R.string.ui_upd_err_conflict, message.orEmpty())
            PackageInstaller.STATUS_FAILURE_STORAGE -> ctx.getString(R.string.ui_upd_err_storage)
            else -> ctx.getString(R.string.ui_upd_err_install, message.orEmpty())
        }
        set { it.copy(phase = Phase.IDLE, error = err) }
    }

    /** After the update replaced the app: open it again. Android may not let a background app start its window, so a notification
     *  that opens it is posted too (removed when the app shows). */
    internal fun reopen(ctx: Context) {
        val launch = ctx.packageManager.getLaunchIntentForPackage(ctx.packageName)?.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK) ?: return
        val nm = ctx.getSystemService(NotificationManager::class.java)
        if (nm != null && nm.getNotificationChannel(CHANNEL) == null)
            nm.createNotificationChannel(NotificationChannel(CHANNEL, ctx.getString(R.string.ui_upd_channel), NotificationManager.IMPORTANCE_DEFAULT))
        runCatching {
            nm?.notify(NOTIFY_ID, NotificationCompat.Builder(ctx, CHANNEL).setSmallIcon(R.drawable.ui_ic_notification)
                .setContentTitle(ctx.getString(R.string.ui_upd_updated, BuildConfig.VERSION_NAME))
                .setContentText(ctx.getString(R.string.ui_upd_tap_to_open)).setAutoCancel(true)
                .setContentIntent(PendingIntent.getActivity(ctx, 0, launch, PendingIntent.FLAG_IMMUTABLE)).build())
        }
        runCatching { ctx.startActivity(launch) }
    }
}
