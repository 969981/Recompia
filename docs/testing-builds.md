# Testing builds

Recompia has two update channels. Questions and reports are welcome on the Recompia Discord: https://discord.recompia.com (also under Settings > About > Community in the launcher). You pick one in the launcher under **Settings > About > Update channel**.

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
- The launcher's **What's New** page, on the Testing channel, has a **Testing** group: **All Testing changes** (every Testing build since the last stable release, newest first, each with what changed and its regressions) and the builds themselves. The **Releases** group below it is the normal release notes. Stable players see none of the Testing entries.
- **Regressions and known problems:** write them under `## Unreleased` in `CHANGELOG.md` as a section named `### Regressions and known problems` (or `### Known problems`), one bullet each: what got worse or is not confirmed yet, compared with the last stable release, from tester reports and our own checks. `None known` is fine. The launcher draws this section as a highlighted block at the top of the build's note and repeats it in All Testing changes. When a Testing build is promoted to Stable, remove the section from the release notes (it is not part of a Stable note).
- **Changes in this build:** the Release workflow adds this list to a Testing build's notes by itself: the pull requests merged into `testing` since the previous release (stable or Testing). If that step fails the notes are still published, without the list.
- **What to test:** every Testing build's notes carry a `### What to test` section under `## Unreleased` in `CHANGELOG.md`, next to `### Regressions and known problems`: concrete steps (what to do, what to look for, what to report) written for that build. Do not publish a Testing build without both sections. The launcher draws What to test as a highlighted block under the regressions block, and All Testing changes repeats it for every build.
- **How to report:** the Release workflow appends a `### How to report` footer to every Testing build's notes (open the bug form, choose Testing as the channel, say the version, attach `pkp.log` and `setup.log` if Set up ran). The bug form has a required Update channel field so Testing reports can be filtered.
- **What we ask of testers:** when you switch the launcher to Testing it asks you to confirm. Testers are expected to say what they find, with the version and `pkp.log`. The launcher's Settings > About has **Report a problem** and **Show pkp.log** buttons while Testing is on.
- **Choosing a build:** Settings > About > Choose a version lists the builds of your channel with their notes. Joining Testing opens the Testing builds right away. A newer build installs like an update. Going back to an older one asks first and copies your save and settings to `rollback-backups` before anything is replaced; a long way back needs a ticked warning.
- **Keeping the list short:** keep the newest three Testing builds on GitHub and delete the older ones (the release and its tag) after each new Testing build is published. Never delete a Stable release or the build a tester has been asked to try. The launcher's **Choose a version** list then stays short, and a player on a deleted build is simply offered the newest one as an update.
