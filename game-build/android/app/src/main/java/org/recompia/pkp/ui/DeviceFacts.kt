// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import android.content.Context
import android.opengl.EGL14
import android.opengl.GLES20
import android.os.StatFs
import java.io.File

/** Reads [PhoneDefaults.Facts] on the phone, and the free-space check of the file-backed game heap. */
object DeviceFacts {
    fun read(ctx: Context): PhoneDefaults.Facts {
        val kb = runCatching {
            File("/proc/meminfo").readLines().filter { it.startsWith("MemTotal:") || it.startsWith("SwapTotal:") }
                .sumOf { it.split(Regex("\\s+"))[1].toLong() }
        }.getOrDefault(0L)
        val turnip = GpuDriver.isAdreno() && (GpuDriver.hasIncluded(ctx) || runCatching { GpuDriver.current() }.getOrNull() != null)
        // the screen the app runs on, landscape (the game is landscape only): a foldable's inner or cover screen, whichever is open now
        val b = runCatching { ctx.getSystemService(android.view.WindowManager::class.java).maximumWindowMetrics.bounds }.getOrNull()
        return PhoneDefaults.Facts(kb * 1024, glRenderer(), turnip,
            ctx.packageManager.hasSystemFeature(android.content.pm.PackageManager.FEATURE_SENSOR_HINGE_ANGLE), ctx.cacheDir.absolutePath,
            b?.let { maxOf(it.width(), it.height()) } ?: 0, b?.let { minOf(it.width(), it.height()) } ?: 0)
    }

    /** GL_RENDERER from a 1x1 pbuffer context ("Adreno (TM) 750", "Mali-G710"); "" when EGL fails. */
    private fun glRenderer(): String = runCatching {
        val d = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        EGL14.eglInitialize(d, IntArray(1), 0, IntArray(1), 0)
        try {
            val cfg = arrayOfNulls<android.opengl.EGLConfig>(1)
            EGL14.eglChooseConfig(d, intArrayOf(EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT, EGL14.EGL_NONE), 0, cfg, 0, 1, IntArray(1), 0)
            val c = EGL14.eglCreateContext(d, cfg[0], EGL14.EGL_NO_CONTEXT, intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0)
            val s = EGL14.eglCreatePbufferSurface(d, cfg[0], intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE), 0)
            EGL14.eglMakeCurrent(d, s, s, c)
            val r = GLES20.glGetString(GLES20.GL_RENDERER) ?: ""
            EGL14.eglMakeCurrent(d, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
            EGL14.eglDestroySurface(d, s); EGL14.eglDestroyContext(d, c)
            r
        } finally { EGL14.eglTerminate(d) }
    }.getOrDefault("")

    /** pkp.ini asks for a file-backed heap ([memory] section_dir) but its folder has less than [PhoneDefaults.HEAP_FILE_FREE_BYTES] free. */
    fun heapFileShort(): Boolean {
        val dir = Ini.load(org.recompia.pkp.Paths.pkpIni).get("memory", "section_dir")?.trim()?.ifEmpty { null } ?: return false
        return runCatching { StatFs(dir).availableBytes < PhoneDefaults.HEAP_FILE_FREE_BYTES }.getOrDefault(false)
    }
}
