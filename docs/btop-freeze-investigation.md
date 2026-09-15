# btop picture freeze — investigation record

Status as of 2026-09-15 02:00 PDT: **closed. The freeze is btop's own
stall recovery deadlocking on the router, proven by btop's log.** The
deck was never at fault. It has not been rebooted and COM6 has not been
opened during the investigation; both are now free to use.

This file is the record of the problem: what was suspected, what the
soak and the router proved, and what to do about it. Read it before
touching the deck or the router.

## The symptom

The deck runs `sudo btop` at a 300 ms refresh over SSH (Secure Shell) to
the OPNsense router at 192.168.1.1, on the same LAN. After one to four
days the picture stops updating. The keyboard, the F12 overlay and
"disconnect" keep working. A fresh session works. Two such freezes were
seen on 2026-09-10 after roughly 36 hours each. The third is the one on
the bench now, after 98.7 hours of link time.

## Conclusion

**btop itself deadlocked on the router. The deck, the SSH transport and
the terminal pipeline are all healthy.** The picture froze because the
program on the far end stopped producing output. The trigger is a short
Wi-Fi outage on the deck: the server's output backs up for several
seconds, btop's frame write blocks, and btop's own stall recovery
(which cancels and recreates its worker thread) leaves the process
waiting on a lock forever.

The evidence, in the order it was gathered, is below. The final proof is
btop's own log line, see "Proof".

## Ruled out first: the DEC 2026 synchronized-update hold

