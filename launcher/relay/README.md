# pokopia-relay

The Link Play relay: the meeting point for internet play (protocol and limits in `src/lib.rs`). One crate, used two ways:

- by the launcher (`launcher/src-tauri`): **Run a relay on this PC** on the Link Play page, and `PokopiaLauncher --host`;
- as the standalone `pokopia-relay` binary, for a server. No window, no WebView, only the standard library, `serde` and `libc`.

Both take the same options (`src/cli.rs`): `pokopia-relay --help`.

## Running it on a Linux server

Build it on the server itself (any architecture Rust supports; the public relay is an ARM64 Ubuntu VM):

    curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
    # copy this folder over (Cargo.toml, Cargo.lock, src/), then in it:
    cargo build --release --locked
    sudo install -m 755 target/release/pokopia-relay /usr/local/bin/pokopia-relay

Run it as a service under its own user (`sudo useradd --system --no-create-home --shell /usr/sbin/nologin pokopia-relay`), in
`/etc/systemd/system/pokopia-relay.service`:

    [Unit]
    Description=Pokopia Link Play relay
    After=network-online.target
    Wants=network-online.target

    [Service]
    User=pokopia-relay
    ExecStart=/usr/local/bin/pokopia-relay --rooms 500 --links 4000 --stats 300
    Restart=always
    RestartSec=2
    LimitNOFILE=65536
    NoNewPrivileges=yes
    ProtectSystem=strict
    ProtectHome=yes
    PrivateTmp=yes

    [Install]
    WantedBy=multi-user.target

then `sudo systemctl daemon-reload && sudo systemctl enable --now pokopia-relay`. Logs (one counters line every `--stats` seconds,
with the bytes carried, the rooms listed, the players in them and the room lists served): `journalctl -u pokopia-relay`.

Rooms opened with no room code are listed: anyone can see them in the game's Visit a Friend list (the room list in `src/lib.rs`).
`--no-list` turns this off (those rooms can then only be joined by code), `--list-per-min` sets how many lists one address may ask for.

Open **TCP and UDP 47610** in every firewall in front of it: the cloud provider's (security list / security group) and the
server's own (`iptables`/`ufw`; Ubuntu images on some clouds reject everything but SSH by default).

Updating: copy the new source, build, `sudo install ...` again, `sudo systemctl restart pokopia-relay`. A restart drops the rooms that are
open; players start Link Play again. A new relay serves older builds unchanged.

The address the launcher uses by default is `PUBLIC_RELAY` in `launcher/src/linkplay-model.js`.
