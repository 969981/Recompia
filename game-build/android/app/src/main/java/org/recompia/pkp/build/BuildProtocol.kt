// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.build

/**
 * The progress protocol of the on-device tools (tools/android/ondevice/driver and tools/android/shaders): one event per
 * stdout line, every other line is log text.
 */
sealed interface BuildEvent {
    data class Step(val index: Int, val count: Int, val name: String) : BuildEvent
    data class Progress(val done: Long, val total: Long, val unit: String) : BuildEvent
    data class Log(val text: String) : BuildEvent
    data class Done(val path: String) : BuildEvent
    data class Error(val text: String) : BuildEvent
    data class Text(val text: String) : BuildEvent
}

object BuildProtocol {
    private val STEP = Regex("""^@@STEP\s+(\d+)\s*/\s*(\d+)\s*(.*)$""")
    private val PROGRESS = Regex("""^@@PROGRESS\s+(\d+)\s*/\s*(\d+)\s*(.*)$""")

    fun parse(line: String): BuildEvent {
        val l = line.trimEnd('\r', '\n')
        STEP.find(l)?.let { m -> return BuildEvent.Step(m.groupValues[1].toInt(), m.groupValues[2].toInt(), m.groupValues[3].trim()) }
        PROGRESS.find(l)?.let { m -> return BuildEvent.Progress(m.groupValues[1].toLong(), m.groupValues[2].toLong(), m.groupValues[3].trim()) }
        return when {
            l.startsWith("@@LOG ") -> BuildEvent.Log(l.removePrefix("@@LOG ").trim())
            l.startsWith("@@DONE ") -> BuildEvent.Done(l.removePrefix("@@DONE ").trim())
            l.startsWith("@@ERROR") -> BuildEvent.Error(l.removePrefix("@@ERROR").trim())
            else -> BuildEvent.Text(l)
        }
    }

    /** Throws on a parser regression. The debug fake builder runs it before every fake build. */
    fun selfCheck() {
        check(parse("@@STEP 3/5 compile") == BuildEvent.Step(3, 5, "compile"))
        check(parse("@@PROGRESS 12/284 pkp_main 3.cpp") == BuildEvent.Progress(12, 284, "pkp_main 3.cpp"))
        check(parse("@@DONE /data/x/libpkpgame.so") == BuildEvent.Done("/data/x/libpkpgame.so"))
        check(parse("@@ERROR not enough free space") == BuildEvent.Error("not enough free space"))
        check(parse("@@LOG hello") == BuildEvent.Log("hello"))
        check(parse("clang: warning") == BuildEvent.Text("clang: warning"))
    }
}

/**
 * Time left in a step from measured speed, the same rules as the PC launcher's setup page (launcher/src/pages/setup.js etaOf):
 * the rate of the last 3 minutes, nothing before a minute of samples, and the number only changes every 15 s so it does not flicker.
 */
class Eta {
    private data class Sample(val t: Long, val done: Long)
    private val samples = ArrayDeque<Sample>()
    private var key = ""
    private var shownAt = 0L
    var leftMs: Long? = null
        private set

    fun reset() { samples.clear(); key = ""; leftMs = null; shownAt = 0 }

    fun update(step: String, done: Long, total: Long, now: Long = System.currentTimeMillis()): Long? {
        if (total <= 0) { reset(); return null }
        if (step != key || (samples.lastOrNull()?.done ?: 0) > done) { reset(); key = step }
        if (samples.lastOrNull()?.done != done) samples.addLast(Sample(now, done))
        while (samples.size > MIN_SAMPLES && now - samples.first().t > WINDOW_MS) samples.removeFirst()
        if (leftMs != null && now - shownAt < REDRAW_MS) return leftMs
        val first = samples.first()
        val span = now - first.t
        val moved = done - first.done
        if (span < MIN_SPAN_MS || samples.size < MIN_SAMPLES || moved <= 0) return leftMs
        leftMs = (total - done) * span / moved
        shownAt = now
        return leftMs
    }

    private companion object {
        const val WINDOW_MS = 180_000L
        const val MIN_SPAN_MS = 60_000L
        const val MIN_SAMPLES = 3
        const val REDRAW_MS = 15_000L
    }
}
