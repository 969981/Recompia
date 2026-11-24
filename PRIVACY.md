# Privacy

This page says what Recompia (the launcher and the game it runs) sends over the internet, to whom, and how to stop it. It is written
from the code in this repository.

In short:

- There are no accounts and no sign-in. Your saves, settings and profiles stay on your PC. Nothing uploads them.
- There is no telemetry, no analytics and no crash-report upload. A crash report is a text file on your PC (`pkp_crash.txt`) that you
  choose to share or not.
- Some features talk to a server because they cannot work without one. Most are off until you turn them on or start them. The two that
  can be on without you doing anything are the update check (every launcher start) and Community Achievements sharing (see below).
- Every connection reveals your IP address to the server at the other end. That is how the internet works, and it is listed below
  where it matters.

Questions about privacy: legal.recompia@gmail.com

"The Recompia maintainers" below means the people who run this project. Where a server belongs to someone else, that party is named.

## Connections at a glance

| What | Goes to | Runs | Turn it off |
|---|---|---|---|
| Update check | GitHub | Every launcher start, and the Check for updates button | Cannot be disabled in the launcher; block the launcher in your firewall |
| Update download | GitHub | Only when you press Update | Do not press it |
| Android app update check | GitHub | When the app opens, at most every 6 hours, and the Check now button | Settings > Updates, Check for updates automatically |
| Link Play, same network | Other PCs on your network | Only when you host or visit in the game | Link Play page, Same network off; or Off |
| Link Play, internet | The public relay (the Recompia maintainers) | Only after you pick Internet on the Link Play page and host or visit | Pick Same network or Off on the Link Play page |
| Public IP lookup | ipify, icanhazip | Only for an internet host with an own relay or address | Do not use that option |
| Online presence | Nobody at present: paused, nothing is sent | Off by default; asked once | Home chip, Settings > Game, or the game's Online tab |
| Mod list and mod downloads | The mod hub (a third party) | When you open the Mods or Achievements page, or the launcher installs Community Achievements | Do not open those pages (see below for the one automatic install) |
| Community Achievements | The Community Achievements service (a third party) | On by default once the mod is installed | Unlock sharing on the Mods page, then switch it off |
| DLSS download | NVIDIA's GitHub repository | Only when you press the download button (Windows) | Do not press it |
| Linux and macOS tools | GitHub, PyPI, ziglang.org | During Set up on Linux and macOS, if the tools are missing | Not optional on those systems |
| Discord status | Your own Discord app (local only) | Off by default | Settings, Discord status |
| Mods that use the network | Whatever hosts the mod names | When the mod runs | Switch the mod off |

## Update check and update download (GitHub)

- What it is for: telling you a new release exists, and showing the release notes.
- When: once per launcher start, and when you press Check for updates. The launcher asks GitHub for the list of releases of this project
  and keeps the answer in its settings folder so the notes also show offline (`releases.rs`).
- What is sent: an ordinary HTTPS request to `api.github.com` with the launcher's name and version as the user agent. Nothing about you, your PC or your save. GitHub sees your IP address, as it does for any
  visit to its site.
- Who runs it: GitHub (Microsoft).
- Download: pressing Update downloads the release package and its checksum file from the project's GitHub release page
  (`update.rs`, `download.rs`). The package is checked against the checksum before anything is replaced.
- Kept: GitHub's own logs are governed by GitHub's privacy statement. The launcher keeps the last release list on your PC.
- Turn it off: the launcher has no switch for the start-up check. You can block it in your firewall; the launcher then works without
  update information.
- Android app: when it opens, at most every 6 hours, the app asks `api.github.com` for the same list with `Recompia-Android` and its
  version as the user agent, and keeps the release it offers in its own storage (`update/Releases.kt`). Pressing Update downloads the
  APK and SHA256SUMS.txt from the release page and checks the APK before Android installs it (`update/Updater.kt`). Turn the check
  off in **Settings > Updates** (Check for updates automatically); Check now still asks when you press it.

## Link Play (playing together)

Link Play only starts when you choose Invite Others to Visit or Visit a Friend in the game.

### Same network (the default)

- What it is for: playing with people on your local network.
- What happens: hosting opens a listening port on your PC (TCP and UDP 47600) and announces the room on your local network. Visiting
  looks for hosts on your local network; on Linux it checks the addresses of your private networks one by one. Nothing is sent to the
  internet.
- What other players learn: the name of the player, a platform tag, a random install ID (made on first use and kept as `coop-uid.txt` in
  your save folder), the game build and a fingerprint of the gameplay mods you have on. All of it goes to the players you play with.
- Turn it off: Link Play page, Off.

### Internet through the public relay

