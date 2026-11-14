#!/usr/bin/env python3
"""Screenshots of shell screens from the simulator, without a window.

Runs one --drive script per font on a fresh copy of sim_storage.example,
headless and with the display effects off. Each `shot:NAME` step writes
NAME-<font>.png into the output directory (BMP when Pillow is missing).
The run fails on a simulator error or a "layout faults" log line.

    python tools/sim_shots.py --out shots --hosts 8 \\
        "key:esc|wait:300|expect:home|shot:home|key: |wait:300|shot:host"

Options: --font picks the fonts (default: all three). --hosts N stores N
example hosts first. --fx keeps the display effects. --sim names the
simulator binary.
"""

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SIM_DEFAULT = ROOT / "build-sim/sim/cyberdeck_sim.exe"
FONTS = ["8x16", "10x20", "12x24"]

# The storage limit is eight hosts. Names and addresses of mixed length.
HOSTS = [
    ("homelab", "root", "192.168.1.10", 22),
    ("build-box", "ci", "build.lan", 22),
    ("pi-garage", "pi", "10.0.0.42", 22),
    ("vps-fra", "deploy", "fra1.example.net", 2222),
    ("router", "admin", "192.168.1.1", 22),
    ("nas", "backup", "nas.lan", 22),
    ("printer", "octo", "10.0.0.77", 22),
    ("bastion", "ops", "bastion.example.org", 22),
]


def profiles_ini(count):
    return "".join(
        f"[{name}]\nhost={host}\nport={port}\nuser={user}\n"
        "auth=password\npassword=x\n\n"
        for name, user, host, port in HOSTS[:count])


def shoot(sim, font, script, hosts, fx, out):
    """Run the script at one font. Returns (ok, log lines worth showing)."""
    with tempfile.TemporaryDirectory(prefix="deck-shots-") as tmp:
        fixture = Path(tmp) / "storage"
        shutil.copytree(ROOT / "sim_storage.example", fixture)
        if hosts is not None:
            (fixture / "profiles.ini").write_text(profiles_ini(hosts),
                                                  encoding="utf-8")
        steps = re.sub(r"(^|\|)shot:([^|]+)",
                       lambda m: f"{m.group(1)}shot:{m.group(2)}-{font}.bmp",
                       script)
        mode = ["--headless"] + ([] if fx else ["--no-fx"])
        try:
            r = subprocess.run(
                [str(sim), *mode, "--font", font, "--storage-dir",
                 str(fixture), "--drive", steps],
                timeout=60, cwd=tmp, capture_output=True, text=True,
                errors="replace")
        except subprocess.TimeoutExpired:
            return False, [f"{font}: timed out"]
        log = (r.stdout + r.stderr).splitlines()
        notes = [f"{font}: {l}" for l in log
                 if "drive:" in l or "layout faults" in l]
        for bmp in Path(tmp).glob("*.bmp"):
            save(bmp, out)
        ok = r.returncode == 0 and not any("layout faults" in n for n in notes)
        if r.returncode:
            notes.append(f"{font}: simulator exit {r.returncode}")
        return ok, notes


def save(bmp, out):
    try:
        from PIL import Image
    except ImportError:
        shutil.copy(bmp, out / bmp.name)
        return
    Image.open(bmp).save(out / (bmp.stem + ".png"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("script", help="--drive steps; shot:NAME takes a picture")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--font", action="append", choices=FONTS,
                    help="font to shoot; repeat for more (default: all)")
    ap.add_argument("--hosts", type=int, choices=range(0, len(HOSTS) + 1),
                    metavar="N", help="store N example hosts first")
    ap.add_argument("--fx", action="store_true",
                    help="keep the display effects on")
    ap.add_argument("--sim", default=str(SIM_DEFAULT))
    args = ap.parse_args()

    sim = Path(args.sim).resolve()
    if not sim.exists():
        print(f"simulator not found: {sim} (build it first)")
        return 2
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    fonts = args.font or FONTS

    with ThreadPoolExecutor(len(fonts)) as pool:
        results = list(pool.map(
            lambda f: shoot(sim, f, args.script, args.hosts, args.fx, out),
            fonts))
    bad = 0
    for ok, notes in results:
        bad += not ok
        for note in notes:
            print(note)
    print(f"{len(fonts) - bad}/{len(fonts)} fonts ok, shots in {out}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
