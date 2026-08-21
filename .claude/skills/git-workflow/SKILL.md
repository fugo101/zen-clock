---
name: git-workflow
description: "ALWAYS use this skill when creating git branches, writing commit messages, committing code, or performing any git commit/branch operation. This includes when the user says 'commit', 'create a branch', 'push changes', 'make a PR', '/commit', or any variation. Enforces Conventional Commits v1.0.0 and project branch naming conventions for Release Please automation."
---

# Git Workflow

This skill governs all git branch creation and commit message formatting. The project uses GitHub
Flow with Conventional Commits v1.0.0 and Release Please for automated versioning. Every commit
message on `main` directly controls version bumps and changelog generation — precision matters.

## Why This Matters

This project uses **squash merging** — the PR title becomes the single commit on `main`. Release
Please reads these messages to:
- Decide whether to bump MAJOR, MINOR, or PATCH version (writes the result into `version.txt`)
- Generate `CHANGELOG.md` automatically
- Create release PRs with correct version numbers

A malformed commit message means Release Please **ignores the change entirely**. A wrong type means
a wrong version bump or a change that's invisible in the changelog. This is not cosmetic — it is
functional.

## Never Commit Unless Asked

Do not run `git commit` proactively as part of finishing a task. Stage and prepare the message if
useful, but only create the commit when the user explicitly asks for one.

## Branch Creation

### Naming Format

```
<prefix>/<kebab-case-description>
```

### Prefixes

| Prefix      | When to use                                        |
|-------------|-----------------------------------------------------|
| `feature/`  | New functionality or capability                    |
| `fix/`      | Bug fix                                            |
| `chore/`    | Maintenance, deps, config (no user-facing change)  |
| `ci/`       | CI/CD pipeline changes                             |
| `docs/`     | Documentation only                                 |
| `refactor/` | Code restructuring without behavior change         |
| `perf/`     | Performance improvement                            |

### Rules
- Use 2-4 words in kebab-case for the description
- Keep it concise but descriptive enough to identify the work
- Always branch from `main`

### Steps

1. Determine the work type and select the matching prefix
2. Summarize the work in 2-4 kebab-case words
3. Run:
```bash
git checkout main && git pull origin main && git checkout -b <prefix>/<description>
```

If the user is already on a non-main branch, ask whether to continue on the current branch or
create a new one. Never silently stay on an existing branch when asked to "create a branch."

## Commit Messages

### Format (Conventional Commits v1.0.0)

```
<type>(<optional scope>): <description>

[optional body]

[optional footer(s)]
```

### Types and Version Impact

| Type       | Version Bump | In CHANGELOG    | Use when                              |
|------------|-------------|-------------------|----------------------------------------|
| `feat`     | MINOR       | Features          | Adding new user-facing functionality  |
| `fix`      | PATCH       | Bug Fixes         | Fixing a bug                          |
| `perf`     | PATCH       | Performance       | Measurable performance improvement    |
| `refactor` | None        | Refactoring       | Code change that is not feat or fix   |
| `docs`     | None        | Documentation     | Documentation changes only            |
| `chore`    | None        | **Hidden**        | Maintenance, deps, tooling            |
| `style`    | None        | **Hidden**        | Formatting, whitespace, linting       |
| `test`     | None        | **Hidden**        | Adding or fixing tests                |
| `ci`       | None        | **Hidden**        | CI/CD changes                         |
| `build`    | None        | **Hidden**        | Build system changes                  |

Only `feat` and `fix` (and `perf`) trigger version bumps. `refactor` and `docs` are visible in the
changelog here but do not bump the version — check `release-please-config.json` before assuming
otherwise; this differs from some other repos in the org where `refactor`/`docs` are hidden. Choose
type based on what the change **does**, not what files it touches.

### Description Rules
- Start with a lowercase letter
- Use imperative mood: "add" not "added" or "adds" — read it as "This commit will ___"
- Do not end with a period
- Keep under 72 characters total (including type + scope + colon + space)
- Be specific: "fix plaintext password leak via unterminated SSID cast" not "fix security bug"

### Scope (Optional)
A noun in parentheses identifying the section of the codebase. Scopes actually used in this repo's
history: `ui`, `wifi`, `ble`/`provisioning`, `security`, `deps`, `release`, `native`, `bsp`, `power`,
`deep-sleep`, `tailscale`/`microlink`, `settings`, `cleanup`, `audit`, `changelog`, `safety-net`.

```
fix(wifi): reconnect after ASSOC_LEAVE without a 15s timeout
feat(settings): add timezone offset range item
chore(deps): bump esp_lvgl_port to 2.9.0
```
Use scope when the change is clearly localized to one area. Omit when it spans multiple areas.

### Body (Optional)
For changes needing more explanation. Separate from description with a blank line. Wrap at 72
characters. Explain **what** and **why**, not how.

