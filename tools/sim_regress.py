#!/usr/bin/env python3
"""Shell-flow regression via the simulator's --drive hook.

Each scenario walks a screen flow, asserts the active screen (and important
published overlay text), then ends with `quit`. A mismatch, crash, or timeout
fails the scenario. Keyboard steps where possible - tile coordinates go
stale. Only the `touch` scenario uses them, and it lists its points.
Each scenario gets a fresh temporary copy of sim_storage.example.
Run from the repo root after building the simulator:

    python tools/sim_regress.py [-k PATTERN] [-j N] [--window] [simulator]

The scenarios run headless, side by side: no window, and a wait costs
no wall time. `-k` keeps the scenarios whose name matches a regular
expression. `--window` shows each run in a window at real speed.
"""

import argparse
import re
import subprocess
import sys
import shutil
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SIM_DEFAULT = ROOT / "build-sim/sim/cyberdeck_sim.exe"


def timeout_s(script, window):
    """Per-scenario budget. In a window, steps fire about 450 ms apart
    after a 2.5 s boot delay. That budget scales with the script.
    A headless run takes a second or two."""
    return 8 + 0.6 * (script.count("|") + 1) if window else 30

# Esc first: skips the boot splash, harmless if HOME is already up.
# Never send a bare Enter on HOME - it would start a real SSH connect.
SCENARIOS = {
    # boot -> HOME, arrow navigation, profile reload + wifi kick keys
    "home-nav":
        "key:esc|wait:400|expect:home|expect-text:Hosts"
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
    # HOME -> Setup pill -> MENU; also verifies the published overlay is
    # the menu frame, catching an accidental second ui_present().
    "config-menu-roundtrip":
        "key:esc|wait:400|expect:home|key:up|key:right|key:enter"
        "|wait:500|expect:menu"
        "|expect-text:CONFIGURATION|key:esc|wait:500|expect:home|quit",
    # Walk the two-column hub: CRT FX, FONT, SYSTEM. Cycling the CRT FX
    # value tile toggles [fx] scanlines and flushes them on page exit. The
    # breadcrumb lands back on CONFIGURATION each time. The hub uses a
    # 2-col row-major layout. Order: 0 PROFILES, 1 WIFI, 2 KEYBOARD,
    # 3 CRT FX, 4 MOTION FX, 5 FONT, 6 SYSTEM, 7 KEYSTORE.
    "config-pages":
        "key:esc|wait:400|expect:home|key:up|key:right|key:enter"
        "|wait:500|expect:menu"
        "|expect-text:CONFIGURATION"
        "|key:down|key:right|key:enter|wait:400|expect-text:CRT FX"
        "|key:enter|wait:300|key:esc|wait:400|expect-text:CONFIGURATION"
        "|key:down|key:down|key:right|key:enter|wait:400"
        "|expect-text:FONT|key:esc|wait:400|expect-text:CONFIGURATION"
        "|key:down|key:down|key:down|key:enter"
        "|wait:400|expect-text:SYSTEM|key:esc|wait:400"
        "|expect-text:CONFIGURATION|key:esc|wait:500|expect:home|quit",
    # Touch: a finger-down alone does nothing. A long press on a tile
    # opens its host page. Taps walk the menu; its header steps back.
    # Points (100x30 grid): 130,64 first HOME tile; 744,24 Setup pill;
    # 150,90 first menu card; 10,8 header back.
    "touch":
        "key:esc|wait:400|expect:home|press:130,64|wait:300|expect:home"
        "|release:130,64|hold:130,64|wait:500|expect:host"
        "|key:esc|wait:500|expect:home|tap:744,24|wait:500|expect:menu"
        "|expect-text:MOTION FX|press:150,90|wait:300|expect-text:MOTION FX"
        "|release:150,90|press:150,90|tap:150,90|wait:400"
        "|expect-text:Import|tap:10,8|wait:400"
        "|expect-text:MOTION FX|tap:10,8|wait:500|expect:home|quit",
    # One tap on a tile connects. The fixture edit points it at a closed
    # loopback port. Esc cancels; the SSH thread needs real time to stop.
    "tap-connect":
        "key:esc|wait:500|expect:home|tap:130,64|wait:150"
        "|expect:connecting|key:esc|sleep:300|wait:700|expect:home|quit",
    # Space opens the host page. Enter or a digit would connect.
    "host-page":
        "key:esc|wait:400|expect:home|key: |wait:400|expect:host"
        "|expect-text:example|expect-text:Connect|expect-text:192.168.1.10"
        "|expect-text:not pinned yet|key:esc|wait:400|expect:home|quit",
    # Delete is two keys right of Connect. It asks first; Cancel is the
    # default. The second pass confirms.
    "host-delete":
        "key:esc|wait:400|expect:home|expect-text:2 of 8|key: |wait:400"
        "|expect:host|key:right|key:right|key:enter|wait:400|expect:ask"
        "|expect-text:Delete example?|key:enter|wait:400|expect:host"
        "|key:enter|wait:400|expect:ask|key:right|key:enter|wait:500"
        "|expect:home|expect-text:1 of 8|quit",
    # Down from Connect reaches Move later (Move earlier is off on the
    # first host). Then the first HOME tile is the other host.
    "host-move":
        "key:esc|wait:400|expect:home|key: |wait:400|expect:host"
        "|expect-text:192.168.1.10|key:down|key:enter"
        "|wait:400|expect:host|expect-text:192.168.1.10|key:esc|wait:400"
        "|expect:home|key: |wait:400|expect:host|expect-text:10.0.0.5|quit",
    # The release after a hold must not connect.
    "hold-opens-host":
        "key:esc|wait:400|expect:home|hold:130,64|wait:400|expect:host"
        "|release:130,64|wait:400|expect:host|expect-text:Connect"
        "|key:esc|wait:400|expect:home|quit",
}

