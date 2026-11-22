// SPDX-License-Identifier: GPL-3.0-or-later
package org.recompia.pkp.ui

import android.content.Context
import androidx.annotation.StringRes
import org.json.JSONObject
import org.recompia.pkp.Paths
import org.recompia.pkp.R

/**
 * The short, phone-safe set of settings, grouped and labelled as in the PC launcher (launcher/src/settings-schema.js). Keys and
 * defaults come from the runtime's generated schemas (runtime/docs/controls_schema.json, pkp_ini_schema.json): a key the file
 * lacks reads as the game's own default, and nothing is written until the player changes it. Gameplay rows are every
 * `group: gameplay` key of the schema in its order (extras keep the schema's default, ON unless the row is opt_in).
 */
object SettingsModel {
    enum class File { PKP, CONTROLS }

    data class Option(val value: String, @StringRes val label: Int = 0, val text: String = "")

    sealed interface Row {
        val id: String
    }

    data class Toggle(override val id: String, val file: File, val section: String, val key: String, @StringRes val label: Int,
                      val def: Boolean, val parent: String? = null, @StringRes val note: Int = 0, val text: String = "") : Row

    data class Choice(override val id: String, val file: File, val section: String, val key: String, @StringRes val label: Int,
                      val def: String, val options: List<Option>, val parent: String? = null, val text: String = "",
                      @StringRes val note: Int = 0) : Row

    data class Group(@StringRes val title: Int, val rows: List<Row>, @StringRes val note: Int = 0)

    // labels of the gameplay mods, from launcher/src/settings-schema.js GAMEPLAY; a key without one shows its name (never hidden)
    private val GAMEPLAY_LABELS = mapOf(
        "jump_momentum" to R.string.ui_mod_jump_momentum,
        "camera_aim" to R.string.ui_mod_camera_aim,
        "camera_aim_reach" to R.string.ui_mod_camera_aim_reach,
        "inventory_sort" to R.string.ui_mod_inventory_sort,
        "inventory_autosort" to R.string.ui_mod_inventory_autosort,
        "box_tabs" to R.string.ui_mod_box_tabs,
        "box_tab_pages" to R.string.ui_mod_box_tab_pages,
        "big_box_tabs" to R.string.ui_mod_big_box_tabs,
        "craft_search" to R.string.ui_mod_craft_search,
        "save_now" to R.string.ui_mod_save_now,
        "item_editor" to R.string.ui_mod_item_editor,
        "pokemon_cap_50" to R.string.ui_mod_pokemon_cap_50,
        "pokemon_cap_limit" to R.string.ui_mod_pokemon_cap_limit,
    )
    private val GAMEPLAY_NOTES = mapOf(
        "camera_aim" to R.string.ui_mod_camera_aim_note,
        "item_editor" to R.string.ui_mod_item_editor_note,
        "pokemon_cap_50" to R.string.ui_mod_pokemon_cap_note,
        "box_tabs" to R.string.ui_mod_big_box_tabs_note,
    )

    /** Game languages by the runtime's names (runtime/src/services/common.cpp kLangs); labels are the languages' own names. */
    private val LANGUAGES = listOf(
        "AmericanEnglish" to "English (US)", "BritishEnglish" to "English (UK)", "Japanese" to "日本語", "French" to "Français",
        "CanadianFrench" to "Français (Canada)", "German" to "Deutsch", "Italian" to "Italiano", "Spanish" to "Español (España)",
        "LatinAmericanSpanish" to "Español (Latinoamérica)", "Korean" to "한국어", "SimplifiedChinese" to "简体中文",
        "TraditionalChinese" to "繁體中文", "Dutch" to "Nederlands", "Russian" to "Русский", "Portuguese" to "Português",
        "BrazilianPortuguese" to "Português (Brasil)",
    )

    private class Schema(json: String) {
        val keys: List<JSONObject> = if (json.isBlank()) emptyList() else buildList {
            val sections = JSONObject(json).getJSONArray("sections")
            for (i in 0 until sections.length()) {
                val s = sections.getJSONObject(i)
                val ks = s.getJSONArray("keys")
                for (j in 0 until ks.length()) add(ks.getJSONObject(j).put("_section", s.getString("name")))
            }
        }
        fun key(section: String, key: String) = keys.firstOrNull { it.getString("_section") == section && it.getString("key") == key }
        fun def(section: String, key: String): String = key(section, key)?.optString("default") ?: ""
    }

