#!/usr/bin/env python3
"""Run `pio check` for CI, guarding against .clang-tidy / platformio.ini drift.

The actual check_tool/check_flags/check_src_filters configuration lives in platformio.ini —
that is what CLion's "Static Code Analysis" action runs too, and it cannot be pointed at a
wrapper script from the UI. .clang-tidy still owns the Checks: list for clangd's live linting.
Both files must declare the exact same checks string; there is no way to derive one from the
other in two static config files, so this script verifies they match on every run instead of
letting them silently drift, then adds --fail-on-defect (not settable via platformio.ini) for
CI's benefit.

Usage:
    python3 scripts/pio_check.py                 # drift check + gated pio check
    python3 scripts/pio_check.py --json-output    # extra pio-check flags pass through
"""

import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ENV = "lilygo-t-display-s3"


def _checks_from_clang_tidy() -> str:
    content = (REPO_ROOT / ".clang-tidy").read_text()
    match = re.search(r'Checks:\s*"(.*?)"', content, re.DOTALL)
    if not match:
        print("pio_check: could not find a Checks: value in .clang-tidy", file=sys.stderr)
        sys.exit(1)
    # Checks: "-*,\n  bugprone-*,\n  ..." — collapse the line-continuation folding.
    return re.sub(r"\\\s*\n\s*", "", match.group(1))


def _checks_from_platformio_ini() -> str:
    content = (REPO_ROOT / "platformio.ini").read_text()
    match = re.search(r"^check_flags\s*=\s*clangtidy:\s*-checks=(.*)$", content, re.MULTILINE)
    if not match:
        print("pio_check: could not find check_flags = clangtidy: -checks=... in platformio.ini", file=sys.stderr)
        sys.exit(1)
    return match.group(1).strip()


def main() -> int:
    from_tidy = _checks_from_clang_tidy()
    from_ini = _checks_from_platformio_ini()
    if from_tidy != from_ini:
        print("pio_check: .clang-tidy and platformio.ini's check_flags have drifted apart:", file=sys.stderr)
        print(f"  .clang-tidy:     {from_tidy}", file=sys.stderr)
        print(f"  platformio.ini:  {from_ini}", file=sys.stderr)
        print("Update both to the same Checks= string.", file=sys.stderr)
        return 1

    cmd = ["pio", "check", "-e", ENV, "--fail-on-defect", "medium", *sys.argv[1:]]
    return subprocess.call(cmd, cwd=REPO_ROOT)


if __name__ == "__main__":
    sys.exit(main())
