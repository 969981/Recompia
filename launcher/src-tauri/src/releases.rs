// SPDX-License-Identifier: GPL-3.0-or-later
//! Releases: the list GitHub publishes and what the launcher offers from it.
//!
//! `load_releases`: one GET of the GitHub releases list per session (plus one per refresh), the raw answer kept in the app config
//! folder and served when the network fails. Every failure is silent (one line on stderr, at worst an empty list).
//! Versions and channels (Stable, Testing) decide which release the Update button offers (`target_here`) and which builds Choose a
//! version lists (`builds_here`), with what going back to an older one means (`downgrade_risk`, `check_pick`). update.rs installs
//! the `Target` picked here.

use serde::{Deserialize, Serialize};
use std::cmp::Ordering;
use std::fs;
use std::io::Read;
use std::path::Path;
use std::sync::Mutex;
use std::time::Duration;

const REPO: &str = "featureJosh/pokopia-recomp";
/// The releases list: small, so a whole-call limit, generous for a slow link.
const API_TIMEOUT: Duration = Duration::from_secs(15);
/// Builds from a checkout carry these (0.0.0 = untagged build, 0.1.1 = the Cargo.toml default); a tagged release stamps its own.
const DEV_VERSIONS: [&str; 2] = ["0.0.0", "0.1.1"];
/// Twenty releases with long notes and their asset lists; a cut JSON fails to parse and the cache is served.
const LIST_LIMIT: u64 = 8 << 20;
const SUMS_ASSET: &str = "SHA256SUMS.txt";

#[cfg(windows)]
const ASSET_SUFFIX: &str = "-win64.zip";
#[cfg(target_os = "macos")]
// The macOS release zip (launcher/scripts/make-mac.sh): PokopiaLauncher.app at its root, whose Contents/MacOS is the
// launcher's own folder and so the install root the update is applied to.
const ASSET_SUFFIX: &str = "-macos-arm64.zip";
#[cfg(all(not(any(windows, target_os = "macos")), target_arch = "aarch64"))]
const ASSET_SUFFIX: &str = "-linux-arm64.tar.gz";
#[cfg(all(not(any(windows, target_os = "macos")), not(target_arch = "aarch64")))]
const ASSET_SUFFIX: &str = "-linux-x64.tar.gz";

#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct Release {
    pub tag: String,
    pub name: String,
    /// when it was published, ISO 8601 as GitHub gives it
    pub date: String,
    /// the release notes, markdown
    pub body: String,
    pub url: String,
    /// a Testing build (a GitHub prerelease): shown only on the Testing channel
    pub prerelease: bool,
}

#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct UpdateInfo {
    /// the tag as published, e.g. "v0.3.0"
    pub tag: String,
    pub name: String,
    /// the release page
    pub url: String,
    /// bytes of the package for this platform
    pub size: u64,
}

#[derive(Serialize, Clone, Debug, PartialEq, Default)]
pub struct Releases {
    pub releases: Vec<Release>,
    pub update: Option<UpdateInfo>,
}

#[derive(Deserialize, Clone, Debug)]
pub struct Asset {
    name: String,
    #[serde(default)]
    size: u64,
    #[serde(default)]
    browser_download_url: String,
}

/// One entry of the API's answer.
#[derive(Deserialize, Clone, Debug)]
pub struct GhRelease {
    tag_name: String,
    #[serde(default)]
    name: Option<String>,
    #[serde(default)]
    html_url: String,
    #[serde(default)]
    published_at: Option<String>,
    #[serde(default)]
    body: Option<String>,
    #[serde(default)]
    draft: bool,
    #[serde(default)]
    prerelease: bool,
    #[serde(default)]
    assets: Vec<Asset>,
}

impl GhRelease {
    fn title(&self) -> String {
        self.name.clone().filter(|n| !n.trim().is_empty()).unwrap_or_else(|| self.tag_name.clone())
    }
}

/// A parsed `major.minor.patch[-pre]` version (a leading `v` and `+build` metadata are ignored).
#[derive(Debug, PartialEq, Eq)]
pub struct Version {
    core: [u64; 3],
    pre: Vec<String>,
}

