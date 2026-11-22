// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

/**
 * The phone keys the runtime needs, written only where missing so the player's own values stay (the configuration that was
 * tested on phones). The memory floor this app wrote before (heap 8 / 8192) is raised to the
 * values that reach the world. pkp.ini [gfx] vulkan_driver_dir is GpuDriver's. Pure (Ini in, Ini out) so it can be checked off a phone.
 */
object PhoneDefaults {
    /** What the defaults depend on, read on the phone by [DeviceFacts.read] (pure data, so the rule can be checked off a phone). */
    data class Facts(
        val ramSwapBytes: Long,        // MemTotal + SwapTotal (/proc/meminfo)
        val gpu: String,               // GL_RENDERER, e.g. "Adreno (TM) 750", "Mali-G710"
        val turnip: Boolean,           // the Turnip driver is active or included (the wizard installs it on Adreno)
        val foldable: Boolean,         // PackageManager.FEATURE_SENSOR_HINGE_ANGLE (a foldable cools worse than a slab)
        val cacheDir: String,          // the app's cache folder: the file-backed heap lives there
        val screenW: Int = 0,          // the screen in pixels, landscape (WindowManager maximumWindowMetrics); 0 = unknown
        val screenH: Int = 0,
    )

    /** The whole classification, in one place. Memory tier from RAM; performance class from the GPU family. */
    const val LOW_MEMORY_BYTES = 12L shl 30          // MemTotal + SwapTotal below this: the Low memory profile
    const val HEAP_FILE_FREE_BYTES = 5L shl 30       // free storage the file-backed heap needs (checked again at every start)
    fun lowMemory(f: Facts) = f.ramSwapBytes in 1 until LOW_MEMORY_BYTES
    private val ADRENO = Regex("""Adreno\D*(\d{3})""")
    fun adreno(f: Facts): Int? = ADRENO.find(f.gpu)?.groupValues?.get(1)?.toInt()
    fun mali(f: Facts) = f.gpu.startsWith("Mali")
    /** Adreno 740 and newer that the bundled Turnip (Mesa 26.3.1) knows: its device table has FD740, FD740v3, FD750, Adreno 753 and the
     *  Adreno 8xx parts 810, 829, 830, 840. Update this set with the Turnip build. */
    val TURNIP_740_PLUS = setOf(740, 750, 753, 810, 829, 830, 840)
    /** The top class: Adreno 740 or newer (Snapdragon 8 Gen 2 and later; measured on a Galaxy Z Fold 6 with an Adreno 750, and a newer
     *  GPU should not get less) on Turnip, and only where the bundled Turnip knows the GPU (the game runs on Turnip). */
    fun foldClass(f: Facts) = f.turnip && (adreno(f) ?: 0) in TURNIP_740_PLUS
    /** An Adreno 740+ the bundled Turnip does not list: cautious defaults, and Home says the GPU is untested. */
    fun untestedGpu(f: Facts) = (adreno(f) ?: 0) >= 740 && (adreno(f) ?: 0) !in TURNIP_740_PLUS
    /** pkp.ini [pkp] resolution (percent of the screen per side) and controls.ini [display] fps_limit for a fresh install. */
    fun performance(f: Facts): Pair<String, String> = when {
        // the same pixels as the earlier defaults (Maximum resolution x Resolution of a 1920 x 1080 game image), as a share of the screen
        // on the phone each class was set for: a foldable's near-square inner screen shows 1920 of the game's width on more pixels, a slab 1:1
        // unmeasured: estimate from GPU generation and cooling
        foldClass(f) && (adreno(f) ?: 0) >= 830 -> "55" to "30"     // Adreno 830+ (8 Elite and later): 60 % of 1080p
        // unmeasured: estimate from GPU generation and cooling
        foldClass(f) && !f.foldable -> "50" to "30"                  // Adreno 740 to 829, not foldable: 50 % of 1080p
        foldClass(f) -> "35" to "30"           // Adreno 740 to 829, foldable: 40 % of 1080p (measured: holds 30 on a hot Snapdragon 8 Gen 3 phone; 60 % does not)
        // cautious class: 40 % of 1080p; on a Mali GPU (Tensor) 50 % with SGSR costs no more (CPU-bound: measured cooled on a Tensor G2 (Mali-G710))
        mali(f) -> "50" to "30"
        else -> "40" to "30"
    }

    /** [pkp] resolution from an ini of an earlier build: [display] render_scale (percent of the game's targets, auto = 100) x [upscale]
     *  scene_scale (percent of that), as a share of the screen, so the picture keeps its pixels. The game's targets are the 16:9 frame
     *  widened or heightened to the screen's shape (1080 high on a wider screen, 1920 wide on a taller one); the runtime works it out the
     *  same way when it finds no value (pkp_render_size.cpp phone_res_install). Null: no earlier value. */
    fun migratedResolution(controls: Ini, f: Facts): String? {
        val rs = controls.get("display", "render_scale")?.trim()
        val ss = controls.get("upscale", "scene_scale")?.trim()
        if (rs.isNullOrEmpty() && ss.isNullOrEmpty()) return null
        val perScreen = if (f.screenW > 0 && f.screenH > 0) maxOf(1920.0 / f.screenW, 1080.0 / f.screenH) else 1.0
        val pct = (rs?.toIntOrNull() ?: 100) * (ss?.toIntOrNull() ?: 100) / 100.0 * perScreen
        return Math.round(pct).toInt().coerceIn(15, 100).toString()
    }

