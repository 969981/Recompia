// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's own icon (src-tauri/icons/icon.ico; a Testing build copies its icons there first), so the old name's file and the
// shortcuts that start it look like the launcher.
use std::path::Path;

fn main() {
    let ico = Path::new(env!("CARGO_MANIFEST_DIR")).join("../src-tauri/icons/icon.ico");
    println!("cargo:rerun-if-changed={}", ico.display());
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        return;
    }
    let ico = ico.canonicalize().expect("launcher/src-tauri/icons/icon.ico");
    let rc = Path::new(&std::env::var("OUT_DIR").unwrap()).join("stub.rc");
    // rc.exe reads a quoted path with forward slashes; a \\?\ prefix it does not
    let path = ico.to_string_lossy().trim_start_matches(r"\\?\").replace('\\', "/");
    std::fs::write(&rc, format!("1 ICON \"{path}\"\n")).unwrap();
    embed_resource::compile(&rc, embed_resource::NONE).manifest_required().unwrap();
}