impl Version {
    pub fn parse(s: &str) -> Option<Version> {
        let s = s.trim();
        let s = s.strip_prefix('v').or_else(|| s.strip_prefix('V')).unwrap_or(s);
        let s = s.split('+').next()?;
        let (core, pre) = match s.split_once('-') {
            Some((c, p)) => (c, Some(p)),
            None => (s, None),
        };
        let mut nums = core.split('.');
        let mut core3 = [0u64; 3];
        for slot in core3.iter_mut() {
            let part = nums.next()?;
            if part.is_empty() || !part.bytes().all(|b| b.is_ascii_digit()) {
                return None;
            }
            *slot = part.parse().ok()?;
        }
        if nums.next().is_some() {
            return None;
        }
        let pre = match pre {
            None => Vec::new(),
            Some(p) => {
                let ids: Vec<String> = p.split('.').map(str::to_string).collect();
                if ids.iter().any(|i| i.is_empty() || !i.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-')) {
                    return None;
                }
                ids
            }
        };
        Some(Version { core: core3, pre })
    }
}

impl Ord for Version {
    fn cmp(&self, other: &Self) -> Ordering {
        self.core.cmp(&other.core).then_with(|| match (self.pre.is_empty(), other.pre.is_empty()) {
            (true, true) => Ordering::Equal,
            (true, false) => Ordering::Greater, // a release is newer than its own prerelease
            (false, true) => Ordering::Less,
            (false, false) => {
                for (a, b) in self.pre.iter().zip(&other.pre) {
                    let o = match (a.parse::<u64>(), b.parse::<u64>()) {
                        (Ok(x), Ok(y)) => x.cmp(&y),
                        (Ok(_), Err(_)) => Ordering::Less, // numeric identifiers rank below alphanumeric ones
                        (Err(_), Ok(_)) => Ordering::Greater,
                        _ => a.cmp(b),
                    };
                    if o != Ordering::Equal {
                        return o;
                    }
                }
                self.pre.len().cmp(&other.pre.len())
            }
        })
    }
}
impl PartialOrd for Version {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

/// True when `tag` is a valid version newer than `current`. Anything unparsable is "not newer".
pub fn is_newer(tag: &str, current: &str) -> bool {
    match (Version::parse(tag), Version::parse(current)) {
        (Some(t), Some(c)) => t > c,
        _ => false,
    }
}

pub fn is_dev_version(v: &str) -> bool {
    DEV_VERSIONS.contains(&v.trim())
}



fn github(url: &str) -> bool {
    url.starts_with("https://github.com/") && !url.chars().any(char::is_whitespace)
}

/// Which releases the player follows. Stable (the default) is what the launcher has always offered: published releases, no prereleases.
/// Testing also lists every prerelease, from every test line (`-beta.N` Testing, `-alpha.N` side lines such as Mods Alpha), for the
/// player to pick by hand in Choose a version. Testing offers no update at all (`offers_updates`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Channel {
    Stable,
    Testing,
}

impl Channel {
    /// launcher.json's `channel`; anything but "testing" (empty, a typo, a value from a newer launcher) is Stable.
    pub fn parse(s: &str) -> Channel {
        if s.trim().eq_ignore_ascii_case("testing") { Channel::Testing } else { Channel::Stable }
    }
    /// How many releases to ask GitHub for. Testing lists every prerelease, so it asks for a full page (GitHub's maximum).
    pub fn list_size(self) -> u32 {
        match self {
            Channel::Stable => 20,
            Channel::Testing => 100,
        }
    }
    /// Whether the Update button follows this channel. Testing runs several test lines side by side (Testing betas, alpha side lines),
    /// so "the newest newer build" would move a tester off their line onto another one: a tester picks each build by hand.
    pub fn offers_updates(self) -> bool {
        self == Channel::Stable
    }
}

/// The API's answer without drafts and anything that does not link to github.com. Prereleases (betas and alphas alike) are kept here:
/// `for_channel` decides.
pub fn parse_releases(body: &[u8]) -> Result<Vec<GhRelease>, String> {
    let all: Vec<GhRelease> = serde_json::from_slice(body).map_err(|e| e.to_string())?;
    Ok(all.into_iter().filter(|r| !r.draft && !r.tag_name.is_empty() && github(&r.html_url)).collect())
}

