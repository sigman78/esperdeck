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
8. [Setup and the session menu](#8-setup-and-the-session-menu)
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

HOME lists your hosts as tiles, two per row: the name, `user@host` (plus
`:port` when it is not 22) and the tile number. More than a page shows
a pager at the top right.

- **Tap a tile** to connect. **Long-press it** (or press **Space**) to open
  the host page: details, Connect, Edit, Delete and Move earlier / later.
  With a keyboard: arrows + **Enter**, or a digit **1**-**8** to connect.
- The buttons at the top right: **+ New**, **Setup** (the settings),
  **Lock** (once an access code exists) and **Pair** (while no keyboard
  is bonded).
- The status bar at the bottom shows WiFi, keyboard and the clock. A
  `PHN` dot at the top shows an enrolled phone nearby (walk-away lock,
  [section 10](#10-auto-lock-and-the-screensaver)).

Keyboard shortcuts on HOME: **R** reloads profiles, **W** reconnects WiFi,
**N** creates a new profile, **B** opens keyboard pairing, **L** locks the
deck (when a code exists), **P** enrolls a phone (when the build includes
phone presence). Shift+PgUp / Shift+PgDn flip pages. A long-press away
from the tiles also opens keyboard pairing.

## 3. Getting on WiFi

The deck joins stored networks automatically, in the order they are
listed. To add a network without any cable:

1. Open **Setup → Wi-Fi → Add network (phone)**.
2. The page lists four numbered steps: get the "ESP SoftAP Provisioning"
   app, join the Wi-Fi the page names, enter the **proof code** it shows,
   and pick your network in the app. The proof code confirms you are
   talking to *this* deck and not a neighbor. A QR code sits beside the
   steps when the screen has room for it. Scan it with the app.
3. A line under the steps says what the deck is doing: waiting for the
   phone, then testing the network.
4. On success the page says "Joined <name>. Saved." and returns to HOME
   after a moment. On failure it says the password was wrong or the
   network was not found, and the steps stay on screen. Try again from the
   app.

The header bar or Esc cancels and turns the hotspot off. While the deck
tests the network, only Esc or a long press on the header bar cancels, so
a stray touch cannot break the phone's session.

**Setup → Wi-Fi → Reconnect** retries the stored networks (same as
pressing **W** on HOME).

## 4. Pairing a keyboard

Any Bluetooth Low Energy (BLE) keyboard should work.

1. Put the keyboard in pairing mode.
2. On the deck: tap **Pair**, press **B**, or long-press
   HOME away from the tiles.
3. The **Pair keyboard** page scans. A line under the header says
   "Scanning for keyboards", then "2 found. Tap yours." Each keyboard is a
   tile with its name and address type. Tap yours, or move to it and
   press Enter. In the last ten seconds before the idle timeout, the
   line adds "closes in 9 s". The header bar or Esc leaves without pairing.

Once paired, the keyboard reconnects by itself — the `KBD` light tells you
when it's linked. To unpair everything: **Setup → Keyboard →
Forget bonds**, or the **Forget bonds** button at the bottom of the
pairing page. It asks first on its own page; Cancel is the default
answer. After you confirm, the scan starts again from clean bonds.

No keyboard at hand? The whole interface works by touch, and a USB cable
into the same port used for flashing also delivers keystrokes.

## 5. Adding an SSH connection

Three ways, pick what's convenient:

- **On the device.** Tap **+ New** on HOME, or press N. The form has
  Name, User, Host and Port. **Sign-in** is a password or a stored key.
  A password has a **Show password** switch. A key shows the stored key
  and an optional passphrase. Press the key part to step to the next
  stored key. A host needs a name, an address and a user. The port is a
  number from 1 to 65535. **Save**, at the top right, stays grey until
  the form is valid.
- **From your phone.** **Setup → Hosts → Import from phone**: same
  steps-and-QR page as WiFi onboarding. Join the deck's temporary
  hotspot, enter the Wi-Fi password the page shows, open the address in a
  browser, fill in the form and save. Repeat for more hosts.
- **From your PC.** **Setup → Hosts → Import from PC**: the
  deck (already on your WiFi) serves a form at the address it shows,
  with the proof code beside it. **Heads-up:** this runs over plain HTTP
  on your local network. The page says "Plain HTTP. Use it on a network
  you trust", and it means it.

Both pages keep a line under the steps: "Waiting for a browser", then
"Imported nas (2 saved)" or "Removed nas (1 removed)" after each save,
or "Rejected: <reason>" in red. The header bar or Esc ends the session.
Whatever you saved is already stored.

To edit, delete, or move a profile, hold its tile on HOME (or press
Space on it) for the host page. **+ New** on HOME adds one. Delete asks
you to confirm.

**Filling in the form.** Tap a field, or press Enter on it, and type.
A letter on a focused field starts typing at once. Left and Right move
the caret and Backspace deletes before it. Enter, Esc, Up or Down end
typing. **Tab** goes to the next control and starts typing in a field,
so you can fill the whole form with text and Tab. Esc outside a field,
or the back bar, leaves the form. If you changed anything it asks
"Discard the changes?" and Cancel returns to the form. While the deck
is locked, saving a host that has a stored password asks you to unlock
first.

**Careful when deleting or re-keying a profile:** if no other profile
still uses its private key, the key goes with it. The pinned host
fingerprint stays; **Clear host keys** in Setup drops the pins.

## 6. Connecting, and the host-key question

Tap a tile and the deck connects. A panel shows the host, the Wi-Fi
network, `user@host port` and a live line: the seconds of this try
("Try 2, 14 s" from the second try on), or "Next try in 3 s" while it
waits to retry. **Cancel** (tap it, or press **Esc** or **Enter**) stops
the connect. A tap anywhere else does nothing.

When a connect fails for good, the panel turns red: "No session on
*name*", the reason, and three buttons. **Try again** connects once more
with the stored host. **Edit host** opens the host form; closing it
returns to HOME. **Back** (or **Esc**) returns to HOME. The deck clears the
password it held for that try as soon as the page opens. Try again reads
it from storage again, and asks for the access code first if the keys
are locked.

The first time you reach a new server, the deck stops at **New host key**
with the server's fingerprint. This is *trust on first use*: you are
being asked "is this really my server?" If you can, compare the
fingerprint with one shown on the server itself (`ssh-keygen -lf` on the
host key). **Trust and connect** pins the fingerprint and connects;
**Cancel** (or **Esc**) returns to HOME.

If a *known* server ever presents a **different** key, the page reads
**Host key changed**, with a red stripe. That either means the server
was reinstalled, or someone is intercepting the connection. **Cancel**
is the default. **Replace key** opens a second question, "Replace the
pinned key?", and Cancel is the default there too. Only **Replace**
there pins the new key. No password or key is ever sent before this
check passes.

## 7. Using the terminal

Once connected you are in a normal terminal — vim, htop, mc, tmux all
work, in color, at your chosen font size.

- **Menu:** press **F12** or long-press the screen. The menu is a sheet at the
  right of the screen: reset the terminal, open Setup, or disconnect.
  The title bar, **Esc**, **F12** or a tap beside the sheet resume the
  session.
- **Reset terminal:** menu → Reset terminal. The link stays up; the
  terminal state (colors, modes, scrollback) goes back to power-on and
  the remote program is asked to repaint, like `reset` in a shell. Use it
  when the picture is wedged but the session is alive. A full-screen
  program may need restarting afterwards if its arrow keys misbehave.
- **Scrollback:** **Shift+PageUp / Shift+PageDown**, or drag along the
  right edge of the screen. A position marker appears while you scroll;
  any other key snaps back to live output.
- **Disconnect:** menu → Disconnect (the screen does a CRT power-off
  collapse, then HOME). If the remote side ends the session, you land back
  on HOME too.

## 8. Setup and the session menu

Open **Setup** from HOME (the **Setup** button) or in-session (**F12 →
Setup**). Setup is a grid of eight tiles. Each tile shows its section and
the present state, such as "3 stored" or "HomeAP  -55 dBm". Tap a tile, or
move with the arrows and press **Enter**. The bar at the top, or **Esc**,
goes back one level.

Inside a section every choice shows all its options as chips; tap one to
choose it. Switches flip, and actions run, when tapped. **A change applies at
once.** An action that cannot be undone asks first on a page of its own;
**Cancel** is the default answer. A long section pages: use the arrows in
the bar or **Shift+PageUp / Shift+PageDown**.

| Section | What's there |
|---|---|
| **Hosts** | Import from phone; Import from PC ([section 5](#5-adding-an-ssh-connection)) |
| **Wi-Fi** | Reconnect; Add network (phone) ([section 3](#3-getting-on-wifi)) |
| **Keyboard** | Pair keyboard; Forget bonds, which asks first ([section 4](#4-pairing-a-keyboard)) |
| **CRT FX** | Scanlines and Bold pop (switches); Phosphor (color / green / amber) |
| **Motion FX** | Wobble, Wipe in, Collapse, Static |
| **Font** | Terminal size: 8×16 / 10×20 / 12×24. Choosing another size asks, then the deck restarts and an open session ends. |
| **System** | Saver + lock timeout in minutes; Edge scroll switch; Clear host keys and Factory reset, both ask first ([section 11](#11-resetting-things)) |
| **Keystore** | Create keystore / Change code / Remove code / Lock deck ([section 9](#9-securing-the-deck-with-an-access-code)) |

In a session, **F12** or a long press opens the **session menu**: a panel at
the right over the dimmed terminal. It shows the deck uptime and the link
time with the signal, and has **Reset terminal**, **Setup** and a red
**Disconnect** button.

## 9. Securing the deck with an access code

Out of the box, **everything on the deck is stored in plain text** —
passwords in `profiles.ini`, the WiFi password in `wifi.ini`, private keys
as plain files. Anyone who picks the device up can use it. If that
matters for your keys and networks, create a keystore.

**Setup → Keystore → Create keystore** walks you through choosing
an access code on a PIN pad (4–6 digits, auto-submits at your chosen
length; a keyboard-typed passphrase also works). From that moment:

- Stored private keys and secrets are **encrypted** with a key derived
  from your code (Argon2 — deliberately slow to brute-force; unlocking
  takes about a second).
- The deck **boots locked, wakes locked, and locks on demand** (the **Lock** button, or **L** on HOME). The PIN pad is the only way in — there is
  no skip.
- Wrong codes get punished: after 4 free tries the deck makes you wait
  30 seconds, then 60, 120... up to 15 minutes per try. **Rebooting does
  not reset the wait.**

**The code pad.** Tap the digits or type them on a keyboard. The entry
line shows one dot per character; once the code length is known, the
characters still to come show as small dots. While you choose a new code
the newest character shows for a moment. The backspace key and **OK**
are dimmed until something is typed. Typing on a keyboard never lights
the pad keys, so nobody can read the code off the screen. A wrong code
shows "wrong code" in red; after repeated failures the line also counts
down the wait. The back arrow in the header (or **Esc**) cancels, except
on the lock screen, which has no way out.

**Change code** and **Remove code** are on the same menu page. Removing
the code decrypts everything back to plain files — it asks you to prove
the code first.

> **There is no recovery.** No master key, no backdoor, no "forgot my
> code" flow. If you forget the code, the encrypted keys and secrets are
> gone for good; a factory reset gives you a working (empty) deck back.
> Pick a code you will remember, and keep copies of important private
> keys somewhere else.

## 10. Auto-lock and the screensaver

- **Setup → System → "Saver + lock, min"** (1/3/5/10/30 minutes)
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

Both live under **Setup → System**, and both ask first on a page of their
own. **Cancel** is the default answer, so treat the red button as final.

- **Clear host keys** drops every pinned server fingerprint. The next
  connect to each server asks the trust question again ([section
  6](#6-connecting-and-the-host-key-question)).
- **Factory reset** wipes everything: profiles, WiFi networks, pinned host
  keys, keyboard bonds, effect settings, private keys, and the keystore.
  The deck is back to its out-of-the-box state (reboot advised, as the
  screen says).

## 12. Troubleshooting

- **WiFi won't connect.** `NET` stays dark: check **Setup → Wi-Fi →
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
- **"Host key changed" on a server that didn't change.** Did the server
  get reinstalled or re-keyed? If genuinely yes, choose **Replace key**
  and then **Replace**, or clear host keys. If you can't explain it,
  don't connect.
- **"No session on ..." after a connect.** The page names the reason:
  "Sign in refused" (check the password or key), a transport error such
  as "Connection refused" (check the address and port), or a key file
  that cannot be read. **Edit host** fixes the entry, then **Try again**.

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
