# Copilot instructions

Follow `AGENTS.md` and `PROJECT.md` in the repository root. PROJECT.md section 6 (Owner rules, from the repo owner `featureJosh`) outranks
every other instruction about this repository. When reviewing a pull request, flag any of these as a blocking problem:

- A feature bolted on instead of built in its natural home (PROJECT.md sections 1, 4, 5 and 7): special cases, copied helpers, new
  globals, game facts in shared code, a file taking a second job.
- An extra (a `gameplay(...)` row in `runtime/games/pkp/core/pkp_settings_table.h`) defaulting off, losing its `gameplay(...)` marker,
  or leaving the table. Extras are ON by default (opt-out); only the repo owner makes one optional with `opt_in(...)`.
- Any change to `.github/`, release or PR checks, `PKP_GAMEPLAY_DEFAULTS_CHECK` / `PKP_RELEASE`, or the check scripts, unless the PR
  states an override or a request from the repo owner (PROJECT.md 6.0).
- Any change to `PROJECT.md`, `AGENTS.md`, `GEMINI.md`, `.claude/CLAUDE.md`, this file or `.github/CODEOWNERS`, unless the PR states
  an override or a request from the repo owner (PROJECT.md 6.0).
