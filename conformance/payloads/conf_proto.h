/*
 * What a payload and the runner (conformance/runner/aar-conf.c) agree on, in a
 * form both can include: plain macros, no types. conf.h has the rest of the
 * payload protocol.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_CONF_PROTO_H
#define AAR_CONF_PROTO_H

/* The message (32 bytes, the keyword first, the rest 0) with which a payload
 * asks the runner to stop it, as a client would (aar_serve_stop). */
#define CONF_MSG_STOP "CONF-STOP-NOW"

/* The one reason with which a payload may skip a MUST row: the platform declines an optional
 * route (C-AUDIO.1 to C-AUDIO.9 when `aar_audio_open` for the route PLATFORM returns AAR_E_NOSYS,
 * spec 8.4). The runner and tools/check_results.py accept a SKIP of a MUST test whose reason
 * starts with this text, and no other. */
#define CONF_SKIP_NO_PLATFORM_ROUTE "no PLATFORM route"

#endif
