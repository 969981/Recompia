# Pause menu: "Save and return to game" (research notes)

Status: research only, no code yet. Tracked in the issue linked from the pull request that added this file.
Earlier request: #174 ("Save and continue"), closed as not planned. This file collects what is known so someone can continue.

## Goal
The pause menu (the + button) has **Save and Quit**. The game itself only autosaves and has no way to save and keep playing. We want an action that writes the save and puts the player back in the world, and we want to keep a way to quit. The player should not have to leave the world to be sure the save is written.

## What is known
- The label "Save and Quit" is not in this repository. It is game text, in the game's packed text archive (`LD_C0_R.BIN` with `LI_C0_R.IDX` in the romfs). There is no reader for that archive here.
- The game ends its own run through the Switch system call `svcExitProcess`. The runtime logs the caller chain and ends the process (`runtime/src/kernel/svc_thread.cpp`, `svc_exit_process`). Whether "Save and Quit" goes through that call or only returns to the title screen is NOT confirmed. Issue #174 says it "forces you to go back to the main menu", which suggests the title screen, so the real quit may come later, from the title.
- The runtime has its own quit paths that do not depend on the game's menu: F1 > Quit game, the `[quit]` key and pad shortcut, and closing the window (`runtime/src/gfx/play/play.cpp`, `quit_now`). So a "save and return" action can be added without removing a way to quit.
- The game's menus are KTGL layouts. `runtime/games/pkp/pkp_menu.cpp` walks the layouts in use every frame, finds entries by pane name, and handles hover and click. `[pkp] menu_log = 2` lists the panes of the layouts on screen; `3` lists the layout chain once a second.
- `runtime/games/pkp/pkp_options.cpp` (the in-game Options) already hooks the game's Settings screen, rewrites row text, and calls game functions by address. The offsets are for `main` of game version 2.0.1. The reverse-engineering notes behind them are not in this repository; the author may already know the pause menu.
- The main menu opens with Escape (`btn_plus`) in the default keyboard bindings (`runtime/games/pkp/pkp_data.cpp`).

## What is not known
1. The layout and pane names of the pause menu, and which pane is "Save and Quit".
2. The game function behind that entry, and whether it saves first and then exits, or returns to the title.
3. Whether the game has a plain save routine that can be called on its own, and what state the menu and the world must be in for it to be safe.
4. Whether the save is committed (the runtime commits save data in two phases, `runtime/src/services/fs_volume.cpp`) before the exit call.

## How to find out (about 20 minutes, needs the game)
1. In `pkp.ini` set `[pkp]` `menu_log = 2`, start the game, load a save, press Escape to open the pause menu, then read `pkp.log`. The `pkp: menu:` lines list the layouts and panes. Note the pane of "Save and Quit" and the order of the entries.
2. Press **Save and Quit** and read the end of `pkp.log`. Look for a `svcExitProcess caller chain` line (the game ended itself) and for the lines about the save commit just before it. Those addresses are the game function to look at (they are offsets into `main`).
3. A scripted version for unattended runs: the runtime's `[play] selftest` script can press keys (`key:Escape:down`, `wait:1`, `key:Escape:up`, `measure:20`, `close`).

## Design options (after the facts above)
- **A. Re-route and relabel behind a switch.** `[pkp] save_return` (off at first, a toggle in Settings > Game > Gameplay): the entry reads "Save and return to game". Pressing it runs the game's save, closes the menu and returns to play instead of leaving. Quitting stays available through F1 and the quit shortcut. Smallest change.
- **B. Add a second row** "Save and return to game" and keep the original row. Matches "offer both" most literally, but a new row has to be injected into a game layout list, which is more work and more risk (the in-game Options code shows a slot-table technique for this).
- Not recommended: only changing the label. A label that says something the action does not do is worse than none.

## Constraints
- Use the game's own save routine and the runtime's two-phase commit. Never skip it.
- Leave the menu and the world in a normal state, with no stuck input.
- New hook sites in `recomp/games/pkp/main.sites` mean players press **Set up again** once.
- Behind a setting, off by default, until testers have tried it.

## Acceptance
- With the setting on: from the pause menu the new action saves, the menu closes, the player is back in the world, and the process is still running. The save files on disk changed (a commit appears in `pkp.log`).
- With the setting off: the pause menu behaves exactly as before.
- No crash or freeze; the scripted run above passes; a person has checked how it looks and feels.
