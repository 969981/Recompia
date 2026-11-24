// SPDX-License-Identifier: GPL-3.0-or-later
//! A new passkey for an account whose passkeys stopped working.
//!
//! A passkey belongs to the portal's host name. When the portal moves to another name, the passkeys made before are
//! dead and their accounts are stranded: nobody can sign in to them, link another device or open the account page.
//! A device that is still signed in is then the only proof of the account left. It asks the portal whether its
//! account is stranded (`/api/recover/device/begin`, with its certificate), proves its key by signing the portal's
//! challenge (`/api/recover/device/finish`), and gets back a one-time link to the portal's recovery page, where the
//! browser makes the account one new passkey. The portal grants nothing else, and nothing at all to an account that
//! can sign in.
//!
//! The key never leaves this device: only the certificate and the signature are sent. A portal without recovery
//! answers 404, which counts as "no new passkey needed".

use crate::device;
use crate::signin::{Fault, Portal};
use base64::engine::general_purpose::URL_SAFE_NO_PAD as B64URL;
use base64::Engine;
use std::path::Path;

/// What the device signs, before the portal's origin, a newline and the challenge (the portal's RECOVERY_PROOF).
const PROOF: &[u8] = b"cloud-islands device recovery v1\n";

struct Begun {
    ceremony: String,
    challenge: Vec<u8>,
}

/// Asks the portal about this device's account: None when it needs no new passkey.
fn begin(portal: &Portal, dir: &Path) -> Result<Option<Begun>, Fault> {
    let certificate = device::certificate_pem(dir).map_err(Fault::Network)?;
    let answer = match portal.post("/api/recover/device/begin", &serde_json::json!({ "certificate": certificate })) {
        Ok(a) => a,
        Err(Fault::Status(404, _)) => return Ok(None),
        Err(e) => return Err(e),
    };
    if answer.get("needed").and_then(|n| n.as_bool()) != Some(true) {
        return Ok(None);
    }
    let unusable = || Fault::Status(200, "the portal sent an unusable answer".into());
    let ceremony = answer.get("ceremony").and_then(|c| c.as_str()).ok_or_else(unusable)?.to_string();
    let challenge = answer.get("challenge").and_then(|c| c.as_str()).and_then(|c| B64URL.decode(c).ok()).filter(|c| c.len() >= 16).ok_or_else(unusable)?;
    Ok(Some(Begun { ceremony, challenge }))
}

/// What the portal says about this device's account.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Need {
    /// Its passkeys were made for another address: it needs a new one, and this device can get it one.
    Passkey,
    /// It can sign in.
    Nothing,
    /// The portal does not know this device (revoked there, or signed in at another portal): nothing to recover with.
    Stranger,
}

pub fn needed(portal: &str, dir: &Path) -> Result<Need, String> {
    match begin(&Portal::new(portal)?, dir) {
        Ok(Some(_)) => Ok(Need::Passkey),
        Ok(None) => Ok(Need::Nothing),
        Err(Fault::Status(403, _)) => Ok(Need::Stranger),
        Err(e) => Err(e.text()),
    }
}

/// The one-time address of the portal's page that makes the account a new passkey, for this PC's browser. None: the
/// account needs none.
pub fn link(portal: &str, dir: &Path) -> Result<Option<String>, String> {
    let portal = Portal::new(portal)?;
    let Some(begun) = begin(&portal, dir).map_err(|e| e.text())? else { return Ok(None) };
    let dev = device::load(dir)?;
    let rng = ring::rand::SystemRandom::new();
    let key = ring::signature::EcdsaKeyPair::from_pkcs8(&ring::signature::ECDSA_P256_SHA256_ASN1_SIGNING, dev.key.secret_der(), &rng)
        .map_err(|_| "this device's key cannot sign (not a P-256 key): sign in again instead".to_string())?;
    let message = [PROOF, portal.base().as_bytes(), b"\n", &begun.challenge].concat();
    let signature = key.sign(&rng, &message).map_err(|_| "could not sign with this device's key".to_string())?;
    let answer = portal
        .post("/api/recover/device/finish", &serde_json::json!({ "ceremony": begun.ceremony, "signature": B64URL.encode(signature.as_ref()) }))
        .map_err(|e| e.text())?;
    let url = answer.get("url").and_then(|u| u.as_str()).unwrap_or_default();
    // only this portal's own recovery page is ever opened
    let ticket = url.strip_prefix(&format!("{}/recover#ticket=", portal.base())).unwrap_or_default();
    if ticket.is_empty() || !ticket.bytes().all(|b| b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_' | b'.')) {
        return Err("the portal sent an unusable answer".into());
    }
    Ok(Some(url.to_string()))
}
