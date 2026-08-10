#!/usr/bin/env python3
"""Fail if a credential Kconfig option carries a real value in a git-tracked sdkconfig.

`pio run -t menuconfig` writes secrets straight into sdkconfig.lilygo-t-display-s3, which is
tracked. This guard is what stops one absent-minded `git commit -a` from publishing a live
Tailscale auth key. Run it manually, from a pre-commit hook, or from CI.

Usage:
    python3 scripts/check_secrets.py            # check tracked sdkconfig files
    python3 scripts/check_secrets.py --staged   # check what is staged for commit instead
"""

import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Kconfig options that must stay empty in any tracked file.
SECRET_KEYS = (
    "CONFIG_ML_TAILSCALE_AUTH_KEY",
    "CONFIG_ML_WIFI_SSID",
    "CONFIG_ML_WIFI_PASSWORD",
)

# Values that are empty or obviously placeholders — not a leak.
PLACEHOLDERS = {"", "tskey-auth-xxxxxxxxxxxx", "xxxx", "changeme", "your-key-here"}

ASSIGNMENT = re.compile(r'^\s*(CONFIG_[A-Z0-9_]+)\s*=\s*"(.*)"\s*$')


def tracked_sdkconfigs():
    out = subprocess.run(
        ["git", "ls-files", "sdkconfig*"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    return [REPO_ROOT / line for line in out.splitlines() if line.strip()]


def read_source(path, staged):
    """Return file text — from the git index when --staged, else from the worktree."""
    rel = path.relative_to(REPO_ROOT)
    if not staged:
        return path.read_text(encoding="utf-8", errors="replace")
    result = subprocess.run(
        ["git", "show", f":{rel}"], cwd=REPO_ROOT, capture_output=True, text=True
    )
    # Not in the index (e.g. deleted or never staged) — nothing to check.
    return result.stdout if result.returncode == 0 else ""


def scan(path, staged):
    findings = []
    for lineno, line in enumerate(read_source(path, staged).splitlines(), start=1):
        match = ASSIGNMENT.match(line)
        if not match:
            continue
        key, value = match.group(1), match.group(2)
        if key in SECRET_KEYS and value not in PLACEHOLDERS:
            # Show only a prefix — never print the full secret into a CI log.
            redacted = value[:8] + "…" if len(value) > 8 else value
            findings.append((lineno, key, redacted))
    return findings


def main():
    staged = "--staged" in sys.argv[1:]
    files = tracked_sdkconfigs()
    if not files:
        print("check_secrets: no tracked sdkconfig files found")
        return 0

    leaks = []
    for path in files:
        for lineno, key, redacted in scan(path, staged):
            leaks.append((path.relative_to(REPO_ROOT), lineno, key, redacted))

    if not leaks:
        where = "staged changes" if staged else "worktree"
        print(f"check_secrets: OK — no credentials set in {len(files)} tracked sdkconfig ({where})")
        return 0

    print("check_secrets: CREDENTIAL FOUND IN A TRACKED FILE — commit blocked\n", file=sys.stderr)
    for rel, lineno, key, redacted in leaks:
        print(f"  {rel}:{lineno}  {key}=\"{redacted}\"", file=sys.stderr)
    print(
        "\nBlank these values before committing, or keep your key in an untracked sdkconfig.\n"
        "See README.md → Tailscale Credentials.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
