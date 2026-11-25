// SPDX-License-Identifier: GPL-3.0-or-later
//! The linked device: the four files linking writes into the device folder (the same ones the server author's
//! launcher.py writes, so a folder linked with it works here).
//!
//! - `launcher.json`: the account and device ids and where the server is (`ingress`: host, port, server_name)
//! - `device.crt`: this device's certificate, issued by the portal (PEM)
//! - `device.key`: its private key (PEM PKCS#8). On Windows launcher.py encrypts it with a random passphrase that
//!   DPAPI protects for the current user in `device.key.dpapi`; elsewhere it is a plain PEM readable by its owner only.
//! - `ingress-ca.pem`: the only authority the server's certificate is checked against
//!
//! The private key is made on this device (signin.rs), written here only, and never logged or sent anywhere. Every
//! file is replaced atomically (written to `<name>.tmp`, then renamed over the old one). A sign-in writes
//! `launcher.json` last and a sign-out removes it first, so a folder read in between counts as signed out rather than
//! as a mix of two devices.

use rustls::pki_types::pem::PemObject;
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer};
use std::path::{Path, PathBuf};

/// Every file of a sign-in (launcher.json last on purpose: see `save`).
pub const FILES: [&str; 5] = ["device.key", "device.key.dpapi", "device.crt", "ingress-ca.pem", "launcher.json"];

pub struct Device {
    pub account: String,
    pub device: String,
    pub host: String,
    pub port: u16,
    pub server_name: String,
    pub chain: Vec<CertificateDer<'static>>,
    pub key: PrivateKeyDer<'static>,
    pub ingress_ca: Vec<CertificateDer<'static>>,
}

/// Where launcher.py keeps a link by default: `%LOCALAPPDATA%\PokopiaLauncher` on Windows, else
/// `$XDG_DATA_HOME/pokopia-launcher` (`~/.local/share/pokopia-launcher`).
pub fn default_dir() -> PathBuf {
    let var = |n: &str| std::env::var_os(n).filter(|v| !v.is_empty()).map(PathBuf::from);
    if cfg!(windows) {
        let base = var("LOCALAPPDATA").or_else(|| var("USERPROFILE").map(|h| h.join("AppData").join("Local"))).unwrap_or_default();
        base.join("PokopiaLauncher")
    } else {
        let base = var("XDG_DATA_HOME").or_else(|| var("HOME").map(|h| h.join(".local").join("share"))).unwrap_or_default();
        base.join("pokopia-launcher")
    }
}

/// Whether the folder holds a link (all four files are there). Nothing is opened or checked.
pub fn linked(dir: &Path) -> bool {
    ["launcher.json", "device.crt", "device.key", "ingress-ca.pem"].iter().all(|f| dir.join(f).is_file())
}

fn read(dir: &Path, name: &str) -> Result<Vec<u8>, String> {
    std::fs::read(dir.join(name)).map_err(|e| format!("cannot read {name} in the device folder: {e}"))
}

fn certs(dir: &Path, name: &str) -> Result<Vec<CertificateDer<'static>>, String> {
    let pem = read(dir, name)?;
    let v: Vec<_> = CertificateDer::pem_slice_iter(&pem).collect::<Result<_, _>>().map_err(|e| format!("{name} is not a PEM certificate: {e}"))?;
    if v.is_empty() {
        return Err(format!("{name} holds no certificate"));
    }
    Ok(v)
}

pub fn load(dir: &Path) -> Result<Device, String> {
    let json: serde_json::Value = serde_json::from_slice(&read(dir, "launcher.json")?).map_err(|e| format!("launcher.json is not readable: {e}"))?;
    let text = |v: &serde_json::Value, k: &str| v.get(k).and_then(|x| x.as_str()).map(str::to_string).ok_or(format!("launcher.json has no \"{k}\""));
    let ingress = json.get("ingress").ok_or("launcher.json has no \"ingress\"")?;
    let port = ingress.get("port").and_then(|p| p.as_u64()).filter(|p| (1..=65535).contains(p)).ok_or("launcher.json has no usable ingress port")? as u16;
    Ok(Device {
        account: text(&json, "account")?,
        device: text(&json, "device")?,
        host: text(ingress, "host")?,
        port,
        server_name: text(ingress, "server_name")?,
        chain: certs(dir, "device.crt")?,
        key: key(dir)?,
        ingress_ca: certs(dir, "ingress-ca.pem")?,
    })
}

