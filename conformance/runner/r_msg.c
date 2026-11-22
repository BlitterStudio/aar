/*
 * Runner side of C-MSG (conformance/TESTS.md): runner-judged tests, peer keywords,
 * payload variants and run observers of the area. Nothing is written yet; see
 * aar-conf.h for what an area can register and conformance/CONTRIBUTING.md for
 * the rules. This file belongs to the C-MSG author.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include "aar-conf.h"

const struct conf_area conf_area_msg = {
    .name = "C-MSG",
};
