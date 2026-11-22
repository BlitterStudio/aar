/*
 * What the C-GL payload (conf_gl.c) and runner/r_gl.c share. The runner supplies its own
 * display table to aar_serve (runner/r_gfx.c), so it reads the frame that aar_gl_swap
 * presents. conf_msg.h has the message helpers.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_GL_H
#define AAR_CONF_GL_H

#include "conf_msg.h"

/* a = x, b = y: the runner answers with a result message whose count is the pixel of its
 * copy of the frame as 0x00RRGGBB (ok is 0 when it has no frame or cannot decode it). */
#define CG_K_PIXEL "CONF-GL-PIXEL"

_Static_assert(sizeof CG_K_PIXEL <= 20, "a GL keyword is longer than 19 characters");

#endif
