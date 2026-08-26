#!/usr/bin/env python3
"""Shell-flow regression via the simulator's --drive hook.

Each scenario walks a screen flow, asserts the active screen (and important
published overlay text), then ends with `quit`. A mismatch, crash, or timeout
fails the scenario. Keyboard-only steps on purpose - no tile coordinates to
go stale. Run from the repo root after building the simulator:

    python tools/sim_regress.py [path\\to\\cyberdeck_sim.exe]
"""

import subprocess
import sys
from pathlib import Path

SIM_DEFAULT = Path("build-sim/sim/cyberdeck_sim.exe")
# Every drive step fires ~450 ms apart after a 2.5 s boot delay, so the
# budget must scale with script length (config-pages is ~50 steps ≈ 25 s).
TIMEOUT_S = 45

# Esc first: skips the boot splash, harmless if HOME is already up.
# Never send a bare Enter on HOME - it would start a real SSH connect.
SCENARIOS = {
    # boot -> HOME, arrow navigation, profile reload + wifi kick keys
    "home-nav":
        "key:esc|wait:400|expect:home|expect-text:CYBERDECK"
        "|key:right|key:down|key:left|key:up|key:r|key:w"
        "|wait:600|expect:home|quit",
    # HOME -> profile editor (push) -> Esc (pop) -> HOME
    "editor-roundtrip":
        "key:esc|wait:400|expect:home|key:n|wait:500|expect:profile"
        "|expect-text:NEW PROFILE|key:esc|wait:500|expect:home|quit",
    # editor field focus + typing + selector row, then cancel
    "editor-typing":
        "key:esc|wait:400|key:n|wait:400|expect:profile|key:a|key:b"
        "|key:tab|key:h|key:tab|key:down|key:down|key:left|wait:300"
        "|expect:profile|key:esc|wait:400|expect:home|quit",
    # HOME -> last tile (Configuration) -> MENU; also verifies the published
    # overlay is the menu frame, catching an accidental second ui_present().
    "config-menu-roundtrip":
        "key:esc|wait:400|expect:home|key:down|key:down|key:down|key:down"
        "|key:right|key:right|key:enter|wait:500|expect:menu"
        "|expect-text:CONFIGURATION|key:esc|wait:500|expect:home|quit",
    # Walk the table-driven pages: EFFECTS (cycle a value tile - toggles
    # [fx] scanlines, flushed on page exit), FONT, SYSTEM; back_to lands
    # on CONFIGURATION each time. Exercises item tables + value rendering.
    "config-pages":
        "key:esc|wait:400|expect:home|key:down|key:down|key:down|key:down"
        "|key:right|key:right|key:enter|wait:500|expect:menu"
        "|expect-text:CONFIGURATION"
        "|key:down|key:down|key:down|key:enter|wait:400|expect-text:EFFECTS"
        "|key:enter|wait:300|key:esc|wait:400|expect-text:CONFIGURATION"
        "|key:down|key:down|key:down|key:down|key:enter|wait:400"
        "|expect-text:FONT|key:esc|wait:400|expect-text:CONFIGURATION"
        "|key:down|key:down|key:down|key:down|key:down|key:down|key:enter"
        "|wait:400|expect-text:SYSTEM|key:esc|wait:400"
        "|expect-text:CONFIGURATION|key:esc|wait:500|expect:home|quit",
}


def main():
    sim = Path(sys.argv[1]) if len(sys.argv) > 1 else SIM_DEFAULT
    if not sim.exists():
        print(f"simulator not found: {sim} (build it first)")
        return 2

    failed = []
    for name, script in SCENARIOS.items():
        try:
            r = subprocess.run([str(sim), "--drive", script],
                               timeout=TIMEOUT_S, cwd=Path.cwd())
            ok = r.returncode == 0
        except subprocess.TimeoutExpired:
            ok = False
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
        if not ok:
            failed.append(name)

    print(f"\n{len(SCENARIOS) - len(failed)}/{len(SCENARIOS)} scenarios pass")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
