// SPDX-License-Identifier: GPL-3.0-or-later
//! The gateway's own identity towards the game: a certificate authority and one server certificate for the names the
//! game dials, made new at every start and kept in memory only. The game is handed the authority's certificate
//! (`[net] cloud_ca`); the authority's private key is dropped as soon as the server certificate is signed, so nothing
//! else can ever be issued under it, and no gateway private key is written to disk.

use rcgen::{BasicConstraints, CertificateParams, DistinguishedName, DnType, ExtendedKeyUsagePurpose, IsCa, KeyPair, KeyUsagePurpose};

/// The names the game dials: `<tenant>.lp1.t.npln.srv.nintendo.net`, `gamesync.npln.nintendo.net` and
/// `storage.npln.nintendo.net` (the runtime resolves them to `[net] cloud_server`; nothing reaches the original servers).
pub const GAME_NAMES: [&str; 5] =
    ["*.npln.nintendo.net", "npln.nintendo.net", "*.npln.srv.nintendo.net", "*.t.npln.srv.nintendo.net", "*.lp1.t.npln.srv.nintendo.net"];

pub struct Identity {
    /// What the game must trust (DER).
    pub ca_der: Vec<u8>,
    pub leaf_der: Vec<u8>,
    /// PKCS#8, memory only.
    pub leaf_key: Vec<u8>,
}

fn name(common: &str) -> DistinguishedName {
    let mut dn = DistinguishedName::new();
    dn.push(DnType::CommonName, common);
    dn
}

/// ECDSA P-256 for both (the game's TLS accepts it; see docs/cloud-island/README.md).
pub fn make() -> Result<Identity, rcgen::Error> {
    // The game checks the dates against the console clock it is given, which a player can move: a wide window.
    let from = rcgen::date_time_ymd(2020, 1, 1);
    let to = time::OffsetDateTime::now_utc() + time::Duration::days(3650);

    let ca_key = KeyPair::generate()?;
    let mut ca = CertificateParams::default();
    ca.distinguished_name = name("Recompia Cloud Gateway (this device)");
    ca.is_ca = IsCa::Ca(BasicConstraints::Unconstrained);
    ca.key_usages = vec![KeyUsagePurpose::DigitalSignature, KeyUsagePurpose::KeyCertSign, KeyUsagePurpose::CrlSign];
    ca.not_before = from;
    ca.not_after = to;
    let ca_cert = ca.self_signed(&ca_key)?;

    let leaf_key = KeyPair::generate()?;
    let mut leaf = CertificateParams::new(GAME_NAMES.iter().map(|n| n.to_string()).collect::<Vec<_>>())?;
    leaf.distinguished_name = name("*.lp1.t.npln.srv.nintendo.net");
    leaf.is_ca = IsCa::ExplicitNoCa;
    leaf.key_usages = vec![KeyUsagePurpose::DigitalSignature, KeyUsagePurpose::KeyEncipherment];
    leaf.extended_key_usages = vec![ExtendedKeyUsagePurpose::ServerAuth];
    leaf.use_authority_key_identifier_extension = true;
    leaf.not_before = from;
    leaf.not_after = to;
    let leaf_cert = leaf.signed_by(&leaf_key, &ca_cert, &ca_key)?;

    Ok(Identity { ca_der: ca_cert.der().to_vec(), leaf_der: leaf_cert.der().to_vec(), leaf_key: leaf_key.serialize_der() })
}