    fun groups(ctx: Context): List<Group> {
        val controls = Schema(AppViewModel.readText(ctx, "controls_schema.json"))
        val pkp = Schema(AppViewModel.readText(ctx, "pkp_ini_schema.json"))
        val c = File.CONTROLS
        val display = Group(R.string.ui_set_display, listOf(
            Choice("render_scale", c, "display", "render_scale", R.string.ui_set_render_scale, controls.def("display", "render_scale"), listOf(
                // the game's Maximum Resolution row: [display] render_scale in percent (auto = the screen's size = 100 %)
                Option("auto", R.string.ui_set_match_screen)) + listOf("30", "40", "50", "60", "70", "80", "100").map { Option(it, text = "$it%") }),
            Choice("fps_limit", c, "display", "fps_limit", R.string.ui_set_fps_limit, controls.def("display", "fps_limit"), listOf(
                // 25 and 20 (phones): any number is valid for the runtime; the game keeps real speed down to 20 fps (its own
                // multiplier, dt * 60 clamped to [1, 3]; the timestep module is not hooked into this game: no [time] keys needed)
                // the runtime's phone choices (kFpsPhoneChoices); a phone snaps the cap to whole refreshes of its screen
                Option("auto", R.string.ui_set_match_display), Option("20", text = "20"), Option("24", text = "24"), Option("30", text = "30"),
                Option("40", text = "40"), Option("60", text = "60"), Option("120", text = "120"), Option("0", R.string.ui_set_unlimited)),
                note = R.string.ui_set_fps_limit_note),
            // [gfx] present_scale (pkp.ini, runtime default 100, read once at the first swapchain): not written by the phone defaults (a squeezed picture after a rotation)
            Choice("present_scale", File.PKP, "gfx", "present_scale", R.string.ui_set_present_scale, "100", listOf(
                Option("100", text = "100%"), Option("83", text = "83%"), Option("67", text = "67%"), Option("50", text = "50%")),
                note = R.string.ui_set_present_scale_note),
            // [gfx] shadow_map_scale (runtime 0.25..1, default 1 = the game's 2048 map, read once at the first shadow map)
            Choice("shadow_map_scale", File.PKP, "gfx", "shadow_map_scale", R.string.ui_set_shadow, "1", listOf(
                Option("1", R.string.ui_set_shadow_high), Option("0.75", R.string.ui_set_shadow_medium), Option("0.5", R.string.ui_set_shadow_low)),
                note = R.string.ui_set_shadow_note),
            // [gfx] ui_native (runtime default 0; the phone defaults write 1)
            Toggle("ui_native", File.PKP, "gfx", "ui_native", R.string.ui_set_ui_native, false, note = R.string.ui_set_ui_native_note),
            Toggle("vsync", c, "display", "vsync", R.string.ui_set_vsync, truthy(controls.def("display", "vsync"))),
            Toggle("fps_show", c, "fps", "show", R.string.ui_set_fps_show, truthy(controls.def("fps", "show"))),
            // [fps] detail: Full (2) also shows heat and load on a phone (the separate [fps] thermal key is retired)
            Choice("fps_detail", c, "fps", "detail", R.string.ui_set_fps_detail, controls.def("fps", "detail").ifEmpty { "1" }, listOf(
                Option("0", R.string.ui_set_fps_detail_0), Option("1", R.string.ui_set_fps_detail_1), Option("2", R.string.ui_set_fps_detail_2))),
            // [upscale] mode: off on phones (FSR 1 measured at 9.4 ms per frame on a large phone screen); FSR 1 stays selectable
            // the phone's one Upscaler row (runtime pkp_options_rows phone_upscaler_get/set): Off / SGSR / Bicubic = pkp.ini [pkp]
            // scene_upscaler 0/1/2 with [upscale] mode off; FSR 1 = [upscale] mode fsr1 with scene_upscaler 0. read()/write() map it.
            Choice(PHONE_UPSCALER, File.PKP, "pkp", "scene_upscaler", R.string.ui_set_upscale, "1", listOf(
                Option("0", R.string.ui_set_upscale_off), Option("1", text = "SGSR"), Option("2", R.string.ui_set_upscale_bicubic),
                Option("3", text = "FSR 1")), note = R.string.ui_set_upscale_note),
        ), note = R.string.ui_set_display_note)
        val game = Group(R.string.ui_set_game, listOf(
            Choice("language", c, "tweaks", "language", R.string.ui_set_language, controls.def("tweaks", "language"),
                listOf(Option("auto", R.string.ui_set_automatic)) + LANGUAGES.map { Option(it.first, text = it.second) }),
        ))
        val rows = pkp.keys.filter { it.getString("_section") == "pkp" && it.optString("group") == "gameplay" && !it.optBoolean("hidden") }
        val gameplay = Group(R.string.ui_set_gameplay, rows.map { k ->
            val key = k.getString("key")
            val label = GAMEPLAY_LABELS[key] ?: 0
            val parent = k.optString("parent").ifEmpty { null }
            if (k.optString("type") == "bool") Toggle(key, File.PKP, "pkp", key, label, truthy(k.optString("default")), parent, GAMEPLAY_NOTES[key] ?: 0, key)
            else {
                val min = k.optInt("min", 0); val max = k.optInt("max", min)
                Choice(key, File.PKP, "pkp", key, label, k.optString("default"),
                    (min..max).map { n -> if (n == 0 && key == "camera_aim_reach") Option("0", R.string.ui_off) else Option(n.toString(), text = n.toString()) },
                    parent, key)
            }
        }.sortedForParents(), note = R.string.ui_set_gameplay_note)
        return listOf(display, game, gameplay)
    }

