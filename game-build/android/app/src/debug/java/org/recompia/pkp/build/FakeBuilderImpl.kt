// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.build

import android.content.Context
import java.io.File

/**
 * DEBUG BUILDS ONLY (src/debug): a stand-in for the on-device build driver that speaks the same protocol, so the build screen, the
 * notification, cancel and errors can be walked through before the real tools exist. It builds nothing: its @@DONE path is an
 * empty file, and the service records the result as "fake" (the UI says so everywhere). Touch `<cacheDir>/fake_fail` to make it
 * fail in the compile step.
 */
object FakeBuilderImpl {
    @JvmStatic
    fun command(ctx: Context, what: String): List<String> {
        if (what == "shaders") return listOf("/system/bin/sh", "-c", """
            echo "fake shader translation: nothing here is real"
            echo "@@STEP 1/3 extract"
            for i in ${'$'}(seq 1 20); do echo "@@PROGRESS ${'$'}i/20 files"; sleep 0.3; done
            echo "@@STEP 2/3 translate"
            for i in ${'$'}(seq 1 150); do echo "@@PROGRESS ${'$'}((i*660))/99000 shaders"; sleep 0.5; done
            echo "@@STEP 3/3 finish"; sleep 1
            echo "@@DONE ${ctx.cacheDir}/fake_shader_cache"
        """.trimIndent())
        BuildProtocol.selfCheck()
        org.recompia.pkp.ui.Ini.selfCheck()
        org.recompia.pkp.ui.PhoneDefaults.selfCheck()
        val out = File(ctx.cacheDir, "fake-libpkpgame.so")
        val fail = File(ctx.cacheDir, "fake_fail")
        val script = """
            echo "fake builder: nothing here is real"
            echo "@@STEP 1/5 check"
            for i in 1 2 3 4 5; do echo "@@PROGRESS ${'$'}i/5 exefs/file${'$'}i"; sleep 0.4; done
            echo "@@LOG Game files match version 2.0.1 (fake)"
            echo "@@STEP 2/5 lift"
            for i in ${'$'}(seq 1 4); do echo "@@PROGRESS ${'$'}i/4 module${'$'}i"; echo "lifting module${'$'}i"; sleep 1.5; done
            echo "@@STEP 3/5 compile"
            for i in ${'$'}(seq 1 200); do
              echo "@@PROGRESS ${'$'}i/200 pkp_main_${'$'}i.cpp"
              echo "clang++ -c pkp_main_${'$'}i.cpp"
              if [ -e "${fail.path}" ] && [ ${'$'}i -eq 40 ]; then echo "@@ERROR clang++ crashed on pkp_main_40.cpp (fake)"; exit 1; fi
              sleep 0.5
            done
            echo "@@STEP 4/5 link"
            echo "@@PROGRESS 0/1 libpkpgame.so"; sleep 2; echo "@@PROGRESS 1/1 libpkpgame.so"
            echo "@@STEP 5/5 finish"
            : > "${out.path}"
            echo "@@DONE ${out.path}"
        """.trimIndent()
        return listOf("/system/bin/sh", "-c", script)
    }
}