/// The releases a channel sees. Stable drops prereleases, so for a Stable player nothing differs from before channels existed.
pub fn for_channel(list: Vec<GhRelease>, channel: Channel) -> Vec<GhRelease> {
    match channel {
        Channel::Stable => list.into_iter().filter(|r| !r.prerelease).collect(),
        Channel::Testing => list,
    }
}

/// This platform's package of a release.
fn asset_for<'a>(r: &'a GhRelease, suffix: &str) -> Option<&'a Asset> {
    r.assets.iter().find(|a| a.name.ends_with(suffix) && !a.name.contains(['/', '\\']) && github(&a.browser_download_url))
}

fn sums_of(r: &GhRelease) -> Option<&Asset> {
    r.assets.iter().find(|a| a.name == SUMS_ASSET && github(&a.browser_download_url))
}

/// The newest release newer than `current` that can be installed here. One without this platform's package or a checksum file is
/// passed over: the Linux package lands an hour after the Windows one, and an Update button that can only fail is worse than none
/// for that hour (What's New lists the release anyway).
fn pick_update<'a>(list: &'a [GhRelease], current: &str, suffix: &str) -> Option<(&'a GhRelease, &'a Asset, &'a Asset)> {
    list.iter()
        .filter(|r| is_newer(&r.tag_name, current))
        .filter_map(|r| Some((r, asset_for(r, suffix)?, sums_of(r)?)))
        .max_by_key(|(r, _, _)| Version::parse(&r.tag_name))
}

/// What get_releases returns. `current`: None for a build that takes no updates.
pub fn view(list: &[GhRelease], current: Option<&str>, suffix: &str) -> Releases {
    let releases = list
        .iter()
        .map(|r| Release {
            tag: r.tag_name.clone(),
            name: r.title(),
            date: r.published_at.clone().unwrap_or_default(),
            body: r.body.clone().unwrap_or_default(),
            url: r.html_url.clone(),
            prerelease: r.prerelease,
        })
        .collect();
    let update = current.and_then(|c| pick_update(list, c, suffix)).map(|(r, a, _)| UpdateInfo { tag: r.tag_name.clone(), name: r.title(), url: r.html_url.clone(), size: a.size });
    Releases { releases, update }
}

pub fn view_here(list: &[GhRelease], current: Option<&str>) -> Releases {
    view(list, current, ASSET_SUFFIX)
}

pub fn get(agent: &ureq::Agent, url: &str, limit: u64) -> Result<Vec<u8>, String> {
    let resp = agent.get(url).set("Accept", "application/vnd.github+json").call().map_err(|e| e.to_string())?;
    let mut body = Vec::new();
    resp.into_reader().take(limit).read_to_end(&mut body).map_err(|e| e.to_string())?;
    Ok(body)
}

pub fn user_agent(version: &str) -> String {
    format!("PokopiaRecomp-Launcher/{}", version)
}

/// The parsed list of this session.
#[derive(Default)]
pub struct ReleaseStore(Mutex<Option<Vec<GhRelease>>>);

/// The releases: this session's copy unless `refresh`, else one GET, then the file `cache` on failure, else nothing. The lock is
/// held over the GET, so the pages' calls at start make one request between them.
pub fn load_releases(store: &ReleaseStore, cache: &Path, version: &str, refresh: bool, channel: Channel) -> Vec<GhRelease> {
    let mut mem = store.0.lock().unwrap();
    if let (Some(list), false) = (mem.as_ref(), refresh) {
        return for_channel(list.clone(), channel);
    }
    let agent = ureq::AgentBuilder::new().timeout(API_TIMEOUT).user_agent(&user_agent(version)).build();
    let url = format!("https://api.github.com/repos/{}/releases?per_page={}", REPO, channel.list_size());
    let fetched = get(&agent, &url, LIST_LIMIT).and_then(|body| parse_releases(&body).map(|list| (body, list)));
    let list = match fetched {
        Ok((body, list)) => {
            if let Err(e) = crate::settings::write_atomic(cache, &String::from_utf8_lossy(&body)) {
                eprintln!("release list not cached: {}", e);
            }
            list
        }
        Err(e) => {
            eprintln!("release check skipped: {}", e);
            fs::read(cache).ok().and_then(|b| parse_releases(&b).ok()).or_else(|| mem.take()).unwrap_or_default()
        }
    };
    *mem = Some(list.clone());
    for_channel(list, channel)
}

