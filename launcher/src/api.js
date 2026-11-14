// Every Rust command the pages call, one named function each. The command names and argument names are the ones in the Rust
// signatures (src-tauri/src/*.rs, listed in main.rs generate_handler!): an argument object goes through as it is. A page never
// types a command name; checks that every name here is registered in Rust, and every one registered is here, live in
// scripts/check-api.mjs.
import { invoke } from '@tauri-apps/api/core';

export const api = {
  // Launcher state, settings and the launcher's own options
  getConfig: () => invoke('get_config'),
  saveConfig: (args) => invoke('save_config', args),   // { config }
  launcherState: () => invoke('launcher_state'),
  getOverview: () => invoke('get_overview'),
  readSettings: () => invoke('read_settings'),
  writeSettings: (args) => invoke('write_settings', args),   // { edits }
  promptIconPack: () => invoke('prompt_icon_pack'),   // ArrayBuffer: assets/prompts/prompt_icons.pak

  // Updates and builds
  getReleases: (args) => invoke('get_releases', args),   // { refresh }
  listBuilds: (args) => invoke('list_builds', args),   // { channel, refresh }
  installUpdate: () => invoke('install_update'),
  installVersion: (args) => invoke('install_version', args),   // { tag, channel, accepted }
  cancelUpdate: () => invoke('cancel_update'),

  // Opening links, files and folders
  openLink: (args) => invoke('open_link', args),   // { url }
  openGameFile: (args) => invoke('open_game_file', args),   // { name }
  openGameDir: () => invoke('open_game_dir'),
  openSaveDir: () => invoke('open_save_dir'),
  openBackupDir: () => invoke('open_backup_dir'),
  openSetupLog: () => invoke('open_setup_log'),

  // Choosing the game files
  pickFolder: (args) => invoke('pick_folder', args),   // { title, start }
  pickPackage: (args) => invoke('pick_package', args),   // { start }
  pickNsp: () => invoke('pick_nsp'),
  validateDump: (args) => invoke('validate_dump', args),   // { path }
  saveDump: (args) => invoke('save_dump', args),   // { path }
  importPackage: (args) => invoke('import_package', args),   // { path }
  importDlc: (args) => invoke('import_dlc', args),   // { path }
  cancelUnpack: () => invoke('cancel_unpack'),

  // Play and setup
  play: (args) => invoke('play', args),   // { withoutMods } (optional: this one start runs without mods)
  gameStatus: () => invoke('game_status'),
  setupStatus: () => invoke('setup_status'),
  startSetup: (args) => invoke('start_setup', args),   // { gentle }
  cancelSetup: () => invoke('cancel_setup'),
  preflight: () => invoke('preflight'),

  // Shader storage and DLSS
  shaderStorage: () => invoke('shader_storage'),
  compactShaders: () => invoke('compact_shaders'),
  expandShaders: () => invoke('expand_shaders'),
  cancelShaderStorage: () => invoke('cancel_shader_storage'),
  dlssStatus: () => invoke('dlss_status'),
  fetchDlss: () => invoke('fetch_dlss'),

  // Saves, backups, repair and profiles
  saveSources: () => invoke('save_sources'),
  importSave: (args) => invoke('import_save', args),   // { source }
  backupStatus: () => invoke('backup_status'),
  backupList: () => invoke('backup_list'),
  backupNow: () => invoke('backup_now'),
  backupRestore: (args) => invoke('backup_restore', args),   // { name }
  saveEditRead: () => invoke('save_edit_read'),   // [{ path, data (base64) }]: the active profile's G00 and S00
  saveEditWrite: (args) => invoke('save_edit_write', args),   // { files: [{ path, data }] }
  saveEditDataReady: () => invoke('save_edit_data_ready'),   // the editor's game data (names, pictures) is made and current
  saveEditPrepare: () => invoke('save_edit_prepare'),   // makes it from the game files; "save-edit-data-progress" { done, total }
  repairPlan: (args) => invoke('repair_plan', args),   // { actions, groups }
  repairApply: (args) => invoke('repair_apply', args),   // { actions, groups }
  repairUndo: () => invoke('repair_undo'),
  repairLast: () => invoke('repair_last'),
  profilesList: () => invoke('profiles_list'),
  profilesPlayTime: () => invoke('profiles_play_time'),   // { [profile id]: seconds played }, only profiles with a time
  profileAdd: (args) => invoke('profile_add', args),   // { name }
  profileRename: (args) => invoke('profile_rename', args),   // { id, name }
  profileRemove: (args) => invoke('profile_remove', args),   // { id }
  profileSelect: (args) => invoke('profile_select', args),   // { id }

  // Steam
  steamStatus: () => invoke('steam_status'),
  applyBranding: () => invoke('apply_branding'),
  steamAdd: (args) => invoke('steam_add', args),   // { target }
  steamRemove: (args) => invoke('steam_remove', args),   // { target }

  // Link Play
  relayStart: (args) => invoke('relay_start', args),   // { port }
  relayStop: () => invoke('relay_stop'),
  relayStatus: () => invoke('relay_status'),
  lanReport: () => invoke('lan_report'),
  publicIp: () => invoke('public_ip'),


  // Mods (Modtopia)
  modsList: () => invoke('mods_list'),
  modsSetEnabled: (args) => invoke('mods_set_enabled', args),   // { id, on, allow }
  modsMove: (args) => invoke('mods_move', args),   // { id, delta }
  modsSetSetting: (args) => invoke('mods_set_setting', args),   // { id, key, number | text }
  modsRemove: (args) => invoke('mods_remove', args),   // { id }
  modsCatalog: (args) => invoke('mods_catalog', args),   // { refresh }
  modsInstall: (args) => invoke('mods_install', args),   // { id }
  modsAutoUpdate: () => invoke('mods_auto_update'),   // [{ name, from, to }] updated, once per session, only with the launcher preference on
  modsKeepLocked: () => invoke('mods_keep_locked'),   // ['install' | 'switch_on' | 'share_on'] done, once at launcher start
  modsSetLock: (args) => invoke('mods_set_lock', args),   // { what: 'mod' | 'sharing', locked }
  openModsDir: () => invoke('open_mods_dir'),
  // Achievements (the community tracker)
  achievementsView: (args) => invoke('achievements_view', args),   // { refresh }
};
