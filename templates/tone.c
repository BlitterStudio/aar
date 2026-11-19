/*
 * Stereo square-wave tone. It uses the normal aar_audio ring, so whatever
 * route the platform chooses needs no app-side changes. The pitch follows the
 * granted rate.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

AAR_NOTE(0, 0, AAR_GROUP_AUDIO, 0)

#define TONE_HZ 440u

int aar_main(int argc, char **argv)
{
    aar_audio audio;
    uint32_t phase = 0u;
    uint32_t period;
    int rc;

    (void)argc;
    (void)argv;
    rc = aar_audio_open(AAR_AUDIO_RATE_22050, 2u, &audio);
    if (rc != AAR_OK) {
        if (aar_should_stop())
            return 0;
        return 20;
    }
    period = audio.rate / TONE_HZ;
    while (!aar_should_stop()) {
        /* Acquire the runtime's read index before reusing the slots it freed. */
        uint32_t read = __atomic_load_n(audio.read_frame, __ATOMIC_ACQUIRE);
        uint32_t write = *audio.write_frame;
        uint32_t used = write - read;
        uint32_t frames;

        if (used > audio.frames)
            break;
        frames = audio.frames - used;
        if (frames > 128u)
            frames = 128u;
        if (frames == 0u) {
            aar_yield();
            continue;
        }
        while (frames--) {
            int16_t value = phase < period / 2u ? 12000 : -12000;
            uint32_t slot = write & (audio.frames - 1u);

            audio.samples[slot * 2u] = value;
            audio.samples[slot * 2u + 1u] = value;
            write++;
            phase++;
            if (phase == period)
                phase = 0u;
        }
        /* Release: the PCM stores above are ordered before the new index. */
        __atomic_store_n(audio.write_frame, write, __ATOMIC_RELEASE);
        aar_yield();
    }
    (void)aar_audio_close();
    return 0;
}
