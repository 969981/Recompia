# Recompia automated upstream sync and mirror prereleases

The [sync workflow](../.github/workflows/sync-upstream.yml) follows the official upstream **[Dittobyte/Recompia](https://github.com/Dittobyte/Recompia)** `main` branch. The older `featureJosh/pokopia-recomp` URL redirects to the new organization.

## What runs

- GitHub Actions cron: `31 */4 * * *` UTC (every four hours; scheduling is best-effort).
- On new upstream commits: merge into mirror `main`, without force-pushing. Conflicts fail closed.
- **Local CI workflows are preserved** during the merge. This avoids the GitHub App/GITHUB_TOKEN limitation on updating `.github/workflows` through a push. Review new upstream workflow changes separately.
- One prerelease tag per upstream history position: `v<next-patch>-rc.<upstream-commit-count>` (for example `v0.9.1-rc.1234`). A tag never gets overwritten and repeated checks do not generate duplicate builds.
- Explicitly dispatch the original [Release](../.github/workflows/release.yml) workflow at the snapshot tag with `sync_snapshot=true`. Because `GITHUB_TOKEN` pushes don't trigger new push workflows, dispatch is essential.
- The release workflow builds its Windows ZIP and Linux tar.gz containing an AppImage and publishes checksums as a GitHub **prerelease**. A failure does not count as a published release.
- The initial installation triggers one baseline snapshot build, even if upstream `main` has not advanced.

## Manual retry

Actions → **Sync upstream and publish** → Run workflow. If the snapshot tag exists but its release is absent, this requests another build. If a release exists, no duplicate build is started.

Normal upstream release semantics remain unchanged: official tags build as before, stable manual releases remain on `main`, and ordinary testing-channel manual RC releases remain on `testing`. Only the explicit `sync_snapshot=true` flag allows a mirror RC publication from `main`.

The release pipeline inherits the upstream packaging dependency `BASE_REPO=Dittobyte/Recompia`, `BASE_RELEASE=v0.8.2`, and requires network downloads and GitHub Actions resources. Rebuild failures must be diagnosed in Actions. No game ROM, game data, or private keys are published.
