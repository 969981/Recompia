# Testing builds

Recompia has two update channels. Questions and reports are welcome on the Recompia Discord: https://discord.gg/qE656Fed5N (also under Settings > About > Community in the launcher). You pick one in the launcher under **Settings > About > Update channel**.

- **Stable** (the default): the releases everyone gets. This is what the launcher has always offered.
- **Testing**: builds that are newer and less tested, published as GitHub prereleases (for example `0.8.1-beta.1`). They come from the `testing` branch. Pick this if you want to try changes early and help find problems before they reach Stable.

## What to know before you switch

- Your saves, profiles and settings are kept. Testing builds do not change how they are stored.
- A Testing build can have bugs that Stable does not. Please report them: open an issue on GitHub, say you were on Testing and which version (Settings > About), and attach `pkp.log` (next to `pkp.exe`).
- Sometimes a Testing build needs **Set up again**, like any update. The launcher tells you.
- Going back to Stable: the launcher only moves forward, so it will not install an older version over a newer Testing build. Stable offers an update again once it has a version newer than the one you have. Until then you can install the Stable release from the releases page.
- When a Testing build becomes the next Stable release (for example `0.8.1-beta.3` becomes `0.8.1`), you get it as a normal update.

## For maintainers

- Branches: `main` is Stable; `testing` is where changes land first. Promote `testing` into `main` with a pull request once a change has had reports.
- Publish a Testing build: run the **Release** workflow on the `testing` branch with `release_tag` set to `v<x.y.z>-beta.N` (`-alpha.N` and `-rc.N` also work). It is published as a GitHub prerelease. Stable tags (`v<x.y.z>`) still publish from `main` only.
- Notes for a Testing build come from its own `## <version>` section in `CHANGELOG.md` if there is one, else from `## Unreleased`, else a short generic text.
- The launcher's Stable channel ignores prereleases, so a Testing build never reaches Stable players.
