/*
 * Compile-only check of the aar.library client headers for AmigaOS m68k (see
 * m68k_check.sh). It includes proto/aar.h as a program does and calls every
 * vector once, so that every inline caller is compiled and its register
 * assignment is checked by the compiler. It is never linked or run.
 *
 * SPDX-License-Identifier: MIT
 */
#include <proto/aar.h>
#include <libraries/aar_zz9000.h>

struct Library *AARBase;

LONG aar_m68k_use(struct MsgPort *port, const char *path, const char *const *argv,
                  const void *data, void *dst, const aar_event *event,
                 AARLogSink sink)
{
    AARInfo info;
    AARImageInfo image;
    AARStatusInfo status;
    AARMessage message;
    AARServiceReq req;
    struct AARSharedMap map;
    AARHandle *handle = 0;
    ULONG reason = 0, got = 0, token = 0;
    LONG rc = 0;
    const struct aar_client_ext_zz9000 *ext;

    rc += AARQuery(&info);
    rc += AAROpen(&handle, port, &reason);
    rc += AARProbeFile(path, &image);
    rc += AARProbeMemory(data, 100, &image);
    rc += AARSetTagList(handle, 0);
    rc += AARSetTags(handle, AAR_TAG_WEIGHT, 4, AAR_TAG_PRIVATE_MIB, 64, TAG_DONE);
    rc += AARLoadFile(handle, path);
    rc += AARLoadMemory(handle, data, 100);
    rc += AARStart(handle, argv, 1);
    rc += AARStatus(handle, &status);
    rc += AARSendMessage(handle, &message);
    rc += AARGetMessage(handle, &message);
    rc += (LONG)AARMessageSignal(handle);
    rc += AARSharedMap(handle, 0x1000, 0x2000, 0, &map);
    rc += AARSharedPublish(handle, &map, 0, 16);
    rc += AARSharedAcquire(handle, &map, 0, 16);
    rc += AARSharedUnmap(handle, &map);
    rc += AARServiceNext(handle, &req);
    rc += AARServiceReply(handle, &req);
    rc += AARPostEvent(handle, event);
    rc += AARAudioRead(handle, dst, 64, &got);
    rc += AARSharedToken(handle, dst, 64, &token);
    ext = (const struct aar_client_ext_zz9000 *)AARExt(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_CLIENT);
    if (AAR_CLIENT_EXT_ZZ9000_COVERS(ext, list))
        rc += ext->list(0, 0);
    rc += AARSetLogSink(handle, sink, dst);
    rc += AARSetLogSink(handle, 0, 0);
    rc += AARStop(handle);
    rc += AARClose(handle);
    return rc;
}
