# Shell UI surface, as built (2026-11-14)

What the shell can show today, read from the code on `feat/ui-kit2`.
It is the checklist a redesign has to cover. Paths are under
`components/cyberdeck_app/` unless noted. Where docs and code disagree,
this follows the code.

BLE = Bluetooth Low Energy. PSK = pre-shared key (the Wi-Fi password).
SoftAP = the deck's own temporary Wi-Fi access point.

## Findings that shape the redesign

- **Done on the slate parts:** HOME, the host page, the question page,
  the setup hub with its sections, the session sheet, the host form, the
  access code pad, connecting and connect failed, the host-key pages,
  pairing, the Wi-Fi and SSH import pages and the status row. Each has a
  frame in `MO_GATE`.
- **Not slate parts:** boot, power-off and the saver. They draw their
  own effects.
- **All text entry needs the BLE keyboard.** Host name, address, port,
  user, password, and an access code with other characters than digits.
  There is no on-screen keyboard.
- **No saved Wi-Fi management.** No list, delete or reorder; a network
  is added only from a phone.
- **Every irreversible action asks on the question page,** including
  "Forget bonds" on the pairing page.

## Limits

| What | Limit | Source |
|------|-------|--------|
| Hosts | 8 stored, 9 with the built-in default | `storage.h:32`, `app_internal.h:18` |
| Host name / user / key id | 31 chars | `storage.h:35-41` |
| Host, password, passphrase | 63 chars | `storage.h:35-41` |
| Port | 5 digits | `app_profile.c` |
| Wi-Fi networks | 8; SSID 32, PSK 64 | `storage.h:52-56` |
| BLE bonds | 8; device name 63 | `storage.h:218-223` |
| Access code | 1..64 bytes; new codes at least 4 | `keystore.h:43-44`, `app_unlock.c` |
| Host key fingerprint | 64 hex chars | `ssh_client.h:151` |
| Stored keys offered by the form | 8 | `app_profile.c` |
| Items in a setup section | `MENU_ITEM_MAX` | `app_menu_defs.h` |
| Navigation stack | 6 entries | `app_nav.c:15` |
| Toast | 64 chars; 3 s, 7 s for errors, 5 s for menu notes | `app_internal.h:21-59` |

Tile grid: two-line host tiles, 30 cells at least. Eight hosts fit at
66x20.

## Screens

Entries in `SCREENS[]` (`cyberdeck_app.c`): boot, home, poweroff,
pairing, hostkey, connecting, session, menu, wifiprov, profile, host,
ask, sshimport, unlock, saver. Boot, poweroff, session and saver draw
no status bar.

### BOOT, POWEROFF
Logo wipe and tagline; CRT collapse after a session. Any input skips.

### HOME (`app_home.c`)
- **Shows:** a header with the CYBERDECK wordmark and the firmware
  version, a pager when the tiles need pages, and the buttons Setup and
  + New. Lock shows with
  a keystore. Pair shows until a keyboard is bonded. A phone dot (PHN)
  shows when a phone is enrolled. A tile per host: name, `user@host`
  (`:port` when not 22), its number. Plugin tiles follow the hosts.
- **Touch:** a tap connects; a hold opens the host page.
- **Keys:** arrows move; Enter connects; Space holds; `1`-`8` connect;
  `N` new, `B` pair, `R` reload, `W` Wi-Fi retry, `L` lock, `P` phone.
- **Empty:** a muted line says to tap + New or import from Setup.

### HOST PAGE (`app_host.c`)
- **Shows:** a text panel (address and port, user, sign-in, host key
  with its pin state), then Connect, Edit, Delete, and the two moves.
- **Leads to:** connecting, the form, the question page; a move
  reorders the list and stays on the page.

### QUESTION PAGE (`app_ask.c`)
- **Shows:** a text panel with a title and two lines, then Cancel and
  the verb. The safe answer holds the first focus. One instance.
- **Asked by:** host delete, the form's discard, the setup actions
  that cannot be undone, the font restart.

### CONNECTING (`app_connect.c`)
- **Shows:** a text panel: "Connecting to" or "Retrying" and the name,
  the Wi-Fi network, `user@host port`, "Try n, s" or "Next try in s",
  and a Cancel button. The controller reports no handshake steps.
- **Input:** Esc or the Cancel button cancels.
- **Failed:** the screen stays as the failed page: the refusal reason,
  `user@host port`, then Back, Try again and Edit host.
- **Leads to:** session, host key prompt, unlock, or HOME.
- **Model:** `mo-connect`, `mo-connect-retry` and `mo-fail`.

### SESSION
- **Shows:** the terminal only. A toast chip top right; a scrollback
  column on the right edge while scrolling.
- **Input:** long-press or F12 opens the menu; right-edge drag or
  Shift+PgUp/PgDn scrolls back; everything else goes to the host.

