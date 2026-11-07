# Changelog

Parent project: [pokopia-native](https://github.com/xFractals/pokopia-native); each of its releases is reviewed and what applies is brought over
(last reviewed: pokopia-native 1.3.0, `4a72d23`). This fork builds on [the original Pokopia Recomp](https://github.com/drakolordx7/pokopia-recomp) 0.1.1. You still need your own copy
of the game (version 2.0.1a): the download contains no game files.

## Unreleased

**Update:** press **Update**, then **Finish update** once when it asks (a shader translation fix: only the 32,630 changed shaders of about 99,000 are translated again, not the whole cache).

### Highlights
- Fixed the dashed outline missing around the tiles a move like Rock Smash or Water Gun will hit (#242)
- Fixed a shader translation mistake in about a third of the game's shaders

### Graphics
- Fixed the dashed white outline missing around move target tiles and the block placement indicator; only the faint tint showed (#242, #120)
- Fixed one common shader instruction being translated wrongly in 32,630 of about 99,000 shaders
  - Other small details with the same cause (masks, patterns, dithering, effect switches) may look right now too; not all checked
  - Something looks worse after this update? Open an issue with a screenshot and `pkp.log`
- Until you run **Finish update**, the game keeps the old translations and the outline stays missing

| Before | After |
|---|---|
| ![Before: tint only](https://raw.githubusercontent.com/featureJosh/Recompia/evidence/aspect-16-10/docs/bugs/242-move-outline/before.png) | ![After: dashed outline](https://raw.githubusercontent.com/featureJosh/Recompia/evidence/aspect-16-10/docs/bugs/242-move-outline/after.png) |

### Link Play
- Still cross-plays with pokopia-native 1.3.3: its co-op wire format, discovery, room codes and relay are unchanged since 1.3.0
- Joining a VPN address (Radmin VPN 26.x, Hamachi 25.x, Tailscale / carrier NAT 100.64.x) on LAN now names the VPN in the refusal and says to turn on internet play for every player
- Co-op module updated to pokopia-native 1.3.3 (`035bd9c`); its Interact everywhere and Story co-op sites for game 1.0.5 come along but do nothing here (this project needs 2.0.1). Based on pokopia-native (MIT) `windows/src/coop` @035bd9c

### For developers
- `tools/shaders/ampere/frontend/AmpereEmit.cs` `EmitLop3Ampere`: read LOP3/ULOP3 operands after the predicate destination
  - The decoder (like nvdisasm) puts Pd before Rd (`LOP3.LUT P2, RZ, R22, 0x1, RZ, 0xc0, !PT`); the emitter read Rd first, so a, b, c and the LUT were one operand late and Pd stayed false
  - 112,542 such instructions in 32,630 corpus blocks; IADD3, LEA, IMAD and MATCH already handled predicate outputs; R2UR, FLO and LDG never carry one here
- `launcher/src-tauri/src/shaders.rs`: `TRANSLATOR_REV` 1 ⇒ 2
  - `stale_blocks` retranslates the 32,630 hashes in `rev2_blocks.txt` (SPIR-V differs before/after the fix over the full 2.0.1 corpus)
  - Blocks the game built at runtime (a few hundred, including the outline's lighting shader) are retranslated by `translate_missing`
  - Loose `.spv` files are replaced; packed `spirv.archive` gets a newer segment; the boot preload pack drops changed `results.tsv` entries
- The outline is drawn by deferred lighting fs `0cd3176a1c7791d8` (G-buffer neighbour test + world-space checker), not the sky/cloud draws `8010f484`/`86aa3b21`
  - The 0.6.x #120 note credited it to the depth range default (#137); that change affects clouds and stays, but never fixed the outline

## 0.7.8 (build K: Kudu)

Press **Update** in the launcher and it does the rest. The game code is unchanged, so you do not need to **Set up again**.

This update adds a **Mods** folder for community translations and texture mods, removes the resize grip that stuttered, and makes the README available in four more languages. If a mod does not load, please open an issue and attach `pkp.log` from the game folder: the log now warns when none of the mod's files replaces a game file.

~Jerban

### Known issues
- **Linux still runs the game and the shader setup through Proton.**
- **Mods that replace files containing shaders are not supported.** Mods that only change text or textures are the intended use.
- **The translated READMEs are machine translations** and may contain mistakes. The English README is the authoritative text.
- **Use the game clock in single player only for now.**

### New

#### A folder for text and texture mods
**Before:** using a translation or texture mod meant replacing files inside the game dump, and going back to the original meant keeping a second copy.

**Now:** **Settings > Game** has a **Mods** row, saved in `pkp.ini` as `[fs] romfs_mod`. Choose a folder, and its files replace the update and base game files that have the same path. Folders are merged, and files that exist only in the mod are served too. Leave the row empty to turn mods off. Adding, swapping or removing a mod does not change the shader cache key and never asks for **Finish update**. No mod is bundled with this project: get one from its author.

**Choosing the right folder:** the folder must contain the same files and folders as the game's own `romfs`, starting at its top level. If a download has an outer folder (for example `2.0.1/romfs/...`), choose the inner `romfs` folder, the one that contains `LD_C0_R.BIN`. If the level is wrong, `pkp.log` now says that none of the mod's files replaces a game file. The README shows an example, in English and in Portuguese (Brazil).

**Still not:** mods that replace files containing shaders are unsupported and need **Set up** to be run again by hand.

#### The README in Simplified Chinese, Spanish, Arabic and Hindi
**Before:** the README was English only.

**Now:** the README and the Linux package's `README.txt` have machine translations in Simplified Chinese, Spanish, Arabic and Hindi, so more people can follow the setup and share their own translation mods. They are in `docs/i18n/`, and the Linux download includes them as `README.<language>.txt`. A pull request check keeps their structure in line with the English file, and `tools/i18n/README.md` explains how to refresh or add a language.

**Still not:** no native speaker has reviewed these translations yet. Corrections are welcome as pull requests.

#### Shader files take about a fifth of the space (prototype, not measured on a full cache yet)
**Before:** the translated shaders were ~99,000 separate files, about 26 GB, for a 12 GB game. Setup needed about 36 GB free.

**Now:** they can be packed into a few compressed files in `shader_cache/spirv.archive`. The game reads them as quickly as loose files,
so it runs the same, and nothing is translated again to switch. When the launcher finds loose shader files after a finished setup, it
asks once per start: **Free up about N GB?** (a few minutes, can be stopped and continued). **Settings > Game > Shader files** has the
same button, and **Use loose files** to go back. A new setup packs as it translates (on by default, **Packed shader files** on the
Set up page), so the loose files never pile up: a new setup now needs about 14 GB free instead of about 31 GB (36 GB was
quoted before), and the free-space check and the Set up page follow the toggle. The launcher writes the real sizes and times to
`setup.log`.
Loose shader files are on their way out: a later release stops reading them.

#### A Nearest upscaler, and the other upscalers are switched off for now (#232)
**Before:** the upscaler list offered FSR 1, FSR 3, DLSS, DLAA, NIS and XeSS. Several of them looked glitchy or no different from Off, and there was no way to keep a sharp pixel look.

**Now:** there is a new **Nearest** upscaler (sharp pixels, no smoothing), next to Off and FSR 1, in the launcher and on the F1 page. FSR 3, DLSS, DLAA, NIS and XeSS are switched off until they are reworked: if your settings use one of them, the game runs with Off instead. Their code stays in place.

If Nearest or the switched-off modes behave oddly, please open an issue.

### Fixes

#### Link Play kept old settings between sessions (#239)
**Before:** a `mode = join:<address>` left in `pkp.ini` from an earlier visit made the game visit-only, so a room you hosted could not be reached, and with a relay set the game switched Link Play off.

**Now:** the launcher puts the Link Play settings back to their defaults when it starts (Off stays Off). The Link Play page still remembers your addresses and room codes, so your last setup is one click away. The page no longer fills in this PC's own relay address while that relay is not running. The log warns when a join address is set, and internet play through a relay uses `auto` even if an old join address is left in the file. Starting the game from Steam with `--play` keeps your last saved settings.

#### Link Play now shows the address to share, and refuses your own PC's address (#245)
**Before:** the Link Play page only listed this PC's local network and VPN addresses, so a host over the internet shared the `127.0.0.1` from the relay field. On a friend's PC that address means the friend's own PC, so the friend could not find the room.

**Now:** when you host over the internet, the first row under "Share with visitors" is **Over the internet**: this PC's public address with a copy button, with the right port for your relay or for direct hosting. A visitor who types a `127.x` address is told it is their own PC. To find the public address the launcher makes one small request to a what-is-my-IP service (`api.ipify.org`, with `ipv4.icanhazip.com` as a backup), only while the page shows an internet host.

**Still not:** on a connection where the provider shares one public address between many customers, the address found is the provider's, and port forwarding cannot work there. The page cannot detect that.

#### A controller switched off and on again was not found on Linux (#226)
**Before:** on SteamOS and other Proton setups, a controller that was turned off and on again was not seen until the game was restarted.

**Now:** under Wine and Proton the game checks the four controller slots once a second and rescans the controllers when they change. The log line `rescanning gamepads` shows when it happens.

**Still not:** a controller that Proton shows only as a generic device (no controller slot) still needs a restart.

#### Photos were blank in the in-game album with HDR on (#229)
**Before:** with HDR on, photos did not show in the in-game album or main menu, while "Save to console album" still wrote a correct picture.

**Now:** the game's pictures that the CPU writes into HDR render targets are converted and uploaded instead of dropped.

If photos are still blank with HDR on, please open an issue and attach `pkp.log`.

#### DLC inside a .nup is unpacked (#236)
**Before:** the .nup unpacker skipped add-on packages, so DLC never reached the add-on folder and had to be built by hand.

**Now:** add-ons with a RomFS are unpacked into `nup_dump/aoc/<id>/romfs/`, which the launcher already finds. An earlier finished unpack gets its add-ons the next time you pick the file.

#### The bottom-right resize grip stuttered while dragging
**Before:** dragging the resize grip in the bottom right corner of the launcher stuttered.

**Now:** the grip is gone. The window edges and corners resize the window as before, without the stutter.

## 0.7.7 (build K: Klipspringer)

Press **Update** in the launcher and it does the rest. The game code is unchanged, so you do not need to **Set up again**.

Sorry for all the updates lately. I look after the Linux side, and now that Josh's one-click updater is in, I want to squash as many Linux bugs as I can and get the fixes out to you right away, instead of saving them up. Thanks a lot for your patience.

If you play on Linux, SteamOS or a Linux handheld, please keep opening issues for any bug you run into there, and always attach your logs: `pkp.log` from the game folder, and `setup.log` too if it is about Set up (the README says where they are, under "Where your saves, logs and settings are"). Without the logs I usually cannot tell what went wrong.

~Jerban

### Known issues
- **Linux still runs the game and the shader setup through Proton.**
- **Use the game clock in single player only for now.** In single player the sky and the in-game time follow it.
- **Dragging the resize grip in the bottom right corner can stutter** while the window changes size. I am looking into it.

### New

#### Set the game's date and time from the launcher (#203)
**Before:** the game's clock always followed the PC, so reaching time-gated things in the game meant changing the system clock.

**Now:** Settings > Game has a **Game clock** row: pick a date and time, or press **Real time** to follow the PC again. It is saved in `pkp.ini` as `[time] clock_offset` (minutes, up to 10 years either way) and used from the next start of the game. The clock the game reads for the player moves; the steady and network clocks stay on the PC's time, as when you set the date and time by hand on a Switch.

**Still not:** it only changes at the start of the game, not while it runs. Use it in single player only for now (the launcher says so under the row).

#### Reach the F1 menu with a controller
**Before:** the in-game settings page opened only with the F1 key and could only be used with a mouse or a touch screen, so on a handheld like the ROG Ally, or on a PC with a controller and no touch screen, you needed a keyboard.

**Now:** the "Close the game?" prompt (View and Menu together, the Switch's minus and plus) has a third choice, **Settings**: press X or tap it, and the F1 page opens. Inside the page the controller works: D-pad or left stick move, A chooses, B goes back, and plus closes it. The game gets no button presses while the page is open. The touch screen and the mouse work as before.

**Still not:** the highlight appears on your first D-pad press, not when the page opens.

### Fixes

#### The ROG Ally was not treated as a handheld
**Before:** the game only knew the Steam Deck and the Legion Go S as handhelds. On a ROG Ally it ran in the Switch's docked mode, sized its picture like a desktop PC and showed keyboard prompts by default.

**Now:** the ROG Ally and Ally X count as handhelds, like the Deck: handheld mode, a picture sized for the screen, the lighter sleep under Proton and controller prompts. The game log says `handheld: yes` with the board name. `[host] handheld = off` in `pkp.ini` turns it off again if you prefer the old behaviour.

#### The launcher was slow on Linux
**Before:** on Linux the launcher took about 0.7 seconds from a click to the next page and scrolled at about 3 frames per second, because the big background picture was drawn again, and blurred again, on every frame.

**Now:** the background is two ready-made pictures (the sharp one and a blurred one) made from the same artwork, so it looks the same. On my PC a page change takes about 55 ms and scrolling runs at about 60 frames per second. `launcher/scripts/render-art.sh` rebuilds the pictures from the SVG if the artwork changes.

#### The launcher did not fit handheld screens and was hard to use by touch
**Before:** on a 1920x1080 screen at 185 % scale (a ROG Ally on the KDE desktop) the window opened taller than the screen, so the bottom went under the taskbar. The window had no title bar, so the only way to resize it was the invisible edge of the window, which a finger cannot hit, and the window buttons were 32 pixels wide.

**Now:** on a small screen (up to 1400 pixels wide or under 800 high, as the screen counts for the launcher) the launcher starts maximized, so the taskbar stays clear. If the screen query ever fails, the launcher just opens at its normal size. On other screens it now opens a bit smaller, at 1040x600 instead of 1120x720, and it remembers the size (and, outside Wayland, the place) you leave it at. The smallest window height is 440 instead of 580, and every page works at 1038x540. Where the main pointer is a finger, buttons, rows, inputs and the window buttons are 44 to 48 pixels. There is a resize grip in the bottom right corner, and the title bar can be dragged with a finger. With a mouse nothing changes except the grip.

#### The Xbox and PlayStation d-pad, trigger and shoulder icons faded out on the move wheel (#230)
**Before:** with Xbox or PlayStation prompts, the icons for the d-pad, the triggers and shoulders, and the menu buttons are plain white art. The game's own button symbols have a dark rim, and so do our keyboard icons, but these did not, so on the pale Select Move wheel (and on white dialogue boxes) they nearly vanished. The d-pad was the worst: a white cross on a pale wheel.

**Now:** those icons get the same dark rim and dark letters as the keyboard icons. The coloured face buttons (A, B, X, Y, cross, circle and so on) are left exactly as they were.

**Still not:** I could only check the icons on their own, on a pale and a dark background, not inside the game. If any prompt still looks faint for you, send a screenshot of it.

## 0.7.6 (build K: Kinkajou)

Press **Update** in the launcher and it does the rest. The game code is unchanged, so you do not need to **Set up again**.

This one is a small fix for people who keep the game in a different folder than the launcher.

### Highlights
- **The game starts again when your game folder is not the launcher's folder.** It used to close the moment you pressed Play, with "Stopped abnormally, code 53".

### Known issues
- **Linux still runs the game and the shader setup through Proton.** A game that runs on Linux without it is something we are working on, but it is not in this release.
- **The launcher can feel slow on Linux** (page changes lag, scrolling stutters). We found the reason, it is the big background picture that gets redrawn all the time, and a fix is ready for testing. It is not in this release.
- **If Update does not work for you on Linux,** extract the new release over your old one like before and tell us what happened.
- If Link Play with pokopia-native players does not work for you, please tell us what you find.

### Fixes

#### The game closes right away with exit code 53 when the game folder is not the launcher's folder (#221)
**Before:** if you picked a game folder other than the one the launcher sits in (common with a games folder of your own on Linux), Set up built `pkp.exe` there but `pkp_rt.dll` and the ASTC decoder DLLs stayed in the launcher's folder. `pkp.exe` looks for them next to itself, so it stopped before it could write a log. Proton showed that as "code 53 (0x00000035)", which tells you nothing. The same thing made the first background run quit after zero seconds.

**Now:** Play and that first background run copy the DLLs into your game folder whenever they are missing or different, so there is nothing for you to do. If `pkp_rt.dll` is missing in both places, Home tells you what is missing and offers the game folder setting, instead of letting the game stop with a number.

**Still not:** if the launcher's own folder has lost `pkp_rt.dll` and your game folder has no copy either, you need to extract the release again.

## 0.7.5 (build K: Koala)

**This is the last update you ever install by hand.** Extract it over your 0.7.0 install one more time, and from the next
release on, updating is one click: press **Update** in the launcher and it does the rest. The game code is unchanged, so no
**Set up again** is needed.

![The new launcher](https://raw.githubusercontent.com/featureJosh/pokopia-recomp/evidence/aspect-16-10/docs/launcher-rework/1-home.png)

### Highlights
- **One-click updates.** No more downloading zips, no more extracting over your install. The launcher fetches the new release,
  checks it against the release's checksums, swaps its files, restarts, and starts setup by itself if the new version needs it.
  Your saves, settings, shader cache and built game are never touched, and if anything goes wrong every file is put back.
- **A brand new launcher.** One big button on Home that always does the next right thing (Choose game files, Set up, Play),
  patch notes right inside the launcher, a crash card with the report and log, and settings that save as you change them.
- **More settings without opening an ini file:** quality preset, view distance, upscaler and its quality, frame rate counter,
  mute when not focused, keep 16:9.
- **Steam Deck and Proton find Link Play hosts on the LAN** (and are found by them), with nothing to type.
- **F1 "Just under the refresh rate" no longer slows the game** on 60 and 90 Hz screens.
- **A typo in `controls.ini` no longer wipes all your controls**, and pad combos take the Switch button names (`l+r`).

### New

#### A new launcher: one button, updates in one click, patch notes inside
**Before:** five pages (Play, Game files, Link Play, Settings, About), a long settings form with a Save button and hint text, and an "Update available" banner that sent you to GitHub to download and extract the release yourself. Release notes were only on GitHub.

**Now:** four pages: Home, Link Play, What's New, Settings, in a frameless glass window. Home has one button that always does the next step (Choose game files, Set up, Continue setup, Finish update, Play) and uses the same check as Play, so it never offers Play when Play would refuse. **Update** downloads the new release, checks it against the release's checksums, replaces the launcher's files, restarts, and starts setup by itself when the new version needs it; saves, `pkp.ini`, `controls.ini`, the shader cache and the built game are never touched, and a failed update puts every file back. What's New reads the release notes in the launcher, and Home shows the newest three. A crash shows a card on Home with its report and the game log. Setup and updates keep running when you change pages.

Settings are tabs (Display, Graphics, Audio, Controls, Saves, Game, About) that save on change, with choices instead of numbers (frame rate "Match display, 60, 120, Unlimited", render resolution "Match window, 1080p..."; Custom keeps any other value). New in the launcher: quality preset (writes the whole preset like the F1 menu), view distance, upscaler and its quality, frame rate counter, mute when not focused, keep 16:9. DLSS moved to Graphics. Link Play is one switch (Off, Same network, Internet) with every field applied by itself; the `pkp.ini` keys are unchanged.

Removed: the shader cache path (it never worked), the RomFS folder (it follows the game files), the Save/Revert bar, the old Link Play protocol choice (a notice offers to switch).

**Still not:** this update itself is installed by hand (0.7.0's launcher cannot install updates); one-click updates start with the next one.

### Fixes

#### Launcher volume could be undone by pkp.ini
**Before:** a `volume` line in `pkp.ini` beat `controls.ini`, so the volume shown was not always what played.

**Now:** the launcher also writes `pkp.ini` when it already holds the line.

#### F1 "Just under the refresh rate" no longer slows the game on 60 and 90 Hz screens
**Before:** the button set the frame limit 3 fps under any refresh rate: 57 on a 60 Hz screen, 87 at 90 Hz. At 57 the game counts every frame as 1/60 s, so it ran about 5 % slow and felt sluggish; on a screen without G-Sync / FreeSync (Steam Deck) the lower rate also showed frames unevenly.

**Now:** below 100 Hz the button sets the refresh rate itself (60, 90). From 100 Hz up it still keeps a few fps under (116 at 120 Hz, 138 at 144 Hz), which is what G-Sync / FreeSync need. The button says it is for G-Sync / FreeSync screens, with a tip. Nothing changes unless you press it.

#### Link Play: Steam Deck and Proton find hosts and are found on the LAN
**Before:** Proton has no DNS-SD (`dnsapi.dll` without `DnsService*`), so a host under Proton was invisible and a visitor under Proton found nothing (`DNS-SD needs Windows 10 1809 or later ... join by address`); every visit needed the host's address. Proton also showed a wrong "this PC's network is set to Public" note.

**Now:** where DNS-SD is missing, each scan sweeps this device's own private network (up to 254 addresses per network, 512 in all, TCP port 47600, about 300 ms) and asks every host that answers for its game over the co-op link, as for a host DNS-SD found. A hosting device answers DNS-SD queries itself (a small mDNS responder on UDP 5353 with the records pokopia-native registers), so Windows players see it. Nothing to type, and nothing to change on another network. The Public-network note is not shown under Wine. Windows with DNS-SD is unchanged. `[net] lan_sweep = off` turns it off; `NXRT_COOP_NO_DNSSD=1` tries it on Windows (a diagnostic).

**Still not:** Wi-Fi that keeps devices apart (cafes, hotels, guest networks; use internet play with a relay), and a host that fell back to another port than 47600.

#### One bad line in controls.ini no longer resets every setting (#218)
**Before:** pad combos (`[quit] pad`, `[overlay] pad`) only took position names like `left_shoulder+right_shoulder`, so `btn_l+btn_r` was an error. Any bad value in a fixed key made the whole `controls.ini` invalid: factory settings loaded, and the next save wrote them over your file. It looked like the setting reset to `none` on every start.

**Now:** combos also take the Switch names of the buttons that sit in the same place on every pad, with or without `btn_`: `l r zl zr plus minus lstick rstick` (the file is written back with the position names). A bad value only resets that one key and is logged in `pkp.log`, together with unknown keys. If a file still cannot be loaded, it is copied to `controls.ini.bad` before anything can save over it.

## 0.7.0 (build K: Kangaroo)

Extract it over your 0.6.x install, then run **Set up again** once: the game code and the shader translator changed. The launcher
asks for it. Link Play builds from before this one cannot join this one, so update together.

### Highlights
- Link Play now speaks pokopia-native's protocol, so you can play with pokopia-native players on game 2.0.1.
- Text on 21:9 and 32:9 screens no longer runs together.
- House and area transitions take the same time at any frame rate, with no 60 fps hold.
- Fewer GPU freezes on NVIDIA, GPU counters that count once, and particle effects that were missing.
- Shader setup writes about 90 files instead of 361,000, which could take hours on a hard drive.
- More graphics memory for 4K, and a smaller default picture size on GPUs with 4 GB or less.

### Known issues
- If Link Play cross-play with pokopia-native does not work for you, please report what you find.

### New

#### Link Play: cross-play with pokopia-native
**Before:** Link Play only worked between builds of this port (our own relay protocol on port 39660), so nobody could play with pokopia-native players.

**Now:** the default `[net] protocol = native` speaks pokopia-native's co-op protocol (their module, vendored in `runtime/third_party/pokopia-coop`), so this port and pokopia-native builds of game 2.0.1 meet in one session. A host listens on 47600 (TCP and UDP) and is found on the LAN by itself (DNS-SD); `[net] mode = join:<IPv4>[:<port>]` joins one host by address; `[net] internet = true` with `room_code` plays over the internet, directly or through a relay on port 47610. The launcher's Link Play page was reworked for it, and `PokopiaLauncher --host` is now a port of pokopia-native's coop relay (pokopia-native's own relay works too). New `[pkp] link_story_coop` (off by default, needs `link_interact_everywhere`, which it turns on) lets visitors talk to the story characters; every player of a session needs the same setting. Between two builds of this port, a launcher relay also carries the game's packets over UDP (a small extension that stays off, and leaves the wire unchanged, when the host, the visitor or the relay does not have it).

**Lost:** Link Play builds from before this one cannot join (set `protocol = linkplay` while everyone still has an old build), the relay hosted inside the game and the broadcast discovery on port 39660 are not used with the new protocol, and on Steam Deck / Proton the host may not be found by itself (use Visit by address). Details: `docs/multiplayer/README.md`; the README guide and the launcher's firewall hint name the new ports. Based on pokopia-native (MIT) `windows/src/coop` and `windows/tools/coop-relay` @4a72d23.

#### Shaders translated while the game runs are used without a restart
**Before:** a shader the game builds while it runs (the romfs cache cannot have it) stayed missing until the next start, even once it
was translated: what used it was not drawn for the rest of the session.

**Now:** the runtime checks `results.tsv` about once a second. When rows are added, the shaders they translate are loaded at the next
bind of the programs that lacked them. Something that translates `[nvn] dump_missing` while the game runs fills them in mid-session.
`[nvn] shader_rescan_ms` sets how often (0 = off). Thanks to @Jeffrharr.

#### Controller shortcut for the F1 menu
**Before:** the F1 menu opened only from the keyboard, so a handheld with a touch screen had no way to open it from the controller.

**Now:** `[overlay] pad` in `controls.ini` takes a button combination (like `[quit] pad`, for example `left_shoulder+right_shoulder`) that opens and closes the F1 menu. It is off (`none`) by default. A combination equal to the quit one is ignored with a log warning. Based on pokopia-native (MIT) `windows/src/nxrt/front/menu.c` @e5d9ac6.

#### Small GPUs: the default picture size stops at 2560x1440
**Before:** on a 4K screen the picture size you had not set followed the screen (3840x2160), which can run out of video memory on a GPU with 4 GB or less. Possibly related to #164 (hang at 4K while Ditto loads).

**Now:** on a GPU with 4 GB of video memory or less, a picture size you have not set is kept to 2560x1440. A set `render_width` / `render_height` or `render_scale` is never changed. `[display] vram_cap = 0` brings back the old behaviour. Based on pokopia-native (MIT) `windows/src/nxrt/vk/vk_dev.c` @e5d9ac6.

### Fixes

#### 4K: more graphics memory for the first world load (#119, #164)
**What was wrong:** at a 3840x2160 render size the game could stay on the Ditto jumping loading screen for good, or crash just after
the first world frame. The game's graphics pools were sized from what start-up uses, not what the first world load needs.

**Now:** both pools grow above 1080p (at 4K: 333 MiB more general memory and 595 MiB more for render targets). 1080p and below are
unchanged. `pkp.log`, the freeze dump and the crash report show how full each pool gets, so a report can tell which one ran out.

#### The shader warm-up no longer gives up after one empty run
**What was wrong:** the hidden first run collects the blocks the game builds as it starts. When it collected none, an empty
`shader_cache/runtime/manifest.tsv` was written, so the warm-up never ran again.

**Now:** an empty run does not mark the warm-up as done; the next Play or Set up tries again, up to two more times. Thanks to @jerbanpls.

#### Particle effects whose count the GPU works out are drawn
**What was wrong:** some effects (for example the one in the intro cave after you choose your look) were not drawn. The game counts
their quads on the GPU and draws them indirectly; the runtime converted quad draws on the CPU, could not read a count the GPU had
not written yet, and skipped them.

**Now:** a small compute shader turns those counts into an ordinary indexed draw on the GPU, at most 65536 quads and instances.
`[nvn] indirect_quads_gpu = false` brings back the old behaviour. Based on pokopia-native 1.2.6.

#### Shaders: GPU counters no longer count twice
**What was wrong:** the shader translator ran an atomic instruction twice when the shader read its result once: every counter a
compute shader keeps with an atomic add (64 of the game's shaders, 115 adds) added its amount twice, and the helper that hands the
result to the other lanes of a warp ran twice. On every GPU.

**Now:** each atomic runs once, as on the console (checked against the game's own instructions: 154 atomics in those shaders, 154
in the new translation). Run **Set up again** once after updating: the launcher asks for it, and it translates only the shaders
with atomics again (the others are kept).

#### NVIDIA: fewer GPU freezes in compute shaders
**What was wrong:** on NVIDIA GPUs (RTX 50 confirmed in pokopia-native, an RTX 3050 Laptop inferred) a compute shader that counts
with one atomic add per warp (`ballot(true)`, the highest lane adds for all) after some of its lanes have returned can stop the GPU
for good: the game freezes and Windows resets the driver ("device lost").

**Now:** on NVIDIA those counters are made per lane: every lane adds its own share and takes a slot of its own, so the warp never
waits on the lanes that returned. 30 of the game's shaders (65 counters, 132 ballots) are changed; shaders with a counter the
runtime cannot follow whole are left as translated. `[nvn] ballot_fix = auto` (NVIDIA only, the default), `on` (any GPU),
`off`. Approach from pokopia-native (MIT) `windows/docs/ballot-hang.md`, `windows/src/nxrt/vk/vk_shader.c` @4a72d23.

#### Wide screens: text no longer squeezed (#176)
**What was wrong:** on displays wider than 16:9 (21:9, 32:9) the letters in text boxes ran into each other and words merged
("Ver. 2.0.1" read as "Ver201"), and fitted text such as button labels was squashed.

**Now:** text has the same letter spacing as at 16:9. 16:9 and taller shapes are unchanged.

#### Area transitions: the iris no longer crawls or freezes above 120 fps (#144)
**What was wrong:** above about 120 fps the house-shaped wipe that closes and opens when you enter or leave a house or area
could crawl for a minute or look frozen, both on the way in and on the way out.

**Now:** the transition takes the same time as at 60 fps at any frame rate. This replaces 0.6.1's 60 fps hold: cutscenes, dialogue
and menus follow your frame limit again, and `[pkp] transition_hold` is gone.

#### Shader setup: the scan writes a few large files instead of 361,000 small ones (#194)
**What was wrong:** the shader scan wrote every shader block as its own file: about 361,000 files and 9 GB in
`shader_cache\corpus`, almost all of them from one 2.4 GB game file that is always the last one scanned. On a hard drive, and
with the virus scanner checking each new file, that can take hours, and the cleanup at the end of setup deleted them all again.
setup.log showed only the count, not which file the scan was on.

**Now:** the scan writes the blocks into a few large pack files (about 90 files, 5 GB), and only the part of each one the
translator reads. On Linux the whole scan takes 38 s instead of about 2.5 minutes. setup.log names each game file as it
finishes and, after a minute without progress, the files still being scanned. The scan uses at most four workers, and the
largest files go first. A shader corpus extracted by an older version is still translated as before. Thanks to @jerbanpls.

### Since 0.6.0
Already in 0.6.1 and 0.6.2, with details in their sections below:
- 0.6.1: a native Linux launcher and download (#52), Steam's keyboard for name entry, `PokopiaLauncher --play`, correct shader results
  on AMD GPUs and the Steam Deck, handheld mode on handhelds, the in-game clock after sleep, and the Proton "graphics device was
  lost" fix (#169).
- 0.6.2: DLC support, and a smaller Linux download that starts on more systems.

## 0.6.2 (build J: Jackrabbit)

Extract it over your 0.6.1 install: the game code is unchanged, so no **Set up again** is needed.

### New

#### DLC (add-on content)
The game can now see the add-on content you own. Put each add-on, already decrypted, in the dump folder as
`aoc/<add-on id>/romfs/` (for the Bubbly Basin key: `aoc/0400A3A01EE4D001/romfs/`). The launcher finds it, shows it on the
Game files page and sets `[fs] aoc_dir` in `pkp.ini`, and the game lists the add-on and opens its data. Like the game itself,
the add-ons must come from your own dump: the download contains none.

### Fixes

#### The Linux download starts on more systems, and is smaller
**What was wrong:** the Linux launcher needed glibc 2.39, so it did not start on Ubuntu 22.04, Debian 12, Mint 21 or Pop!_OS 22.04,
and its AppImage needed FUSE 2, which Ubuntu 24.04 and others no longer install. The download also carried the Windows-only
Python, Zig and start script (about 330 MB unpacked), and its README.txt described Windows.

**Now:** the launcher needs glibc 2.35 and no FUSE 2, the Windows-only files are left out, and README.txt explains the Linux steps.

### For developers
- `.github/workflows/launcher-checks.yml`: on pull requests and pushes to main that touch `launcher/` or `game-build/release_b/`,
  builds the launcher front end, runs `cargo check` (native and `x86_64-pc-windows-gnu`) and `cargo clippy`, and checks
  `pkp_build.py` compiles, with the cargo registry and target cached.

## 0.6.1 (build J: Jackal)

Hotfix for 0.6.0. Extract it over your 0.6.0 install: the game code is unchanged, so no **Set up again** is needed.

### New

#### The launcher runs on Linux (#52)
The launcher builds as a native Linux program (and as an AppImage), so it no longer needs Microsoft's WebView2, which Proton
does not have. On Linux it fetches its build tools on the first Set up, compiles pkp.exe there, and runs the shader translator
and the game through Steam's Proton (in the Steam Linux Runtime, as Steam starts games), umu-run or Wine. Set up, Play, Link
Play and the save import work there; see `launcher/README.md`. Thanks to @jerbanpls.

**Download for Linux:** `PokopiaRecomp-0.6.1-linux-x64.tar.gz` is the same package with `PokopiaLauncher.AppImage` in place of
the Windows launcher. Install Proton Experimental in Steam, extract it anywhere and run the AppImage; see "Linux, SteamOS and the
Steam Deck" in the README.

#### Name entry with Steam's keyboard (Steam Deck, SteamOS)
When the game asks for a name under Proton on a Steam Deck, on SteamOS or in Steam's gamepad UI, Steam's own on-screen keyboard
comes up with the question, and its Enter answers. Press any button to get the in-game keys instead; Steam + X brings Steam's back.
`[applet] steam_keyboard = off` turns it off. Thanks to @tinfoil-chewer.

#### Start the game straight from a shortcut
`PokopiaLauncher --play` starts the game the way the Play button does, without the launcher window, and waits for it: for a
Steam shortcut, Sunshine/Moonlight streaming or a desktop icon.

### Bug fixes

#### AMD GPUs (Steam Deck included): shaders computed wrong results
**What was wrong:** the game's shaders were written for NVIDIA's 32-wide subgroups; AMD runs 64 wide by default, so shaders that
shuffle, ballot or vote across a subgroup gave wrong results. Depth views in a 24-bit format were refused on AMD (it has no 24-bit
depth), so fog and soft particles that read the depth buffer covered the ground in white on the Steam Deck.

**Now:** game pipelines ask for 32-wide subgroups where the driver allows it (`[nvn] subgroup32`), and those depth views read the
32-bit depth the image really has. NVIDIA is unchanged. Thanks to @tinfoil-chewer.

#### Handhelds run in handheld mode
On a Steam Deck or Legion Go S the game is now told it is in handheld mode, as on the console's own screen, and picks its own
lighter graphics settings: a Legion Go S went from 50-57 to 62-69 fps. Under Proton on a handheld, the game's very short waits
for the GPU now sleep instead of spinning a CPU core, which leaves more power to the GPU (Steam Deck LCD: 52 to 54 fps, 1% lows
~27 to ~30). `[am] operation_mode = docked` and `[kernel] short_sleep = yield` bring the old behaviour back. Windows desktops are
unchanged. Thanks to @tinfoil-chewer.

#### Leaving an area above 60 fps: the Poké Ball wipe crawled or seemed stuck (#144)
**What was wrong:** 0.6.0's fix was not enough. Above 60 fps the wipe that opens after leaving a house crawled: about 15 seconds at
180 fps, and at higher frame rates it looked stuck until the frame limit was set to 60. A part of the transition breaks on the shorter
frames the game gets above 60 fps.

**Now:** whenever you can't move the camera (an area transition and its load, but also cutscenes, dialogue and menus) the game is held
at 60 fps, as on the console; back in free play your frame limit applies again. `[pkp] transition_hold = false` turns it off.

#### "The graphics device was lost" a few seconds after launch under Proton (#169)
**What was wrong:** on SteamOS / Proton (Steam Deck, Legion Go S) the game showed a black screen with the menu music playing, then
closed with "The graphics device was lost", unless pkp.ini had `tex_write_watch = false`. Every Proton version failed the same way.

**Now:** the game starts without the ini line. `[nvn] tex_write_watch` is `auto` (off under Proton, on under Windows).

#### The in-game clock fell behind after the handheld slept (#172)
**What was wrong:** put a Steam Deck or Legion Go S to sleep with the game open, wake it, and the in-game clock was behind by the
whole nap until you restarted the game: under Proton the counter the game's clock runs on stops during sleep.

**Now:** a few seconds after waking, the in-game clock catches up with the device's clock. `[time] resync = false` turns it off.
Thanks to @Exon0t for finding and measuring this on a Legion Go S.

## 0.6.0 (build J: Jaguar)

### Highlights
- The 3D world is drawn in the right place again below 100% scene size on Steam Deck, Legion Go S, 1440p and ultrawide screens.
- No more getting stuck on the Poké Ball wipe when you leave an area above 60 fps. Frame limits above 60 and unlimited work again.
- Lamp lights no longer show a ghost copy at 16:10 or 1440p.
- The in-game clock shows your real time.
- New one-click presets: Steam Deck, Legion Go S, Laptop and Desktop PC.
- Steam Deck: smoother play, plus a batch of anti-stutter changes.

### Bug fixes

#### The world was drawn in the wrong spot below 100% scene size (#163)
**What was wrong:** with Scene size under 100% (FSR 3 / DLSS quality presets and dynamic resolution count too) on any screen that is
not exactly 1920x1080, the 3D world went into the wrong part of the screen. On 16:10 (Steam Deck, Legion Go S) the player and objects
stood at the wrong height, floors such as the planks by the Pokémon Center were missing and a blank band sat under the picture. At
1440p and wider, only a corner of the screen showed the world.

**Now:** the world fills the frame. Left = before, right = 0.6.0 (16:10, scene size 75%):

![16:10, before and after](https://raw.githubusercontent.com/featureJosh/pokopia-recomp/evidence/aspect-16-10/docs/bugs/scene-viewport-163/ab_1610_75.jpg)

1440p, scene size 75%:

![1440p, before and after](https://raw.githubusercontent.com/featureJosh/pokopia-recomp/evidence/aspect-16-10/docs/bugs/scene-viewport-163/ab_1440_75.jpg)

#### Stuck on the Poké Ball wipe when leaving an area (#144)
**What was wrong:** above 60 fps (or unlimited), leaving an area could leave the Poké Ball wipe half open with your controls locked.
**Now:** fixed. The stopgap that held everyone to 60 fps is gone, so frame limits above 60 and unlimited work normally again.

#### Ghost lights at 16:10, 1440p and other sizes (#44)
**What was wrong:** lamp light and other glows showed up shifted, or as a second copy, whenever the picture was not 1920x1080.
**Now:** lighting follows your render size.

#### Wrong time of day in the game (#120)
**What was wrong:** the in-game clock was off by however long your PC or handheld had been switched on (a whole day ahead on a
handheld left on for days).
**Now:** the game shows your real local time. Times saved by older versions count as coming from another clock, like a console whose
clock was reset.

#### Missing placement outline (#120)
The dashed outline when you place an item shows up again.

#### Crash on Pokédex pictures (NVIDIA) (#121)
Some Pokédex pictures crashed the game with a "GPU lost" error. Fixed.

#### Freeze at start with an incomplete game dump (#128)
Some update dumps are missing the base game's files, and the game froze at start ("exited with code 4"). If your dump also has the base
game, the launcher now combines the two for you; otherwise it tells you which files are missing and what to do.

#### Silent crash when Windows' page file is off (error 1455)
The game now tells you what is wrong and how to turn the page file back on, instead of closing without a word.

#### D-pad prompts all looked the same (#95)
With Xbox or PlayStation icons, "press up" and "press left" showed the same picture. Each direction now lights up its own arm.

#### Smaller fixes
- Setup checks that it can write to your game folder (Windows "Controlled folder access" and some antivirus block it) and says so
  plainly instead of failing halfway.
- A rare GPU hang caused by garbage draw data is prevented.
- If the GPU is lost anyway, the log records what it was doing, which makes bug reports much more useful.

### New

#### Device presets (F1 > Graphics)
One pick sets view distance, texture sharpness, render scale and frame limit:

| Preset | Render scale | Frame limit |
|---|---|---|
| Steam Deck | 75% | 40 fps |
| Legion Go S | 80% | 60 fps |
| Laptop | 75% | 60 fps |
| Desktop PC | auto | your screen's refresh rate |

Change any value and it becomes Custom. 40 and 45 fps are now in the frame limit list.

#### Scene size now works (#116)
The Scene size slider used to do nothing. Now FSR 1, NIS, FSR 3 and DLSS can draw the world smaller and scale it up, and the F1 page
tells you what is in effect.

#### F1 shows your real render resolution (#117)
The Display page shows the size actually being drawn, and what scales it up to your screen.

#### Link Play: Interact everywhere (experimental)
Outside Palette Town the game puts every Link Play player in Spectator Mode. A new toggle on the launcher's Link Play page lets you
interact in the story areas too. **Everyone in the room needs the same setting**: a player whose setting differs is refused with a
message. Off by default.

#### Steam Deck: smoother play
Shaders are no longer rebuilt in the background while you play. On a Steam Deck's 4 cores that work fought with the game itself.
Measured on a Steam Deck OLED with this change alone:

![Steam Deck fps before and after](https://raw.githubusercontent.com/featureJosh/pokopia-recomp/evidence/aspect-16-10/docs/release/0.6.0/deck-rebuilds-off-fps.png)

#### Steam Deck and Linux: less stutter (not yet measured on a Deck)
- No more hitch about once a second: reading a picture back from the GPU no longer pauses the game.
- Something new on screen no longer freezes the game while its shader builds: it pops in a frame or two later instead. This is on for
  every platform now.
- The game's memory is handed to the GPU in a cheaper way, and each frame is sent to the GPU in one go instead of two.

These should help, but we haven't measured them on a Deck yet. Tell us how it plays.

#### Steam Deck: "Cap FPS to 30" box (F1 > Display)
On the 90 Hz Deck OLED, a 40 fps limit runs at an even 30, because frame rates that divide the refresh rate look smoother. Pick 80 Hz in
Steam's Performance menu if you want a real 40. Ticked by default only on a Steam Deck; untick it to keep your limit as set.

### Known issues
- The Steam Deck changes under "less stutter" are not measured on a Deck yet.
- On some Windows PCs the slowest frames may be a little less smooth than in 0.5.0. The average frame rate is unchanged.
- Some players crash at 4K in the Ditto jumping scene (#119). Not fixed yet.
- The launcher doesn't start under Proton / Linux (it needs WebView2), so handheld players don't see the update banner.

### For tinkerers: switching things back
Every change above can be undone in `pkp.ini` (details in CONFIG.md): `[pkp] scene_viewport_fix = 0`, `[pkp] target_fix = off`,
`[nvn] gpl_upgrade = whole`, `[nvn] readback_async = false`, `[nvn] gpl_gate = time`, `[nvn] async_whole = false`, `[nvn] async_compute = false`,
`[nvn] compute_prewarm = false`, `[nvn] pool_copy = off`, `[nvn] one_submit = off`, `[display] fps_snap = false`. New:
`[pkp] link_interact_everywhere`, `[gfx] gpu_checkpoints`, `[fs] romfs_base`, `[fs] romfs_check`.

## 0.5.0 (build I: Iguana)

### What's new
- Multiplayer (Link Play) is part of every release now. A new Link Play page in the launcher: play alone, host on this PC or visit
  a friend. Hosting shows the address to give out (LAN, Tailscale or ZeroTier) with Copy buttons, warns when Windows treats your
  network as Public, and gives you a room code. In the game, pick Local connection. Each player needs their own save: a copied save
  can't join its original.
- Room codes: a room hosted with a code is hidden from everyone else and only players with the code can join. Rooms without a code
  work as before on a LAN. Every player and the host need this version (older ones are refused with a message).
- Game traffic can travel over UDP to the relay, so a lossy internet link no longer stalls everything. TCP stays as the fallback
  (`[net] udp = off` to always use TCP).
- Steam Deck, Legion Go S and other 16:10 screens: with `aspect = auto` the picture fills the whole screen with the taller view, and
  objects and players at the right height (#73). Not yet verified in the world. `taller_fov = false` under `[pkp]` goes back to bars.
- Import your save from Eden or Ryujinx (or any folder) on the Settings page. Your current save is moved to a dated backup first.
- Set up: a setup log (Open setup log button), checks before it starts (free disk space, Vulkan, memory, a warning for Intel
  13th/14th gen desktop CPUs), and a "fewer cores" option that keeps the PC usable while it builds.
- "Set up again" is much faster after an update: compiled game code is kept and reused (up to 3 GB, `--cache-mb`).
- .nup dumps: checked against their checksums, the version is checked before the big unpack, free space is checked, and the unpack
  can be cancelled.
- Clearer dump errors: files copied over FTP in text mode, an older game version or a base-only dump now tell you what to do.
- Both sticks reach full speed by default (outer zone 0.92, new `right_stick_max`). Saved settings keep their value.
- A way out of the game besides Alt+F4 (#70): hold Select and Start (View and Menu on an Xbox pad) together, or press "Quit game" on
  the F1 page, and confirm with A.
- Steam Deck, Legion Go S and other Linux / SteamOS players: drawing work is handed to the graphics card from a separate thread by
  default (`[nvn] submit_thread = auto`); about 26 -> 53 fps on a Steam Deck (measured by a player).

### Fixes
- Saves: if a save can't be written (full disk, a file locked by OneDrive or antivirus), the last good save is kept and a notice
  says so. Before, a half-written save could replace it.
- Sound comes back after unplugging headphones, a Bluetooth reconnect or the monitor turning off, instead of staying silent.
- Windows 11 no longer slows the game down when its window is behind other windows or on a second monitor.
- Graphics: particles, light culling and a volume effect get their real buffer sizes; depth-of-field blur and lens flares get the
  right instance; volumetric fog sees its real size; shadow tests on default textures match the console.
- A rare hang in the game's waiting loops is prevented (needs Set up again).
- Safer on GPUs that lack some formats (blending, filtering and vertex formats are checked; signed 10-bit textures fall back).
- Faster pipeline lookups (less CPU per frame).

### For developers
- New opt-in keys, all off: `[sched] precise_sleep`, `[sched] precise_waits`, `[sched] yield_backoff`, `[diag] yield_census`,
  `[nvn] shader_key_check`, `[nvn] reduced_precision`. On by default: `[sched] power_throttling`, `[nvn] pipe_last`. See CONFIG.md.
- Releases run the launcher tests first and ship SHA256SUMS.txt; pull requests are scanned for private text.
- Uses code and ideas from pokopia-native (MIT), credited in CREDITS.md.

## 0.4.1 (build H: Hedgehog, hotfix)

### Fixes
- Steam Deck, Legion Go S and other 16:10 screens: trees, rocks, stairs, players and Pokémon are drawn at the right height again. They
  used to float above the screen centre and sink into the ground below it, and only settled when you walked up to them. With
  `aspect = auto` the picture now stays 16:9 with thin bars on screens taller than 16:9 (#73). `aspect = 16:10` still gives the full
  taller view, with the old placement problem, until it is fixed properly.

## 0.4.0 (build H: Hedgehog)

### What's new
- Much faster on the Legion Go S: in the world at its native 1920x1200, with no upscaler, about 55 -> 75 fps (+36%), and the slowest
  frames are smoother (1% low 31 -> 38 fps). Measured on this release build, three before/after pairs.
- Windows PCs gain too: the game needs about 20% less CPU time per frame to hand its drawing work to the graphics card (RX 9060 XT:
  6.9 -> 5.5 ms per frame).
- Each speed-up has its own setting in `pkp.ini` (see the developer notes below), so a problem on one machine can be switched off
  without losing the rest.
- The launcher shows an "Update available" banner with a "What's new" link when a newer release is out. One request at start,
  silent when offline, nothing is downloaded.

### Fixes
- Render resolutions of about 3800x2140 and up (4K) no longer crash at start: the game's graphics memory now grows with the render
  size. 1920x1080 and below are unchanged. Very large sizes (above about 5300x2990) are lowered to the largest that fits, with a
  warning in the log. Setup builds pkp.exe again once (new hook sites).
- Xbox and PlayStation button prompts now show in the game when picked in the settings or by "auto" (before, Switch prompts stayed).
- When the graphics driver loses the GPU, the game shows one clear error and closes, instead of hanging and filling the log.
- Timed waits inside the game no longer end too early (they assumed the wrong console clock rate).

### Known issues
- The launcher does not start under Proton/Linux: it needs Microsoft's WebView2, which Proton does not include. So Steam Deck and
  Legion Go S players do not see the update banner.
- Legion Go S: about 75 fps at native, not yet a steady 90.

---

### Release notes for developers

Base: fork 0.3.0 (`9edefb8`).

- **#59** (fixes #49) `pkp_gfx_heap.cpp`: mid hooks at `0x710112f008` (pool config: general `sp+0x40`, render targets `sp+0xa0`) and `0x710112fb08`
  (graphics arena `[x3+0x48]`), registered only for game targets above 1920x1080. General pool +28 B per extra pixel, render-target
  pool only past 3500 MiB (937.6 MiB + 160 B/pixel + 1 GiB), arena by the sum; pools capped under 4 GiB, larger sizes clamped.
  `main.sites` changed: installs re-lift.
- **#40** pkp prompts: the UI atlas glyph patch (`pkp_kbm.cpp`) now covers the Xbox and PlayStation styles, not only keyboard
  (fixes #39).
- **#50** Kernel wait deadlines use the registered console tick frequency instead of 19.2 MHz; regression test with 10 ms hook slicing.
- **#51** The first `VK_ERROR_DEVICE_LOST` is latched at the Vulkan device layer: replay, acquire, submit and present stop, NVN serials
  complete on the CPU so game threads leave GPU waits, one error on the main thread, then exit.
- **#42**, **#43**, **#56** README: multiplayer work-in-progress note, new layout, link to the multiplayer tracking issue.
- **#71** Performance overhaul (runtime only; no re-lift). Each item names its ini key and default:
  - Config lookups cached per thread (`[config] cache`, on). Idea from ultratears (GPL-3.0).
  - Self-sampling draws use `VK_EXT_attachment_feedback_loop_layout` instead of a copy of the attachment (`[nvn] feedback_loop`,
    auto). Legion Go S: +9% on its own.
  - Guest memory is imported into Vulkan lazily in 16 MiB pieces when first bound under Wine (`[nvn] pool_lazy`, auto = Wine only).
  - Descriptor sets are reused when the bindings did not change (`[nvn] set_reuse`, on).
  - Fewer Vulkan calls per draw: dynamic state and bindings are only set when they change (`[nvn] bind_coalesce`, on); texture
    handles are resolved once per draw (`[nvn] resolve_once`, on) with a pool lookup cache (`[nvn] pool_cache`, on). PC: dynamic-state
    calls 1542 -> 384 per frame, resolves 4.2 -> 1.0 per draw.
  - Cheaper per replay call setup: write-watch polls read a summary bitmap (`[nvn] setup_coalesce`, on). PC: 1.5 -> 0.24 ms per frame.
  - The GPU wait before the input sample lets the GPU hold two frames instead of one (`[latency] gpu_frames_ahead`, 2; 1 = the old
    wait, one frame less latency).
  - A submit thread for NVN submissions and presents (`[nvn] submit_thread`, off: no gain at the frame rate on the Legion Go S).
  - Profiling: `[prof] gpu_passes`, `replay_fine`, `render_split` and the `ftrace_report.py` / `gpu_pass_report.py` scripts (all off).
  - Legion Go S numbers: SteamOS, Proton 11, in the world at native 1920x1200 on a time-based walk, three ABAB pairs on the release
    build: 54.0 / 55.3 / 56.9 -> 74.5 / 76.1 / 75.0 fps (mean 55.4 -> 75.2, +36%), 1% low 31.3 -> 37.5, 0 error lines.
- **#72** `launcher/src-tauri/src/update.rs`: one GET of `/repos/featureJosh/pokopia-recomp/releases/latest` at start (ureq 2 +
  rustls, 5 s timeout), semver compare against the launcher's own version; untagged builds (0.0.0, 0.1.1) skip it. The banner's link
  opens only `https://github.com/` URLs.

## 0.3.0 (build G: Gecko)

### What's new
- Name entry has an on-screen keyboard drawn in the game. It works with a controller, mouse or keyboard, including in Steam's Game Mode
  on handhelds, where the old Windows popup lost keys or never got focus.
- Faster start: after the first launch, the game packs its boot shaders into one file (about 285 MB) and reads that instead of
  thousands of small files. The first launch writes it in the background.
- Steam Deck and Legion Go S are detected. Button prompts stay as controller icons when you use the trackpad or touchscreen (the
  title screen used to show Enter for A), and automatic render scale follows the screen instead of rendering at 1080p or more.
- Setup works when the Windows user folder has accented or other non-English characters in its name (it failed after compiling).

### Known issues
- The launcher does not start under Proton/Linux: it needs Microsoft's WebView2, which Proton does not include.
- Legion Go S: 60 fps, not yet a steady 90.

---

### Release notes for developers

Base: fork 0.2.0 (`98e50b8`).

- **#31** swkbd: ImGui on-screen keyboard instead of the Win32 prompt (fork #29). The game blocks its window thread while the applet
  is open: kernel waits on that thread run a hook in 10 ms slices and the last frame is presented again with the overlay on top.
- **#32** `pkp_build.py` writes the `zig ar` response files as UTF-8 (orezraey, upstream drakolordx7/pokopia-recomp#6).
- **#33** `host_is_handheld()`: `SteamDeck=1`, SMBIOS product Jupiter/Galileo or family `Legion Go S*` (registry BIOS key, else
  Wine's `Z:\sys\class\dmi`); `[host] handheld`. Render scale auto = window size; prompt style auto keeps pad prompts on mouse
  input (based on tinfoil-chewer/pokopia-recomp-linux 8b150a4).
- **#34** `<shader_cache>.preload.pack`: the order file's SPIR-V in one zstd file (1096 -> 284 MiB), written in the background after
  a boot preload, read front to back on later boots; stale entries by results.tsv line hash; `[nvn] shader_pack`. zstd 1.5.7 pinned.
  Go S: written in 20 s, preload 2.6 -> 1.7 s (tinfoil-chewer/pokopia-recomp-linux c7a15e4).

## 0.2.0 (build F: Fennec fox)

### What's new

Builds A to F were the test builds that led to 0.2.0.

**Build A: the starting point**
- Everything from the original 0.1.1 release, plus a long list of small fixes to setup, the launcher and stability.
- Colours look right now. Before, the game looked washed out and too bright (the Pokédex was orange instead of red, Ditto was pale pink).
- AMD graphics cards no longer show black or white blocks instead of textures.
- First-time setup no longer runs out of memory while it builds the game, and a step that fails is retried.
- The launcher checks shaders before every start and warms them up, so there is less stutter the first time you see something new.

**Build B: names and menus**
- Typing your name works: before, only the first letter was kept ("Ash" became "A").
- The mouse no longer makes the menu selection jump around. Click to pick what is selected.

**Build C: Steam Deck and Linux picture fix**
- On Steam Deck, Legion Go and other Linux/Proton devices, the game showed a black screen. It now draws the game.
- The sky and backgrounds are no longer black, and there are no more bright white halos around things on those devices.

**Build D: tidier game files**
- The game files moved into the folder layout the launcher expects.

**Build E: one game file, smoother on handhelds**
- The launcher can use the game as a single `.nup` package file instead of a folder of extracted files.
- Name entry: the game accepts your name and characters say it correctly.
- New controller setting: "stick max" (F1 > Controls > Gamepad) for sticks that never reach their edge.
- First speed-up for handheld PCs on Linux/Proton.

**Build F: much faster on Legion Go S and Steam Deck class devices**
- In the game world on a Legion Go S, about 35 fps before, about 55-60 fps now.
- Windows PCs are not affected by this change.

**Also in 0.2.0**
- Playing above 1080p (for example 1440p): dark outlines of characters drawn in the wrong place, and blotchy shadows, are fixed.
- Updating: when a new version changes the game code, the launcher asks you to run Set up again (once) so the new code is built.
  Every existing install rebuilds once after this update.
- Steam Deck / Legion Go S: for more fps, lower the render scale or turn on FSR 3 in F1 > Graphics (about 70-80 fps on a
  Legion Go S at 67%).

### Known issues
- Legion Go S: 60 fps, not yet a steady 90 (being worked on).
- First setup still takes a while: about 7 minutes to build the game, then 1 to 2 hours for the shaders (you can pause and continue).

---

### Release notes for developers

Base: upstream `2c29515` (v0.1.1). Fork PRs on featureJosh/pokopia-recomp; issue numbers are fork issues.

- **#1** sweep: build, test, NVN, kernel, shader and launcher fixes (upstream tests compile again).
- **#2** spirv_harness with server GC (~4x faster translation). **#4** HFMA2/HADD2 `.H0_NH1` swaps lanes and negates the low one.
  The package's `spirv_harness` is built from this source; the upstream 0.1.1 binary does not match its source (34k of 99k shaders
  differ, washed-out colours, fork #8).
- **#3** the launcher translates shaders the game built at run time; shader warm-up run before Play.
- AMD depth: `D24S8` unsupported -> `D32S8` view fallback applied to views as well as images (black/white textures on RDNA).
- **#5** setup compiles within free memory and retries a failed unit alone.
- **#9 / #20** swkbd: SDK 0x140010 packed 0x530-byte config (use_utf8 at 0x3AA); `ngc:u` name check answered; text check in UTF-8;
  #16 reverted (fork #6).
- **#10** menus: pointer movement no longer drives the selection; click = A on the game's selection (fork #7).
- **#14** NVN: every vertex-stage output starts at (0,0,0,1) at module load (`spirv_init_outputs`, `[nvn] init_outputs`); RADV left
  unwritten components NaN -> black sky, NaN bloom halos (fork #13).
- **#18** heap: private memory under Wine so pools are imported as userptr (Wine sections are file-backed), whole-pool imports
  (fork #11). Under Proton keep `[nvn] tex_write_watch = false`: write-watch mprotect invalidates the userptr BOs.
- **#15 / #17** `.nup` package as the game source (launcher + runtime, stored zstd blocks).
- **#21** `[pad] stick_max`.
- **#22** `[nvn] submit_batch` (auto = `frame` under Wine): one kernel submission per frame, merged into the present's
  `vkQueueSubmit2`; early flush only when a thread waits on or polls a batched serial. amdgpu re-walks every userptr BO on each CS
  (~4-5 ms with 4.7 GB imported). Go S in game 33-39 -> 55-60 fps (fork #19; follow-up #23).
- **#25** viewports recorded by 0x17c0420 (64 callers) follow the render size: at render heights above 1080 the G-buffer AO
  composite landed at 1.333x (dark silhouettes, misplaced ground AO; upstream drakolordx7/pokopia-recomp#17).
- **#26** rebuild on update: pkp_build.py tool stamp (lifter, main.sites, sdk, pkp_rt.lib, flags; FORMAT 2) and `--check`; the
  launcher treats a stale build as not set up.
- Packaging: `game-build/scripts/make-release.ps1` + `.github/workflows/release.yml`; third-party payload from the upstream 0.1.1 zip.
