# Experimental builds

Recompia has two update channels. Questions and reports are welcome on the Recompia Discord: https://discord.gg/VyRcUUQNKH (also under Settings > About > Community in the launcher). You pick one in the launcher with the two dropdowns next to the **Play** button (also under **Settings > About > Update channel**): the channel, and the version.

- **Stable** (the default): the releases everyone gets. **Latest** follows the newest release and offers each update, as the launcher always has. Pick a specific release instead to install it and stay on it: no update is offered until you pick Latest again.
- **Experimental**: every GitHub prerelease (for example `0.8.1-beta.1` or `0.9.0-alpha.1`). Each one tests its own new features, so you pick the one you want to try; there is no Latest here, and nothing updates by itself. Until you pick one, you keep the version you have. The old Testing channel is gone: a launcher that was on Testing is moved to Stable with Latest.

## What to know before you switch

- Your saves, profiles and settings are kept. Experimental builds do not change how they are stored.
- An Experimental build can have bugs that Stable does not. Please report them: open an issue on GitHub, say which Experimental build you have (Settings > About), and attach `pkp.log` (next to `pkp.exe`).
- Sometimes an Experimental build needs **Set up again**, like any update. The launcher tells you.
- Going back to Stable: switch the channel to Stable and pick Latest or a version. Picking a release older than the build you have asks first and copies your save and settings to `rollback-backups` before anything is replaced. With Latest, Stable offers an update once it has a version newer than the one you have.
- Launchers older than the Stable/Experimental picker still have the Testing channel and Settings > About > Choose a version.

## For maintainers

- Branches: `main` is Stable; `testing` is where changes land first. Promote `testing` into `main` with a pull request once a change has had reports.
- Publish a Testing build: run the **Release** workflow on the `testing` branch with `release_tag` set to `v<x.y.z>-beta.N` (`-alpha.N` and `-rc.N` also work). It is published as a GitHub prerelease. Stable tags (`v<x.y.z>`) still publish from `main` only.
- Notes for a Testing build come from its own `## <version>` section in `CHANGELOG.md` if there is one, else from `## Unreleased`, else a short generic text.
- The launcher's Stable channel ignores prereleases, so a prerelease never reaches Stable players. Experimental lists every prerelease the GitHub API returns (all pages), each as a choice of its own.
- The launcher's **What's New** page, on the Experimental channel, has an **Experimental** group: **All Experimental changes** (every Testing build since the last stable release, newest first, each with what changed and its regressions) and the builds themselves. The **Releases** group below it is the normal release notes. Stable players see none of the Experimental entries.
- **Regressions and known problems:** write them under `## Unreleased` in `CHANGELOG.md` as a section named `### Regressions and known problems` (or `### Known problems`), one bullet each: what got worse or is not confirmed yet, compared with the last stable release, from tester reports and our own checks. `None known` is fine. The launcher draws this section as a highlighted block at the top of the build's note and repeats it in All Testing changes. When a Testing build is promoted to Stable, remove the section from the release notes (it is not part of a Stable note).
- **Changes in this build:** the Release workflow adds this list to a Testing build's notes by itself: the pull requests merged into `testing` since the previous release (stable or Testing). If that step fails the notes are still published, without the list.
- **What to test:** every Testing build's notes carry a `### What to test` section under `## Unreleased` in `CHANGELOG.md`, next to `### Regressions and known problems`: concrete steps (what to do, what to look for, what to report) written for that build. Do not publish a Testing build without both sections. The launcher draws What to test as a highlighted block under the regressions block, and All Testing changes repeats it for every build.
- **How to report:** the Release workflow appends a `### How to report` footer to every Testing build's notes (open the bug form, choose Testing as the channel, say the version, attach `pkp.log` and `setup.log` if Set up ran). The bug form has a required Update channel field so Testing reports can be filtered.
- **What we ask of testers:** when you switch the launcher to Experimental it asks you to confirm, then lists the Experimental builds with their notes to pick one. Testers are expected to say what they find, with the version and `pkp.log`. The launcher's Settings > About has **Report a problem** and **Show pkp.log** buttons while Experimental is on.
- **Choosing a build:** the version dropdown next to Play lists the builds of your channel. Picking one asks first, then installs it like an update. Going back to an older one copies your save and settings to `rollback-backups` before anything is replaced; a long way back needs a ticked warning.
- **Keeping the list short:** every prerelease on GitHub is listed on Experimental, so delete a prerelease (the release and its tag) once its feature test is over. Never delete a Stable release or the build a tester has been asked to try.
