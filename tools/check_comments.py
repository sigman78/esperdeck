#!/usr/bin/env python3
"""Comment lint wrapper: runs `uncomment` (github.com/sigman78/uncomment) via
uvx. Scope lives in uncomment.toml (exclude globs, respect-gitignore,
skip-generated); this wrapper only picks the scan root and adapts the
Claude Code hook. Rationale in docs/DEVELOPMENT.md, "Comment lint".

Modes:
  check_comments.py                   gate comments changed vs origin/master.
  check_comments.py --baseline REF    gate vs another git ref.
  check_comments.py --check           full scan (backlog view, not a gate).
  check_comments.py --hook            Claude Code PostToolUse hook. Reads the
                                      hook JSON on stdin and gates the edited
                                      file vs git HEAD. Exit 2 feeds findings
                                      back.

Exit codes follow uncomment: 0 clean, 1 findings, 2 hook-feedback/bad input.
"""

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UNCOMMENT = ["uvx", "--from", "git+https://github.com/sigman78/uncomment",
             "uncomment"]

# Hook-only filter. A file named explicitly on the CLI always scans.
# Naming is intent, so the config excludes cannot cover the per-file hook
# path. This mirrors uncomment.toml's exclude list plus build/fetched trees.
VENDORED = {"esp_hid", "monocypher", "libssh2_esp", "terminal"}
SKIP_DIRS = {"_deps", "__pycache__", "managed_components", ".git", ".cache"}
EXTS = {".c", ".h", ".cpp", ".hpp", ".py"}


def in_scope(path_str):
    p = Path(path_str).resolve()
    if p.suffix.lower() not in EXTS:
        return False
    try:
        rel = p.relative_to(ROOT)
    except ValueError:
        return False
    return not any(part in VENDORED or part in SKIP_DIRS
                   or part.startswith("build") for part in rel.parts)


def run(args):
    return subprocess.run(UNCOMMENT + args + ["."], cwd=ROOT).returncode


def hook_mode():
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, OSError):
        return 0
    path = (payload.get("tool_input") or {}).get("file_path", "")
    if not path or not in_scope(path):
        return 0
    rel = Path(path).resolve().relative_to(ROOT)
    proc = subprocess.run(
        UNCOMMENT + ["gate", str(rel),
                     "--baseline", "git:HEAD", "--format", "agent"],
        cwd=ROOT, capture_output=True, text=True, encoding="utf-8",
        errors="replace")
    if proc.returncode == 1:
        sys.stderr.write(proc.stdout + proc.stderr)
        return 2  # PostToolUse contract: exit 2 surfaces stderr to the agent
    return 0


def main(argv):
    if "--hook" in argv:
        return hook_mode()
    if "--check" in argv:
        return run(["check"])
    baseline = "git:origin/master"
    if "--baseline" in argv:
        ref = argv[argv.index("--baseline") + 1]
        baseline = ref if ref.startswith("git:") else "git:" + ref
    return run(["gate", "--baseline", baseline, "--format", "agent"])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
