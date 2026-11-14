# Shell UI surface, as built (2026-11-13)

What the shell can show today, read from the code on `feat/ui-kit2`.
It is the checklist a redesign has to cover. Paths are under
`components/cyberdeck_app/` unless noted. Where docs and code disagree,
this follows the code.

BLE = Bluetooth Low Energy. PSK = pre-shared key (the Wi-Fi password).
SoftAP = the deck's own temporary Wi-Fi access point.

## Findings that shape the redesign

- **HOME drops tiles.** 66x20 and 80x24 hold 9 tiles; the worst case is
  11 (8 profiles + Pair + Lock + Configuration). Configuration is lost
  first, and HOME then has no route to it (`app_home.c:207`).
- **The unlock pad has no touch cancel.** Outside the device gate, only
  Esc or a 60 s idle timeout leaves it (`app_unlock.c:63`).
- **The pad enters digits only.** An access code with other characters
  needs a keyboard.
- **All text entry needs the BLE keyboard.** Profile name, host, port,
  user, password. There is no on-screen keyboard.
- **No saved Wi-Fi management.** No list, delete or reorder; a network
  is added only from a phone.
- **Phone presence is keyboard-only** (`P` on HOME).
- **Confirmations are in-place two-step arms**, never dialogs.
  `ui_dialog`, `ui_pager`, `ui_input`, `ui_meter`, `ui_btn` have no
  shell callers yet.
- **Cancel drops edits silently** in the profile editor.
- **Value tiles cycle forward only.** No backward step by touch.
- **Font change reboots with no confirm.**
- **Only the menu table pages draw with the new kit.**

## Limits

| What | Limit | Source |
|------|-------|--------|
| Profiles | 8 stored, 9 with the built-in default | `storage.h:32`, `app_internal.h:18` |
| Profile name / user / key id | 31 chars | `storage.h:35-41` |
| Host, password, passphrase | 63 chars | `storage.h:35-41` |
| Port | 5 digits | `app_profile.c` |
| Wi-Fi networks | 8; SSID 32, PSK 64 | `storage.h:52-56` |
| BLE bonds | 8; device name 63 | `storage.h:218-223` |
| Access code | 1..64 bytes; new codes at least 4 | `keystore.h:43-44`, `app_unlock.c:60` |
| Host key fingerprint | 64 hex chars | `ssh_client.h:151` |
| Menu page | 8 items | `app_menu_defs.h:60` |
| Navigation stack | 6 entries | `app_nav.c:15` |
| Toast | 64 chars; 3 s, 7 s for errors, 5 s for menu notes | `app_internal.h:21-59` |

Tile grid capacity today (`ui_parts.c:183`): 12 at 100x30, 9 at 80x24,
9 at 66x20.

## Screens

Thirteen entries in `SCREENS[]` (`cyberdeck_app.c:297`): boot, home,
poweroff, pairing, hostkey, connecting, session, menu, wifiprov,
profile, sshimport, unlock, saver. Boot, poweroff, session and saver
draw no status bar.

### BOOT, POWEROFF
Logo wipe and tagline; CRT collapse after a session. Any input skips.

### HOME (`app_home.c`)
- **Shows:** three status lines (NET with SSID, address, signal; KBD
  with state and name; RAM), an optional phone-presence dot, wordmark,
  version, clock. A tile per profile (name, `user@host:port [key]`).
  Trailing tiles: New profile (only when none stored), Pair keyboard
  (only when no bond), Lock deck (only with a keystore), Configuration.
- **Touch:** tap a profile connects; long-press a profile edits it;
  long-press elsewhere opens pairing.
- **Keys only:** `R` reload, `W` Wi-Fi retry, `N` new profile, `B`
  pair, `L` lock, `P` phone presence.
- **Overflow:** tail tiles are dropped. Titles are cut; the detail line
  scrolls only on the keyboard-selected tile.

### CONNECTING (`app_connect.c`)
- **Shows:** target, attempt count, activity bar, elapsed seconds; an
  amber drain bar while waiting to retry.
- **Input:** Esc, any tap or any long-press cancels. Nothing on screen
  says so.
- **Leads to:** session, host key prompt, unlock, or HOME with a toast.

### SESSION
- **Shows:** the terminal only. A toast chip top right; a scrollback
  column on the right edge while scrolling.
- **Input:** long-press or F12 opens the menu; right-edge drag or
  Shift+PgUp/PgDn scrolls back; everything else goes to the host.