fn key(dir: &Path) -> Result<PrivateKeyDer<'static>, String> {
    let pem = read(dir, "device.key")?;
    let text = std::str::from_utf8(&pem).map_err(|_| "device.key is not a PEM file".to_string())?;
    if !text.contains("ENCRYPTED PRIVATE KEY") {
        return PrivateKeyDer::from_pem_slice(&pem).map_err(|e| format!("device.key is not a PEM private key: {e}"));
    }
    let protected = dir.join("device.key.dpapi");
    if !protected.is_file() {
        return Err("device.key is encrypted but its passphrase file (device.key.dpapi) is missing: link this device again".into());
    }
    let mut passphrase = unprotect(&std::fs::read(&protected).map_err(|e| format!("cannot read device.key.dpapi: {e}"))?)?;
    let opened = decrypt(text, &passphrase);
    passphrase.iter_mut().for_each(|b| *b = 0);
    opened
}

/// PKCS#8 with password-based encryption (PBES2), as OpenSSL and Python's cryptography write it.
fn decrypt(pem: &str, passphrase: &[u8]) -> Result<PrivateKeyDer<'static>, String> {
    let (label, doc) = pkcs8::Document::from_pem(pem).map_err(|e| format!("device.key is not a PEM file: {e}"))?;
    if label != "ENCRYPTED PRIVATE KEY" {
        return Err("device.key is not an encrypted PKCS#8 key".into());
    }
    let info = pkcs8::EncryptedPrivateKeyInfo::try_from(doc.as_bytes()).map_err(|e| format!("device.key is not an encrypted PKCS#8 key: {e}"))?;
    let plain = info.decrypt(passphrase).map_err(|_| "device.key could not be opened with this user's passphrase: link this device again".to_string())?;
    Ok(PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(plain.as_bytes().to_vec())))
}

#[cfg(windows)]
fn unprotect(data: &[u8]) -> Result<Vec<u8>, String> {
    use windows_sys::Win32::Foundation::LocalFree;
    use windows_sys::Win32::Security::Cryptography::{CryptUnprotectData, CRYPTPROTECT_UI_FORBIDDEN, CRYPT_INTEGER_BLOB};
    let input = CRYPT_INTEGER_BLOB { cbData: data.len() as u32, pbData: data.as_ptr() as *mut u8 };
    let mut out = CRYPT_INTEGER_BLOB { cbData: 0, pbData: std::ptr::null_mut() };
    // SAFETY: `input` points at `data` for the call; on success `out` is a LocalAlloc buffer of cbData bytes that is
    // copied, wiped and freed here.
    unsafe {
        if CryptUnprotectData(&input, std::ptr::null_mut(), std::ptr::null(), std::ptr::null(), std::ptr::null(), CRYPTPROTECT_UI_FORBIDDEN, &mut out) == 0 {
            return Err("Windows could not unprotect device.key.dpapi (it was made by another user or on another PC): link this device again".into());
        }
        let bytes = std::slice::from_raw_parts_mut(out.pbData, out.cbData as usize);
        let v = bytes.to_vec();
        bytes.iter_mut().for_each(|b| *b = 0);
        LocalFree(out.pbData.cast());
        Ok(v)
    }
}

#[cfg(not(windows))]
fn unprotect(_data: &[u8]) -> Result<Vec<u8>, String> {
    Err("device.key is encrypted with a Windows-protected passphrase, which only Windows can open: link this device again here".into())
}

