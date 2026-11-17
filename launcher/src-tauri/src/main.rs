// SPDX-License-Identifier: GPL-3.0-or-later
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod backup_cmds;
mod branding;
mod branding_cmds;
mod child_lines;
mod cli;
mod config;
mod coop;
mod coop_cmds;
mod coop_launch;
mod coop_save;
mod dialogs;
mod dlss;
mod dlss_cmds;
mod download;
mod dump;
mod dump_cmds;
mod error;
mod events;
mod exe_icon;
mod achievements;
mod achievements_cmds;
mod fsutil;
mod gamebuild;
mod guard;
mod harness;
mod home_count;
mod host;
mod ini;
mod lan;
mod modtopia_ffi;
mod mods;
mod mods_catalog;
mod mods_cmds;
mod mods_install;
mod mods_lock;
mod mods_scope;
mod mods_update;
mod nsp;
mod launch;
mod nup;
mod nx;
mod online;
mod open;
mod paths;
mod play;
mod play_time;
mod presence;
mod proc_tree;
mod preflight;
mod prompts;
mod profile_cmds;
mod profiles;
mod public_ip;
mod readiness;
use pokopia_relay as relay;
mod relay_cmds;
mod releases;
mod repair;
mod repair_cmds;
mod rollback;
mod runtime_api;
mod savebackup;
mod saveedit;
mod saveedit_cmds;
mod saveedit_data;
mod saveimport;
mod saves;
mod settings;
mod settings_cmds;
mod setup_job;
mod setuplog;
mod shader_archive;
mod shaders;
mod steam;
mod steam_cmds;
mod states;
mod storage;
#[cfg(debug_assertions)]
mod testhook;
mod unpack;
mod update;
mod update_apply;
mod update_game;
mod update_cmds;
mod window;
mod writecheck;

use std::sync::atomic::Ordering;
use tauri::Manager;

use config::{load_config, game_dir};
use paths::{exe_dir, launcher_path};
use states::States;

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.iter().any(|a| a == "--host") {
        std::process::exit(cli::run_headless(&args));
    }
    if args.iter().any(|a| a == "--play") {
        std::process::exit(cli::run_play(&args));
    }
    readiness::UPDATED.store(args.iter().any(|a| a == "--updated"), Ordering::SeqCst);
    // WebKitGTK's DMA-BUF renderer leaves the window blank or kills it ("Error 71 (Protocol error)") on NVIDIA's driver
    #[cfg(target_os = "linux")]
    if std::env::var_os("WEBKIT_DISABLE_DMABUF_RENDERER").is_none() && std::path::Path::new("/sys/module/nvidia").exists() {
        std::env::set_var("WEBKIT_DISABLE_DMABUF_RENDERER", "1");
    }
    states::install(tauri::Builder::default().plugin(tauri_plugin_dialog::init()))
        // a swap cut off by closing the window would have to wait for the next start to be undone
        .on_window_event(window::on_event)
        // the save editor's game data (saveedit_data.rs), read only, from the game folder's pokohex_data
        .register_uri_scheme_protocol(saveedit_data::PROTOCOL, |ctx, request| saveedit_data::respond(&game_dir(ctx.app_handle()), &request))
        .setup(|app| {
            if let Some(window) = app.get_webview_window("main") {
                // a failed screen query only means the window keeps its default size: it must never stop the launcher from starting
                let _ = window::fit_window(&window, load_config(app.handle()).window);
            }
            branding::apply_window(app.handle(), &branding::Branding::of(&load_config(app.handle())));
            // Link Play starts from its defaults on every launcher start: nothing left from an earlier session decides the next one (#239)
            if let Err(e) = settings::reset_link_play(&game_dir(app.handle())) {
                eprintln!("Link Play reset: {e}");
            }
            // what the last update left: its folder, the old launcher (still exiting after a restart), a cut-off swap
            if let Some(launcher) = launcher_path() {
                let handle = app.handle().clone();
                let st = app.updates().inner().clone();
                st.set_cleaning(true);
                std::thread::spawn(move || {
                    let (root, game) = (exe_dir(), game_dir(&handle));
                    update::clean_start(&root, &update::old_files(&launcher, &game));
                    st.set_cleaning(false);
                    branding::on_start(&load_config(&handle));
                });
            }
            #[cfg(debug_assertions)]
            testhook::drive_page(app);
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            #[cfg(debug_assertions)]
            testhook::test_report,
            config::get_config,
            readiness::launcher_state,
            update_cmds::get_releases,
            update_cmds::install_update,
            update_cmds::list_builds,
            update_cmds::install_version,
            update_cmds::cancel_update,
            open::open_link,
            config::save_config,
            readiness::get_overview,
            dialogs::pick_folder,
            dump_cmds::validate_dump,
            dialogs::pick_package,
            dialogs::pick_nsp,
            dump_cmds::import_dlc,
            dump_cmds::import_package,
            dump_cmds::cancel_unpack,
            dump_cmds::save_dump,
            settings_cmds::read_settings,
            settings_cmds::write_settings,
            settings_cmds::prompt_icon_pack,
            launch::play,
            setup_job::setup_status,
            setup_job::start_setup,
            setup_job::preflight,
            setup_job::open_setup_log,
            setup_job::cancel_setup,
            storage::shader_storage,
            storage::compact_shaders,
            storage::expand_shaders,
            storage::cancel_shader_storage,
            dlss_cmds::dlss_status,
            dlss_cmds::fetch_dlss,
            launch::game_status,
            saves::save_sources,
            saves::import_save,
            saves::open_save_dir,
            profile_cmds::profiles_list,
            profile_cmds::profiles_play_time,
            profile_cmds::profile_add,
            profile_cmds::profile_rename,
            profile_cmds::profile_remove,
            profile_cmds::profile_select,
            coop_cmds::coop_list,
            coop_cmds::coop_create,
            coop_cmds::coop_add_member,
            coop_cmds::coop_remove_member,
            coop_cmds::coop_invite,
            coop_cmds::coop_invite_members,
            coop_cmds::coop_accept,
            coop_cmds::coop_play,
            repair_cmds::repair_plan,
            repair_cmds::repair_apply,
            repair_cmds::repair_undo,
            repair_cmds::repair_last,
            backup_cmds::backup_status,
            backup_cmds::backup_list,
            backup_cmds::backup_now,
            backup_cmds::backup_restore,
            backup_cmds::open_backup_dir,
            saveedit_cmds::save_edit_read,
            saveedit_cmds::save_edit_write,
            saveedit_cmds::save_edit_data_ready,
            saveedit_cmds::save_edit_prepare,
            branding_cmds::apply_branding,
            steam_cmds::steam_status,
            steam_cmds::steam_add,
            steam_cmds::steam_remove,
            open::open_game_file,
            open::open_game_dir,
            relay_cmds::relay_start,
            relay_cmds::relay_stop,
            relay_cmds::relay_status,
            relay_cmds::lan_report,
            relay_cmds::public_ip,

            mods_cmds::mods_list,
            mods_cmds::mods_set_enabled,
            mods_cmds::mods_move,
            mods_cmds::mods_set_setting,
            mods_cmds::mods_remove,
            mods_cmds::mods_catalog,
            mods_cmds::mods_install,
            mods_cmds::mods_auto_update,
            mods_cmds::mods_keep_locked,
            mods_cmds::mods_set_lock,
            mods_cmds::open_mods_dir,
            achievements_cmds::achievements_view,
        ])
        .run(tauri::generate_context!())
        .expect("error while running the launcher");
}
