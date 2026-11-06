// SPDX-License-Identifier: GPL-3.0-or-later
//! What a relay host tells visitors: which of this PC's IPv4 addresses to give out, and whether Windows will let
//! visitors in. Read only; nothing here changes the network profile or the firewall.
//!
//! Idea and the phone-hotspot bug from pokopia-native (MIT), windows/src/coop/coop_plat.c `cplat_local_addrs` /
//! `public_adapters` @7c3655d: a PC advertised the address of an iPhone's USB hotspot adapter (a Public network), and the
//! visitor's connection was blocked. The ranking below is our own: it is a pure function over [`Adapter`] so it is tested
//! without a network.

use serde::Serialize;
use std::net::Ipv4Addr;

pub const PUBLIC_WARNING: &str = "Windows treats this network as Public, so other PCs may not reach you. Set it to Private in Settings > Network.";
pub const FIREWALL_HINT: &str = "If Windows Firewall asks about this program, allow it on Private networks. If it was answered wrongly once, remove the Block rules for the program in Windows Defender Firewall > Allow an app.";

/// IfType values we care about (IANA).
pub const IF_WIFI: u32 = 71;
pub const IF_PPP: u32 = 23;
pub const IF_TUNNEL: u32 = 131;
pub const IF_LOOPBACK: u32 = 24;

/// One IPv4 address on one adapter, as read from the OS.
#[derive(Clone, Debug)]
pub struct Adapter {
    pub friendly_name: String,
    pub description: String,
    pub if_type: u32,
    pub up: bool,
    pub ip: Ipv4Addr,
    /// The adapter has an IPv4 default gateway.
    pub has_gateway: bool,
    /// Interface metric (lower wins); only compared between adapters with a gateway.
    pub metric: u32,
    /// Some(true): Windows profile Public. Some(false): Private or Domain. None: unknown.
    pub public: Option<bool>,
}

impl Adapter {
    fn name(&self) -> String {
        if self.friendly_name.is_empty() { self.description.clone() } else { self.friendly_name.clone() }
    }
}

#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct LanAddr {
    pub ip: String,
    pub adapter: String,
    /// Holds the default route.
    pub default_route: bool,
    pub public_profile: bool,
}

#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct OverlayAddr {
    pub ip: String,
    pub adapter: String,
    /// "internet via Tailscale" / "internet via ZeroTier"
    pub label: String,
}

#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct Skipped {
    pub ip: String,
    pub adapter: String,
    pub reason: String,
}

#[derive(Serialize, Clone, Debug, Default, PartialEq)]
pub struct LanReport {
    /// Best first: show lan[0] to visitors on this network.
    pub lan: Vec<LanAddr>,
    pub overlay: Vec<OverlayAddr>,
    pub skipped: Vec<Skipped>,
    /// Set when the address in lan[0] sits on a Public Windows network.
    pub warning: Option<String>,
    /// Set together with a warning, or when there is no usable address.
    pub firewall_hint: Option<String>,
}

