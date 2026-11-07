<h1 align="center">🏝️ Pokopia Recomp</h1>

<p align="center">
  <b>Pokémon Pokopia (Switch 2) recompiled to run natively on Windows, with multiplayer support.</b>
</p>

<p align="center">
  <a href="https://github.com/featureJosh/pokopia-recomp/releases/latest"><img src="https://img.shields.io/github/v/release/featureJosh/pokopia-recomp?label=latest&color=d8b46a&labelColor=1b2740" alt="Latest release"></a>
  <a href="https://github.com/featureJosh/pokopia-recomp/releases/latest"><img src="https://img.shields.io/github/downloads/featureJosh/pokopia-recomp/latest/total?label=latest%20downloads&color=3fb950&labelColor=1b2740" alt="Latest release downloads"></a>
  <a href="https://github.com/featureJosh/pokopia-recomp/releases"><img src="https://img.shields.io/github/downloads/featureJosh/pokopia-recomp/total?label=all%20downloads&color=3fb950&labelColor=1b2740" alt="All downloads">
  <img src="https://img.shields.io/endpoint?url=https%3A%2F%2Fraw.githubusercontent.com%2FDittobyte%2Fpokopia%2Fbadges%2Fwin64.json&label=Windows%20downloads&labelColor=1b2740" alt="Windows downloads">
  <img src="https://img.shields.io/endpoint?url=https%3A%2F%2Fraw.githubusercontent.com%2FDittobyte%2Fpokopia%2Fbadges%2Flinux-x64.json&label=Linux%20downloads&labelColor=1b2740" alt="Linux downloads"></a>
  <img src="https://img.shields.io/badge/platform-Windows%2010%2F11-0078d6?logo=windows&logoColor=white&labelColor=1b2740" alt="Platform: Windows 10/11">
  <img src="https://img.shields.io/badge/platform-Linux%20AppImage-e95420?logo=linux&logoColor=white&labelColor=1b2740" alt="Platform: Linux AppImage">
  <a href="https://github.com/featureJosh/pokopia-recomp/commits/main"><img src="https://img.shields.io/github/last-commit/featureJosh/pokopia-recomp/main?label=last%20commit&color=58a6ff&labelColor=1b2740" alt="Last commit on main"></a>
  <a href="https://github.com/featureJosh/pokopia-recomp/issues"><img src="https://img.shields.io/github/issues/featureJosh/pokopia-recomp?label=open%20issues&color=db61a2&labelColor=1b2740" alt="Open issues"></a>
</p>

<p align="center">
  <a href="https://github.com/featureJosh/pokopia-recomp/releases/latest"><img src="https://img.shields.io/badge/%E2%AC%87_Download-latest_for_Windows-d8b46a?style=for-the-badge&labelColor=1b2740" alt="Download the latest release for Windows"></a>
  <a href="https://github.com/featureJosh/pokopia-recomp/releases/latest"><img src="https://img.shields.io/badge/%E2%AC%87_Download-latest_for_Linux_(AppImage)-e95420?style=for-the-badge&labelColor=1b2740" alt="Download the latest release for Linux (AppImage)"></a>
</p>

<p align="center">
  <b><a href="https://github.com/featureJosh/pokopia-recomp/releases">Releases</a></b> ·
  <a href="CHANGELOG.md">Changelog</a> ·
  <a href="CONTRIBUTING.md">Contributing</a> ·
  <a href="CREDITS.md">Credits</a> ·
  <a href="https://github.com/xFractals/pokopia-native">Parent: pokopia-native</a> ·
  <a href="SECURITY.md">Security</a> ·
  <a href="LEGAL.md">Legal</a> ·
  <a href="https://github.com/featureJosh/pokopia-recomp/issues">Issues</a>
</p>

> [!IMPORTANT]
> **No game files are included.** You need your own extracted copy of the game, version **2.0.1a**. This project provides no game files, keys or dumping tools. Not affiliated with Nintendo, The Pokémon Company or Koei Tecmo; Pokémon is a trademark of Nintendo, Creatures Inc. and GAME FREAK inc. (see [LEGAL.md](LEGAL.md)).

> [!NOTE]
> **Built with AI, supervised by a human.** This project is developed primarily with AI, with me as the supervisor. I have a handful of years of development experience, so it's not all slop. Every commit is reviewed, verified and tested by me before it lands.

## Screenshots

<table>
  <tr>
    <td width="50%"><img src="docs/assets/screenshots/title-screen.svg" alt="Title screen (screenshot placeholder)"><br><sub>Title screen</sub></td>
    <td width="50%"><img src="docs/assets/screenshots/island-gameplay.svg" alt="Island gameplay (screenshot placeholder)"><br><sub>Building your island</sub></td>
  </tr>
  <tr>
    <td width="50%"><img src="docs/assets/screenshots/item-placement.svg" alt="Item placement (screenshot placeholder)"><br><sub>Placing items with Joy-Con 2 mouse mode</sub></td>
    <td width="50%"><img src="docs/assets/screenshots/link-play.svg" alt="Link Play (screenshot placeholder)"><br><sub>Link Play with a friend (work in progress)</sub></td>
  </tr>