### Breaking Changes
Breaking changes trigger a MAJOR version bump. Signal them in one of two ways:

**Option 1 — Exclamation mark:**
```
feat!: change NVS key layout for settings
```

**Option 2 — Footer (for detailed explanation):**
```
feat: change NVS key layout for settings

BREAKING CHANGE: existing devices lose saved settings on upgrade and revert to defaults.
```

Always confirm with the user before marking something as breaking.

## Pre-Commit Gates

Run these before every commit — `.github/workflows/ci.yml` runs the same gates (minus `--staged`)
on every push, so catching failures here saves a CI round-trip:

```bash
python3 scripts/check_secrets.py --staged   # credential leak guard — run first
python3 scripts/format.py --check           # clang-format check
pio test -e native                          # host-side unit tests
python3 scripts/pio_check.py                # clang-tidy static analysis
```

`--staged` is the human/agent variant (checks the index via `git show :path`); CI runs
`check_secrets.py` without it, against the worktree.

## Step-by-Step Commit Flow

1. **Stage specific files** — never use `git add .` or `git add -A`:
   ```bash
   git add src/path/to/file.c components/path/to/other.c
   ```

2. **Analyze the staged diff** to determine type:
   ```bash
   git diff --cached --stat
   git diff --cached
   ```

3. **Determine type** by asking:
   - New user-facing functionality? -> `feat`
   - Fixes a bug? -> `fix`
   - Performance improvement? -> `perf`
   - Code restructure, no behavior change? -> `refactor`
   - Only docs/comments? -> `docs`
   - CI/CD work? -> `ci`
   - Deps, config, tooling? -> `chore`

4. **Determine scope** from which directory or module the changes concentrate in

5. **Write the description** in imperative mood

6. **Commit** using a heredoc, with whatever `Co-Authored-By` trailer the current harness
   specifies (do not hardcode a model name — it goes stale):
   ```bash
   git commit -m "$(cat <<'EOF'
   fix(wifi): reconnect after ASSOC_LEAVE without a 15s timeout

   Co-Authored-By: <harness-specified trailer>
   EOF
   )"
   ```

## Safety Rules

- **Never commit unless the user explicitly asks** (see above).
- **Always create new commits.** Never amend unless the user explicitly asks.
- If a pre-commit hook fails, the commit did NOT happen. Do NOT use `--amend` afterward — that would
  modify the **previous** (unrelated) commit. Fix the issue and create a new commit.
- Never use `--no-verify` to skip hooks unless the user explicitly requests it.
- Never force-push unless the user explicitly requests it.

## PR Titles

Since squash merging makes the PR title the commit on `main`, apply the same Conventional Commits
format:
```
fix(wifi): reconnect after ASSOC_LEAVE without a 15s timeout
feat(settings): add timezone offset range item
```

When a change encodes a non-obvious tradeoff — one that's hard to reverse, would surprise a future
reader without context, and was a real choice among genuine alternatives — record it as an ADR in
`docs/adr/` following the `mattpocock-skills` domain-modeling convention (sequentially numbered
`NNNN-slug.md`, minimal template: title + a few sentences of context/decision/why). Superseded
decisions get a `Status: superseded by ADR-NNNN` frontmatter line rather than being deleted — ADRs
are a record of what was decided and why, not a live "current state" doc. Smaller gotchas that don't
clear that bar belong in the nearest relevant code comment or in CLAUDE.md's Non-Architectural Notes
section instead.

## Real Examples from This Project

```
fix(ui): read firmware version from app descriptor instead of hardcoded literal
fix(security): stop plaintext password leak via unterminated SSID cast
fix(deps): upgrade esp_lvgl_port to 2.9.0, pin platform, bump checkout
refactor(ui): extract shared scroll/timer helpers, collapse show_*_screen()
refactor(ui): unify settings/menu row indices under one enum, add screen destroy()
feat(deep-sleep): cut LCD rail and latch it through sleep
test(native): add Unity host test harness for ui_circ_next/prev and timezone_fmt
docs: replace audit backlog with a decisions record
docs(security): close credential leak path, add audit backlog and provenance
ci(release): add dependabot for Actions, ship bootloader.bin+checksums, document rollback/PAT decisions
chore(cleanup): remove 6 dead public API symbols
```

## Quick Reference

```
Branch:  <prefix>/<2-4-word-kebab-description>
         Prefixes: feature/ fix/ chore/ ci/ docs/ refactor/ perf/

Commit:  <type>(<scope>): <lowercase imperative description, no period, <72 chars>
         Version bump: feat=MINOR, fix/perf=PATCH
         Visible, no bump: refactor, docs
         Hidden from changelog: chore, style, test, ci, build
         Breaking: add ! before colon OR BREAKING CHANGE: footer
```