#[cfg(windows)]
fn protect(data: &[u8]) -> Result<Vec<u8>, String> {
    use windows_sys::Win32::Foundation::LocalFree;
    use windows_sys::Win32::Security::Cryptography::{CryptProtectData, CRYPTPROTECT_UI_FORBIDDEN, CRYPT_INTEGER_BLOB};
    let input = CRYPT_INTEGER_BLOB { cbData: data.len() as u32, pbData: data.as_ptr() as *mut u8 };
    let mut out = CRYPT_INTEGER_BLOB { cbData: 0, pbData: std::ptr::null_mut() };
    let description: Vec<u16> = "Recompia device key".encode_utf16().chain([0]).collect();
    // SAFETY: `input` and `description` live for the call; on success `out` is a LocalAlloc buffer of cbData bytes
    // that is copied and freed here.
    unsafe {
        if CryptProtectData(&input, description.as_ptr(), std::ptr::null(), std::ptr::null(), std::ptr::null(), CRYPTPROTECT_UI_FORBIDDEN, &mut out) == 0 {
            return Err("Windows could not protect the device key's passphrase".into());
        }
        let v = std::slice::from_raw_parts(out.pbData, out.cbData as usize).to_vec();
        LocalFree(out.pbData.cast());
        Ok(v)
    }
}

/// Makes the device folder (owner only on POSIX; on Windows %LOCALAPPDATA% is the user's own already).
pub fn make_dir(dir: &Path) -> Result<(), String> {
    std::fs::create_dir_all(dir).map_err(|e| format!("cannot make the device folder: {e}"))?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(dir, std::fs::Permissions::from_mode(0o700)).map_err(|e| format!("cannot make the device folder private: {e}"))?;
    }
    Ok(())
}

/// Replaces `dir/name` atomically; private files are created readable by their owner only (POSIX 0600).
fn write(dir: &Path, name: &str, data: &[u8], private: bool) -> Result<(), String> {
    use std::io::Write;
    let temp = dir.join(format!("{name}.tmp"));
    let mut options = std::fs::OpenOptions::new();
    options.write(true).create(true).truncate(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(if private { 0o600 } else { 0o644 });
    }
    #[cfg(not(unix))]
    let _ = private;
    let fail = |e: std::io::Error| format!("cannot write {name} in the device folder: {e}");
    let mut file = options.open(&temp).map_err(fail)?;
    file.write_all(data).and_then(|()| file.sync_all()).map_err(fail)?;
    drop(file);
    std::fs::rename(&temp, dir.join(name)).map_err(fail)
}

fn delete(dir: &Path, name: &str) -> Result<(), String> {
    match std::fs::remove_file(dir.join(name)) {
        Err(e) if e.kind() != std::io::ErrorKind::NotFound => Err(format!("cannot remove {name} from the device folder: {e}")),
        _ => Ok(()),
    }
}

/// A new sign-in, as the portal answered it, with the key made for it.
pub struct NewDevice<'a> {
    pub portal: &'a str,
    pub account: &'a str,
    pub account_name: &'a str,
    pub device: &'a str,
    pub host: &'a str,
    pub port: u16,
    pub server_name: &'a str,
    pub certificate_pem: &'a str,
    pub ingress_ca_pem: &'a str,
    /// The private key, PKCS#8 DER.
    pub key_pkcs8: &'a [u8],
}