/// One build the player can pick: what What's New knows about it plus whether it can be installed here and what going to it means.
#[derive(Serialize, Clone, Debug, PartialEq)]
pub struct Build {
    pub tag: String,
    pub name: String,
    pub date: String,
    /// the release notes (the "readme" of the build), markdown
    pub notes: String,
    pub url: String,
    pub prerelease: bool,
    /// this system's package and the checksum file are there
    pub installable: bool,
    /// "newer", "installed" or "older" than the running build
    pub relation: String,
    /// "none" (newer or the same), "low" (older, same release line) or "high" (older release line)
    pub risk: String,
    pub reasons: Vec<String>,
}

/// Facts about what an older version does not know, by the version that introduced them: going back across one of them is a risk.
const BOUNDARIES: [(&str, &str); 2] = [
    ("0.8.0", "Profiles (a separate save and name for each player) arrived in 0.8.0. An older version ignores profiles.json and uses one save, so a second player's save may not be found."),
    ("0.7.5", "The one-click updater arrived in 0.7.5. An older launcher cannot update itself, so moving forward again would mean extracting a release by hand."),
];

/// What going from `current` to the build `tag` means: ("none" | "low" | "high", the reasons). Newer or the same: none. Older in the same
/// release line (same major and minor): low. Older release line, or one that crosses a BOUNDARY: high. Anything unparsable: high.
pub fn downgrade_risk(current: &str, tag: &str) -> (String, Vec<String>) {
    let (Some(c), Some(t)) = (Version::parse(current), Version::parse(tag)) else {
        return ("high".into(), vec!["That version could not be compared with the one you have.".into()]);
    };
    if t >= c {
        return ("none".into(), Vec::new());
    }
    let mut reasons = Vec::new();
    let mut crossed = false;
    for (since, text) in BOUNDARIES {
        if let Some(s) = Version::parse(since) {
            if t < s && s <= c {
                crossed = true;
                reasons.push(text.to_string());
            }
        }
    }
    reasons.push("The game code may have to be built again for the older version, before it is installed or with Set up again after it: from a few minutes to an hour, depending on your PC.".into());
    reasons.push("Files that only the newer version has stay in the folder; they are not removed.".into());
    let line = |v: &Version| (v.core[0], v.core[1]);
    let level = if crossed || line(&t) != line(&c) { "high" } else { "low" };
    (level.into(), reasons)
}

/// The rule for installing a build the player picked: Ok(true) when it is an older build (the player's data is copied first), Ok(false)
/// for a newer one. Err: the same build, or an older one the player did not confirm.
pub fn check_pick(current: &str, tag: &str, accepted: bool) -> Result<bool, String> {
    let bare = |v: &str| v.trim().trim_start_matches(['v', 'V']).to_string();
    if bare(current) == bare(tag) {
        return Err("You already have that build.".into());
    }
    let (risk, _) = downgrade_risk(current, tag);
    if risk != "none" && !accepted {
        return Err("Going back to an older build needs your confirmation first. Nothing was changed.".into());
    }
    Ok(risk != "none")
}

/// Every release of the list as a pick for the player, newest first. `current`: the running version.
pub fn builds(list: &[GhRelease], current: &str, suffix: &str) -> Vec<Build> {
    let mut out: Vec<(Option<Version>, Build)> = list
        .iter()
        .map(|r| {
            let v = Version::parse(&r.tag_name);
            let relation = match (&v, Version::parse(current)) {
                (Some(t), Some(c)) if *t > c => "newer",
                (Some(t), Some(c)) if *t == c => "installed",
                (Some(_), Some(_)) => "older",
                _ => "newer",
            };
            let (risk, reasons) = downgrade_risk(current, &r.tag_name);
            let b = Build {
                tag: r.tag_name.clone(),
                name: r.title(),
                date: r.published_at.clone().unwrap_or_default(),
                notes: r.body.clone().unwrap_or_default(),
                url: r.html_url.clone(),
                prerelease: r.prerelease,
                installable: v.is_some() && asset_for(r, suffix).is_some() && sums_of(r).is_some(),
                relation: relation.into(),
                risk,
                reasons,
            };
            (v, b)
        })
        .collect();
    out.sort_by(|a, b| b.0.cmp(&a.0));
    out.into_iter().map(|(_, b)| b).collect()
}