#[derive(Debug, PartialEq)]
enum Class {
    Lan,
    Overlay(&'static str),
    Skip(&'static str),
}

fn has_any(hay: &str, needles: &[&str]) -> bool {
    needles.iter().any(|n| hay.contains(n))
}

fn classify(a: &Adapter) -> Class {
    let text = format!("{} {}", a.friendly_name, a.description).to_ascii_lowercase();
    let o = a.ip.octets();
    if !a.up {
        return Class::Skip("adapter is down");
    }
    if a.ip.is_loopback() || a.if_type == IF_LOOPBACK {
        return Class::Skip("loopback");
    }
    if a.ip.is_link_local() {
        return Class::Skip("link-local (169.254), no DHCP answer");
    }
    if a.ip.is_unspecified() {
        return Class::Skip("no address");
    }
    if text.contains("tailscale") {
        return Class::Overlay("internet via Tailscale");
    }
    if text.contains("zerotier") {
        return Class::Overlay("internet via ZeroTier");
    }
    if has_any(&text, &["rndis", "remote ndis", "apple mobile device", "iphone", "android", "tether"]) {
        return Class::Skip("USB-tethered phone");
    }
    // iPhone Personal Hotspot range over a wired (USB) link; a PC that joined it over Wi-Fi is a real, if tiny, LAN
    if o[0] == 172 && o[1] == 20 && o[2] == 10 && o[3] < 16 && a.if_type != IF_WIFI {
        return Class::Skip("USB-tethered phone");
    }
    if has_any(
        &text,
        &[
            "vethernet", "hyper-v", "wsl", "virtualbox", "vmware", "vmnet", "host-only", "docker", "npcap", "loopback adapter",
            "bluetooth", "pseudo", "wi-fi direct", "hosted network",
        ],
    ) {
        return Class::Skip("virtual adapter");
    }
    if has_any(
        &text,
        &["wintun", "tap-windows", "tap adapter", "openvpn", "wireguard", "nordlynx", "vpn", "anyconnect", "globalprotect", "fortinet", "tun0", "utun"],
    ) || a.if_type == IF_PPP
        || a.if_type == IF_TUNNEL
    {
        return Class::Skip("VPN or tunnel adapter");
    }
    Class::Lan
}

/// Sort key, smaller first: default-route holders, then RFC 1918 before other ranges, then lower metric, then address.
fn lan_key(a: &Adapter) -> (bool, bool, u32, u32) {
    (!a.has_gateway, !a.ip.is_private(), if a.has_gateway { a.metric } else { u32::MAX }, u32::from(a.ip))
}

/// Pure ranking over the adapter list. The default route is the gateway-holding LAN adapter with the lowest metric.
/// A Public profile does not demote an address (the visitor still has to use it); it raises the warning instead.
pub fn rank(adapters: &[Adapter]) -> LanReport {
    let mut rep = LanReport::default();
    let mut lan: Vec<&Adapter> = Vec::new();
    for a in adapters {
        match classify(a) {
            Class::Lan => lan.push(a),
            Class::Overlay(label) => rep.overlay.push(OverlayAddr { ip: a.ip.to_string(), adapter: a.name(), label: label.to_string() }),
            Class::Skip(why) => {
                // an adapter that is down or empty is not worth listing
                if a.up && !a.ip.is_unspecified() {
                    rep.skipped.push(Skipped { ip: a.ip.to_string(), adapter: a.name(), reason: why.to_string() });
                }
            }
        }
    }
    lan.sort_by_key(|a| lan_key(a));
    let mut first = true;
    for a in &lan {
        let default_route = a.has_gateway && std::mem::take(&mut first);
        if !a.has_gateway {
            first = false;
        }
        rep.lan.push(LanAddr { ip: a.ip.to_string(), adapter: a.name(), default_route, public_profile: a.public == Some(true) });
    }
    if rep.lan.first().map_or(false, |l| l.public_profile) {
        rep.warning = Some(PUBLIC_WARNING.to_string());
    }
    if rep.warning.is_some() || rep.lan.is_empty() {
        rep.firewall_hint = Some(FIREWALL_HINT.to_string());
    }
    rep
}

/// Reads this PC's adapters and ranks them. Off Windows there is nothing to read.
pub fn report() -> LanReport {
    rank(&sys::adapters())
}

/// Log block for `--host`.
pub fn log_lines(rep: &LanReport, port: u16) -> Vec<String> {
    let mut v = Vec::new();
    match rep.lan.first() {
        Some(a) => v.push(format!("Give visitors this address: {}:{} ({})", a.ip, port, a.adapter)),
        None => v.push("No usable LAN address found on this PC.".to_string()),
    }
    for a in rep.lan.iter().skip(1) {
        v.push(format!("  other LAN address: {}:{} ({}{})", a.ip, port, a.adapter, if a.public_profile { ", Public network" } else { "" }));
    }
    for o in &rep.overlay {
        v.push(format!("  {}: {}:{} ({})", o.label, o.ip, port, o.adapter));
    }
    for s in &rep.skipped {
        v.push(format!("  not offered: {} on {} ({})", s.ip, s.adapter, s.reason));
    }
    if let Some(w) = &rep.warning {
        v.push(format!("WARNING: {}", w));
    }
    if let Some(h) = &rep.firewall_hint {
        v.push(format!("Hint: {}", h));
    }
    v
}

#[cfg(windows)]
mod sys {
    use super::*;
    use std::collections::HashMap;
    use windows::core::GUID;
    use windows::Win32::Foundation::{ERROR_BUFFER_OVERFLOW, NO_ERROR};
    use windows::Win32::NetworkManagement::IpHelper::{
        GetAdaptersAddresses, GAA_FLAG_INCLUDE_GATEWAYS, GAA_FLAG_SKIP_ANYCAST, GAA_FLAG_SKIP_DNS_SERVER, GAA_FLAG_SKIP_MULTICAST, IP_ADAPTER_ADDRESSES_LH,
    };
    use windows::Win32::NetworkManagement::Ndis::IfOperStatusUp;
    use windows::Win32::Networking::NetworkListManager::{INetworkListManager, NetworkListManager, NLM_NETWORK_CATEGORY_PUBLIC};
    use windows::Win32::Networking::WinSock::{AF_INET, SOCKADDR_IN};
    use windows::Win32::System::Com::{CoCreateInstance, CoInitializeEx, CoUninitialize, CLSCTX_ALL, COINIT_MULTITHREADED};

    fn guid_str(g: &GUID) -> String {
        format!(
            "{{{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}}}",
            g.data1, g.data2, g.data3, g.data4[0], g.data4[1], g.data4[2], g.data4[3], g.data4[4], g.data4[5], g.data4[6], g.data4[7]
        )
    }

    /// adapter id -> is Public, from INetworkListManager (the API behind Settings > Network). Empty if COM fails.
    fn profiles() -> HashMap<String, bool> {
        let mut out = HashMap::new();
        unsafe {
            let init = CoInitializeEx(None, COINIT_MULTITHREADED);
            if let Ok(mgr) = CoCreateInstance::<_, INetworkListManager>(&NetworkListManager, None, CLSCTX_ALL) {
                if let Ok(en) = mgr.GetNetworkConnections() {
                    loop {
                        let mut c = [None];
                        let mut got = 0u32;
                        if en.Next(&mut c, Some(&mut got)).is_err() || got == 0 {
                            break;
                        }
                        let Some(conn) = c[0].take() else { break };
                        if let (Ok(id), Ok(net)) = (conn.GetAdapterId(), conn.GetNetwork()) {
                            if let Ok(cat) = net.GetCategory() {
                                // an adapter with several connections: Public wins
                                let e = out.entry(guid_str(&id)).or_insert(false);
                                *e |= cat == NLM_NETWORK_CATEGORY_PUBLIC;
                            }
                        }
                    }
                }
            }
            if init.is_ok() {
                CoUninitialize();
            }
        }
        out
    }

    pub fn adapters() -> Vec<Adapter> {
        let prof = profiles();
        let mut out = Vec::new();
        unsafe {
            let flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
            let mut len = 16384u32;
            let mut buf: Vec<u64> = Vec::new();
            let mut r = ERROR_BUFFER_OVERFLOW.0;
            for _ in 0..3 {
                if r != ERROR_BUFFER_OVERFLOW.0 {
                    break;
                }
                buf = vec![0u64; (len as usize + 7) / 8];
                r = GetAdaptersAddresses(AF_INET.0 as u32, flags, None, Some(buf.as_mut_ptr() as *mut IP_ADAPTER_ADDRESSES_LH), &mut len);
            }
            if r != NO_ERROR.0 {
                return out;
            }
            let mut p = buf.as_ptr() as *const IP_ADAPTER_ADDRESSES_LH;
            while !p.is_null() {
                let a = &*p;
                let id = a.AdapterName.to_string().unwrap_or_default();
                let friendly = a.FriendlyName.to_string().unwrap_or_default();
                let desc = a.Description.to_string().unwrap_or_default();
                let has_gw = !a.FirstGatewayAddress.is_null();
                let public = prof.iter().find(|(k, _)| k.eq_ignore_ascii_case(&id)).map(|(_, v)| *v);
                let mut u = a.FirstUnicastAddress;
                while !u.is_null() {
                    let ua = &*u;
                    let sa = ua.Address.lpSockaddr;
                    if !sa.is_null() && (*sa).sa_family == AF_INET {
                        let sin = &*(sa as *const SOCKADDR_IN);
                        let ip = Ipv4Addr::from(u32::from_be(sin.sin_addr.S_un.S_addr));
                        out.push(Adapter {
                            friendly_name: friendly.clone(),
                            description: desc.clone(),
                            if_type: a.IfType,
                            up: a.OperStatus == IfOperStatusUp,
                            ip,
                            has_gateway: has_gw,
                            metric: a.Ipv4Metric,
                            public,
                        });
                    }
                    u = ua.Next;
                }
                p = a.Next;
            }
        }
        out
    }
}

#[cfg(not(windows))]
mod sys {
    use super::*;
    /// No Windows API here (native Linux has no network profiles): nothing to list.
    pub fn adapters() -> Vec<Adapter> {
        Vec::new()
    }
}
