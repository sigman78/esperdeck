# Cyberdeck User Guide

How to use the device — no source code knowledge needed. If you want to
build or modify the firmware instead, start with the
[README](../README.md) and [`DEVELOPMENT.md`](DEVELOPMENT.md).

**What is this thing?** A standalone SSH (Secure Shell) terminal: a 7"
touchscreen that connects to your servers over WiFi, with a Bluetooth
keyboard for typing. No laptop involved. You store connection profiles on
the device, tap one, and you are in a remote shell.

Contents:

1. [Turning it on](#1-turning-it-on)
2. [The HOME screen](#2-the-home-screen)
3. [Getting on WiFi](#3-getting-on-wifi)
4. [Pairing a keyboard](#4-pairing-a-keyboard)
5. [Adding an SSH connection](#5-adding-an-ssh-connection)
6. [Connecting, and the host-key question](#6-connecting-and-the-host-key-question)
7. [Using the terminal](#7-using-the-terminal)
8. [The Configuration menu](#8-the-configuration-menu)
9. [Securing the deck with an access code](#9-securing-the-deck-with-an-access-code)
10. [Auto-lock and the screensaver](#10-auto-lock-and-the-screensaver)
11. [Resetting things](#11-resetting-things)
12. [Troubleshooting](#12-troubleshooting)
13. [Trying it on a PC first](#13-trying-it-on-a-pc-first)

---

## 1. Turning it on

Power on and you get a boot splash (the CYBER*DECK logo). Any key or tap
skips it.

- **No access code set** (factory state): you land directly on the HOME
  screen.
- **Access code set** (see [section 9](#9-securing-the-deck-with-an-access-code)):
  you land on a PIN pad first. The deck stays locked until you enter the
  code.

**First boot note:** a fresh device shows one placeholder profile tile
(usually `user@192.168.1.100`). It comes from build-time defaults, not from
anything real — edit or delete it and add your own
([section 5](#5-adding-an-ssh-connection)).

## 2. The HOME screen

HOME is the profile picker: a grid of tiles, one per stored connection,
labeled `user@host:port` (profiles that log in with a key show `[key]`).

- **Tap a tile once** to select it, **tap it again to connect**. With a
  keyboard: arrows + **Enter**.
- Special tiles appear when useful: **+ New profile**, **+ Pair keyboard**,
  **Lock deck** (once an access code exists), and **Configuration**.

Top of the screen, the status readouts:

| Light | Meaning |
|---|---|
| `NET` | WiFi: connected or not |
| `KBD` | Bluetooth keyboard: linked or not |
| `PHN` | Enrolled phone nearby (walk-away lock, [section 10](#10-auto-lock-and-the-screensaver)) |
| `RAM` | Free memory |

Keyboard shortcuts on HOME: **R** reloads profiles, **W** reconnects WiFi,
**N** creates a new profile, **B** opens keyboard pairing, **L** locks the
deck (when a code exists), **P** enrolls a phone (when the build includes
phone presence). Long-press anywhere also opens keyboard pairing.

## 3. Getting on WiFi

The deck joins stored networks automatically, in the order they are
listed. To add a network without any cable:

1. Open **Configuration → WiFi → Add network (phone)**.
2. The screen shows a QR code and four numbered steps. Scan the code with
   your phone: it joins the deck's own temporary WiFi hotspot.
3. Your phone opens a small web page served by the deck. A short **proof
   code** shown on the deck's screen confirms you are talking to *this*
   deck and not a neighbor.
4. Enter your network's name and password in the form. The deck tests the
   credentials, saves them, and tears the hotspot down.

**Configuration → WiFi → Reconnect** retries the stored networks (same as
pressing **W** on HOME).

## 4. Pairing a keyboard

Any Bluetooth Low Energy (BLE) keyboard should work.

1. Put the keyboard in pairing mode.
2. On the deck: tap **+ Pair keyboard**, press **B**, or long-press
   anywhere on HOME.
3. The PAIR KEYBOARD screen scans and lists what it finds; pick yours.

Once paired, the keyboard reconnects by itself — the `KBD` light tells you
when it's linked. To unpair everything: **Configuration → Keyboard →
Forget bonds** (activate it twice — the first tap arms it, the second
confirms).

No keyboard at hand? The whole interface works by touch, and a USB cable
into the same port used for flashing also delivers keystrokes.

## 5. Adding an SSH connection

Three ways, pick what's convenient:

- **On the device.** Tap **+ New profile** (or **Configuration → Profiles
  → Add**). Fill in host, port, user, and either a password or a key
  reference.
- **From your phone.** **Configuration → Profiles → Import → SoftAP
  (phone)**: same QR-plus-form flow as WiFi onboarding — join the deck's
  temporary hotspot, fill in the form, done.
- **From your PC.** **Configuration → Profiles → Import → Web (PC)**: the
  deck (already on your WiFi) serves a form at the address it shows.
  **Heads-up:** this runs over plain HTTP on your local network — the
  screen says "LAN only — use on a network you trust", and it means it.

Profiles can be edited, reordered, and deleted under **Configuration →
Profiles** (delete asks you to activate it twice).

**Careful when deleting or re-keying a profile:** if no other profile
still uses its private key or its pinned host fingerprint, those are
cleaned up with it.

## 6. Connecting, and the host-key question

Tap-tap a tile and the deck connects: "Connecting to user@host:port", with
a retry countdown if the host is slow.

The first time you reach a new server, the deck stops and shows **NEW HOST
KEY** with the server's fingerprint. This is *trust on first use*: you are
being asked "is this really my server?" If you can, compare the
fingerprint with one shown on the server itself (`ssh-keygen -lf` on the
host key). **Enter** trusts and connects; the fingerprint is then pinned.

If a *known* server ever presents a **different** key, you get the
hazard-striped **HOST KEY ALERT** instead. That either means the server
was reinstalled — or someone is intercepting the connection. The safe
answer (**Cancel**) is the default; overriding takes a deliberate
double-activation. No password or key is ever sent before this check
passes.

## 7. Using the terminal

Once connected you are in a normal terminal — vim, htop, mc, tmux all
work, in color, at your chosen font size.

- **Menu:** press **F12** or long-press the screen. From the menu: resume,
  disconnect, or enter Configuration.
- **Scrollback:** **Shift+PageUp / Shift+PageDown**, or drag along the
  right edge of the screen. A position marker appears while you scroll;
  any other key snaps back to live output.
- **Disconnect:** menu → Disconnect (the screen does a CRT power-off
  collapse, then HOME). If the remote side ends the session, you land back
  on HOME too.

## 8. The Configuration menu

Reachable from HOME (the **Configuration** tile) or in-session (**F12 →
Configuration**). The pages:

| Page | What's there |
|---|---|
| **Profiles** | Add / Edit / Reorder / Delete / Import ([section 5](#5-adding-an-ssh-connection)) |
| **WiFi** | Reconnect; Add network (phone) ([section 3](#3-getting-on-wifi)) |
| **Keyboard** | Pair; Forget bonds ([section 4](#4-pairing-a-keyboard)) |
| **Effects** | CRT look: scanlines, phosphor color (color/green/amber), bold pop, wobble, wipe-in, collapse, static. Changes apply live. |
| **Font** | Terminal size: 8×16 / 10×20 / 12×24. The device reboots to apply. |
| **Keystore** | Create keystore / Change code / Remove code / Lock deck ([section 9](#9-securing-the-deck-with-an-access-code)) |
| **System** | Saver + lock timeout; edge-scroll gesture on/off; Clear host keys; Factory reset ([section 11](#11-resetting-things)) |

## 9. Securing the deck with an access code

Out of the box, **everything on the deck is stored in plain text** —
passwords in `profiles.ini`, the WiFi password in `wifi.ini`, private keys
as plain files. Anyone who picks the device up can use it. If that
matters for your keys and networks, create a keystore.

**Configuration → Keystore → Create keystore** walks you through choosing
an access code on a PIN pad (4–6 digits, auto-submits at your chosen
length; a keyboard-typed passphrase also works). From that moment:

- Stored private keys and secrets are **encrypted** with a key derived
  from your code (Argon2 — deliberately slow to brute-force; unlocking
  takes about a second).
- The deck **boots locked, wakes locked, and locks on demand** ("Lock
  deck" tile, or **L** on HOME). The PIN pad is the only way in — there is
  no skip.
- Wrong codes get punished: after 4 free tries the deck makes you wait
  30 seconds, then 60, 120... up to 15 minutes per try. **Rebooting does
  not reset the wait.**

**Change code** and **Remove code** are on the same menu page. Removing
the code decrypts everything back to plain files — it asks you to prove
the code first.

> **There is no recovery.** No master key, no backdoor, no "forgot my
> code" flow. If you forget the code, the encrypted keys and secrets are
> gone for good; a factory reset gives you a working (empty) deck back.
> Pick a code you will remember, and keep copies of important private
> keys somewhere else.

## 10. Auto-lock and the screensaver

- **Configuration → System → "Saver + lock after"** (1/3/5/10/30 minutes)
  controls when the digital-rain screensaver starts on an idle HOME
  screen — and, once a keystore exists, when the deck re-locks. Waking
  from the saver lands on the PIN pad.
- **Phone presence** (if the build includes it): press **P** on HOME and
  follow the toast — pair the deck from your phone once. Afterwards the
  `PHN` light shows whether your phone is in range, and about a minute
  after the phone disappears, the deck locks itself.
- A live SSH session is never interrupted by the saver — an open session
  keeps the deck open.

## 11. Resetting things

Both live under **Configuration → System**, and both confirm by making you
activate the same item twice — there is no extra "are you sure" dialog, so
treat the second tap as final.

- **Clear host keys** drops every pinned server fingerprint. The next
  connect to each server asks the trust question again ([section
  6](#6-connecting-and-the-host-key-question)).
- **Factory reset** wipes everything: profiles, WiFi networks, pinned host
  keys, keyboard bonds, effect settings, private keys, and the keystore.
  The deck is back to its out-of-the-box state (reboot advised, as the
  screen says).

## 12. Troubleshooting

- **WiFi won't connect.** `NET` stays dark: check **Configuration → WiFi →
  Reconnect** first; if the network is new, re-run "Add network (phone)".
  Remember the factory placeholder network is not real.
- **Keyboard won't pair.** Make sure it is in pairing mode and not still
  bonded to another host. **Forget bonds**, then pair again.
- **"ACCESS DENIED", then a countdown.** Wrong code, and you are inside the
  retry backoff. Waiting is the only option — the timer survives reboots.
- **Forgot the access code.** There is no recovery ([section
  9](#9-securing-the-deck-with-an-access-code)). Factory reset restores a
  usable, empty device.
- **Import page rejected my proof code.** The code expires with the
  session — restart the import from the menu and use the freshly shown
  code.
- **HOST KEY ALERT on a server that didn't change.** Did the server get
  reinstalled or re-keyed? If genuinely yes, override (double-activate) or
  clear host keys. If you can't explain it, don't connect.

## 13. Trying it on a PC first

The same firmware builds as a Windows program:

```
cyberdeck_sim.exe [host [port [user [password]]]]
```

The optional arguments become a ready-made profile. The simulator reads
its storage from a plain `sim_storage/` folder next to it (edit
`profiles.ini` and `wifi.ini` with a text editor), the mouse acts as the
touchscreen (long-press = right-click), and **F12** opens the menu, same
as the device. Differences from hardware: no Bluetooth (your PC keyboard
is the keyboard), WiFi is assumed connected, and the font size is fixed
at build time. Build instructions: [`DEVELOPMENT.md`](DEVELOPMENT.md).