/// The release with exactly this tag, when it can be installed here (this system's package and the checksum file).
pub fn target_for_tag(list: &[GhRelease], tag: &str, suffix: &str) -> Option<Target> {
    let r = list.iter().find(|r| r.tag_name == tag)?;
    Version::parse(&r.tag_name)?;
    let (a, s) = (asset_for(r, suffix)?, sums_of(r)?);
    Some(Target { tag: r.tag_name.clone(), asset: a.name.clone(), asset_url: a.browser_download_url.clone(), size: a.size, sums_url: s.browser_download_url.clone() })
}

pub fn target_for_tag_here(list: &[GhRelease], tag: &str) -> Option<Target> {
    target_for_tag(list, tag, ASSET_SUFFIX)
}

pub fn builds_here(list: &[GhRelease], current: &str) -> Vec<Build> {
    builds(list, current, ASSET_SUFFIX)
}

/// What `install` fetches.
#[derive(Clone, Debug, PartialEq)]
pub struct Target {
    pub tag: String,
    pub asset: String,
    pub asset_url: String,
    pub size: u64,
    pub sums_url: String,
}

pub fn target_of(list: &[GhRelease], current: &str, suffix: &str) -> Option<Target> {
    pick_update(list, current, suffix).map(|(r, a, s)| Target {
        tag: r.tag_name.clone(),
        asset: a.name.clone(),
        asset_url: a.browser_download_url.clone(),
        size: a.size,
        sums_url: s.browser_download_url.clone(),
    })
}

pub fn target_here(list: &[GhRelease], current: &str) -> Option<Target> {
    target_of(list, current, ASSET_SUFFIX)
}

#[cfg(test)]
mod channel_tests {
    use super::*;

    fn rel(tag: &str, prerelease: bool) -> String {
        let asset = format!("PokopiaRecomp-{}-linux-x64.tar.gz", tag.trim_start_matches('v'));
        format!(
            r#"{{"tag_name":"{tag}","name":"{tag}","html_url":"https://github.com/featureJosh/Recompia/releases/tag/{tag}","draft":false,"prerelease":{prerelease},"assets":[{{"name":"{asset}","size":1,"browser_download_url":"https://github.com/featureJosh/Recompia/releases/download/{tag}/{asset}"}},{{"name":"SHA256SUMS.txt","size":1,"browser_download_url":"https://github.com/featureJosh/Recompia/releases/download/{tag}/SHA256SUMS.txt"}}]}}"#
        )
    }

    fn list_of(items: &[(&str, bool)]) -> Vec<GhRelease> {
        let body = format!("[{}]", items.iter().map(|(t, p)| rel(t, *p)).collect::<Vec<_>>().join(","));
        parse_releases(body.as_bytes()).unwrap()
    }

    #[test]
    fn channel_names() {
        assert_eq!(Channel::parse("testing"), Channel::Testing);
        assert_eq!(Channel::parse(" Testing "), Channel::Testing);
        for s in ["", "stable", "beta", "TESTINGS", "alpha"] {
            assert_eq!(Channel::parse(s), Channel::Stable, "{s:?}");
        }
        assert_eq!(Channel::Stable.list_size(), 20);
        assert!(Channel::Testing.list_size() > 20);
    }

    #[test]
    fn prereleases_rank_between_the_stable_releases() {
        assert!(is_newer("v0.8.1-beta.1", "0.8.0"));
        assert!(!is_newer("v0.8.1-beta.1", "0.8.1"), "a release is newer than its own prerelease");
        assert!(is_newer("v0.8.1", "0.8.1-beta.9"));
        assert!(is_newer("v0.8.1-beta.10", "0.8.1-beta.2"));
        assert!(!is_newer("v0.8.0", "0.8.1-beta.1"), "no downgrade from Testing back to an older stable");
    }

