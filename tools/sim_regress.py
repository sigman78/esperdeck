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

PAD_OPEN = ("key:esc|wait:400|tap:744,24|wait:500|key:left|key:down|key:down"
            "|key:right|key:enter|wait:300|expect-text:Create keystore"
            "|key:enter|wait:300|expect:unlock")
PAD_MAKE = (PAD_OPEN + "|type:1234|key:enter|wait:300|type:1234|sleep:2500"
            "|wait:700|expect:menu|expect-text:Lock deck")

HUB = ("key:esc|wait:400|expect:home|key:up|key:right|key:enter|wait:500"
       "|expect:menu")
WIFI_OPEN = (HUB + "|key:right|key:enter|wait:300|expect-text:Setup / Wi-Fi"
             "|key:right|key:enter|wait:400|expect:wifiprov")
PAIR_OPEN = "key:esc|wait:400|expect:home|key:b|wait:200|expect:pairing"

# Esc first: skips the boot splash, harmless if HOME is already up.
# Never send a bare Enter on HOME - it would start a real SSH connect.
SCENARIOS = {
    # boot -> HOME, arrow navigation, profile reload + wifi kick keys
    "home-nav":
        "key:esc|wait:400|expect:home|expect-text:Hosts"
        "|key:right|key:down|key:left|key:up|key:r|key:w"
        "|wait:600|expect:home|quit",
    # An unchanged form leaves on one Esc, with no question.
    "editor-roundtrip":
        "key:esc|wait:400|expect:home|key:n|wait:500|expect:profile"
        "|expect-text:New host|key:esc|wait:500|expect:home|quit",
    "editor-typing":
        "key:esc|wait:400|key:n|wait:400|expect:profile|type:wxz|key:left"
        "|type:y|expect-text:wxyz|key:bs|expect-text:wxz|key:esc|wait:300"
        "|expect:profile|key:tab|type:bob|key:tab|type:h|key:tab|type:x"
        "|type:5|expect-text:225|key:esc|key:esc"
        "|wait:300|expect:ask|key:esc|wait:300|expect:profile|quit",
    "editor-new":
        "key:esc|wait:400|expect:home|key:n|wait:400|expect:profile"
        "|type:nas|key:tab|type:me|key:tab|type:10.0.0.9|key:tab|key:tab"
        "|key:tab|type:pw|key:tab|key:enter|wait:200|expect-text:On"
        "|key:tab|key:enter|wait:500|expect:home|expect-text:2 of 8|quit",
    "editor-dup":
        "key:esc|wait:400|key:n|wait:400|type:example|key:tab|type:me"
        "|key:tab|type:h|key:tab|key:tab|key:tab|key:tab|key:tab|key:enter"
        "|wait:300|expect:profile|expect-text:That name is in use.|quit",
    "editor-edit":
        "key:esc|wait:400|expect:home|key: |wait:400|expect:host"
        "|key:right|key:enter|wait:400|expect:profile|expect-text:Edit example"
        "|type:2|key:tab|key:tab|key:tab|key:tab|key:tab|key:tab|key:tab"
        "|key:enter|wait:500|expect:host|expect-text:example2|quit",
    # Save is off. A tap there does nothing. A tap hides the focus ring,
    # so the question page needs a left key first.
    "editor-bad-port":
        "key:esc|wait:400|key:n|wait:400|type:nas|key:tab|type:me|key:tab"
        "|type:h|key:tab|key:bs|key:bs"
        "|expect-text:Port: a number from 1 to 65535."
        "|key:esc|wait:300|tap:744,24|wait:300|expect:profile"
        "|expect-text:Port: a number|type:99999|expect-text:Port: a number"
        "|key:esc|key:esc|wait:300|expect:ask|key:left|key:right|key:enter"
        "|wait:400|expect:home|quit",
    "editor-discard":
        "key:esc|wait:400|expect:home|key:n|wait:400|expect:profile"
        "|type:abcd|key:esc|key:esc|wait:300|expect:ask"
        "|expect-text:Discard the changes?|expect-text:The new host is not stored."
        "|key:enter|wait:300|expect:profile|expect-text:abcd|key:esc|wait:300"
        "|expect:ask|key:right|key:enter|wait:500|expect:home"
        "|expect-text:1 of 8|quit",
    "editor-key":
        "key:esc|wait:400|key:n|wait:400|type:nas|key:tab|type:me|key:tab"
        "|type:h|key:tab|key:tab|key:right|key:enter|wait:300"
        "|expect-text:none stored|expect-text:No stored key. Setup"
        "|expect-text:Passphrase|key:esc|wait:300|expect:ask|key:right"
        "|key:enter|wait:400|expect:home|quit",
    # Points (100x30 grid): 100,72 Name; 300,120 Host; 230,168 key chip.
    "editor-tap":
        "key:esc|wait:400|expect:home|key:n|wait:400|expect:profile"
        "|tap:100,72|wait:200|type:zz|expect-text:zz|tap:300,120|wait:200"
        "|type:hh|expect-text:hh|tap:230,168|wait:300|expect-text:none stored"
        "|key:esc|wait:300|expect:ask|key:left|key:right|key:enter"
        "|wait:400|expect:home|quit",
    # HOME -> Setup pill -> the hub; also verifies the published overlay
    # is the hub frame, catching an accidental second ui_present().
    "config-menu-roundtrip":
        "key:esc|wait:400|expect:home|key:up|key:right|key:enter"
        "|wait:500|expect:menu|expect-text:Setup|expect-text:1 stored"
        "|expect-text:HomeAP  127.0.0.1|key:esc|wait:500|expect:home|quit",
    # Hub tiles, row-major, three columns: Hosts, Wi-Fi, Keyboard (off),
    # CRT FX, Motion FX, Font, System, Keystore. Two switches flip and a
    # chip picks Phosphor: the CRT FX note then reads "phosphor".
    "config-pages":
        "key:esc|wait:400|expect:home|key:up|key:right|key:enter"
        "|wait:500|expect:menu|expect-text:scanlines, bold pop"
        "|key:down|key:enter|wait:300|expect-text:Setup / CRT FX"
        "|key:enter|wait:200|key:right|key:enter|wait:200"
        "|key:down|key:left|key:left|key:left|key:right|key:enter|wait:200"
        "|key:esc|wait:400|expect-text:phosphor"
        "|key:right|key:enter|wait:300|expect-text:Setup / Motion FX"
        "|expect-text:Wobble|key:esc|wait:300|key:right|key:enter|wait:300"
        "|expect-text:Size (restarts the deck)|key:esc|wait:300|expect:menu"
        "|key:esc|wait:500|expect:home|quit",
    # A switch shows its new word at once.
    "config-switch":
        "key:esc|wait:400|expect:home|key:up|key:right|key:enter"
        "|wait:500|expect:menu|key:down|key:down|key:enter|wait:300"
        "|expect-text:Setup / System|key:down|key:left|expect-text:On"
        "|key:enter|wait:300|expect-text:Off|key:enter|wait:300"
        "|expect-text:On|key:esc|wait:300|key:esc|wait:500|expect:home|quit",
    # A destructive action asks on its own page; Cancel is the default.
    # A new font size asks too, and the active size does nothing.
    "config-destructive":
        "key:esc|wait:400|expect:home|key:up|key:right|key:enter"
        "|wait:500|expect:menu|key:down|key:down|key:enter|wait:300"
        "|expect-text:Setup / System|key:down|key:enter|wait:300|expect:ask"
        "|expect-text:Clear every pinned host key?|key:enter|wait:300"
        "|expect:menu|expect-text:Setup / System"
        "|key:enter|wait:300|expect:ask|key:right|key:enter|wait:300"
        "|expect:menu|expect-text:Setup / System|expect-text:host keys"
        "|key:esc|wait:300|key:up|key:right|key:right|key:enter|wait:300"
        "|expect-text:Size (restarts the deck)|key:enter|wait:300|expect:menu"
        "|key:right|key:enter|wait:300|expect:ask"
        "|expect-text:Restart with the 10x20 font?|key:esc|wait:300"
        "|expect:menu|expect-text:Size (restarts the deck)"
        "|key:enter|wait:300|key:right|key:enter|wait:300|expect:menu"
        "|expect-text:saved|key:esc|wait:300|key:esc|wait:500|expect:home|quit",
    # Touch: a finger-down alone does nothing; a long press opens the host
    # page; empty space does nothing. Points: 130,64 first HOME tile;
    # 744,24 Setup pill; 130,80 first hub tile; 40,24 header back.
    "touch":
        "key:esc|wait:400|expect:home|press:130,64|wait:300|expect:home"
        "|release:130,64|hold:130,64|wait:500|expect:host"
        "|key:esc|wait:500|expect:home|tap:744,24|wait:500|expect:menu"
        "|expect-text:1 stored|press:130,80|wait:300|expect-text:1 stored"
        "|release:130,80|tap:400,300|wait:300|expect-text:1 stored"
        "|tap:130,80|wait:400|expect-text:Import from phone"
        "|tap:400,300|wait:300|expect-text:Import from PC"
        "|tap:40,24|wait:400|expect-text:1 stored|tap:40,24|wait:500"
        "|expect:home|quit",
    # One tap on a tile connects. The fixture edit points it at a closed
    # loopback port. Esc cancels; the SSH thread needs real time to stop.
    "tap-connect":
        "key:esc|wait:500|expect:home|tap:130,64|wait:150"
        "|expect:connecting|key:esc|sleep:300|wait:700|expect:home|quit",
    "pad-create":
        PAD_OPEN + "|expect-text:Choose a new code|type:1234|key:enter"
        "|wait:300|expect-text:Enter the new code again|type:1234"
        "|sleep:2500|wait:700|expect:menu|expect-text:Lock deck|quit",
    "pad-lock-unlock":
        PAD_MAKE + "|key:left|key:enter|wait:400|expect:unlock|expect-text:Deck locked"
        "|key:esc|wait:200|expect:unlock|type:1234|sleep:2500|wait:700"
        "|expect:home|quit",
    "pad-wrong-code":
        PAD_MAKE + "|key:left|key:enter|wait:400|expect:unlock|type:9999|sleep:2500"
        "|wait:700|expect:unlock|expect-text:wrong code|quit",
    "pad-mismatch":
        PAD_OPEN + "|type:1234|key:enter|wait:300|type:1235|sleep:200|wait:300"
        "|expect:unlock|expect-text:The codes do not match"
        "|expect-text:Choose a new code|quit",
    "pad-short":
        PAD_OPEN + "|type:12|key:enter|wait:300"
        "|expect-text:At least 4 characters|quit",
    # Points (8x16 grid): 316,200 key 1; 396,200 key 2; 476,200 key 3;
    # 316,248 key 4; 476,344 OK; 40,24 header back.
    "pad-touch":
        PAD_OPEN + "|tap:316,200|tap:396,200|tap:476,200|tap:316,248"
        "|wait:300|tap:476,344|wait:300|expect-text:Enter the new code again"
        "|tap:40,24|wait:400|expect:menu|expect-text:Create keystore|quit",
    "pad-cancel":
        PAD_OPEN + "|key:esc|wait:400|expect:menu|expect-text:Create keystore"
        "|key:esc|wait:400|expect:menu|key:esc|wait:500|expect:home|quit",
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
    "connect-panel":
        "key:esc|wait:500|expect:home|tap:130,64|wait:150|expect:connecting"
        "|expect-text:Connecting to example|expect-text:Wi-Fi  HomeAP"
        "|expect-text:me@127.0.0.1 port 1|expect-text:Cancel"
        "|tap:20,20|wait:200|expect:connecting|key:left"
        "|sleep:4000|wait:700|expect-text:Retrying example"
        "|expect-text:Next try in|key:esc|sleep:300|wait:700|expect:home|quit",
    "connect-cancel-tap":
        "key:esc|wait:500|expect:home|tap:130,64|wait:150|expect:connecting"
        "|tap:264,280|wait:200|expect-text:Cancelling"
        "|sleep:300|wait:700|expect:home|quit",
    "connect-cancel-enter":
        "key:esc|wait:500|expect:home|tap:130,64|wait:150|expect:connecting"
        "|key:left|key:enter|wait:200|expect-text:Cancelling"
        "|sleep:300|wait:700|expect:home|quit",
    "connect-failed":
        "key:esc|wait:500|expect:home|tap:130,64|wait:300|expect:connecting"
        "|expect-text:No session on example"
        "|expect-text:Key 'id_missing' cannot be read"
        "|expect-text:me@127.0.0.1 port 22|expect-text:Back"
        "|expect-text:Try again|expect-text:Edit host|key:esc|wait:300"
        "|expect:home|quit",
    "connect-failed-back-tap":
        "key:esc|wait:500|expect:home|tap:130,64|wait:300|expect-text:Try again"
        "|tap:256,280|wait:300|expect:home|expect-text:1 of 8|quit",
    "connect-failed-again":
        "key:esc|wait:500|expect:home|tap:130,64|wait:300|expect-text:Try again"
        "|key:left|key:enter|wait:300|expect:connecting"
        "|expect-text:Key 'id_missing' cannot be read"
        "|tap:372,280|wait:300|expect:connecting"
        "|expect-text:Key 'id_missing' cannot be read|quit",
    "connect-failed-edit":
        "key:esc|wait:500|expect:home|tap:130,64|wait:300|expect-text:Edit host"
        "|tap:508,280|wait:300|expect:profile|expect-text:Edit example"
        "|key:esc|wait:300|expect:home|expect-text:1 of 8|quit",
    "connect-failed-edit-discard":
        "key:esc|wait:500|expect:home|tap:130,64|wait:300|expect-text:Edit host"
        "|key:left|key:right|key:enter|wait:300|expect:profile|type:2"
        "|key:esc|key:esc|wait:300|expect:ask|key:right|key:enter|wait:400"
        "|expect:home|expect-text:example|quit",
    "connect-failed-idle-locks":
        PAD_MAKE + "|key:esc|wait:300|key:esc|wait:500|expect:home"
        "|tap:130,64|wait:300|expect-text:Try again|wait:170000"
        "|expect-text:Try again|wait:15000|expect:saver|key:esc|wait:400"
        "|expect:unlock|expect-text:Deck locked|quit",
    "host-delete-key":
        "key:esc|wait:400|expect:home|key: |wait:400|expect:host"
        "|key:right|key:right|key:enter|wait:400|expect:ask"
        "|expect-text:The entry and its SSH key files go.|quit",
    "host-delete-shared-key":
        "key:esc|wait:400|expect:home|key: |wait:400|expect:host"
        "|key:right|key:right|key:enter|wait:400|expect:ask"
        "|expect-text:The entry goes. Its key stays in use.|quit",
    # The release after a hold must not connect.
    "hold-opens-host":
        "key:esc|wait:400|expect:home|hold:130,64|wait:400|expect:host"
        "|release:130,64|wait:400|expect:host|expect-text:Connect"
        "|key:esc|wait:400|expect:home|quit",
    # Points (8x16): 40,24 back bar; 400,300 empty; 100,90 tile; 100,448 Forget.
    "wifiprov-esc":
        WIFI_OPEN + "|expect-text:Add network (phone)|expect-text:Join this Wi-Fi:"
        "|expect-text:CYBERDECK-SIM|expect-text:Proof code:|expect-text:1A2B3C4D"
        "|expect-text:Waiting for the phone|expect-text:scan with the app"
        "|key:esc|wait:400|expect:home|quit",
    "wifiprov-back-tap":
        WIFI_OPEN + "|tap:400,300|wait:300|expect:wifiprov"
        "|tap:40,24|wait:400|expect:home|quit",
    "import-phone":
        HUB + "|key:enter|wait:300|expect-text:Setup / Hosts|key:enter|wait:400"
        "|expect:sshimport|expect-text:Import from phone"
        "|expect-text:Join this Wi-Fi:|expect-text:DECK-SETUP-SIM"
        "|expect-text:Wi-Fi password:|expect-text:Open in a browser:"
        "|expect-text:Waiting for a browser|expect-text:scan to join"
        "|key:esc|wait:400|expect:home|quit",
    "import-pc-back-tap":
        HUB + "|key:enter|wait:300|key:right|key:enter|wait:400"
        "|expect:sshimport|expect-text:Import from PC"
        "|expect-text:Open on your PC:|expect-text:Proof code:"
        "|expect-text:Plain HTTP. Use it on a network you trust."
        "|expect-text:scan to open|tap:400,300|wait:300|expect:sshimport"
        "|tap:40,24|wait:400|expect:home|quit",
    "pair-esc":
        PAIR_OPEN + "|expect-text:Pair keyboard|expect-text:Scanning for keyboards"
        "|expect-text:Put the keyboard in pairing mode.|wait:3300"
        "|expect-text:2 found. Tap yours.|expect-text:Keychron K2"
        "|expect-text:public address|expect-text:random address"
        "|key:esc|wait:400|expect:home|quit",
    "pair-pill-tap":
        "key:esc|wait:400|expect:home|tap:590,23|wait:300|expect:pairing"
        "|expect-text:Pair keyboard|tap:40,24|wait:400|expect:home|quit",
    "pair-back-tap":
        PAIR_OPEN + "|wait:3300|expect-text:2 found|tap:40,24|wait:400"
        "|expect:home|quit",
    "pair-tap-device":
        PAIR_OPEN + "|wait:3300|expect-text:2 found|tap:100,90|wait:400"
        "|expect:home|expect-text:pairing Keychron K2|quit",
    "pair-enter-device":
        PAIR_OPEN + "|wait:3300|expect-text:2 found|key:enter|wait:400"
        "|expect:home|expect-text:pairing Keychron K2|quit",
    "pair-forget":
        PAIR_OPEN + "|wait:3300|expect-text:2 found|key:down|key:enter|wait:300"
        "|expect:ask|expect-text:Forget every paired keyboard?"
        "|key:enter|wait:300|expect:pairing|expect-text:Scanning for keyboards"
        "|wait:3300|expect-text:2 found|tap:100,448|wait:300|expect:ask"
        "|key:left|key:right|key:enter|wait:300|expect:pairing"
        "|expect-text:bonds cleared|expect-text:Scanning for keyboards|quit",
    "pair-timeout":
        PAIR_OPEN + "|wait:3300|expect-text:2 found|wait:20000"
        "|expect-text:closes in|wait:11000|expect:home|quit",
    "status-boot":
        "expect:home|expect-text:HomeAP 127.0.0.1|wait:6000"
        "|expect-no-text:HomeAP|expect-text:NET|quit",
    "status-lost-still-page":
        "key:esc|wait:400|expect:home|settle|key: |wait:400|expect:host"
        "|wifi:down|wait:300|expect-text:link lost|expect:host|wait:5000"
        "|expect-no-text:link lost|expect-text:NET|wifi:up|wait:300"
        "|expect-text:HomeAP 127.0.0.1|wait:5000|expect-no-text:HomeAP"
        "|expect:host|quit",
    "status-failed-page-toast":
        "key:esc|wait:500|expect:home|settle|tap:130,64|wait:300"
        "|expect-text:Try again|wifi:down|key:left|key:enter|wait:300"
        "|expect-text:wifi not connected yet|expect:connecting|wait:3500"
        "|expect-no-text:wifi not connected yet|expect-text:Try again|quit",
    "status-connect-refused":
        "key:esc|wait:500|expect:home|settle|wifi:down|tap:130,64|wait:300"
        "|expect:home|expect-text:wifi not connected yet|wait:3500"
        "|expect-no-text:wifi not connected yet|expect:home|quit",
}

