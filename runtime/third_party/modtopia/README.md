Modtopia (MIT, `LICENSE`), based on 0.6.1 with local API 0.7 save identity and core 0.8 profile overrides. The runtime links
the core as `nxrt_modtopia` and embeds the mappings. Preserve these local extensions when updating the vendored sources.

All mods inherit global settings from `config/<mod-id>.toml` (falling back to manifest defaults) and global enabled states from
`mods/modtopia.toml`. A profile may override individual keys in `config/accounts/<32-hex-account-key>/<mod-id>.toml`, and enabled
states in that folder's `.enabled.toml` (`[enabled]` maps mod IDs to booleans). Absent keys inherit; explicit equal values remain
pinned. Clearing an override resumes inheritance. No mod manifest flag or mod-side profile-management code is needed.

The runtime selects the account through the existing backend save identity. Launcher editors pass an account key to
`mt_loader_set_config_account` before scanning, or NULL for globals. Editing scope does not select the profile used by Play.
Load order, installation, code trust, and `data_dir` remain global. Only the playing profile supplies the hook plan.

`mt_config.c` owns configuration layering and sparse writes. Invalid override files are reported and preserved. Settings changes
notify mods after a successful write; enabled changes retain their normal next-start behavior. Existing per-account files are
preserved as explicit overrides and can be returned to global values through the editor.