</table>

## Features

### Play together (Link Play)
- **Visit a friend's island, or invite them to yours**, on the same Wi-Fi or over the internet.
- **Cross-play with [pokopia-native](https://github.com/xFractals/pokopia-native) players** on game version 2.0.1.
- Set it up in the launcher's **Link Play** page. No files to edit.
- Step-by-step guide: [Playing together](#playing-together).

### Import your saves
- **Already playing on Eden or Ryujinx?** One button in **Settings** finds your save and brings it over. Your emulator's copy isn't touched.
- **Got a save somewhere else?** Pick its folder (the one with `B00`, `G00` and `S00` inside).
- **Your current save is never deleted.** It's kept as a backup folder first.
- If a save can't be written (a full disk, OneDrive or antivirus holding the file), your last good save is kept and the game tells you.

### Easy setup
- **Use your dump as it comes:** a folder (with `base` and `updates`) or a single `.nup` file.
- **Clear messages if something is wrong with your dump:** the wrong game version, a missing update, or files damaged while copying. The launcher tells you what to do.
- **Checks before it starts:** enough disk space, a working graphics driver, enough memory.
- **Use fewer cores** keeps your PC usable while it builds. You can pause and carry on later.
- **Updates are quick:** only the parts that changed are rebuilt.
- The launcher tells you when a new version is out.

### Controls
- **Any controller:** Xbox, PlayStation, Switch Pro and more, with rumble and motion. Buttons follow the Switch layout by default.
- **Keyboard and mouse.** The mouse also works in menus: click to pick, scroll to move.
- **Change any button** on the F1 **Controls** page.
- **Joy-Con 2 mouse mode** for placing items: press the middle mouse button.
- **On-screen button icons match what you're holding:** keyboard, Xbox, PlayStation or Switch.
- **Typing your name** works with a controller, mouse or keyboard.
- **Quit without Alt+F4:** hold Select and Start (View and Menu on an Xbox pad), or use **Quit game** in F1.
- **Open the F1 menu from a controller:** off by default; set `[overlay] pad` in `controls.ini` (for example `left_shoulder+right_shoulder`). It cannot be the quit combination.

### Steam Deck and handhelds
- Steam Deck, Legion Go S and other handhelds are recognised: controller icons stay on screen and the picture size suits the screen.
- 16:10 screens are filled edge to edge, with no black bars.
- Runs through Proton on SteamOS, with handheld speed-ups (about 26 to 53 fps on a Steam Deck and 55 to 75 fps on a Legion Go S, measured by players).
- The launcher runs on Linux and SteamOS too: see [Linux, SteamOS and the Steam Deck](#linux-steamos-and-the-steam-deck).

### Graphics
- **Play at any resolution**, 4K and above. Lower **Render scale** if it runs slowly.
- **Quality presets** (Switch 2, High, Ultra) set how far you can see and how sharp textures are.
- **HDR** on HDR screens.
- **Window, borderless or full screen.** Choose the monitor, cap the frame rate, show an FPS counter. Alt+Enter switches modes while playing.
- **Upscalers:** FSR 1 and NVIDIA Image Scaling work.
- **FSR 3 and DLSS / DLAA are there but don't look good yet** and need rework. Leave them off for now.

### In-game settings
Press **F1** in the game to change graphics, controls and sound (volume, output device, mute when the window is in the background). Most changes apply straight away.

## Quick start

1. Download the zip from the [latest release](https://github.com/featureJosh/pokopia-recomp/releases/latest) and extract it anywhere.
2. Run `PokopiaLauncher.exe`. If SmartScreen warns, choose **More info → Run anyway**. Pick your dump folder and press **Set up**: it compiles the game on your PC (about 7 minutes), then builds the shader cache (1 to 2 hours, you can pause and continue later).
3. Press **Play**.

F1 opens the settings in game. Middle mouse toggles Joy-Con 2 mouse mode for placing items.

### Linux, SteamOS and the Steam Deck

1. In Steam, install **Proton Experimental** (Library, Tools; on a Steam Deck switch to Desktop Mode first).
2. Download `PokopiaRecomp-<version>-linux-x64.tar.gz` from the [latest release](https://github.com/featureJosh/pokopia-recomp/releases/latest) and extract it anywhere (for example your home folder).
3. Run `PokopiaLauncher.AppImage` in that folder (double-click it, or `./PokopiaLauncher.AppImage` in a terminal). Pick your dump folder and press **Set up**: the first time it downloads its build tools (Python and Zig, about 100 MB), compiles the game, then builds the shader cache through Proton. You can pause and continue later.
4. Press **Play**. The game itself runs through Steam's Proton.

If `PokopiaLauncher.AppImage` does not start on a system without FUSE, run `./PokopiaLauncher.AppImage --appimage-extract-and-run`.

To start the game from Steam (Game Mode on the Deck): in Desktop Mode, **Add a Non-Steam Game**, pick `PokopiaLauncher.AppImage`, and set its launch options to `--play`. More detail, and what was tested, in [launcher/README.md](launcher/README.md#linux-steamos-the-steam-deck-bazzite-other-distributions).

## Updating from an older version

Your saves, settings and shaders stay.

1. Download the new zip from the [latest release](https://github.com/featureJosh/pokopia-recomp/releases/latest). Close the game and the launcher.
2. Extract it over your install, into the same folder. The zip holds no saves, settings, shader cache or game files, so extracting does not replace them.
3. Run `PokopiaLauncher.exe`. If the update changes the game code, the launcher says so: open **Game files** and press **Set up** again. It rebuilds `pkp.exe` (about 7 minutes). The old `pkp.exe` stays until the new one is ready.
4. Press **Play**.

Copy the `save` folder somewhere first if you want a backup. A save that a newer version wrote may not load in an older one.

## Playing together

Link Play puts several players on one island. Cross-play works with pokopia-native players on game version **2.0.1**. Players on game 1.0.5 cannot join.

**What changed.** Link Play now speaks pokopia-native's co-op protocol. It does not work with earlier Link Play builds of this port, so everyone updates to this release. (The launcher keeps the old protocol under **Advanced**, for a session where everyone still has an old build.)

**Each player needs their own save.** A copy of someone else's save cannot join its original.

**Set it up.** In the launcher, open **Link Play** and pick one:

| Choice | Use it for |
|---|---|
| Alone | No Link Play. |
| Automatic (LAN) | The default. Same Wi-Fi or network: the host is found by itself. |
| Visit by address | Join one host by its address, for example `192.0.2.10`. The host's Link Play page shows it. |
| Internet | Play through a relay with a room code. |

Changes apply the next time the game starts.

**In the game**, open the PC in your first house, then **Link Play** (it unlocks after Environment Level 2 in the Withered Wasteland) and pick **Local connection**. (**Online connection** is Nintendo's own service and does not work here.) The host picks **Invite Others to Visit**, the others **Visit a Friend**.

**Same network.** Everyone picks **Automatic (LAN)**. Steam Deck and other Proton or Linux devices find hosts and are found too, on whatever network they are on. If the host does not show up, the visitor picks **Visit by address** and types the host's LAN address. Cafe, hotel and guest Wi-Fi often keeps devices apart: there, play over the internet instead.

**Over the internet.** Everyone picks **Internet** and goes through a relay:
- The host picks **I host**. The page shows a room code. Give the visitors the relay address and the code. **Copy** and **New code** are on the page.
- A visitor picks **I visit** and types the relay address and the code.
- To run a relay yourself, press **Run a relay here** on the host's Link Play page (it works while the launcher stays open), or run `PokopiaLauncher --host` on a server. Any pokopia-native co-op relay works too.
- Without a relay, leave the relay address empty on the host and forward TCP and UDP **47600** on the router. Visitors use **Visit by address** with the host's public address.
- The relay checks the room code but does not encrypt the game's traffic.

**Ports and firewall.** The first time you host, Windows Firewall asks about `pkp.exe`: allow **Private networks**. Your network must be Private too (Settings, Network & internet, your connection, Network profile type: **Private**).

| Port | Who | Protocol |
|---|---|---|
| 47600 | the host | TCP and UDP |
| 47610 | the relay | TCP, and UDP for the faster path |

**Interact everywhere and Story co-op (experimental, off by default).** In the story areas the game puts visitors in Spectator Mode. **Interact everywhere** lets them use moves and items, pick up, place and build there. **Story co-op** lets them talk to the story characters, and turns Interact everywhere on. Every player needs the same two switches, or the host is not listed. Only the host's save keeps the story progress.

Link Play is still being tested: joining and walking around together work. Long sessions, more than two players and play over the internet are not tested yet. See [docs/multiplayer](docs/multiplayer/README.md) for how it works and the `pkp.ini` keys.

## Common problems

- **The window is black, or the game stutters a lot, on the first launch.** The game makes some shaders while it runs. The launcher translates the ones it has seen before every start and again after you quit, so the first runs are rough and later ones are better. Give it a minute and keep playing. If the setup stopped half way, open **Game files** and press **Set up** to continue. It keeps finished work.
- **The launcher says the game files are from an older version, or that the update is missing.** This release needs version 2.0.1a. Dump the game again with the update installed. The base game alone is not enough.
- **The launcher says it found no usable game files, or Set up says a file does not match.** Pick the folder that holds `base` and `updates` (for example `Pokopia_v2.0.1a`). Each needs `exefs` (`main`, `sdk`, `subsdk0`, `rtld`, `main.npdm`) and `romfs`. A file that does not match is damaged. Dump it again, or copy it again.
- **The launcher says the game data (romfs) is incomplete, or the game closed at start with "pkp.exe exited with code 4".** Your dumping tool wrote the update's `romfs` without the base game's files under it (an update only holds the files it changes). Keep the base game in the same dump (`base` next to `updates`, each with `exefs` and `romfs`) and pick the folder that holds both: the launcher adds the missing files from `base` itself. Then press **Set up** once more. Otherwise dump the game again with the update merged into the base game.
- **The launcher says the dump was copied in text mode.** You copied it with FTP in text mode, which damages `main`, `sdk` and `subsdk0`. Copy the `exefs` and `romfs` folders again with the transfer type set to **Binary** (FileZilla: Transfer, Transfer type, Binary).
- **Your dump is one file** (`.nsp`, `.xci`). The launcher takes the game as folders, or as a `.nup`. Extract the `.nsp` or `.xci` to folders with your dumping tool.
- **Setup stops with an error about a file that cannot be opened or removed.** Put the folder somewhere plain, like `C:\Pokopia`, not in a folder that OneDrive syncs. Your antivirus may be scanning the files it is building: wait for it, or allow the folder. **Open setup log** shows where it stopped.
- **Set up warns before it starts.** It checks free disk space (about 36 GB while it runs), Vulkan, memory and the processor first. On an Intel 13th or 14th generation desktop CPU, update the BIOS before you set up: without Intel's fix these CPUs can crash under long full load. If the PC gets hot or hard to use while it builds, tick **Use fewer cores**.
- **Low frame rate, or a blurry picture.** The picture size is fixed when the game starts. It is the size of the window or screen at that moment (**Settings**, **Display**).
  - Start in **Borderless** on a 4K screen and the game draws at 4K. That is heavy. Lower **Render scale**.
  - Start **Windowed** (1280x720 by default) and press Alt+Enter, and the picture stays 720p, scaled up. Set **Window mode** before you press **Play**.
  - Set **Frame rate limit** to what your screen can show.
- **Link Play: the visitor cannot find the host.** Pick **Visit by address** and type the host's address (the host's Link Play page shows it). Check that the host started the game and picked **Invite Others to Visit**, that Windows Firewall allows `pkp.exe` on Private networks, and that the network is Private. Cafe, hotel and guest Wi-Fi that keeps devices apart blocks LAN play for everyone: use **Internet** with a relay and a room code. Over the internet, check the relay address and the room code. Changes on the Link Play page apply the next time the game starts.
- **Link Play: the connection is refused.** The players do not match. Everyone needs game 2.0.1, the same release (or pokopia-native for cross-play), and the same mods and the same **Interact everywhere** and **Story co-op** switches.
- **Link Play: "Communication error" (9005) when you visit.** You are both playing copies of the same save, so both have the same player ID. Each player needs their own save.
- **The controller is not detected.** Open the game with F1 and look at the **Gamepad** line on the Controls page. It says "No gamepad open" when the game sees none. Connect it before you press **Play**, then try the other cable or port. Close other programs that may be holding the pad.

If it is none of these, see [Reporting problems](#reporting-problems).

## Where your saves, logs and settings are

All of these are in the game folder: the folder with `PokopiaLauncher.exe` and `pkp.exe`, unless you chose another one in **Settings**, **Game folder**.

| What | Where |
|---|---|
| Saves | `save` (**Settings** can point it elsewhere: **Save folder**) |
| Settings | `pkp.ini` and `controls.ini` (F1 in game writes `controls.ini`) |
| Log | `pkp.log`, written on every start from the launcher. A crash also writes `pkp_crash.txt`, `pkp_freeze.txt` or `pkp_fatal.txt`. |
| Translated shaders | `shader_cache` (about 26 GB) |
| Build files | `build` (the setup's work folder) |
| Unpacked `.nup` | `nup_dump`, if you picked a `.nup` |
| Launcher settings | `%APPDATA%\org.pkp-recomp.launcher\launcher.json` (your dump folder and game folder) |

Never attach game files, the `build` or `shader_cache` folders, or saves to an issue. `pkp.log` is enough.

## Reporting problems

[Open an issue](https://github.com/featureJosh/pokopia-recomp/issues/new/choose) with `pkp.log` from the session where it happened, your GPU and your Windows version.

## License

GPL-3.0. Third-party code keeps its own licence, see [CREDITS.md](CREDITS.md).
