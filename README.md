<h1 align="center">🏝️ Pokopia Recomp</h1>

<p align="center">
  <b>Pokémon Pokopia (Switch 2) recompiled to run natively on Windows, with multiplayer support.</b>
</p>

<p align="center">
  <a href="https://github.com/featureJosh/pokopia-recomp/releases/latest"><img src="https://img.shields.io/github/v/release/featureJosh/pokopia-recomp?label=latest&color=d8b46a&labelColor=1b2740" alt="Latest release"></a>
  <a href="https://github.com/featureJosh/pokopia-recomp/releases/latest"><img src="https://img.shields.io/github/downloads/featureJosh/pokopia-recomp/latest/total?label=latest%20downloads&color=3fb950&labelColor=1b2740" alt="Latest release downloads"></a>
  <a href="https://github.com/featureJosh/pokopia-recomp/releases"><img src="https://img.shields.io/github/downloads/featureJosh/pokopia-recomp/total?label=downloads&color=3fb950&labelColor=1b2740" alt="Total downloads"></a>
  <img src="https://img.shields.io/badge/platform-Windows%2010%2F11-0078d6?logo=windows&logoColor=white&labelColor=1b2740" alt="Platform: Windows 10/11">
  <a href="https://github.com/featureJosh/pokopia-recomp/commits/main"><img src="https://img.shields.io/github/last-commit/featureJosh/pokopia-recomp/main?label=last%20commit&color=58a6ff&labelColor=1b2740" alt="Last commit on main"></a>
  <a href="https://github.com/featureJosh/pokopia-recomp/issues"><img src="https://img.shields.io/github/issues/featureJosh/pokopia-recomp?label=open%20issues&color=db61a2&labelColor=1b2740" alt="Open issues"></a>
</p>

<p align="center">
  <a href="https://github.com/featureJosh/pokopia-recomp/releases/latest"><img src="https://img.shields.io/badge/%E2%AC%87_Download-latest_for_Windows-d8b46a?style=for-the-badge&labelColor=1b2740" alt="Download the latest release for Windows"></a>
</p>

<p align="center">
  <b><a href="https://github.com/featureJosh/pokopia-recomp/releases">Releases</a></b> ·
  <a href="CHANGELOG.md">Changelog</a> ·
  <a href="CONTRIBUTING.md">Contributing</a> ·
  <a href="CREDITS.md">Credits</a> ·
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

### Your game, your way in
- **Folder dumps or a `.nup` package.** Pick the folder that holds `base` and `updates`, or a single `.nup` file. The launcher checks a `.nup` against its checksums and its version before the big unpack, checks free space, and lets you cancel.
- **Clear dump checks.** An older game version, a dump without the update, or files damaged by a text-mode FTP copy each get their own message saying what to do.
- **Set up with checks first.** Before it starts, Set up checks free disk space, Vulkan, memory and the processor (with a warning for Intel 13th/14th gen desktop CPUs). **Use fewer cores** keeps the PC usable while it builds, and **Open setup log** shows every step. Setup can be paused and continued.
- **Fast updates.** Set up again after an update reuses the compiled game code that did not change.
- **Update banner.** The launcher tells you when a new release is out, with a "What's new" link. Nothing is downloaded for you.

### Bring your island
- **Import a save from Eden or Ryujinx.** **Settings**, **Save data**: **Find Eden / Ryujinx save and import** finds the newest Pokopia save on this PC and imports it. The emulator's save is only read.
- **Or from anywhere else.** **Pick a folder to import** takes any folder that holds the game's save folders, `B00`, `G00` and `S00`: another emulator's, a backup, a save from a friend's PC. In Eden, right-click the game, **Open Save Data Location** shows it.
- **Nothing is lost.** Your current save is moved to a dated `save.before-<date>` folder first and never deleted. Close the game before you import.
- **Safe saves.** If a save cannot be written (full disk, a file locked by OneDrive or antivirus), the last good save is kept and the game tells you.