The first theory (PR #28, merged 2026-09-11) was an unbounded
synchronized-update hold. Terminal programs bracket a frame with
`CSI ?2026 h` (BSU, begin synchronized update) and `CSI ?2026 l` (ESU,
end synchronized update). The bridge between the terminal-state machine
and the display, `vterm`, skipped presenting while a BSU was open and had
no timeout, so one missed ESU would freeze the picture for the session.
PR #28 added a 500 ms watchdog, idle-wake flushes, heap columns in the
`net_bench` log line, and a temporary red `SYNC` chip in the status bar.

The soak proves this was not the cause:

- The watchdog fired 108 times in 99 hours. Every firing was a late ESU:
  the BSU and ESU counters were equal again at the next report and the
  byte stream continued.
- At the moment of the real freeze no watchdog fired. The counters were
  equal (`sy=1492800/1492800`) and no hold was open.
- The `SYNC` chip prints one digit and clamps at 9. A chip reading "9"
  means "9 or more"; it read 9 while the log held 108.

The 500 ms watchdog is still a correct bound to keep. The chip carries
no diagnostic weight for this bug and can go.

## What the deck log shows

Log: `D:/esp32/unbreezy/soak-logs/soak-20260910-215906.log` (wall-clock
stamped from the PC; the value in parentheses is the deck's millisecond
uptime). Timeline on 2026-09-14:

| PC time | Line | Meaning |
|---|---|---|
| 23:25:40 | `vterm_bench` KB=1574 | steady btop stream, normal |
| 23:25:52 | `wifi:bcn_timeout,ap_probe_send_start` | deck missed AP beacons, probes the AP |
| 23:25:55 | `net_bench gap_max=9070ms` | one 9-second hole between data wakes, then a final burst |
| 23:26:10 | `vterm_bench` KB=256 | only 256 KB in that 30 s window (normal is ~1570 KB) |
| after | nothing from `vterm_bench` or `net_bench` | both lines are silent when no bytes arrive |

No warning or error was logged afterwards. The session was never dropped.
Heap stayed flat. Ping to the deck answers today, so its Wi-Fi is up.

The beacon timeout is the only second one in the whole soak. The first,
on 2026-09-13 11:53, produced a 6.1 s gap and the stream recovered. So
the outage has to be long enough to matter; see "Why the hiccup kills
btop".

## What the router shows

All of this was read as the normal user and, where marked, with `sudo`.
Nothing was changed.

**Session and process state.** The deck's login session on the router
is still alive: `sshd-session: sigman@pts/2`, PID 82489, started
2026-09-10 22:26:46. `sudo btop` runs through sudo's pseudo-terminal
relay: the login pty is `pts/2`, sudo gives btop its own pty `pts/3` and
copies between them. Both sudo relays and btop have accumulated no CPU
time since the freeze.

**Pty timestamps** (`stat /dev/pts/N`) pin the stall to the second:

| pty | last write | writer |
|---|---|---|
| `/dev/pts/3` | 09-14 23:25:49 | btop's last frame |
| `/dev/pts/2` | 09-14 23:25:52 | sudo relay's last copy toward sshd |

**TCP socket** (`sudo netstat -an -p tcp | grep 192.168.1.158`):

```
tcp4  0  0  192.168.1.1.22  192.168.1.158.56742  ESTABLISHED
```

Receive queue 0, send queue 0. The server's TCP has nothing waiting for
the deck.

**Packet capture** (`sudo tcpdump -i igc0 -n -tttt 'host 192.168.1.158
and port 22'`, 2026-09-15 01:33, 30 seconds):

```
01:33:18.600  deck > router  [P.] len 52    deck keepalive (want_reply=0)
01:33:18.600  router > deck  [.]  ack       bare ACK
01:33:48.620  router > deck  [P.] len 60    sshd's own keepalive, wants a reply
01:33:48.663  deck > router  [P.] len 52    deck keepalive, 30 s after the last
01:33:48.682  deck > router  [.]  ack 61
01:33:48.683  deck > router  [P.] len 36    SSH_MSG_REQUEST_FAILURE, libssh2's answer
01:33:48.683  router > deck  [.]  ack 140
```

Both sides speak SSH to each other every 30 s and the deck answers the
server's request within 20 ms. The deck's read task, the libssh2
transport and the socket are healthy 26 hours into the freeze, and have
been the whole time: sshd drops a client after a few unanswered
keepalives, and it never did. Only the channel is silent. The deck's
advertised TCP window (10680 of the configured 11520 bytes) is lwIP's
lazy window announcement after many 60-byte reads, not unread data.

**Pty output queues** (`sudo pstat -t`):

```
 LINE   INQ  CAN  LIN  LOW  OUTQ  USE  LOW   COL  SESS  PGID STATE
pts/2  1920    0    0  192  1984    0  199 99999 82648 44045 Oil
pts/3  1920    0    0  192  1984    0  199 99999 69855 70943 Oi
```

`USE` is bytes waiting in the output queue. Zero on both ptys: sshd
drained everything the relay ever gave it, and the relay drained
everything btop ever wrote. Nothing is stuck between btop and the deck.
This rules out the SSH channel window as the cause: a window-blocked
sshd would have left `pts/2` full.

**btop's threads** (`sudo procstat -kk 70943`):

```
 PID    TID COMM  KSTACK
70943 115923 btop  ... umtxq_sleep do_wait __umtx_op_wait sys__umtx_op ...
70943 495525 btop  ... umtxq_sleep do_wait __umtx_op_wait sys__umtx_op ...
```

Both threads sleep in `__umtx_op_wait`, the FreeBSD primitive behind
`std::atomic::wait`, mutexes and condition variables. Neither is in a
tty write, a `select`, or a system-statistics call. The main thread is
waiting for the worker; the worker is waiting on something that will
never be released. That is a user-space deadlock inside btop.

The worker's thread ID (495525) is far above the main thread's (115923).
btop creates its single worker at startup, so a worker with a much later
ID was created later, which is exactly what btop's stall recovery does.

## Why the hiccup kills btop

btop draws each frame from a worker thread that writes the whole frame
to the terminal, while the main thread waits for it with a bounded
atomic wait. If the worker has not finished within that bound, btop logs
"Stall in Runner thread, restarting!", cancels the worker with
`pthread_cancel`, and starts a new one.

On the deck side a radio outage of a few seconds means sshd cannot send.
Its channel buffer fills, it stops reading `pts/2`, the relay stops
reading `pts/3`, and btop's worker blocks inside its frame write with
btop's internal state locked. Once the outage outlasts the stall bound,
the main thread cancels a worker that is parked in a blocking write with
that lock held. The new worker waits for the lock, the main thread waits
for the new worker, and the process is dead from then on. When the link
recovers, sshd drains the queued bytes (the deck's final 256 KB burst),
after which there is nothing more to send.

This also fits the near miss: the 6.1 s outage on 2026-09-13 did not
freeze btop, the 9 s one did. The exact bound depends on the btop
version; the check below shows whether the stall message is there.

## Proof

btop 1.4.7 writes its log under the XDG state directory
(`Config::get_log_file()` in the v1.4.7 source), so with `HOME=/root`
the file is `/root/.local/state/btop.log`. Its last line:

```
2026-09-15 (06:25:49) | ERROR: Stall in Runner thread, restarting!
```

btop stamps in UTC. 06:25:49 UTC is 23:25:49 PDT, the same second as the
last write to `pts/3`. The v1.4.7 `Runner::run` waits 5000 ms on
`active`, logs this line, calls `pthread_cancel` on the worker, joins
it, and creates a new one. FreeBSD's thread library does not unwind C++
frames on cancellation, so the lock the worker held while blocked in its
frame write is never released. The new worker parks on that lock, the
main thread parks waiting for the new worker: the two `__umtx_op_wait`
stacks above.

The log holds earlier stall lines as well. Whether every one of them
matches a freeze (the two on 2026-09-10) or some restarts succeeded is
worth recording next to this line if the timestamps are still there.

## Why only the deck, never the PC

The same router runs btop for days in a Windows Terminal session over
OpenSSH without hanging. The difference is how much output the chain can
absorb while the client is unreachable, which is the client's advertised
SSH channel window plus the server's TCP send buffer:

| Client | SSH channel window | btop output absorbed (≈52 KB/s at 300 ms) |
|---|---|---|
| OpenSSH on a PC | 2 MB (OpenSSH default) | ≈40 s, before TCP buffers |
| Deck (`CONFIG_SSH_RECV_WINDOW`, until 2026-09-15) | 32 KB | 1–2 s with the TCP buffers |

btop's stall detector fires after 5 s. A PC has to vanish for most of a
minute inside one detector window; a wired PC never does. The deck turns
any dropout longer than about a second into backpressure: sshd stops
reading the pty, the 2 KB pty queue fills, btop's frame write blocks,
and any outage of roughly 6 s or more trips the detector. The soak saw a
6.1 s beacon loss survive and a 9 s one kill btop.

The 32 KB window was a RAM-diet choice, not a hard limit, and the RAM
argument does not hold: the window is a promise to the server, not a
buffer. libssh2 only reads the socket inside `channel_read`, so a stalled
consumer stops at lwIP's 11.5 KB TCP window, whatever the SSH window
says, and libssh2's allocations go to PSRAM first anyway. **Bumped to
256 KB on 2026-09-15** (`main/Kconfig.projbuild` default, both sdkconfig
files, and the simulator mirror in `cmake/cyberdeck_features.cmake`).
That gives a 300 ms btop about five seconds of slack against a dropout
and removes the acknowledgment round-trip from bulk output. It softens
the trigger; it does not fix btop.

## What this means for the firmware

Nothing in the deck caused the freeze, but the deck also cannot tell a
dead program from a quiet one. Two changes make the situation visible
and recoverable from the chair:

- **A visible "stream stopped" state.** The session was streaming at
  1.5 MB per 30 s and then went to zero for 26 hours with the status bar
  saying "connected". A per-session "seconds since last byte" figure in
  the F12 overlay, or a status-bar hint after the stream has been silent
  for many times its recent cadence, tells the user what happened.
- **A real link check.** The keepalive is `want_reply=0`, so a
  half-dead channel can never be detected. Switching to `want_reply=1`
  with a reply deadline, or counting sshd's own keepalives (they arrive
  every 30 s and the deck already answers them), gives a positive
  "server alive" signal that costs nothing.

These do not fix btop. For the soak itself, run btop under `tmux` or
`screen` on the router: a multiplexer keeps consuming btop's output
during a client stall, so btop never blocks and never triggers its stall
recovery, and the deck can re-attach after a drop.

## Releasing the bench

The evidence is all on record, so the specimen can go: kill btop on the
router (`sudo kill 70943`), disconnect on the deck, stop the soak logger
(PID 22572 on the PC), and reflash as needed. COM6 is safe to open again.

## Follow-ups

- Firmware: the two visibility items above. (Window bump done 2026-09-15, not yet built or flashed.)
- Firmware: remove the temporary `SYNC` chip, keep the 500 ms watchdog.
- Firmware: the terminal-state machine's private-mode handler reads only
  the first parameter, so `CSI ?25;2026 l` ignores 2026. Needs a unit
  test and a fix on its own branch. Unrelated to this freeze, found on
  the way.
- Router: check the btop version against its upstream issue tracker for
  the stall-recovery deadlock, and upgrade if a fix exists.
