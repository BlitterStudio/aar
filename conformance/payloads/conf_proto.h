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

#endif