### Multiplayer (Link Play)
Play on one island with friends, on the same network or over the internet. The game thinks it is using local wireless, and the launcher runs a small relay that carries it between PCs. See [Playing together](#playing-together) for the steps.
- Host or visit from the launcher's **Link Play** page, no files to edit.
- Room codes keep strangers out of a shared relay.
- The host page lists the addresses to give out (your network, Tailscale or ZeroTier) with Copy buttons, and warns when Windows treats your network as Public.
- Game traffic goes over UDP when it can, so a lossy link does not stall, and falls back to TCP.

### Picture
- **Any resolution**, including 4K and above, with **Render scale** to trade sharpness for speed.
- **Upscalers:** AMD FSR 1, NVIDIA Image Scaling, AMD FSR 3 (included), and NVIDIA DLSS / DLAA on RTX cards (the launcher downloads `nvngx_dlss.dll` from NVIDIA for you, under **Game files**).
- **Quality presets** (Switch 2, High, Ultra), longer view distance, texture anisotropy.
- **HDR output** on HDR displays.
- **Windowed, borderless or exclusive full screen**, a monitor picker, a frame rate limit and a frame rate counter. Alt+Enter switches modes while playing.
- **16:10 screens** (Steam Deck, Legion Go S) fill the whole screen with a taller view instead of bars.

### Controls
- **Controllers:** Xbox, PlayStation, Switch Pro and others, with rumble and motion, and the Switch face-button layout by default. Rebind every button and set dead zones and stick range on the F1 **Controls** page. If a pad does not show up, close Steam or turn off Steam Input for it.
- **Keyboard and mouse,** fully rebindable. The mouse works in the game's menus: click to confirm, the wheel to move.
- **Joy-Con 2 mouse mode** for placing items: middle mouse toggles it.
- **Button prompts** in the game match what you use: keyboard, Xbox, PlayStation or Switch icons, picked automatically or by hand.
- **On-screen keyboard** for name entry that works with a pad, mouse or keyboard, including Steam's Game Mode.
- **Quit without Alt+F4:** hold Select and Start (View and Menu on an Xbox pad), or press **Quit game** on the F1 page.

### Handhelds and Linux
- Steam Deck, Legion Go S and other handhelds are detected: prompts stay as controller icons on the touchscreen and trackpad, and the render scale follows the screen.
- Runs under Proton on SteamOS and Linux, with speed-ups tuned there (about 26 -> 53 fps on a Steam Deck, 55 -> 75 fps on a Legion Go S, measured by players). The launcher does not start under Proton yet (it needs Microsoft's WebView2): set up on a Windows PC and copy the game folder over.

### In-game settings (F1)
F1 opens a settings page over the game: Display, Graphics, Controls, Audio (volume, output device, mute when unfocused) and Game. Most changes apply live. Everything is also in `pkp.ini` and `controls.ini`; [runtime/docs/CONFIG.md](runtime/docs/CONFIG.md) lists every key.

## Quick start

1. Download the zip from the [latest release](https://github.com/featureJosh/pokopia-recomp/releases/latest) and extract it anywhere.
2. Run `PokopiaLauncher.exe`. If SmartScreen warns, choose **More info → Run anyway**. Pick your dump folder and press **Set up**: it compiles the game on your PC (about 7 minutes), then builds the shader cache (1 to 2 hours, you can pause and continue later).
3. Press **Play**.

F1 opens the settings in game. Middle mouse toggles Joy-Con 2 mouse mode for placing items.

## Updating from an older version

Your saves, settings and shaders stay.

1. Download the new zip from the [latest release](https://github.com/featureJosh/pokopia-recomp/releases/latest). Close the game and the launcher.
2. Extract it over your install, into the same folder. The zip holds no saves, settings, shader cache or game files, so extracting does not replace them.
3. Run `PokopiaLauncher.exe`. If the update changes the game code, the launcher says so: open **Game files** and press **Set up** again. It rebuilds `pkp.exe` (about 7 minutes). The old `pkp.exe` stays until the new one is ready.
4. Press **Play**.

Copy the `save` folder somewhere first if you want a backup. A save that a newer version wrote may not load in an older one.

## Playing together

Link Play puts two players on one island. Every player needs this release, and **each player needs their own save**: a copy of someone else's save cannot join its original.

**To host**
1. In the launcher, open **Link Play** and pick **Host on this PC**, then **Start hosting**.
2. Give your friends one of the addresses it shows (the first one is for your own network) and the room code. Use **Copy** for both.
3. Keep the launcher open while you play, and press **Play**.

**To visit**
1. In the launcher, open **Link Play**, pick **Visit**, type the host's address and room code, and press **Save**.
2. Press **Play**.

**Before you start (Windows):** the first time, Windows Firewall asks about the launcher: allow **Private networks**. Your network must be Private too (Settings, Network & internet, your connection, Network profile type: **Private**). On a Public network Windows keeps the other PCs out; the host page warns you.

**In the game**, open the PC in your first house, then **Link Play** (it unlocks after Environment Level 2 in the Withered Wasteland) and pick **Local connection**. (**Online connection** is Nintendo's own service and does not work here.) The host picks **Invite Others to Visit**, the visitor **Visit a Friend**.

**Over the internet:** the easy way is [Tailscale](https://tailscale.com) or [ZeroTier](https://www.zerotier.com) (both free for personal use) on every PC: no router settings, and the traffic between you is encrypted. The host page lists that address too. Without one, the host forwards port **39660, TCP and UDP**, to their PC on the router's port forwarding page, and gives out their internet address (search "what is my IP" on the host's network). TCP alone works, but UDP keeps a lossy link from stalling. Always use a room code then, and remove the forward when you are done: the relay checks the code but does not encrypt the game's traffic.

Link Play is still being tested: joining and walking around together work, long sessions and more than two players are not tested yet. See [docs/multiplayer](docs/multiplayer/README.md) for how it works and the `pkp.ini` keys.

## Common problems

- **The window is black, or the game stutters a lot, on the first launch.** The game makes some shaders while it runs. The launcher translates the ones it has seen before every start and again after you quit, so the first runs are rough and later ones are better. Give it a minute and keep playing. If the setup stopped half way, open **Game files** and press **Set up** to continue. It keeps finished work.
- **The launcher says the game files are from an older version, or that the update is missing.** This release needs version 2.0.1a. Dump the game again with the update installed. The base game alone is not enough.
- **The launcher says it found no usable game files, or Set up says a file does not match.** Pick the folder that holds `base` and `updates` (for example `Pokopia_v2.0.1a`). Each needs `exefs` (`main`, `sdk`, `subsdk0`, `rtld`, `main.npdm`) and `romfs`. A file that does not match is damaged. Dump it again, or copy it again.
- **The launcher says the dump was copied in text mode.** You copied it with FTP in text mode, which damages `main`, `sdk` and `subsdk0`. Copy the `exefs` and `romfs` folders again with the transfer type set to **Binary** (FileZilla: Transfer, Transfer type, Binary).
- **Your dump is one file** (`.nsp`, `.xci`). The launcher takes the game as folders, or as a `.nup`. Extract the `.nsp` or `.xci` to folders with your dumping tool.
- **Setup stops with an error about a file that cannot be opened or removed.** Put the folder somewhere plain, like `C:\Pokopia`, not in a folder that OneDrive syncs. Your antivirus may be scanning the files it is building: wait for it, or allow the folder. **Open setup log** shows where it stopped.
- **Set up warns before it starts.** It checks free disk space (about 36 GB while it runs), Vulkan, memory and the processor first. On an Intel 13th or 14th generation desktop CPU, update the BIOS before you set up: without Intel's fix these CPUs can crash under long full load. If the PC gets hot or hard to use while it builds, tick **Use fewer cores**.
- **Low frame rate, or a blurry picture.** The picture size is fixed when the game starts. It is the size of the window or screen at that moment (**Settings**, **Display**).
  - Start in **Borderless** on a 4K screen and the game draws at 4K. That is heavy. Lower **Render scale**.
  - Start **Windowed** (1280x720 by default) and press Alt+Enter, and the picture stays 720p, scaled up. Set **Window mode** before you press **Play**.
  - Set **Frame rate limit** to what your screen can show.
- **Link Play: "Communication error" (9005) when you visit.** You are both playing copies of the same save. Each player needs their own save.
- **Link Play: the visitor cannot find the host.** Check that the host pressed **Start hosting** and still has the launcher open, that both typed the same room code, and that both networks are Private in Windows. Over the internet, check the port forward (39660, TCP and UDP) or use Tailscale or ZeroTier. Every player needs the same release. Changes on the Link Play page apply the next time the game starts.
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
