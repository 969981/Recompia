# Cloud Islands against a self-hosted server (experimental)

With `[net] cloud_server` set, the game's own NPLN client talks to a self-hosted server such as
[Raikaru/pokopia-cloud-server](https://github.com/Raikaru/pokopia-cloud-server). Nothing contacts the original servers: the NPLN names resolve to
the configured address or fail. Everything is off without `cloud_server`.

## Status

Run-tested on game version 2.0.1a with the 0.11.1 runtime: create an island, enter it, upload, return to it (download), a second player
joining by address and Link Code, building together. Two machines on a LAN and over the internet (router port forward, direct UDP).
Run-tested on 0.13.1 with this branch (two copies on one PC): online, travel to the island, upload on arrival and on leaving, the
owner's return (download), a second player joining by address and Link Code, and the game's own "Kick a player". "Send a report"
reaches `Screening/IssueUploadUri`, which the server does not answer yet.

Accounts (see Account mode below): run-tested on 0.15.0, hosting an island through the launcher's account gateway on a server in
account mode (create, matchmaking record, island upload).

Not done: more than two players, a relay for strict NATs, TLS in the runtime, a lifter site for the fiber switch (see below).

## Configuration (`pkp.ini`)

```ini
[net]
online_test = on
cloud_server = 127.0.0.1          ; IPv4 address of the server
cloud_ca = C:/path/to/ca.der      ; the server's CA certificate, DER
cloud_gateway_port = 18443        ; local plaintext-to-TLS gateway for the island upload and download (0 = off)
cloud_storage_port = 0            ; or: the server's plain HTTP storage port (its --http-port); no gateway needed
```

With `cloud_storage_port` set, the island file goes straight to `cloud_server` on that port in plain HTTP (start the server with
`--http-port <port>`; that flag cannot be combined with its `--client-ca` account mode). The file is not encrypted on the way.
Without it:

Run `gateway.py --server <address>:443 --ca <ca.crt> --sni storage.npln.nintendo.net` next to the game. The game does its own TLS for
gRPC; the island file goes through `nn::ssl`, which the runtime does not implement, so that one connection goes through the gateway.

## Account mode (sign-in from the launcher)

A server in account mode (`--client-ca`) takes only connections that present a device certificate its account portal issued. The
game cannot present one, so for each game run the launcher starts a small gateway on 127.0.0.1 (`launcher/cloud-gateway`, program
`recompia-cloud-gateway`) that pipes the game's connections to the server over mutual TLS with the device certificate, and checks
the server's certificate against the CA the portal handed out at sign-in. Signing in is optional: without `cloud_portal` nothing
changes, and signed out the game plays as before with only Cloud Islands unavailable.

```ini
[net]
cloud_server = <server address>
cloud_portal = https://<portal>      ; the account portal: the Home screen shows a Sign in chip and account mode is on
cloud_account = on                   ; optional: on / off overrides what cloud_portal implies
cloud_account_dir = <folder>         ; optional: the device folder (default: the launcher's data folder under %LOCALAPPDATA% or ~/.local/share)
cloud_account_tls_port = 18443       ; optional: the gateway's ports on 127.0.0.1 (these are the launcher's defaults)
cloud_account_plain_port = 18444
```

Home, account chip, **Online account**: **Use this PC** opens the portal's `/link` page in the browser and takes the answer on a
one-shot listener on 127.0.0.1 (PKCE S256, state checked); **Use my phone** shows a QR code and a short code, and a phone signed in
to the portal approves the device at `/link?ticket=...` (the device polls `/api/link/remote/poll`; the phone then says the device is
signed in). Either way the device makes its own key and sends only a CSR; the key stays in the device folder (Windows: encrypted, the
passphrase protected by DPAPI; elsewhere owner-only files). Removing the device on the portal's account page revokes it.

The launcher hands the game the gateway's ports and its CA (`NXRT_NET_CLOUD_ACCOUNT_TLS_PORT`, `NXRT_NET_CLOUD_ACCOUNT_PLAIN_PORT`,
`NXRT_NET_CLOUD_CA`) for that run only; pkp.ini is not written. The gateway makes a new CA at every start and drops its key after
signing, starts even when signed out, and picks up a sign-in or sign-out at the next connection. Log: `cloud_gateway.log` next to
`pkp.log`. Without the launcher: `recompia-cloud-gateway signin --portal <url> [--home <dir>] [--method browser|phone]`, `status`,
`signout`, and `run [--home <dir>] [--tls-port N] [--plain-port N] [--ca-out <file>]` next to a game whose pkp.ini sets the two
ports and `cloud_ca` = that file.

## The game executable needs 131 more entries

`main-extra-entries-2.0.1a.txt` lists 131 function entries in main's online code (0x1730000 to 0x1770000) that the lifter does not
find: 74 are only stored as 64-bit words, 57 are only reached through a computed address. This branch has them in
`extra_entries['main']` (`recomp/games/pkp/config.py`), and `recomp/nxrecomp/env.py` gives an extra entry in main a real extent (up to
the next known start) instead of 4 bytes. A game built before this change must be rebuilt.
Other computed targets in that range are jump-table labels and must not become entries.

## Version-specific parts still in the runtime

`network_cloud.cpp` redirects `nn::os::detail::DispatchFiberContext` (sdk+0x14f648 in 2.0.1a) to host fibers by patching the lifted
function at run time, and answers one thread entry (main+0x1769584) on the host. Both belong in the lifter as sites.

## Testing with two copies of one save

Two copies of a save are the same player and the host refuses its own id (error 9005). `save_uuid.py <save dir> reroll` gives a copy
a new player id. `[account] network_account_id` must differ between the two as well.
