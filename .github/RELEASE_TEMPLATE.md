# Release notes template

The release workflow publishes a version's `CHANGELOG.md` section (`## x.y.z (build L: Animal)`) as the GitHub release, and the
launcher shows the same text as its patch notes. To release: rename `## Unreleased` to the version heading, write it in the shape
below, merge, then tag `v<x.y.z>` (or run `release.yml` on main with `release_tag`).

## Rules

- **Lists, not paragraphs.** One change = one bullet, one line. Need more? One level of sub-bullets, no deeper (the launcher
  renders one level).
- **Lead with what the player sees,** then the issue number: `- Fixed the dashed outline around Rock Smash tiles missing (#242)`.
- **Start each bullet with a verb:** Added, Fixed, Changed, Removed.
- **Numbers as before ⇒ after:** `- Setup free space needed: 31 GB ⇒ 14 GB`.
- **Group by area,** most played first. Leave out areas with no changes. Areas: Graphics, Performance, Gameplay, Controls,
  Link Play, Launcher, Linux & Steam Deck, Mods.
- **Highlights** = 2 to 4 bullets. The launcher's update card shows the first two, so put the biggest first.
- **No HTML** (comments, `<details>`): the launcher shows it as text. Images show as their alt text in the launcher, so write
  a useful alt text.
- Developer detail (paths, functions, hashes) goes only under **For developers** at the end.

## Copy this

```markdown
## x.y.z (build L: Animal)

**Update:** press **Update** in the launcher. No **Set up again** needed.

### Highlights
- <Biggest change, in player words>
- <Second biggest>
- <Third, optional>

### Known issues
- <What is still broken> — <workaround, if any>

### Graphics
- Fixed <what looked wrong> (#123)
  - <optional detail: where, what else may look different>
- Changed <setting>: <old> ⇒ <new>

### Performance
- <Thing>: <before> ⇒ <after>

### Link Play
- Added <thing players can now do> (#123)
- Fixed <symptom> (#123)

### Launcher
- Added <setting> in **Settings > Game > <row>** (`pkp.ini` `[section] key`)

### Linux & Steam Deck
- Fixed <symptom> (#123)

| Before | After |
|---|---|
| ![Before: <what is wrong>](<url>) | ![After: <what is right>](<url>) |

### For developers
- `<path>` `<function>`: <what changed and why>
```

The **Update** line takes one of:

- `press **Update** in the launcher. No **Set up again** needed.`
- `press **Update**, then **Finish update** once when it asks (<why>, about <N> minutes).`
- `press **Update**, then **Set up again** once (<why>).`