/// Writes a sign-in in the layout launcher.py uses (and `load` reads), replacing any earlier one. The key: on Windows
/// PKCS#8 encrypted with a random passphrase (PBES2, PBKDF2-SHA256 + AES-256-CBC) that DPAPI protects for this user in
/// `device.key.dpapi`; elsewhere a plain PKCS#8 PEM readable by its owner only.
pub fn save(dir: &Path, new: &NewDevice) -> Result<(), String> {
    make_dir(dir)?;
    delete(dir, "launcher.json")?;   // signed out until the last file is in place
    let key = pkcs8::PrivateKeyInfo::try_from(new.key_pkcs8).map_err(|e| format!("the new key is not PKCS#8: {e}"))?;
    if cfg!(windows) {
        let rng = ring::rand::SystemRandom::new();
        let random = |n: usize| -> Result<Vec<u8>, String> {
            let mut b = vec![0u8; n];
            ring::rand::SecureRandom::fill(&rng, &mut b).map_err(|_| "no random numbers from the system".to_string())?;
            Ok(b)
        };
        // the passphrase is text, as launcher.py makes it (token_urlsafe(32))
        use base64::Engine;
        let mut passphrase = base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(random(32)?).into_bytes();
        let (salt, iv) = (random(16)?, random(16)?);
        let iv: [u8; 16] = iv.try_into().unwrap_or([0; 16]);
        // the passphrase is 256 random bits, not a password: key stretching adds nothing, and the gateway opens the
        // key again whenever the folder changes
        let params = pkcs8::pkcs5::pbes2::Parameters::pbkdf2_sha256_aes256cbc(10_000, &salt, &iv).map_err(|e| e.to_string())?;
        let sealed = key.encrypt_with_params(params, &passphrase).map_err(|e| format!("could not encrypt the device key: {e}"));
        #[cfg(windows)]
        let protected = protect(&passphrase);
        #[cfg(not(windows))]
        let protected: Result<Vec<u8>, String> = Err(String::new());
        passphrase.iter_mut().for_each(|b| *b = 0);
        let pem = sealed?.to_pem("ENCRYPTED PRIVATE KEY", pkcs8::LineEnding::LF).map_err(|e| e.to_string())?;
        write(dir, "device.key.dpapi", &protected?, true)?;
        write(dir, "device.key", pem.as_bytes(), true)?;
    } else {
        let doc = pkcs8::SecretDocument::try_from(key).map_err(|e| e.to_string())?;
        let pem = doc.to_pem("PRIVATE KEY", pkcs8::LineEnding::LF).map_err(|e| e.to_string())?;
        delete(dir, "device.key.dpapi")?;
        write(dir, "device.key", pem.as_bytes(), true)?;
    }
    write(dir, "device.crt", new.certificate_pem.as_bytes(), false)?;
    write(dir, "ingress-ca.pem", new.ingress_ca_pem.as_bytes(), false)?;
    let json = serde_json::json!({
        "portal": new.portal,
        "account": new.account,
        "account_name": new.account_name,
        "device": new.device,
        "ingress": { "host": new.host, "port": new.port, "server_name": new.server_name },
    });
    write(dir, "launcher.json", serde_json::to_string_pretty(&json).map_err(|e| e.to_string())?.as_bytes(), false)
}

/// This device's certificate as the portal issued it (PEM), to show the portal which device asks.
pub fn certificate_pem(dir: &Path) -> Result<String, String> {
    String::from_utf8(read(dir, "device.crt")?).map_err(|_| "device.crt is not a PEM certificate".to_string())
}

/// Records the portal this sign-in belongs to now (launcher.json `portal`), after the portal moved to another address
/// and still knows this device. Nothing else of the sign-in changes.
pub fn set_portal(dir: &Path, portal: &str) -> Result<(), String> {
    let mut json: serde_json::Value = serde_json::from_slice(&read(dir, "launcher.json")?).map_err(|e| format!("launcher.json is not readable: {e}"))?;
    let fields = json.as_object_mut().ok_or("launcher.json is not readable")?;
    fields.insert("portal".into(), portal.into());
    write(dir, "launcher.json", serde_json::to_string_pretty(&json).map_err(|e| e.to_string())?.as_bytes(), false)
}

/// Signs this device out here: removes the sign-in's files (launcher.json first). The device stays valid on the
/// portal until it is removed there, which is what revokes it.
pub fn remove(dir: &Path) -> Result<(), String> {
    for name in FILES.iter().rev() {
        delete(dir, name)?;
    }
    Ok(())
}

/// Who this device is signed in as, read without opening the key.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Summary {
    pub portal: String,
    pub account: String,
    /// Empty for a folder linked by launcher.py, which does not keep it.
    pub account_name: String,
    pub device: String,
    /// The certificate's end, Unix seconds.
    pub not_after: i64,
}

pub fn summary(dir: &Path) -> Result<Summary, String> {
    if !linked(dir) {
        return Err("not signed in".into());
    }
    let json: serde_json::Value = serde_json::from_slice(&read(dir, "launcher.json")?).map_err(|e| format!("launcher.json is not readable: {e}"))?;
    let text = |k: &str| json.get(k).and_then(|x| x.as_str()).unwrap_or_default().to_string();
    let cert = certs(dir, "device.crt")?;
    let not_after = crate::signin::cert_fields(&cert[0]).map(|(_, end)| end).ok_or("device.crt is not a certificate this launcher can read")?;
    Ok(Summary { portal: text("portal"), account: text("account"), account_name: text("account_name"), device: text("device"), not_after })
}