    #[test]
    fn stable_sees_exactly_what_it_saw_before_channels() {
        let all = list_of(&[("v0.8.1-beta.1", true), ("v0.8.0", false), ("v0.7.9", false), ("v0.7.9-rc.1", true)]);
        let tags: Vec<String> = for_channel(all, Channel::Stable).into_iter().map(|r| r.tag_name).collect();
        assert_eq!(tags, ["v0.8.0", "v0.7.9"]);
    }

    #[test]
    fn testing_sees_everything_that_is_not_a_draft() {
        let mut body = format!("[{}]", [rel("v0.8.1-beta.1", true), rel("v0.8.0", false)].join(","));
        body = body.replacen(r#""draft":false,"prerelease":true"#, r#""draft":true,"prerelease":true"#, 1);
        let all = parse_releases(body.as_bytes()).unwrap();
        assert_eq!(all.len(), 1, "a draft never shows");
        let body = format!("[{}]", [rel("v0.9.0-alpha.1", true), rel("v0.8.1-beta.1", true)].join(","));
        let all = parse_releases(body.as_bytes()).unwrap();
        assert_eq!(all.len(), 2, "an alpha is a prerelease like any other: Testing lists it");
        let all = list_of(&[("v0.8.1-beta.2", true), ("v0.8.1-beta.1", true), ("v0.8.0", false)]);
        assert_eq!(for_channel(all, Channel::Testing).len(), 3);
    }

    #[test]
    fn updates_follow_the_channel() {
        let all = list_of(&[("v0.8.1-beta.2", true), ("v0.8.1-beta.1", true), ("v0.8.0", false)]);
        let stable = for_channel(all.clone(), Channel::Stable);
        assert!(pick_update(&stable, "0.8.0", "-linux-x64.tar.gz").is_none(), "Stable never offers a prerelease");
        let testing = for_channel(all, Channel::Testing);
        let v = view(&testing, Some("0.8.0"), "-linux-x64.tar.gz");
        assert!(v.releases.iter().any(|r| r.prerelease) && v.releases.iter().any(|r| !r.prerelease));
    }

    #[test]
    fn testing_lists_every_test_line_and_offers_no_update() {
        assert!(Channel::Stable.offers_updates() && !Channel::Testing.offers_updates());
        let all = list_of(&[("v0.12.0-beta.1", true), ("v0.12.0-alpha.1", true), ("v0.11.1", false), ("v0.10.4-alpha.1", true)]);
        let testing = for_channel(all, Channel::Testing);
        assert_eq!(builds(&testing, "0.11.1", "-linux-x64.tar.gz").iter().filter(|b| b.prerelease).count(), 3, "every prerelease is listed, older test lines too");
    }

    #[test]
    fn going_back_is_ranked_by_how_far() {
        let r = |cur: &str, tag: &str| downgrade_risk(cur, tag).0;
        assert_eq!(r("0.8.1-beta.6", "v0.8.1-beta.7"), "none");
        assert_eq!(r("0.8.1-beta.6", "v0.8.1-beta.6"), "none");
        assert_eq!(r("0.8.1-beta.6", "v0.8.1-beta.5"), "low", "an earlier build of the same line");
        assert_eq!(r("0.8.1", "v0.8.1-beta.6"), "low", "a release is newer than its own beta");
        assert_eq!(r("0.8.1", "v0.8.0"), "low");
        assert_eq!(r("0.8.0", "v0.7.9"), "high", "another release line, and profiles are older than it");
        assert_eq!(r("0.7.9", "v0.7.8"), "low");
        assert_eq!(r("0.7.9", "v0.7.0"), "high", "the one-click updater is older than 0.7.5");
        assert_eq!(r("0.7.9", "v0.7.5"), "low");
        assert_eq!(r("0.8.0", "not-a-version"), "high");
        let (_, reasons) = downgrade_risk("0.8.0", "v0.7.9");
        assert!(reasons.iter().any(|x| x.contains("Profiles")) && reasons.iter().any(|x| x.contains("Set up again")), "{:?}", reasons);
        assert!(downgrade_risk("0.8.1", "v0.8.0").1.iter().any(|x| x.contains("Set up again")));
        assert!(downgrade_risk("0.8.1", "v0.9.0").1.is_empty(), "nothing to warn about going forward");
    }

    #[test]
    fn builds_are_listed_newest_first_with_their_relation_to_the_running_one() {
        let all = list_of(&[("v0.8.0", false), ("v0.8.1-beta.6", true), ("v0.7.9", false), ("v0.8.1-beta.5", true)]);
        let b = builds(&all, "0.8.1-beta.5", "-linux-x64.tar.gz");
        let tags: Vec<&str> = b.iter().map(|x| x.tag.as_str()).collect();
        assert_eq!(tags, ["v0.8.1-beta.6", "v0.8.1-beta.5", "v0.8.0", "v0.7.9"]);
        let rel: Vec<&str> = b.iter().map(|x| x.relation.as_str()).collect();
        assert_eq!(rel, ["newer", "installed", "older", "older"]);
        assert_eq!(b[0].risk, "none");
        assert_eq!(b[2].risk, "low");
        assert_eq!(b[3].risk, "high");
        assert!(b.iter().all(|x| x.installable), "each fixture has a package and a checksum file");
        assert!(b[0].prerelease && !b[2].prerelease);
    }

    #[test]
    fn a_build_without_a_package_or_checksum_is_listed_but_not_installable() {
        let body = format!("[{},{}]", rel("v0.8.1", false), r#"{"tag_name":"v0.8.0","name":"x","html_url":"https://github.com/featureJosh/Recompia/releases/tag/v0.8.0","draft":false,"prerelease":false,"assets":[]}"#);
        let all = parse_releases(body.as_bytes()).unwrap();
        let b = builds(&all, "0.8.1", "-linux-x64.tar.gz");
        assert_eq!(b.len(), 2);
        assert!(b[0].installable && !b[1].installable);
        assert!(target_for_tag(&all, "v0.8.0", "-linux-x64.tar.gz").is_none());
        assert!(target_for_tag(&all, "v0.8.1", "-linux-x64.tar.gz").is_some());
    }

    #[test]
    fn a_tag_that_is_not_in_the_list_is_refused_and_a_channel_list_decides_what_can_be_picked() {
        let all = list_of(&[("v0.8.1-beta.6", true), ("v0.8.0", false)]);
        assert!(target_for_tag(&all, "v9.9.9", "-linux-x64.tar.gz").is_none());
        assert!(target_for_tag(&all, "nonsense", "-linux-x64.tar.gz").is_none());
        let stable = for_channel(all.clone(), Channel::Stable);
        assert!(target_for_tag(&stable, "v0.8.1-beta.6", "-linux-x64.tar.gz").is_none(), "a Stable list never offers a Testing build");
        assert!(builds(&stable, "0.8.0", "-linux-x64.tar.gz").iter().all(|b| !b.prerelease));
        assert!(target_for_tag(&all, "v0.8.1-beta.6", "-linux-x64.tar.gz").is_some());
    }

    #[test]
    fn an_older_build_is_refused_without_the_players_confirmation_and_the_same_build_always() {
        assert_eq!(check_pick("0.8.1-beta.6", "v0.8.1-beta.7", false), Ok(false), "going forward needs no question");
        assert!(check_pick("0.8.1-beta.6", "v0.8.1-beta.5", false).unwrap_err().contains("confirmation"));
        assert_eq!(check_pick("0.8.1-beta.6", "v0.8.1-beta.5", true), Ok(true));
        assert!(check_pick("0.8.0", "v0.7.9", false).is_err());
        assert_eq!(check_pick("0.8.0", "v0.7.9", true), Ok(true));
        assert!(check_pick("0.8.0", "v0.8.0", true).unwrap_err().contains("already"));
        assert!(check_pick("0.8.0", "V0.8.0", false).is_err());
        assert!(check_pick("0.8.0", "garbage", false).is_err(), "an unknown version counts as risky");
    }

    #[test]
    fn the_plain_update_still_picks_the_newest_newer_build_and_ignores_older_ones() {
        let all = list_of(&[("v0.8.1", false), ("v0.8.0", false), ("v0.7.9", false)]);
        assert_eq!(pick_update(&all, "0.8.0", "-linux-x64.tar.gz").unwrap().0.tag_name, "v0.8.1");
        assert!(pick_update(&all, "0.8.1", "-linux-x64.tar.gz").is_none(), "nothing newer: an older build is never an update");
    }
}