# Per-scenario edits to the fixture's profiles.ini (old text, new text).
SECOND_HOST = ("#[server]\n#host=server.local\n#port=22\n#user=me\n"
               "#auth=key\n#key_id=id_deck",
               "[server]\nhost=10.0.0.5\nport=22\nuser=ops\n"
               "auth=password\npassword=secret")
LOOPBACK_CLOSED = [("host=192.168.1.10", "host=127.0.0.1"),
                   ("port=22", "port=1")]
KEY_MISSING = [("host=192.168.1.10", "host=127.0.0.1"),
               ("auth=password\npassword=changeme",
                "auth=key\nkey_id=id_missing")]
FIXTURE_EDITS = {
    "connect-panel": LOOPBACK_CLOSED,
    "connect-cancel-tap": LOOPBACK_CLOSED,
    "connect-cancel-enter": LOOPBACK_CLOSED,
    "connect-failed": KEY_MISSING,
    "connect-failed-back-tap": KEY_MISSING,
    "connect-failed-again": KEY_MISSING,
    "connect-failed-edit": KEY_MISSING,
    "connect-failed-edit-discard": KEY_MISSING,
    "status-failed-page-toast": KEY_MISSING,
    "connect-failed-idle-locks": KEY_MISSING,
    "host-delete-key": KEY_MISSING,
    "host-delete-shared-key": [
        ("auth=password\npassword=changeme", "auth=key\nkey_id=id_deck"),
        ("#[server]\n#host=server.local\n#port=22\n#user=me\n"
         "#auth=key\n#key_id=id_deck",
         "[server]\nhost=10.0.0.5\nport=22\nuser=ops\nauth=key\nkey_id=id_deck")],
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
            mode = ([] if window else ["--headless"]) + (
                ["--fake-ble"] if name.startswith("pair-") else [])
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
