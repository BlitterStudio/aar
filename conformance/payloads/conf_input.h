/*
 * What the C-INPUT payload (conf_input.c) and runner/r_input.c share: the keywords the
 * app sends and the events the runner posts for them. The runner posts the events with
 * AARPostEvent (and signals itself Ctrl-D, Ctrl-E and Ctrl-F for the break events);
 * the app polls them with event_poll and compares. conf_msg.h has the message helpers.
 *
 * Variants (conf:variant=<name>): std (every row but C-INPUT.9), overfull (the runner
 * judges C-INPUT.9), show (the operator row C-INPUT.11).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_INPUT_H
#define AAR_CONF_INPUT_H

#include "conf_msg.h"

/* The runner posts one AAR_EV_STOP with AARPostEvent. */
#define CI_K_STOPEV    "CONF-INPUT-STOPEV"
/* The runner posts the events of ci_keys, ci_mouse or ci_joy. */
#define CI_K_KEYS      "CONF-INPUT-KEYS"
#define CI_K_MOUSE     "CONF-INPUT-MOUSESEQ"
#define CI_K_JOY       "CONF-INPUT-JOY"
/* a = AAR_BREAK_CTRL_D, _E or _F: the runner signals its own task Ctrl-D, Ctrl-E or Ctrl-F. */
#define CI_K_BREAK     "CONF-INPUT-BREAK"
/* a = n: the runner posts n key events (code 1 to n), retrying a refused post for a while,
 * and answers with a result message (count = how many were accepted). */
#define CI_K_FLOOD     "CONF-INPUT-FLOOD"
/* The runner posts 100 key events and then 20 mouse events of dx 1, with no retry, and
 * answers with a result message (count = key posts refused, errors = mouse posts refused,
 * x = mouse posts accepted). */
#define CI_K_MOUSEFULL "CONF-INPUT-MFULL"
/* The runner answers with a result message: count = events dropped since MOUSEFULL that
 * were mouse events (the counter minus the refused key posts). */
#define CI_K_DROPS     "CONF-INPUT-DROPS"
/* Variant overfull: the runner posts 300 key events (code 1 to 300) with no retry. */
#define CI_K_OVERFULL  "CONF-INPUT-OVERFULL"
/* Variant overfull: a = events the app polled, b = 1 when they were codes 1, 2, 3 ... */
#define CI_K_GOT       "CONF-INPUT-GOT"
/* Variant show: a = events the app saw (for the operator's list). */
#define CI_K_SHOWN     "CONF-INPUT-SHOWN"

#define CI_OVERFULL_POSTS 300u

struct ci_ev {
    uint16_t type, code;
    int16_t dx, dy;
    uint16_t buttons;
    uint32_t time_ms;
};

/* Raw keys with qualifiers and a time stamp: down, down, up, up. */
static const struct ci_ev ci_keys[] = {
    { 1u, 0x21u, 0, 0, 0x0001u, 1000u },
    { 1u, 0x22u, 0, 0, 0x0009u, 2000u },
    { 2u, 0x22u, 0, 0, 0x0009u, 3000u },
    { 2u, 0x21u, 0, 0, 0x0001u, 4000u },
};

/* Consecutive mouse events differ in their buttons, so none is folded into the one before. */
static const struct ci_ev ci_mouse[] = {
    { 3u, 0u, 3, -2, 0x0000u, 5000u },
    { 3u, 0u, -5, 7, 0x0001u, 5100u },
    { 3u, 0u, 100, 0, 0x0002u, 5200u },
    { 3u, 0u, 0, -100, 0x0004u, 5300u },
    { 3u, 0u, 1, 1, 0x0000u, 5400u },
};

static const struct ci_ev ci_joy[] = {
    { 4u, 0u, -1, 0, 0x0000u, 6000u },
    { 4u, 0u, 1, -1, 0x0001u, 6100u },
    { 4u, 0u, 0, 1, 0x0000u, 6200u },
    { 4u, 0u, 0, 0, 0x0001u, 6300u },
};

#define CI_COUNT(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

/* A keyword is at most 19 characters (cm_keyword). */
_Static_assert(sizeof CI_K_STOPEV <= 20 && sizeof CI_K_KEYS <= 20 && sizeof CI_K_MOUSE <= 20 &&
                   sizeof CI_K_JOY <= 20 && sizeof CI_K_BREAK <= 20 && sizeof CI_K_FLOOD <= 20 &&
                   sizeof CI_K_MOUSEFULL <= 20 && sizeof CI_K_DROPS <= 20 &&
                   sizeof CI_K_OVERFULL <= 20 && sizeof CI_K_GOT <= 20 && sizeof CI_K_SHOWN <= 20,
               "an input keyword is longer than 19 characters");

#endif
