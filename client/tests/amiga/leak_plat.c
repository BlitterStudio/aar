/*
 * The platform entry point of the leak-test library: the reference platform,
 * reset once.
 *
 * SPDX-License-Identifier: MIT
 */
#include "../../src/aar_plat.h"
#include "../../src/refplat.h"

struct aar_plat *refplat_get(void);

struct aar_plat *aar_plat_get(void)
{
    static int started;

    if (!started) {
        refplat_reset();
        started = 1;
    }
    return refplat_get();
}