- What it is for: playing with people who are not on your network, without opening ports on your router.
- When: only after you choose Internet on the Link Play page (it is off until you do) and then host or visit in the game. The
  launcher keeps that choice until you change it.
- What is sent to the relay: your IP address and port (the relay sees every connection it carries), a room ID derived from the room's
  code, the game build ID and a fingerprint of your gameplay mods. A host who lists a room (a room with no private code) sends the room
  listing: the room code, the build, the mods fingerprint and the start of the game's own description of the room, which is the
  information the game's Visit a Friend list shows. Visitors ask the relay for that list with their build ID and mods fingerprint.
- Your game traffic with the other players (their names, game state) goes through the relay, which copies bytes between the two sides.
  The traffic is authenticated between the players but not encrypted, so the relay operator can read it. The relay program does not
  store it.
- What the relay keeps in memory: the open rooms (a room lives at most 12 hours, then closes), the room listings and counters. This is
  gone when a room closes or the relay restarts.
- What the relay writes to its log (`launcher/relay/src/engine.rs`): when a room opens or closes (the host's IP address and port, the
  first 8 hex digits of the room ID, how long it was open and how many bytes it carried), when a visitor waits for or is paired with a
  room (the visitor's IP address), refused connections (IP address and the reason), and a counters line (rooms, links, bytes, lists). It
  writes the log to standard output; how long it is kept is decided by the server's log settings, not by the relay program.
- Co-op invite codes (co-op builds): when you make a co-op invite while your co-op save plays on the public relay, the launcher
  stores the invite there under a short code (like `K7Q-M2X`). The invite holds what a friend needs to join: the co-op save it is
  for, its players' names and the room's private code. The relay keeps it in memory only, for up to a day, and gives it to anyone who
  types the short code; it is gone when the day ends or the relay restarts. The long invite code works without the relay storing
  anything.
- Who runs it: the Recompia maintainers.
- Other players: in a session, the players you play with see your player name and the same details listed for Same network.
- Turn it off: Link Play page, choose Same network or Off. Anyone can also run their own relay (Advanced on the Link Play page, or
  `launcher/relay`); a relay run by someone else sees the same things, and its operator decides what it keeps.

### Direct internet play

If you choose Use my own relay or address, you can host without the public relay. A visitor then connects to the address you give them,
so they see your IP address, and you see theirs. The launcher can show you your public address for this (next section).

## Public IP lookup (ipify and icanhazip)

- What it is for: showing a host the public address to give to visitors.
- When: only when you chose Internet, then Advanced > Use my own relay or address, then Host. Not otherwise.
- What is sent: a plain HTTPS request to `api.ipify.org`, and if that fails `ipv4.icanhazip.com` (`public_ip.rs`). They see your IP
  address and the launcher name and version.
- Who runs it: ipify and icanhazip (third parties). Kept in the launcher for the rest of the run, then dropped.
- Turn it off: do not use that option.

## Online presence (paused)

**Paused:** the project runs no presence server at present, and the launcher sends nothing, whatever the switch says. Launchers
before this change send to `api.recompia.com`, a name the project no longer holds: switch presence off there, or update. What follows
describes presence as it was and as it will be when it returns.

- What it is for: a "players online" list on the project's website.
- When: off by default. The first time you press Play with a new launcher it asks once, and No is the default answer. You can switch it
  on or off on Home, in Settings > Game, or on the Online tab of the game's own Settings. Nothing is sent while it is off.
- What is sent, once when the game starts and then every 30 seconds while it runs, and a leave message when it ends:
  - an online ID: a SHA-256 hash of your profile's user ID and a random per-install value, so the server sees a stable ID that cannot
    be turned back into your user ID;
  - your online name, if you typed one in the settings (empty: shown as anonymous). The name of your game profile is never sent;
  - the launcher version, and `windows` or `linux`;
  - how many of the game's residents in your save have a home (read from the save file on your PC; the save itself is not sent).
- Who runs it: the Recompia maintainers. The server sees your IP address like any server does.
- Kept: the server shows you as offline 90 seconds after the last message. What the server stores beyond that is not part of this
  repository; ask at the contact address above, giving your online ID if you want something removed.
- Turn it off: Home chip, Settings > Game, or the game's Online tab. Reset my online ID makes a new ID that is not tied to the old one.

## Mod list and mod downloads (the mod hub)

- What it is for: the Browse tab of the Mods page, mod updates and the Achievements page.
- Who runs it: the mod hub is a third-party service (`pokopia-native.vercel.app`, downloads from `downloads.pokopia-native.workers.dev`),
  not the Recompia maintainers. The launcher also lists a few mods the maintainers added, which download from their authors' GitHub
  release pages (`github.com`, from the file `hub-additions.json` in the launcher).
- When: the launcher asks the hub for its mod list when you open the Mods or Achievements page, when the launcher has to install
  Community Achievements, and, only if you switched on automatic mod updates (off by default), once per launcher start. A mod is
  downloaded only when you install or update it.
- What is sent: an ordinary HTTPS request with the launcher name and version. No identifier from you. The hub sees your IP address.
- Kept: the launcher keeps the last list and the pictures on your PC so the pages work offline.
- Turn it off: do not open those pages. Automatic mod updates are off unless you switch them on in the launcher's settings.

## Community Achievements

Community Achievements is a mod the launcher keeps installed and switched on, and its sharing is on by default. This is a choice of
the project, so it is described here in full.

- What the mod does on your PC: it reads your save to count things (collection entries, habitats, recipes and so on) and shows an
  achievements page and messages. It only reads.
- What it sends, with sharing on: a random player ID made on your PC and kept in the mod's folder (not tied to your name, an account or
  your save), the game version, and the list of achievement IDs you have earned. At most one report a minute, when something is new
  and once per session. It never sends your save, your player name or anything from your PC. The mod's own documentation
  (its `DESIGN.md` in the Modtopia source) says the same.