    /** [root]: the data root (absolute). Returns whether controls.ini changed (pkp.ini is always saved by the caller). */
    fun apply(ini: Ini, controls: Ini, root: String, f: Facts): Boolean {
        fun missing(s: String, k: String, v: String) { if (ini.get(s, k).isNullOrBlank()) ini.set(s, k, v) }
        fun raise(s: String, k: String, old: String, v: String) { if (ini.get(s, k)?.trim() == old) ini.set(s, k, v) }
        missing("fs", "save_dir", "save")
        missing("nvn", "shader_cache", "shader_cache/spirv")
        missing("nvn", "dump_missing", "shader_cache/missing")
        missing("nvn", "shader_order", "shader_cache/shader_order.txt")
        missing("nvn", "gpl", "false")
        missing("nvn", "shader_preload_mb", if (lowMemory(f)) "0" else "256")
        // pool_copy_mb = 768 saved no memory and stuttered more: the value this app wrote before goes, the runtime's default applies
        if (ini.get("nvn", "pool_copy_mb")?.trim() == "768") ini.remove("nvn", "pool_copy_mb")
        // [gfx] present_scale is internal (no row since the one resolution): 75 on the Low memory profile, else the runtime's 100. The
        // values the removed row offered go back to that (a 50 would also cap the resolution at half the screen); another number stays
        // (a tester's).
        if (ini.get("gfx", "present_scale")?.trim() in setOf("83", "67", "50")) { if (lowMemory(f)) ini.set("gfx", "present_scale", "75") else ini.remove("gfx", "present_scale") }
        // checked by eye on a phone: read-only depth kept, no frame grid (the HUD at the screen's resolution is always on: the runtime's)
        missing("nvn", "depth_ro_keep", "1")
        missing("latency", "grid", "false")
        missing("nvn", "compute_shadows", "false")   // checked by eye on a phone: about 1.5 ms of GPU time and 21 MB of copies less per frame
        missing("prompts", "dir", "$root/assets/prompts")   // the icon pack the app copies there: PlayStation and Xbox glyphs
        missing("pkp", "scene_upscaler", "1")   // SGSR (0 off, 1 SGSR, 2 bicubic): the phone default checked by eye
        if (lowMemory(f)) {
            // Low memory profile (8 GB phones, measured on a Tensor G2 / Mali-G710): a file-backed game heap in the cache folder (its
            // free space is checked again at every start: GameActivity falls back to the zram heap below HEAP_FILE_FREE_BYTES)
            missing("memory", "heap_section", "1")
            missing("memory", "section_dir", f.cacheDir)
            missing("nvn", "pool_ahb", "off")          // AHB pins pages the file backing wants to release
            missing("gfx", "bc_decode_small", "true")
            missing("gfx", "texture_cap", "2")
            missing("gfx", "texture_cap_min_px", "256")
            missing("gfx", "shadow_map_scale", "0.25")
            missing("gfx", "present_scale", "75")
            missing("pkp", "menu_in_game", "0")
        }
        raise("memory", "heap_gb", "8", "9")
        raise("memory", "app_memory_mb", "8192", "8704")
        missing("memory", "heap_gb", "9")
        missing("memory", "app_memory_mb", "8704")
        missing("log", "path", "$root/logs/pkp.log")
        missing("album", "dir", "$root/album")     // must be absolute on Android
        var changed = false
        for ((k, v) in listOf("window_mode" to "borderless")) {
            if (controls.get("display", k).isNullOrBlank()) { controls.set("display", k, v); changed = true }
        }
        // [upscale] mode off on phones: FSR 1 is no longer offered there (9.4 ms per frame on a large phone screen); an fsr1 of an
        // earlier build becomes SGSR, the phone's upscaler. The scene sharpen is the runtime's.
        val mode = controls.get("upscale", "mode")?.trim()
        if (mode.isNullOrEmpty() || mode.equals("fsr1", true)) {
            controls.set("upscale", "mode", "off"); changed = true
            if (mode.equals("fsr1", true)) ini.set("pkp", "scene_upscaler", "1")
        }
        val (res, cap) = performance(f)
        missing("pkp", "resolution", migratedResolution(controls, f) ?: res)
        if (controls.get("display", "fps_limit").isNullOrBlank()) { controls.set("display", "fps_limit", cap); changed = true }
        return changed
    }

