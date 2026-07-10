/*
 * splash.h — Splash screen and status line helpers.
 *
 * All output goes through vterm_write() using ANSI escape sequences.
 */

#pragma once

void splash_show(void);
void splash_status_info(const char *msg);
void splash_status_ok(const char *msg);
void splash_status_fail(const char *msg);
