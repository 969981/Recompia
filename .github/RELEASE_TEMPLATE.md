<!--
Release text template. The release workflow publishes the CHANGELOG.md section `## <x.y.z>` as the release notes, and takes the
release title from its heading: "Pokopia Recomp <x.y.z> (build <letter>: <animal>)". So, to release:
  1. Rename `## Unreleased` to the heading below (build letter = animal; same letter for a minor series, next letter for a new one).
  2. Fill it in this order; leave out empty parts.
  3. Merge, then tag v<x.y.z> (or run release.yml on main with release_tag = v<x.y.z>).
Write for players: plain words, what they see, what they must do. Implementation details go only under "Release notes for developers".
-->

## x.y.z (build L: Animal)

Press **Update** in the launcher and it does the rest. <!-- Then ONE of: -->
The game code is unchanged, so you do not need to **Set up again**.
<!-- or: Run **Finish update** once when it asks: <why, and how long>. -->
<!-- or: Run **Set up again** once: <why>. -->

<!-- One short paragraph: the two or three things players will notice. Where to report problems (issue + pkp.log). -->

~Jerban

### Known issues
- **<Short bold statement.>** <One sentence of detail or workaround.>

### New

#### <What players can now do> (#issue)
**Before:** <what it was like>

**Now:** <what it does, where the setting is (`pkp.ini` key in backticks), what to press>

**Still not:** <limits, or delete this line>

### Fixes

#### <The problem as a player saw it> (#issue)
**Before:** <the symptom>

**Now:** <what changed for the player>

**Still not:** <what is still broken, or delete this line>

<!-- Optional for visual fixes: -->
| Before | Now |
|---|---|
| ![Before: ...](<image url>) | ![Now: ...](<image url>) |

### Release notes for developers
- `<path>` `<function>`: <what changed and why>.
