/*
 * session_guard.h — deck idle/security policy (shell-internal). The
 * screensaver plugin consumes session_guard_idle(); cyberdeck_app.c
 * wires the rest. Policy lives here so a build without the saver
 * plugin still auto-locks (docs/extensibility.md item 7).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

void session_guard_init(uint64_t now);      /* load [saver], start timer */
void session_guard_activity(uint64_t now);  /* any input or HOME entry   */
void session_guard_tick(uint64_t now);      /* idle + walk-away lock     */
bool session_guard_idle(uint64_t now);      /* idle timeout crossed      */

uint32_t session_guard_idle_min(void);      /* SYSTEM menu knob          */
void     session_guard_set_idle_min(uint32_t min);
