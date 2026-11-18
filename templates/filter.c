/*
 * File-in/file-out service template. It writes the SHA-256 of an input file,
 * while exercising the client's file calls and the services group. It needs
 * the optional aar_map_file call and fails with 10 where the runtime has none.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

AAR_NOTE(0, 0, 0, 0)

static int write_all(int fd, const uint8_t *data, uint32_t bytes)
{
    uint32_t done = 0;

    while (done < bytes) {
        uint32_t written = 0;
        int rc = aar_file_write(fd, data + done, bytes - done, &written);

        if (rc != AAR_OK || written == 0u)
            return rc != AAR_OK ? rc : AAR_E_IO;
        done += written;
    }
    return AAR_OK;
}

int aar_main(int argc, char **argv)
{
    const void *input;
    uint64_t input_bytes;
    uint8_t digest[AAR_SHA256_BYTES];
    int out;
    int rc;

    if (argc != 3)
        return 5;
    rc = aar_map_file(argv[1], &input, &input_bytes);
    if (rc != AAR_OK)
        return 10;
    rc = aar_sha256(input, input_bytes, digest);
    aar_unmap_file(input);
    if (rc != AAR_OK)
        return 10;
    out = aar_file_open(argv[2], AAR_FILE_WRITE | AAR_FILE_CREATE);
    if (out < 0)
        return 20;
    rc = write_all(out, digest, sizeof digest);
    (void)aar_file_close(out);
    return rc == AAR_OK ? 0 : 20;
}
