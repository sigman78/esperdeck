/*
 * uilab.h — what the lab's screen files share: the screen record, the
 * two themes and the mock host list. Host-only; nothing here ships.
 */

#pragma once

#include "cyberdeck_ui.h"

#define LAB_FG RGB565(85, 255, 85)
#define LAB_BG RGB565(0, 0, 0)
#define LAB_SLATE_FG RGB565(208, 216, 216)
#define LAB_SLATE_BG RGB565(0, 0, 0)

typedef struct {
    const char *name;
    void (*draw)(void);
    bool term;                    /* fake terminal behind it            */
    ui_id_t focus;                /* pose: keyboard focus on this id    */
    ui_id_t down;                 /* pose: finger held on this id       */
    bool slate;                   /* slate theme, else the lab default  */
} lab_screen_t;

typedef struct { const char *name, *where; } lab_profile_t;

#define LAB_NPROFILES 8
extern const lab_profile_t PROFILES[LAB_NPROFILES];

/** The frame clock of the frame being drawn, for animated parts. */
extern uint32_t lab_frame;

/** The mo-* screens (uilab_mo.c); @p n gets their count. */
const lab_screen_t *lab_mo_screens(int *n);
