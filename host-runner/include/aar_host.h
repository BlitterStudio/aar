/*
 * Calls that exist only in libaar-host, for tests and for a harness that
 * links the runner into its own process. An app never includes this header:
 * it is not part of AAR.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_HOST_H
#define AAR_HOST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Resolves a shared-buffer token to the address the process uses for it. The
 * token is aar_shared.haddr of a live block; `offset` and `bytes` name a range
 * inside the block. Returns AAR_OK and stores the address in *out, or
 * AAR_E_INVAL for an unknown token or a range outside the block. This is what
 * the client side of a real platform does with the token.
 */
int aar_host_shared_resolve(uint32_t haddr, uint64_t offset, uint64_t bytes,
                            void **out);

#ifdef __cplusplus
}
#endif

#endif