# Per-scenario edits to the fixture's profiles.ini (old text, new text).
SECOND_HOST = ("#[server]\n#host=server.local\n#port=22\n#user=me\n"
               "#auth=key\n#key_id=id_deck",
               "[server]\nhost=10.0.0.5\nport=22\nuser=ops\n"
               "auth=password\npassword=secret")
FIXTURE_EDITS = {
    "tap-connect": [("host=192.168.1.10", "host=127.0.0.1"),
                    ("port=22", "port=1")],
    "hold-opens-host": [("host=192.168.1.10", "host=127.0.0.1"),
                        ("port=22", "port=1")],
    "host-delete": [SECOND_HOST],
    "host-move": [SECOND_HOST],
}


def run(sim, name, script, window):
    """Run one scenario on its own storage copy. Returns (ok, output)."""
    try:
        with tempfile.TemporaryDirectory(prefix="deck-regress-") as tmp:
            fixture = Path(tmp) / "storage"
            shutil.copytree(ROOT / "sim_storage.example", fixture)
            ini = fixture / "profiles.ini"
            text = ini.read_text(encoding="utf-8")
            for old, new in FIXTURE_EDITS.get(name, []):
                if old not in text:
                    return False, f"fixture lacks '{old}'"
                text = text.replace(old, new, 1)
            ini.write_text(text, encoding="utf-8")
            mode = [] if window else ["--headless"]
            r = subprocess.run(
                [str(sim), *mode, "--storage-dir", str(fixture),
                 "--drive", script],
                timeout=timeout_s(script, window), cwd=tmp,
                capture_output=True, text=True, errors="replace",
            )
            return r.returncode == 0, r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        return False, "timed out"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("sim", nargs="?", default=str(SIM_DEFAULT))
    ap.add_argument("-k", metavar="PATTERN", default="",
                    help="run the scenarios whose name matches")
    ap.add_argument("-j", metavar="N", type=int, default=8,
                    help="scenarios side by side (default 8)")
    ap.add_argument("--window", action="store_true",
                    help="show each run in a window, at real speed")
    args = ap.parse_args()

    sim = Path(args.sim).resolve()
    if not sim.exists():
        print(f"simulator not found: {sim} (build it first)")
        return 2
    names = [n for n in SCENARIOS if re.search(args.k, n)]
    if not names:
        print(f"no scenario matches '{args.k}'")
        return 2

    jobs = 1 if args.window else max(1, args.j)
    with ThreadPoolExecutor(jobs) as pool:
        results = list(pool.map(
            lambda n: run(sim, n, SCENARIOS[n], args.window), names))

    failed = []
    for name, (ok, out) in zip(names, results):
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
        if not ok:
            failed.append(name)
            # The simulator names the failed step on a "drive:" line.
            lines = [l for l in out.splitlines() if "drive:" in l]
            for line in lines[-3:] or out.splitlines()[-3:]:
                print(f"      {line}")

    print(f"\n{len(names) - len(failed)}/{len(names)} scenarios pass")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