### HOSTKEY (`app_hostkey.c`)
- **Normal:** "New host key", a text panel with the name, host and
  port, a fingerprint panel (2 lines of 8 groups of 4 hex digits),
  Trust and connect / Cancel. Trust is the default.
- **Mismatch:** "Host key changed", a red-striped text panel with the
  host, Cancel / Replace key. Cancel is the default. Replace asks on the
  question page.
- The key type is not shown in either.
- **Model:** `mo-hostkey` and `mo-hostkey-changed`.

### UNLOCK (`app_unlock.c`)
- **Flavours:** device gate (no back target), connect-time unlock, set
  or change code (old, new, confirm), remove keystore. Flows have a
  back target; Esc or a 60 s idle also leave them.
- **Shows:** a title and a note per phase, an entry panel, a 3 x 4 pad
  (1-9, erase, 0, OK). The panel shows the known code length as bullets
  and muted dots, the newest character while a code is defined,
  "Checking" while the key is derived, a red note (wrong code, codes do
  not match) and the backoff seconds.
- **Input:** pad taps; keys for any character. Input is swallowed for
  1-2 s while the key is derived. Keys never light a pad key.

### MENU (`app_menu.c`, `app_menu_defs.c`)
One screen, three pages. The session sheet opens over a dimmed
terminal by F12 or a long-press: Reset terminal, Setup, Disconnect, with
uptime and link time. The setup hub is a tile per section. A section
shows switches, choices and actions on one page; a long one pages.

| Section | Items |
|---------|-------|
| Hosts | Import from phone, Import from PC |
| Wi-Fi | Link facts, Reconnect, Add from phone, saved networks (page: Join now, Move, Forget) |
| Keyboard | Link facts, Pair keyboard, paired keyboards (page: Forget), Forget all (asks) |
| CRT FX | Scanlines, Bold pop, Phosphor; Row glow and Glow decay when built |
| Motion FX | Wobble, Wipe in, Collapse, Static |
| Font | Size 8x16, 10x20, 12x24 (asks, then restarts) |
| System | Saver + lock (1/3/5/10/30 min), Edge scroll, Clear host keys (asks), Factory reset (asks) |
| Keystore | Lock deck, Create keystore or Change code, Remove code |

Edit, delete and reorder moved to the host page; the pickers are gone.

### HOST FORM (`app_profile.c`)
- **Shows:** Name, User, Host, Port, Sign-in chips (password, key). With
  a password: the Password entry and a show switch. With a key: the Key
  part (id, type, place in the list) and the Passphrase entry with its
  show switch. A Save button in the header, off while the form is
  invalid. One alert line under the last row.
- **Input:** an entry types in place with a movable caret. Tab walks the
  controls and opens an entry for typing. Port takes digits only.
- **Errors, inline:** port, name, address and user rules; no stored key;
  and, from the save, name in use, list full, could not save, host gone,
  unlock the deck first.
- **Leaving:** back leaves at once when nothing changed, else asks on
  the question page.

### PAIRING (`app_pairing.c`)
Found keyboards as tiles, paged, and Forget bonds (asks on the question
page). Closes itself after 30 s idle. No passkey prompt ever appears.

### WIFIPROV, SSHIMPORT
Instruction screens for a phone or a browser: numbered steps, a QR code
where it fits, a proof code, a status line. Nothing is edited on the
deck. Esc or the header bar cancels. Both land on HOME, even when opened
from a session.

### SAVER, PACMAN
Saver: braille rain and a clock that needs 72 columns; 66x20 shows a
floating chip. Any input wakes; a locked store wakes onto the gate.
Pacman: a decorative strip on HOME.

## Status bar (`app_statusbar.c`)

Bottom row. NET chip with a four-step signal ladder (on, busy, alert,
off). KBD chip with caps and num cells. A temporary SYNC diagnostic
chip. Clock at the right once time is known. A panel-tone bar drawn by
the kit's `ui_status` (twin: `statusBar` in `kit.js`). Toasts draw as
a note in the gap between chips and clock: about 36 characters at 66
columns. The in-session toast is a `ui_toast` box at the top right.
Chips are not touch targets. On a state edge a chip stretches with a
detail text (NET: SSID and address, "link lost", "join failed"; KBD:
name, "disconnected"), holds 3 s and collapses a cell per 100 ms. The
status tick in the shell drives it. Designed but not built: phone chip,
memory chip, a status page, a keystore chip, a summon in session.

## Layout literals that assume a grid

Only the saver branches on the grid size: its clock block needs 72
columns. The slate screens take their sizes from the layout helpers.

## Not determined

Whether the QR code fits at 66x20: it does not beside the Wi-Fi steps,
and the page drops it.