    /** A fresh install writes every key; an existing ini keeps the player's values and only gains what is missing. */
    private const val GIB = 1L shl 30
    val FOLD = Facts(19 * GIB, "Adreno (TM) 750", true, true, "/data/user/0/org.recompia.pkp/cache", 2160, 1856)
    val PIXEL7A = Facts(11 * GIB, "Mali-G710", false, false, "/data/user/0/org.recompia.pkp/cache", 2400, 1080)
    val OTHER = Facts(16 * GIB, "Adreno (TM) 650", true, false, "/data/user/0/org.recompia.pkp/cache")
    val ELITE = Facts(16 * GIB, "Adreno (TM) 830", true, false, "/data/user/0/org.recompia.pkp/cache")

    /** The rule per device class, fresh and existing ini files (player values kept, line endings kept). */
    fun selfCheck() {
        fun fresh(f: Facts): Pair<Ini, Ini> = (Ini.parse("") to Ini.parse("")).also { (i, c) -> check(apply(i, c, "/sdcard/Recompia", f)) }
        val (fi, fc) = fresh(FOLD)
        check(fi.get("pkp", "resolution") == "35" && fc.get("display", "fps_limit") == "30" && fc.get("display", "render_scale") == null) { fi.text() + fc.text() }
        check(fi.get("memory", "heap_section") == null && fi.get("nvn", "shader_preload_mb") == "256" && fi.get("pkp", "scene_upscaler") == "1")
        check(fi.get("gfx", "ui_native") == null && fi.get("nvn", "depth_ro_keep") == "1" && fi.get("latency", "grid") == "false" &&
            fi.get("nvn", "compute_shadows") == "false" && fi.get("memory", "heap_gb") == "9" && fi.get("gfx", "present_scale") == null)
        check(fc.get("upscale", "mode") == "off" && fc.get("upscale", "scene_auto") == null && fi.get("upscale", "scene_auto") == null)
        val (pi, pc) = fresh(PIXEL7A)
        check(pi.get("pkp", "resolution") == "50" && pc.get("display", "fps_limit") == "30") { pi.text() + pc.text() }
        check(pi.get("memory", "heap_section") == "1" && pi.get("memory", "section_dir") == PIXEL7A.cacheDir && pi.get("nvn", "shader_preload_mb") == "0" &&
            pi.get("nvn", "pool_ahb") == "off" && pi.get("gfx", "present_scale") == "75" && pi.get("pkp", "menu_in_game") == "0") { pi.text() }
        val (oi, oc) = fresh(OTHER)
        check(oi.get("pkp", "resolution") == "40" && oc.get("display", "fps_limit") == "30") { oi.text() + oc.text() }
        check(!foldClass(FOLD.copy(turnip = false)) && foldClass(FOLD) && foldClass(ELITE) && !foldClass(OTHER) && lowMemory(PIXEL7A) && !lowMemory(FOLD))
        check(untestedGpu(ELITE.copy(gpu = "Adreno (TM) 850")) && !foldClass(ELITE.copy(gpu = "Adreno (TM) 850")) && !untestedGpu(FOLD))
        check(performance(ELITE).first == "55" && performance(FOLD.copy(foldable = false)).first == "50" && performance(FOLD).first == "35" &&
            performance(OTHER).second == "30" && performance(PIXEL7A).second == "30")
        // an earlier build's keys keep their pixels: a foldable's near-square inner screen at Maximum 70 and Resolution 100 is 62 % of the screen
        check(migratedResolution(Ini.parse("[display]\nrender_scale = 70\n[upscale]\nscene_scale = 100\n"), FOLD) == "62")
        check(migratedResolution(Ini.parse("[display]\nrender_scale = 70\n[upscale]\nscene_scale = 57\n"), FOLD.copy(screenW = 2376, screenH = 968)) == "45")
        check(migratedResolution(Ini.parse("[display]\nrender_scale = auto\n"), PIXEL7A) == "100" && migratedResolution(Ini.parse(""), FOLD) == null)
        val old = Ini.parse("[gfx]\r\nui_native = 0\r\npresent_scale = 83\r\n[latency]\r\ngrid = true\r\n[nvn]\r\npool_copy_mb = 768\r\ncompute_shadows = true\r\n[memory]\r\nheap_gb = 8\r\n")
        val oldControls = Ini.parse("[display]\nwindow_mode = windowed\nfps_limit = 60\nrender_scale = 50\n[upscale]\nmode = fsr1\nscene_scale = 80\n")
        check(apply(old, oldControls, "/sdcard/Recompia", PIXEL7A))   // missing keys are added, retired choices converted
        check(oldControls.get("display", "fps_limit") == "60" && oldControls.get("display", "window_mode") == "windowed" &&
            oldControls.get("upscale", "mode") == "off" && old.get("pkp", "scene_upscaler") == "1" && old.get("pkp", "resolution") == "40") { oldControls.text() + old.text() }
        check(old.get("gfx", "ui_native") == "0" && old.get("latency", "grid") == "true" && old.get("gfx", "present_scale") == "75") { old.text() }
        check(old.get("nvn", "compute_shadows") == "true" && old.get("nvn", "pool_copy_mb") == null && old.get("memory", "heap_gb") == "9") { old.text() }
        check(old.text().contains("\r\n") && !old.text().replace("\r\n", "").contains("\n")) { "line endings" }
    }
}
