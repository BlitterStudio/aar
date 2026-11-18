/*
 * Opens 320x240 RGB565, fills the back buffer, flips until stop or a key.
 * Draws the CPU mapping from aar_display_open. Does not call GL. A key is an
 * input event, so the note lists AAR_GROUP_INPUT as optional; without it the
 * app still ends on stop.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aar/app.h"
#include "aar/note.h"

AAR_NOTE(0, 0, AAR_GROUP_DISPLAY, AAR_GROUP_INPUT)

static void put_rgb565(uint8_t *dst, uint16_t pixel, uint32_t layout)
{
    if (layout == AAR_LAYOUT_BGR565_BE || layout == AAR_LAYOUT_BGR565_LE)
        pixel = (uint16_t)((pixel & 0x07e0u) |
                           ((pixel & 0xf800u) >> 11) |
                           ((pixel & 0x001fu) << 11));
    if (layout == AAR_LAYOUT_RGB565_BE || layout == AAR_LAYOUT_BGR565_BE) {
        dst[0] = (uint8_t)(pixel >> 8);
        dst[1] = (uint8_t)pixel;
    } else {
        dst[0] = (uint8_t)pixel;
        dst[1] = (uint8_t)(pixel >> 8);
    }
}

int aar_main(int argc, char **argv)
{
    aar_surface surface = {0};
    aar_event ev;
    uint32_t n;
    uint32_t y;
    uint32_t x;
    int rc;

    (void)argc;
    (void)argv;
    rc = aar_display_open(320, 240, AAR_FMT_RGB565, AAR_DISPLAY_VSYNC, &surface);
    if (rc != AAR_OK)
        aar_exit(rc);
    if (surface.layout == 0u)
        surface.layout = AAR_LAYOUT_RGB565_BE;
    if (!surface.back || surface.pitch < surface.width * 2u || surface.height == 0 ||
        (surface.layout != AAR_LAYOUT_RGB565_BE &&
         surface.layout != AAR_LAYOUT_RGB565_LE &&
         surface.layout != AAR_LAYOUT_BGR565_BE &&
         surface.layout != AAR_LAYOUT_BGR565_LE))
        aar_exit(AAR_E_IO);

    for (y = 0; y < surface.height; y++) {
        uint8_t *row = (uint8_t *)surface.back + (size_t)y * surface.pitch;
        for (x = 0; x < surface.width; x++) {
            uint16_t px = (uint16_t)(((x & 32u) ? 0xf800u : 0u) |
                                     ((y & 32u) ? 0x07e0u : 0u) |
                                     0x001fu);
            put_rgb565(row + x * 2u, px, surface.layout);
        }
    }
    if (surface.front && surface.front != surface.back) {
        for (y = 0; y < surface.height; y++)
            __builtin_memcpy((uint8_t *)surface.front + (size_t)y * surface.pitch,
                             (uint8_t *)surface.back + (size_t)y * surface.pitch,
                             surface.width * 2u);
    }

    for (;;) {
        if (aar_should_stop())
            break;
        n = 0;
        rc = aar_event_poll(&ev, 1, &n);
        if (rc == AAR_OK && n == 1 &&
            (ev.type == AAR_EV_STOP || ev.type == AAR_EV_KEY_DOWN))
            break;
        rc = aar_display_flip(surface.id);
        if (rc != AAR_OK) {
            if (aar_should_stop())
                break;
            aar_exit(rc);
        }
        rc = aar_display_refresh(&surface);
        if (rc != AAR_OK && rc != AAR_E_NOSYS) {
            if (aar_should_stop())
                break;
            aar_exit(rc);
        }
        aar_yield();
    }
    aar_display_close(surface.id);
    return 0;
}
