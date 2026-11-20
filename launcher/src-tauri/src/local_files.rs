// SPDX-License-Identifier: GPL-3.0-or-later
//! Serving a folder of the player's own files to the window, read only, through a custom protocol (the save editor's game data,
//! the player's themes). Plain relative paths only, inside the folder.

use std::path::{Component, Path, PathBuf};

/// A path the protocol may serve: relative, plain names only, inside the folder. No `\` or `:` on any system (a drive or a stream
/// on Windows), so the rule is the same everywhere.
pub fn safe_path(path: &str) -> Option<PathBuf> {
    let path = path.trim_start_matches('/');
    let p = Path::new(path);
    (!path.is_empty() && !path.contains(['\\', ':', '\0']) && p.components().all(|c| matches!(c, Component::Normal(_)))).then(|| p.to_path_buf())
}

pub fn content_type(path: &Path) -> &'static str {
    match path.extension().and_then(|e| e.to_str()).map(str::to_ascii_lowercase).as_deref() {
        Some("png") => "image/png",
        Some("webp") => "image/webp",
        Some("jpg" | "jpeg") => "image/jpeg",
        Some("json") => "application/json",
        Some("txt") => "text/plain; charset=utf-8",
        _ => "application/octet-stream",
    }
}

/// The answer to a protocol request for a file of `root`: the bytes, or 404. Any origin may read it (the window's own origin differs
/// from the protocol's, and the pictures are drawn to a canvas).
pub fn respond(root: &Path, request: &tauri::http::Request<Vec<u8>>) -> tauri::http::Response<Vec<u8>> {
    let builder = tauri::http::Response::builder().header("Access-Control-Allow-Origin", "*");
    let path = percent_decode(request.uri().path());
    let file = safe_path(&path).map(|p| root.join(p));
    match file.as_deref().map(|f| (f, std::fs::read(f))) {
        Some((f, Ok(bytes))) => builder.header("Content-Type", content_type(f)).body(bytes),
        _ => builder.status(404).body(Vec::new()),
    }
    .unwrap_or_else(|_| tauri::http::Response::new(Vec::new()))
}

pub fn percent_decode(s: &str) -> String {
    let b = s.as_bytes();
    let hex = |c: u8| (c as char).to_digit(16).map(|d| d as u8);
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' && i + 2 < b.len() {
            if let (Some(h), Some(l)) = (hex(b[i + 1]), hex(b[i + 2])) {
                out.push((h << 4) | l);
                i += 3;
                continue;
            }
        }
        out.push(b[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn serves_only_plain_paths_inside_the_folder() {
        assert_eq!(safe_path("/images/items/item_0006.png"), Some(PathBuf::from("images/items/item_0006.png")));
        assert_eq!(safe_path("manifest.json"), Some(PathBuf::from("manifest.json")));
        for bad in ["", "/", "../pkp.ini", "/images/../../pkp.ini", "/a\\b", "/C:/x", "/./x"] {
            assert_eq!(safe_path(bad), None, "{bad}");
        }
    }

    #[test]
    fn decodes_escaped_paths() {
        assert_eq!(percent_decode("/text/jp/text_item_jp.txt"), "/text/jp/text_item_jp.txt");
        assert_eq!(percent_decode("/a%20b%2Fc"), "/a b/c");
        assert_eq!(percent_decode("/50%"), "/50%");
    }

    #[test]
    fn pictures_have_their_types() {
        assert_eq!(content_type(Path::new("a/bg.WEBP")), "image/webp");
        assert_eq!(content_type(Path::new("x.jpeg")), "image/jpeg");
        assert_eq!(content_type(Path::new("x.bin")), "application/octet-stream");
    }

    #[test]
    fn a_missing_or_escaping_file_is_404() {
        let req = |p: &str| tauri::http::Request::builder().uri(format!("theme-data://localhost{p}")).body(Vec::new()).unwrap();
        let root = std::env::temp_dir();
        assert_eq!(respond(&root, &req("/pkp-no-such-file.png")).status(), 404);
        assert_eq!(respond(&root, &req("/%2E%2E%2Fx.png")).status(), 404);
    }
}
