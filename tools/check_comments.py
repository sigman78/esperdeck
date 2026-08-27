#!/usr/bin/env python3
"""Comment lint wrapper: runs `uncomment` (github.com/sigman78/uncomment) via
uvx over first-party sources only. Config in uncomment.toml; rationale in
docs/DEVELOPMENT.md, "Comment lint".

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

# Vendored / third-party components: not ours to relint.
VENDORED = {"esp_hid", "monocypher", "libssh2_esp", "terminal"}
# Path components that mark generated / fetched trees. uncomment does not
# read .gitignore, so this script filters build outputs itself.
SKIP_DIRS = {"_deps", "__pycache__", "managed_components", ".git", ".cache"}
EXTS = {".c", ".h", ".cpp", ".hpp", ".py"}


def first_party_roots():
    comps = sorted(p for p in (ROOT / "components").iterdir()
                   if p.is_dir() and p.name not in VENDORED)
    extra = [ROOT / d for d in ("main", "sim", "idfsim", "tools", "tests")]
    return comps + [d for d in extra if d.is_dir()]


def skipped(rel):
    return any(part in SKIP_DIRS or part.startswith("build")
               for part in rel.parts)


def first_party_files():
    files = []
    for root in first_party_roots():
        for p in sorted(root.rglob("*")):
            rel = p.relative_to(ROOT)
            if p.suffix.lower() in EXTS and not skipped(rel):
                files.append(str(rel))
    return files


def in_scope(path_str):
    p = Path(path_str).resolve()
    if p.suffix.lower() not in EXTS:
        return False
    try:
        rel = p.relative_to(ROOT)
    except ValueError:
        return False
    if skipped(rel):
        return False
    return any(root == p or root in p.parents for root in first_party_roots())


def run(args):
    return subprocess.run(UNCOMMENT + args + first_party_files(),
                          cwd=ROOT).returncode


def hook_mode():
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, OSError):
        return 0
    path = (payload.get("tool_input") or {}).get("file_path", "")
    if not path or not in_scope(path):
        return 0
    # Repo-relative path: uncomment resolves the git baseline from it.
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
