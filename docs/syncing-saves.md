# Playing on more than one device: keep your save in step with Syncthing

You play at your desk, and you want to carry on from the same spot on your Steam Deck, laptop or second PC. Recompia does not have built-in save sync yet, but a free, popular tool called **Syncthing** can do the carrying for you, and the launcher's own **Backups** make it safe.

This guide is for people who have never used Syncthing. It takes about 20 minutes once, and about a minute each time you change device.

> **Please read this first.** This guide is written from how Recompia's Backups and Restore work. The project team has not yet run it on two machines end to end. If a step does not match what you see, tell us on Discord or in issue #543 and we will fix the guide. Your saves are protected either way: Restore always makes a copy of your current save first.

## The idea in one minute

- You play on **one device at a time**.
- When you stop, the launcher puts a **backup copy** of your save in a folder called `save-backups`.
- Syncthing copies that folder to your other device, by itself, in the background.
- On the other device you press **Restore** on the newest copy, and carry on.

We sync the backup copies, **not** the live save. A backup copy is a finished, sealed snapshot, so Syncthing can never copy a save that is half written. That is the whole reason this is safe.

What this is **not**: it is not Steam Cloud, it does not let two devices play at the same time, and it never merges two different saves. If both devices have new progress, you pick one.

## What you need

- Two or more devices that each run Recompia (0.13 or newer), each with its own copy of the game set up in the usual way. Syncthing never touches your game files.
- A few minutes on both devices, switched on, at the same time for the first sync.
- Both devices need the **same save account**. Step 3 shows you how to check. If you have never used Profiles or imported a save, you almost certainly already match.

## Step 1: Turn on automatic backups (on every device you play on)

Backups are **off** until you pick a time, so nothing would be synced without this.

1. Open the Recompia launcher.
2. Go to **Settings**, then **Saves**.
3. Under **Backups**, set **Back up automatically** to **Every 10 minutes** (or 5 or 15).
4. Set **Backups to keep** to **20**.
5. Press **Back up now** once. The **Backups** line now shows that you have a copy.

A copy is also made when the game closes. Leave this on.

## Step 2: Find your backup folder (you will need its address)

1. Still under **Backups**, press **Open folder**. A window opens showing the backup folder, called `save-backups`.
2. Click in the address bar of that window and copy the full address. On Windows it looks like `D:\Games\Recompia\save-backups`. Yours will be different.
3. Write it down or keep the window open. This is the only folder you will share.

> **Do not share your whole game folder.** It is huge, it holds files that are only right for that one PC, and sharing it can break the install on your other device. Share `save-backups` only.

## Step 3: Check that both devices use the same save account

Your save lives in a folder named after your player account, a long string of letters and numbers. The backup copies are labelled with it too. **Restore only accepts copies from the same account**, so the names must match.

1. Under **Settings**, **Saves**, find **Save folder** and press its folder button.
2. Open the folder called `account`.
3. You will see a folder with a long name (32 characters, for example `000000000000000141434e4852656370`) and a file with the same name ending in `.meta`.
4. Do the same on your other device. **The long names must be identical.**

If you have only ever started the game normally on each device, both will usually show the same built-in name, and you are done. If the names differ, see "If the account names do not match" near the end before you carry on.

## Step 4: Install Syncthing on every device

Syncthing is made by a separate open-source project. It is free, it has no accounts and no adverts, and it is not part of Recompia.

Go to **syncthing.net** and open **Downloads**. The download page changes from time to time; if a name below is different, look for the section for your system.

**Windows**
1. Pick a Windows build from the Downloads page. A tray application that wraps Syncthing (listed there) is the easiest, because it starts with Windows and has a menu icon. The plain `syncthing.exe` also works.
2. Run it. If Windows asks about the firewall, allow it on **Private networks**.
3. Syncthing opens its control page in your web browser at `http://127.0.0.1:8384`. That page is only on your own PC.

**Linux (desktop)**
1. Install the `syncthing` package from your software centre or package manager.
2. Start it, then open `http://127.0.0.1:8384` in a browser. Many systems can start it at login with `systemctl --user enable --now syncthing`.

**Steam Deck**
1. Press the Steam button, choose **Power**, then **Switch to Desktop**.
2. Open **Discover**, search for **Syncthing**, and install it. If several results appear, choose the one that opens Syncthing in its own window.
3. Start it, then open `http://127.0.0.1:8384` in the Deck's browser. Syncthing only runs while the Deck is awake, so keep that in mind.

Do this on **every** device. Do **not** copy Syncthing's settings from one device to another: each device must have its own identity.

## Step 5: Introduce your devices to each other

You do this once.

1. On device A, open Syncthing (`http://127.0.0.1:8384`). Click **Actions**, then **Show ID**. A long code and a QR code appear. Copy the code.
2. On device B, open Syncthing. Click **Add Remote Device**. Paste the code from device A. Give it a name you will recognise, like "Desk PC". Click **Save**.
3. Back on device A, a banner appears: "Device B wants to connect". Click **Add Device** and **Save**.
4. After a few seconds, each device shows the other as **Connected** at the bottom of the page.

> **Only add your own devices.** A device ID is like a key. Never post yours publicly and never accept a device you do not recognise.

Both devices must be switched on, awake and online for this step. They do not have to be on the same network.

