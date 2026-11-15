# Link Play guide

Play Pokopia together: several players on one island, between PCs, Steam Decks and other handhelds. This is the short guide. Relays of
your own, port forwarding and every detail are in the [full multiplayer page](multiplayer/README.md).

**On this page**

- [What you need](#what-you-need)
- [Play over the internet](#play-over-the-internet)
- [Make your room private](#make-your-room-private)
- [Play on the same network](#play-on-the-same-network)
- [Where it is in the game](#where-it-is-in-the-game)
- [Options everyone must share](#options-everyone-must-share)
- [When it does not work](#when-it-does-not-work)

## What you need

- **The same release for everyone.** Update the launcher on every PC first. Players on different releases do not see each other.
- **Your own save.** Every player plays their own save. A copy of a friend's save cannot join that friend.
- **Link Play unlocked in the game.** It unlocks after Environment Level 2 in the Withered Wasteland.
- **The same mods that change gameplay.** The launcher's Link Play page lists yours; everyone in the session needs the same ones.

## Play over the internet

There are no room codes and no addresses to type any more. Every room shows up in the game's own list.

1. Everyone opens the launcher, goes to **Link Play** and picks **Internet**.
2. Everyone presses **Play**.
3. In the game, everyone goes to the PC in their first house, then **Link Play**, then **Local connection**.
4. The host picks **Invite Others to Visit**.
5. The visitors pick **Visit a Friend** and choose the host's room from the list.

That is all. Nobody forwards a port: every player connects out to the project's public relay, and the relay brings them together.

The list shows every open room of players on your release with the same shared options, so you can also join other players, and other
players can join you. If you only want your friends, make the room private.

## Make your room private

When you host, the game asks **"Would you like to set a Link Code?"**

- Say **yes**, pick a code and give it to your friends.
- Your room still shows up in the list, but only players who type the right Link Code get in.
- The game checks the code itself, the same way it does on a Switch.

Say **no** and anyone who sees the room can visit.

## Play on the same network

For players on the same Wi-Fi or home network. Nothing leaves your network.

1. Everyone opens **Link Play** in the launcher and picks **Same network** (it is the default).
2. Everyone presses **Play** and goes to the PC, **Link Play**, **Local connection** in the game.
3. The host picks **Invite Others to Visit**; the visitors pick **Visit a Friend** and choose the host.

**The host is not in the list?** On the host's Link Play page, read the address under **This PC** (for example `192.168.1.20:47600`). Each
visitor types it in **Join directly**, then closes the game and starts it again.

Steam Decks and other Linux devices find hosts and are found the same way.

Cafe, hotel and guest Wi-Fi often keeps devices apart, and then nobody can find anybody. Use **Internet** there.

## Where it is in the game

| Step | Where |
|---|---|
| 1 | The PC in your first house |
| 2 | **Link Play** |
| 3 | **Local connection** |
| 4 | Host: **Invite Others to Visit**. Visitor: **Visit a Friend** |

**Online connection** in that menu is Nintendo's own service. It does not work here; always pick **Local connection**, also for play
over the internet.

## Options everyone must share

On the launcher's Link Play page, under **Same for everyone**:

| Option | What it does |
|---|---|
| **Visitors can build everywhere** | Visitors can use moves and items and build in the story areas too, not only in Palette Town. |
| **Seamless co-op** | Every player plays their own story in the host's world: their own requests, talks and progress, kept in their own save. Turning it on turns the first one on too. |

Both are experimental and off by default. Every player in the session needs the same two settings, or the host is not listed.

Changes on the Link Play page apply the next time the game starts.

## When it does not work

**The host's room is not in the list**

- Is everyone on the same release? Check **Settings > About** in the launcher.
- Did everyone pick the same choice at the top of the Link Play page (**Internet** for all, or **Same network** for all)?
- Are the two **Same for everyone** options the same for everyone?
- Does everyone have the same gameplay mods?
- Did the host already pick **Invite Others to Visit**? There is no room to list before that.
- Did you change something on the Link Play page while the game was running? Close the game and start it again.

**"Communication error" (9005) when visiting**

Two players have the same save. Each player needs a save of their own; a copied save needs a new game.

**Windows asks about `pkp.exe` the first time you host**

Allow it on **Private networks**. If you said no, allow `pkp.exe` in Windows Firewall, and check that your network is set to Private
(Windows Settings, Network & internet, your connection, Network profile type).

**The right Link Code is refused**

The host and the visitor must type exactly the same code. The host sets it when the game asks, right after **Invite Others to Visit**.

**Still stuck?**

[Open an issue](https://github.com/Dittobyte/Recompia/issues/new/choose) and attach `pkp.log` (next to `pkp.exe`) from every player.
The [full multiplayer page](multiplayer/README.md) covers your own relay, port forwarding, the ports the game uses and play with
pokopia-native players.
