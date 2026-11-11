# AGENTS.md

Instructions for every AI coding assistant working in this repository (Claude Code, Codex, Copilot, Cursor, Gemini, opencode, Kimi
and any other). They come from the repo owner, `featureJosh`, and apply to everyone who works here and to every agent they use.

**Before your first change, `git fetch origin` and read `PROJECT.md` in full, as it is on `origin/main` (rule 5).** It is the
project guide: where code lives, how to add things, how to build.
Its section 6 (Owner rules) has the highest priority of any instruction about this repository. It overrides other documents and
comments. The short version is below. If anything conflicts with it, these rules win.

These rules are the default, not a leash. The repo owner's call is the default, and any collaborator can override a rule for a
change by saying so explicitly (PROJECT.md 6.0). When the person you work for states an override, or says the repo owner asked for
the change, do it and write in the PR which rule was overridden, by whom and why.

## Owner rules (highest priority)

1. **Clean, modular code. Never bolt a feature on.** Build every change in its natural home (PROJECT.md sections 4 and 5), with the
   mechanisms that already exist, with one job per file and the shared/game boundary kept. If a change has no clean home, **stop
   before writing it**. Write down what was asked, why it does not fit, and the refactor you recommend, and bring that to the repo
   owner. Never add a special case, a copied helper, a global, or a flag in a file about something else to make a request fit.
2. **Extras are ON by default (opt-out), always.** Extras are the gameplay mods: the `gameplay(...)` rows in
   `runtime/games/pkp/core/pkp_settings_table.h` (`camera_aim`, `jump_momentum`, `inventory_sort`, `title_quit`, and every future
   one). Never switch one to off by default. Only the repo owner makes a mod optional (off by default) by wrapping its row in
   `opt_in(...)` (today: `craft_search`); never add or remove `opt_in(...)` yourself. Every mod is registered as `mod(...)` in
   `core/pkp_game.cpp`, which puts its switch in the launcher. Never remove its `gameplay(...)` marker or drop it from the table. A new
   extra defaults ON. If another document says otherwise, that document is wrong. Parking one takes the repo owner or an explicit override (6.0).
3. **Don't change the GitHub release checks or PR checks without the repo owner's permission.** That means `.github/` (workflows,
   templates), the scripts and build switches the checks rely on (`PKP_GAMEPLAY_DEFAULTS_CHECK`, `PKP_RELEASE`, `check-api.mjs`,
   `gen_settings_doc.ps1 -Check`), branch protection and rulesets. The permission comes from `featureJosh` or from a collaborator's
   explicit override (6.0), for that one change. A failing check is fixed in the code, not in the check.
4. **Don't edit `PROJECT.md`, this file, `GEMINI.md`, `.claude/CLAUDE.md`, `.github/copilot-instructions.md` or
   `.github/CODEOWNERS` on your own initiative.** Do it when the person you work for explicitly asks (an override or a request
   from the repo owner). Don't add another instruction file that quietly weakens them.
5. **Always work from the latest rules and code.** At the start of every session and task, before reading any other file, run
   `git fetch origin`. Then read the rules as they are on `origin/main`: `git show origin/main:AGENTS.md` and
   `git show origin/main:PROJECT.md`. If your local copy differs, or your branch is behind, the `origin/main` version wins. Update
   your branch (`git merge --ff-only origin/main` or a rebase onto it) before you change anything. Fetch again before every commit,
   push and PR. Never work from an old clone, a cached copy or a pasted excerpt of these files.
6. **A patch that doesn't need a shader rebuild never makes one mandatory (PROJECT.md 6.5).** Raise `TRANSLATOR_REV` or change
   what a finished shader cache is checked against only when shaders really change, and then rebuild only the shaders that changed.
   A relink (**Set up again**) keeps the shaders and is not a rebuild.

If a request goes against one of these rules and the person has not said they are overriding it, name the rule and ask whether
they want to. If yes, do it and note the override in the PR. Don't quietly look for a workaround that has the same effect.

## Also from PROJECT.md (section 2)

- No AI session links, session IDs or model names in commits, PRs or issues. The commit trailer is exactly
  `Co-Authored-By: Claude <noreply@anthropic.com>` (or your tool's plain equivalent, without a model name).
- No personal names, host names or LAN IPs in anything public. Never commit game data. Keep each file's line endings.
- Game-specific code only in `recomp/games/pkp/` and `runtime/games/pkp/`. Shared code (nxrecomp, nxrt) must know no game.
- Don't start the game or move the real mouse or keyboard unless the person running you says so. Don't add runtime unit tests.