- Who runs the counting service: a third party, a Cloudflare Worker at `achievements.pokopia-native.workers.dev` (not run by the
  Recompia maintainers). It sees your IP address while handling the request.
- What the service keeps (its source, `service/worker.js` and `schema.sql` in the mod): the random player ID, the day of its first
  report, the time of its last report, the game version, the achievement IDs reported with the day, and counts. The service's code
  keeps no IP address and no name; Cloudflare's rate limiter counts requests per address for a minute and stores nothing. The code
  has no expiry for these records. They are not linked to you: deleting the mod's `player.id` file makes a new random ID on the next
  report, and the old one is no longer used.
- The launcher also fetches the public counts (`GET /api/stats`) and the achievement pictures from the hub to fill the Achievements
  page. That request sends nothing of yours.
- Turn it off: the launcher keeps the mod and its sharing locked on until you unlock them. On the Mods page (or the Achievements
  page) press Unlock sharing, confirm, then switch Share my achievements off in the mod's settings. Your achievements are then
  still recorded on your PC, and no longer counted. Unlock the mod itself to switch it off or remove it. The in-game Mods tab shows the
  same locks. Locking again switches sharing back on.

## DLSS download (NVIDIA)

- What it is for: the optional NVIDIA DLSS upscaler, whose file cannot be shipped with the release.
- When: only when you press the download button (Windows).
- What is sent: an HTTPS request to `raw.githubusercontent.com` for one file in NVIDIA's own GitHub repository at a fixed version
  (`fetch_dlss.py`). The file is checked against a fixed checksum. GitHub sees your IP address.
- Turn it off: do not press the button.

## Linux and macOS set-up tools

On Linux and macOS, Set up downloads build tools when they are missing: Python from the python-build-standalone project (GitHub), two
Python packages (from PyPI, `files.pythonhosted.org`) and, on Linux, the Zig compiler (`ziglang.org`). Each download is checked against
a fixed checksum (`host.rs`). Windows packages bundle them and download nothing.

## Discord status

- What it is for: showing "Playing Recompia" and the time since the game started on your Discord profile.
- When: off by default. You switch it on in the launcher's settings.
- How: the launcher talks to the Discord app on your own PC through its local connection (`presence.rs`). The launcher itself sends
  nothing to a server. Discord then shows the status according to its own terms and your Discord settings.
- Turn it off: the launcher's settings.

## Mods that use the network

A mod is code that runs inside the game. A mod can ask the game for HTTPS requests, but only to the hosts it names in its own
`mod.toml`; the mod list shows them ("Goes online") and the log records them at start. The request has no cookies, no sign-in, no
redirects. This is a promise of the mod loader, not a sandbox: a mod is native code you choose to install, so only install mods you
trust. Community Achievements is the only mod in the project's own set that names a host. A mod you install may talk to a server
that is not described here; read its page. Switching the mod off stops it.

## The game itself

- The game is told that it has an internet connection so events that need one can start (`online_events`, on by default). Nothing is
  sent: the game's own online services still fail, and the clock is not changed.
- The game and the launcher keep your saves, settings, profiles, shader caches and logs on your PC. The launcher's own settings are in
  its settings folder.

## Your choices and contact

Every setting named above is in the launcher (Home, Settings, Link Play and Mods pages) or in the game's own Settings. To ask what
is held about you on a server run by the Recompia maintainers, to ask for removal, or to report a privacy problem, write to
legal.recompia@gmail.com. For third-party servers (GitHub, the mod hub, Cloudflare, NVIDIA, Discord, ipify, icanhazip), contact them.

This page changes when the software does. Its history is in the repository.
