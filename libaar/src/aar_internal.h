/*
 * Symbols crt0 and the GL wrappers share with the rest of libaar. Not an app
 * header.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_INTERNAL_H
#define AAR_INTERNAL_H

#include "aar/app.h"

/* crt0 calls this first, with the table pointer the runtime passed in r0 or
 * x0. It also resets the bound GL table. aar_api_ptr is declared in aar/app.h. */
void aar_crt0_bind(const aar_api *api);

/* The table that the last successful aar_gl_bind returned, or NULL. */
const aar_gl *aar_gl_bound(void);

#endif
