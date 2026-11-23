/*
 * The areas that have a runner file r_<area>.c, one line each. Every area has its
 * line already; an area author edits r_<area>.c and not this file. The core
 * (aar-conf.c) includes this list twice: to declare and to collect
 * `conf_area_<area>`, which r_<area>.c defines.
 *
 * SPDX-License-Identifier: MIT
 */
CONF_R_AREA(elf)
CONF_R_AREA(entry)
CONF_R_AREA(table)
CONF_R_AREA(mem)
CONF_R_AREA(shared)
CONF_R_AREA(msg)
CONF_R_AREA(queue)
CONF_R_AREA(file)
CONF_R_AREA(con)
CONF_R_AREA(time)
CONF_R_AREA(life)
CONF_R_AREA(client)
CONF_R_AREA(display)
CONF_R_AREA(input)
CONF_R_AREA(audio)
CONF_R_AREA(gl)
CONF_R_AREA(services)
CONF_R_AREA(free)
