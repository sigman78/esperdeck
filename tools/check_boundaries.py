#!/usr/bin/env python3
"""Component-boundary guard: the include graph diffed against the allowed
edges (docs/ARCHITECTURE.md "Component boundaries" table).

Runs on sources only -- no toolchain needed, same result on device and
simulator builds. Fails the build when a component grows an include edge
the table does not sanction, so a fixed edge stays fixed and a widened
contract is a decision, not an accident.

Usage: check_boundaries.py [repo_root]
"""

import re
import sys
from pathlib import Path

# Allowed edges: component -> components it may include headers from.
# Keep in step with docs/ARCHITECTURE.md; edges marked DEBT carry their
# repair plan in docs/extensibility.md and shrink this list when fixed.
# Out of scan scope (they cross through FETCHED sources, not in-tree
# files): ssh -> libssh2_esp (libssh2.h lives in the fetched fork) and
# libssh2_esp -> monocypher (the fork's mbedtls.c does the include).
ALLOWED = {
    "vterm":         {"tsm", "display"},   # render data plane, fused by design
    "display":       {"font"},
    "ssh":           {"vterm",             # DEBT: drain loop feeds the terminal (item 7)
                      "display"},          # DEBT: PTY geometry, undeclared (item 7)
    "cyberdeck_app": {"storage", "ssh", "wifi", "display", "vterm", "font"},
    "wifi":          {"storage"},
    "input":         {"storage", "esp_hid",
                      "display"},          # tolerable: DISPLAY_WIDTH for the edge strip
    "storage":       {"monocypher"},
}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
SOURCE_EXTS = {".c", ".h", ".inc"}


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent.parent
    comp_root = root / "components"

    # Header basename -> (component, is_public). Basenames must be unique
    # across components or the include resolution below is ambiguous.
    # A component without an include/ dir (flat vendored layout, e.g.
    # monocypher's INCLUDE_DIRS .) exports every header it has.
    owner = {}
    for comp_dir in sorted(comp_root.iterdir()):
        if not comp_dir.is_dir():
            continue
        has_include = any(p.is_dir() and p.name == "include"
                          for p in comp_dir.rglob("include"))
        for h in comp_dir.rglob("*.h"):
            rel = h.relative_to(comp_dir)
            public = "include" in rel.parts[:-1] or not has_include
            prev = owner.get(h.name)
            if prev and prev[0] != comp_dir.name:
                print(f"check_boundaries: header name collision: {h.name} "
                      f"in {prev[0]} and {comp_dir.name}")
                sys.exit(1)
            # A private header never shadows the same component's public one.
            if not prev or public:
                owner[h.name] = (comp_dir.name, public)

    errors = []
    seen = {}   # (src_comp, dst_comp) -> first "file:line" for the report
    for comp_dir in sorted(comp_root.iterdir()):
        if not comp_dir.is_dir():
            continue
        src = comp_dir.name
        for f in comp_dir.rglob("*"):
            if f.suffix not in SOURCE_EXTS or not f.is_file():
                continue
            text = f.read_text(encoding="utf-8", errors="replace")
            for m in INCLUDE_RE.finditer(text):
                name = Path(m.group(1)).name
                hit = owner.get(name)
                if not hit or hit[0] == src:
                    continue        # system/IDF header, or a self include
                dst, public = hit
                line = text.count("\n", 0, m.start()) + 1
                where = f"{f.relative_to(root)}:{line}"
                seen.setdefault((src, dst), where)
                if dst not in ALLOWED.get(src, set()):
                    errors.append(f"  {src} -> {dst}  ({where}: \"{m.group(1)}\")")
                elif not public:
                    errors.append(f"  {src} -> {dst} PRIVATE header {name}  ({where})")

    stale = [f"  {s} -> {d}" for s, dsts in ALLOWED.items() for d in dsts
             if (s, d) not in seen]

    if errors:
        print("check_boundaries: FAIL -- include edges outside "
              "docs/ARCHITECTURE.md's boundary table:")
        print("\n".join(sorted(set(errors))))
        print("Either remove the include or amend the table AND this "
              "script's ALLOWED list (a widened contract is a decision).")
        sys.exit(1)
    if stale:
        print("check_boundaries: FAIL -- ALLOWED edges no longer present "
              "(prune here and in docs/ARCHITECTURE.md):")
        print("\n".join(sorted(stale)))
        sys.exit(1)

    print(f"check_boundaries: OK -- {len(seen)} component edges, "
          f"all in the boundary table")


if __name__ == "__main__":
    main()