### HOSTKEY (`app_hostkey.c`)
- **Normal:** "NEW HOST KEY", host and port, fingerprint in a box
  (2 x 32 hex), Trust &amp; Connect / Cancel. Trust is the default.
- **Mismatch:** red theme, hazard tape, "HOST KEY CHANGED", Replace key
  / Cancel. Cancel is the default. Replace arms on the first tap and
  fires on the second. The host name is not shown. `Y` replaces at once.
- The key type is not shown in either.

### UNLOCK (`app_unlock.c`)
- **Flavours:** device gate, connect-time unlock, set or change code
  (old, new, confirm), remove keystore.
- **Shows:** title, a corner tag, one slot per character, a status
  (deriving, denied, retry countdown), a 3 x 4 pad (1-9, DEL, 0, OK).
- **Input:** pad taps; keys for any character. Input is swallowed for
  1-2 s while the key is derived.
- **Leaving:** the gate has no exit. Other flavours: Esc or idle.

### MENU (`app_menu.c`, `app_menu_defs.c`)
Reached from HOME's Configuration tile, or over a dimmed session by F12
or long-press. A header that is the back target, and a two-column grid
of cards. Tapping outside a card also goes back.

| Page | Items |
|------|-------|
| MENU (in session) | Resume session, Reset terminal, Disconnect, Configuration. Shows uptime and link time. |
| CONFIGURATION | Profiles, Wi-Fi, Keyboard, CRT FX, Motion FX, Font, System, Keystore |
| PROFILES | Add, Edit, Reorder, Delete, Import |
| IMPORT | SoftAP (phone), Web (PC) |
| WIFI | Reconnect, Add network (phone) |
| KEYBOARD | Pair keyboard, Forget bonds (two-step) |
| SYSTEM | Saver + lock after (1/3/5/10/30 min), Edge scroll (on/off), Clear host keys (two-step), Factory reset (two-step) |
| CRT FX | Scanlines (off/on), Phosphor (color/green/amber), Bold pop (off/on) |
| MOTION FX | Wobble (off/subtle/medium/deep), Wipe in (off/0.3 s/0.6 s), Collapse (off/0.2 s/0.5 s), Static (off/light/heavy) |
| FONT | 8x16, 10x20, 12x24; a tap saves and reboots |
| KEYSTORE | Lock deck, Create keystore or Change code, Remove code |

**Pickers** (Edit, Delete, Reorder): a 40-wide scrolling list, 5 rows
visible at 66x20 for up to 8 profiles. Delete is two-step. Reorder:
first tap grabs, arrows move, a tap on another row swaps the two.

### PROFILE EDITOR (`app_profile.c`)
- **Fields:** Name, Host, Port, User, Auth (password / key), Pass or
  Phrase (masked), Key (cycles up to 8 key ids; key auth only).
- **Input:** tap focuses a row; tap toggles Auth and steps Key forward.
  Typing, caret moves and stepping Key backward need the keyboard. Rows
  are one cell tall at 66x20.
- **Errors, inline:** name required, no `[` or `]`, host required, user
  required, bad port, no key, name in use, list full, save failed.

### PAIRING (`app_pairing.c`)
Scan list (up to 8 found; 7 fit at 66 and 80 columns), Forget bonds
(two-step), Cancel. Closes itself after 30 s idle. No passkey prompt
ever appears.

### WIFIPROV, SSHIMPORT
Instruction screens for a phone or a browser: a QR code, numbered
steps, a proof code, a status line. Nothing is edited on the deck. Any
tap cancels. Both land on HOME, even when opened from a session.

### SAVER, PACMAN
Saver: braille rain and a clock that needs 72 columns; 66x20 shows a
floating chip. Any input wakes; a locked store wakes onto the gate.
Pacman: a decorative strip on HOME.

## Status bar (`app_statusbar.c`)

Bottom row. NET chip with a four-step signal ladder (on, busy, alert,
off). KBD chip with caps and num cells. A temporary SYNC diagnostic
chip. Clock at the right once time is known. Toasts draw in the gap
between chips and clock: about 36 characters at 66 columns. Chips are
not touch targets. Designed but not built: phone chip, memory chip,
chip expansion, a status page, a keystore chip, a summon in session.

## Layout literals that assume a grid

`cols >= 97` and `rows >= 22/24/28` branches in home, profile, unlock,
hostkey, wifiprov, sshimport and `ui_parts.c`. Fixed columns and widths
in hostkey (rows 5-11, box 43), connecting (bar 42), pickers (40 wide),
saver (`head[100]`).

## Not determined

Whether the QR code fits at 66x20.