## Step 6: Share the backup folder

1. On device A, click **Add Folder**.
2. **Folder Label:** `Pokopia backups`. Leave **Folder ID** as it is; you will see it again on device B.
3. **Folder Path:** paste the address you copied in Step 2.
4. Open the **Sharing** tab and tick device B.
5. Open the **Ignore Patterns** tab and type this one line: `.tmp-*`. It stops Syncthing from copying a backup that is still being made.
6. Click **Save**.
7. On device B, a banner appears: "Device A wants to share folder Pokopia backups". Click **Add**.
8. Syncthing asks for the **Folder Path** on device B. Choose the `save-backups` folder in **device B's own game folder** (do Step 2 on device B to find it, or press **Back up now** there first so the folder exists). The Folder ID must stay the same.
9. Click **Save**.

Wait until both devices show **Up to Date** next to the folder. For a few backup copies this takes seconds.

## Using it every day

**When you stop playing on device A**
1. Close the game. A backup copy is made automatically.
2. Wait about a minute, then look at Syncthing on device B (or on A, under **Remote Devices**). The folder should say **Up to Date**. If device B is asleep or off, the copy waits and arrives when it wakes.

**When you start on device B**
1. Make sure the game is **closed** on B.
2. Open **Settings**, **Saves**, **Backups** and press **Restore**.
3. Choose the **newest** copy. Its time should match when you stopped on device A.
4. Confirm. The launcher first saves a copy of what B had, so nothing is lost, then puts A's progress in place.
5. Press **Play** and carry on.

**Golden rule: restore first, then play.** If you start the game on B without restoring, you carry on from B's old save.

## Common mistakes, and what to do

| What happened | Why | What to do |
|---|---|---|
| **Nothing arrives on the other device** | **Back up automatically** is still **Off**, or you never pressed **Back up now** | Step 1, on the device you played on |
| **The Restore list is empty or says the backup belongs to another profile** | The two devices use different save accounts | Step 3, then "If the account names do not match" below |
| **Restore is greyed out** | The game is still running, or there are no copies yet | Close the game completely, including in the system tray. If it is still grey, press **Back up now** on the other device and wait for it to sync |
| **You restored an old copy** | You restored before Syncthing said **Up to Date** | Wait for **Up to Date** on both devices, then restore the newest copy again. The copy you replaced is still in the list as a `before-restore` backup |
| **You played on both devices before syncing** | Each device now has progress the other does not | There is no automatic merge. Decide which to keep. Restore that device's newest copy on the other one. The other side's progress is still in its backups, so nothing is lost until the old copies are pruned |
| **You synced the whole game folder** | Wrong folder | Remove the folder in Syncthing (**Edit**, **Remove**; this does not delete files), then go to Step 6 and share `save-backups` only. If the game misbehaves afterwards, try **Repair** in the launcher's Settings, or ask on Discord |
| **You synced the live `save` folder** | Not recommended | Same fix. The live save can be half written while the game runs; the backup copies cannot |
| **The Deck copy never arrived** | The Deck was asleep | Wake it and open Syncthing. It catches up on its own |
| **Windows asks about the firewall** | Syncthing needs to talk to your other device | Allow it on **Private networks** |
| **Two devices will not find each other** | One of them is off, asleep or offline, or the device ID was mistyped | Check both are on and online. Paste the ID again. Syncthing finds devices across the internet by itself, so you do not need to open any router ports |
| **A file named `sync-conflict` appears** | Rare for this folder | It is not a real backup. You can delete it |
| **You deleted old backups by hand on one device** | Syncthing copies deletions too, so they vanish on the other device | Let the launcher tidy them with **Backups to keep** instead |
| **The game folder sits inside OneDrive, Dropbox or iCloud** | Two sync tools fight over the same files | Keep the game folder outside cloud folders |
| **The same Syncthing was copied to a second device** | Both now claim to be one device | Uninstall it on the second device, delete its Syncthing settings folder, and install fresh |

## If the account names do not match

This happens when one device had its save imported from another program, or you use **Profiles**, and the other did not.

1. On **both** devices, close the game and press **Back up now**, so every save is safe.
2. Decide which device has the progress you want to keep. Call it device A.
3. Copy A's long-named account folder and the `.meta` file with the same name, from A's `save/account` folder into B's `save/account` folder. A USB stick works.
4. On B, the game now uses that account. Press **Back up now** on both, and carry on from Step 6.

If you use **Profiles**, the simplest route for now is to use the **Default** profile on both devices. Ask on Discord if you need help with more than that.

## What this does not do, and what keeps you safe

- It is one device at a time, not two players or two devices at once.
- It does not choose a winner for you if both devices changed.
- Syncthing sends data directly between your own devices, encrypted. It does not go through Recompia's website or any Recompia server.
- Restore always keeps a copy of what it replaces, and the launcher never deletes your backups when it updates.
- To stop syncing, remove the folder or pause the device in Syncthing. Your save and your backups stay where they are.

## Undo, in one line

Syncthing: **Edit** the folder, then **Remove**. Recompia: nothing to undo, your save never moved.

## Still stuck?

Ask on Discord and say: which two devices, what Syncthing says next to the folder (for example **Up to Date** or **Out of Sync**), and what the **Backups** line in **Settings**, **Saves** says on each device. Do not post your Syncthing device ID.
