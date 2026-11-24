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
  // the online account (cloud_signin.rs; Home's Sign in, cloud-signin.js)
  cloudSigninState: () => invoke('cloud_signin_state'),
  cloudSigninBegin: (args) => invoke('cloud_signin_begin', args),   // { method: 'browser' | 'phone' | null }
  cloudSigninCancel: () => invoke('cloud_signin_cancel'),
  cloudSignOut: () => invoke('cloud_sign_out'),
  cloudAccountPage: () => invoke('cloud_account_page'),
  openLicensesDir: () => invoke('open_licenses_dir'),
  openGameFile: (args) => invoke('open_game_file', args),   // { name }
  openGameDir: () => invoke('open_game_dir'),
  openSaveDir: () => invoke('open_save_dir'),
  openBackupDir: () => invoke('open_backup_dir'),
  openSetupLog: () => invoke('open_setup_log'),
  saveLogBundle: (args) => invoke('save_log_bundle', args),   // { publicRelay }: { path }: a zip of the logs, settings and co-op save info (no secrets) for a bug report

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

  // Co-op saves: each view is { profile: { id, name, members: [{ id, name }], me, split, ... }, host: { host, role }, folder }
  coopList: () => invoke('coop_list'),
  coopCreate: (args) => invoke('coop_create', args),   // { name, myName, relay }; resolves the list
  coopCreateSplit: (args) => invoke('coop_create_split', args),   // { name, players }; resolves the list
  coopAddMember: (args) => invoke('coop_add_member', args),   // { id, name }
  coopRemoveMember: (args) => invoke('coop_remove_member', args),   // { id, member }
  coopInvite: (args) => invoke('coop_invite', args),   // { id, relay }: { long, short }; short ('XXX-XXX') is null when that relay keeps none
  coopResolveInvite: (args) => invoke('coop_resolve_invite', args),   // { text, relay }: the long invite for a short code; other text as it is; rejects { code: 'invite_not_found' } when no invite has it
  coopInviteMembers: (args) => invoke('coop_invite_members', args),   // { invite }: { id, members: [{ id, name }] }, null when it is not an invite
  coopAccept: (args) => invoke('coop_accept', args),   // { invite, member }
  coopPlay: (args) => invoke('coop_play', args),   // { id, startAs: 'auto' | 'host' | 'visit', split: { side, screens, pads, sound } }; options may be omitted
  coopDelete: (args) => invoke('coop_delete', args),   // { id, relay }: relay = the one coop_invite was given; resolves the list
  coopLastRun: (args) => invoke('coop_last_run', args),   // { since }: unix seconds; { mismatch: 'build' | 'mods', own_mods } or null

  // Steam
  steamStatus: () => invoke('steam_status'),
  applyBranding: () => invoke('apply_branding'),
  listThemes: () => invoke('list_themes'),   // { packs: [{ folder, name, version, themes: [...], error }] } (themes.rs)
  addTheme: (args) => invoke('add_theme', args),   // { path }: a pack folder or .zip; { name, themes }
  setTheme: (args) => invoke('set_theme', args),   // { theme }: '<pack folder>/<id>', '' for the launcher's own looks (save_config never changes it)
  removeThemePack: (args) => invoke('remove_theme_pack', args),   // { pack }: the pack's folder name
  openThemesDir: () => invoke('open_themes_dir'),
  pickThemeFile: () => invoke('pick_theme_file'),   // a .zip; null when the dialog is closed
  steamAdd: () => invoke('steam_add'),   // the launcher shortcut
  steamRemove: (args) => invoke('steam_remove', args),   // { target: 'launcher' | 'game' } (game: the old shortcut)

  // Link Play
  relayStart: (args) => invoke('relay_start', args),   // { port }
  relayStop: () => invoke('relay_stop'),
  relayStatus: () => invoke('relay_status'),
  lanReport: () => invoke('lan_report'),
  publicIp: () => invoke('public_ip'),


  // Mods (Modtopia)
  modsList: (args = {}) => invoke('mods_list', args),   // profile id or active:true; absent = global defaults
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
  // Zonith (zonith.rs): the launcher fetches its newest release and starts it; Zonith's own setup builds the game the first time
  zonithStatus: (args) => invoke('zonith_status', args),   // { refresh }
  zonithDownload: () => invoke('zonith_download'),   // progress: "zonith-progress"
  zonithCancel: () => invoke('zonith_cancel'),
  zonithPlay: () => invoke('zonith_play'),   // "zonith-game" { state: 'running' | 'exited', code }
  zonithOpenDir: () => invoke('zonith_open_dir'),

  // Controller (pad.rs): the pad as the launcher reads it, for a webview with no Gamepad API; "pad-state" on every change
  padPoll: () => invoke('pad_poll'),

  // Achievements (the community tracker)
  achievementsView: (args) => invoke('achievements_view', args),   // { refresh }
};
