# tools/i18n: translated READMEs

The English `README.md` and the Linux release `game-build/release_b/README.linux.txt` have machine translations in Simplified
Chinese (`zh-CN`), Spanish (`es`), Arabic (`ar`), Hindi (`hi`), Brazilian Portuguese (`pt-BR`) and European Portuguese (`pt-PT`):

| English file | Translations |
|---|---|
| `README.md` | `docs/i18n/README.<lang>.md` |
| `game-build/release_b/README.linux.txt` | `game-build/release_b/README.linux.<lang>.txt` (the release workflow ships them as `README.<lang>.txt`) |

Translations are machine output and can contain mistakes; each one starts with a short note saying so and pointing at the English file,
which is the authoritative text. Corrections from native speakers are welcome as pull requests.

## Making or refreshing a translation

1. Translate the whole English file. Keep exactly as they are: code blocks, `inline code`, file names, paths, commands, setting keys,
   version numbers, URLs, and the badge and image HTML. Do not add or drop headings, code blocks or links. Do not use em dashes.
2. Start the file with the note (in the target language) that it is a machine translation of the English file, which is authoritative.
   For Arabic, wrap the text in `<div dir="rtl">` blocks (leave a blank line inside them so the Markdown still renders).
3. Run `python3 tools/i18n/check_readmes.py --fix-links` (links to files in the repository, such as `docs/assets/logo.png` or `CHANGELOG.md`, are written relative to the repository root in the English README and must become `../../docs/assets/logo.png` from `docs/i18n/`), then `python3 tools/i18n/check_readmes.py --stamp` to write the `source-sha256` line (the hash of the English file this translation
   was made from), then `python3 tools/i18n/check_readmes.py` to check the structure.

The check is also a pull request check (`.github/workflows/readme-translations.yml`). A structure mismatch fails it. A translation made
from an older English file only shows a warning ("stale"), so editing the English README never blocks a pull request: update the
translations afterwards, or ask for them to be refreshed.

The language bar at the top of `README.md` (between `i18n-bar` comments) is not part of the translated text and does not change the hash.

## Adding a language

Add its code to `LANGS` in `check_readmes.py`, add a link to the language bar, and translate both files as above.