    /** Each mod's settings directly under its switch, in schema order (launcher gameplayRows). */
    private fun List<Row>.sortedForParents(): List<Row> {
        val parentOf = { r: Row -> (r as? Toggle)?.parent ?: (r as? Choice)?.parent }
        return filter { parentOf(it) == null }.flatMap { sw -> listOf(sw) + filter { parentOf(it) == sw.id } }
    }

    fun truthy(v: String?) = v?.trim()?.lowercase() in setOf("1", "true", "yes", "on")

    private fun fileOf(f: File) = if (f == File.PKP) Paths.pkpIni else Paths.controlsIni

    /** Current values by row id: the file's value, else the schema default. */
    const val PHONE_UPSCALER = "phone_upscaler"

    fun read(groups: List<Group>): Map<String, String> {
        val inis = File.entries.associateWith { Ini.load(fileOf(it)) }
        return groups.flatMap { it.rows }.associate { r ->
            when {
                r.id == PHONE_UPSCALER && inis[File.CONTROLS]!!.get("upscale", "mode")?.trim().equals("fsr1", true) -> r.id to "3"
                else -> readOne(r, inis)
            }
        }
    }

    private fun readOne(r: Row, inis: Map<File, Ini>): Pair<String, String> =
            when (r) {
                is Toggle -> r.id to (inis[r.file]!!.get(r.section, r.key)?.let { if (truthy(it)) "1" else "0" } ?: if (r.def) "1" else "0")
                is Choice -> r.id to (inis[r.file]!!.get(r.section, r.key)?.ifBlank { null } ?: r.def)
            }

    fun write(row: Row, value: String) {
        if (row.id == PHONE_UPSCALER) {   // each choice clears the other key, as in the game's own row
            val pkp = Ini.load(fileOf(File.PKP)); pkp.set("pkp", "scene_upscaler", if (value == "3") "0" else value); pkp.save(fileOf(File.PKP))
            val c = Ini.load(fileOf(File.CONTROLS)); c.set("upscale", "mode", if (value == "3") "fsr1" else "off"); c.save(fileOf(File.CONTROLS))
            return
        }
        val (file, section, key) = when (row) {
            is Toggle -> Triple(row.file, row.section, row.key)
            is Choice -> Triple(row.file, row.section, row.key)
        }
        val ini = Ini.load(fileOf(file))
        ini.set(section, key, value)
        ini.save(fileOf(file))
    }

}
