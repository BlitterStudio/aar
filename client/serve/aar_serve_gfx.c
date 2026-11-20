/*
 * aar_serve: the display backend (a Picasso96 screen, two screen buffers or a
 * panned tall screen, the palette, the input window).
 *
 * This file is part of aar_serve.c, which includes it after the services of the
 * loop and before the public calls; it is not compiled alone. State lives in the
 * instance (struct aar_serve in aar_serve_priv.h).
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */

/* Tokens are board offsets: AARSharedToken turns the address of a P96 bitmap
 * into the token the platform understands and refuses an address outside the
 * platform's shared aperture. */

static int bitmap_token(struct aar_serve *s, struct BitMap *bm, ULONG *token,
                        ULONG *pitch)
{
    APTR mem;
    ULONG bytes, rows;
    if (!bm)
        return 0;
    mem = (APTR)X_P96_BITMAP_ATTR(s, bm, P96BMA_MEMORY);
    if (!mem)
        mem = bm->Planes[0];
    bytes = (ULONG)X_P96_BITMAP_ATTR(s, bm, P96BMA_BYTESPERROW);
    if (!bytes)
        bytes = bm->BytesPerRow;
    if (!bytes)
        return 0;
    rows = bm->Rows ? (ULONG)bm->Rows : 1UL;
    if (!claim_live(s) ||
        claim_note(s, X_AAR_SHARED_TOKEN(s->handle, mem, bytes * rows, token)) !=
            AAR_OK)
        return 0;
    /* The platform validates the spans of the front, back and spare buffers
     * (it answers AAR_E_INVAL for a bad one), as the library does for an
     * address outside the shared aperture. */
    *pitch = bytes;
    return 1;
}

/* Only SafeMessage has a reply port: DisplayMessage is not used. Every
 * successful ChangeScreenBuffer contributes one reply before teardown. */
static void flip_reap_safe_messages(struct aar_serve *s)
{
    while (s->gfx.safe_port && X_GET_MSG(s->gfx.safe_port))
        s->gfx.safe_pending = aar_serve_screen_safe_completed(s->gfx.safe_pending);
}

static void flip_wait_safe_messages(struct aar_serve *s)
{
    while (s->gfx.safe_pending != 0) {
        X_WAIT_PORT(s->gfx.safe_port);
        flip_reap_safe_messages(s);
    }
}

static void close_display(struct aar_serve *s)
{
    if (s->gfx.have_two && s->gfx.buf_front && s->gfx.buf_back && s->gfx.screen && s->gfx.safe_port) {
        /* A prior app flip may still own either buffer. */
        flip_wait_safe_messages(s);
        s->gfx.flip_pending = 0;
        if (s->core.display.showing && X_CHANGE_SCREEN_BUFFER(s->gfx.screen, s->gfx.buf_front)) {
            s->gfx.safe_pending = aar_serve_screen_safe_started(s->gfx.safe_pending);
            s->core.display.showing = 0;
            /* The restoring change has its own SafeMessage. */
            flip_wait_safe_messages(s);
        }
    }
    if (s->gfx.input_win) {
        X_CLOSE_WINDOW(s->gfx.input_win);
        s->gfx.input_win = NULL;
    }
    if (s->gfx.buf_back && s->gfx.screen)
        X_FREE_SCREEN_BUFFER(s->gfx.screen, s->gfx.buf_back);
    if (s->gfx.buf_front && s->gfx.screen)
        X_FREE_SCREEN_BUFFER(s->gfx.screen, s->gfx.buf_front);
    if (s->gfx.safe_port) {
        X_DELETE_PORT(s->gfx.safe_port);
        s->gfx.safe_port = NULL;
    }
    s->gfx.safe_pending = 0;
    s->gfx.buf_back = NULL;
    s->gfx.buf_front = NULL;
    if (s->gfx.screen) {
        X_P96_CLOSE_SCREEN(s, s->gfx.screen);
        s->gfx.screen = NULL;
    }
    /* AllocScreenBuffer owns both double-buffer bitmaps. */
    s->gfx.have_two = 0;
    s->gfx.have_pan = 0;
    s->gfx.pan_count = 0;
    s->core.display.showing = 0;
    if (s->gfx.pub_name[0]) {
        struct Screen *pub = X_LOCK_PUB_SCREEN((CONST_STRPTR)s->gfx.pub_name);
        if (pub) {
            X_SCREEN_TO_FRONT(pub);
            X_UNLOCK_PUB_SCREEN(NULL, pub);
        }
    }
}

static ULONG mode_for(struct aar_serve *s, uint32_t w, uint32_t h,
                      uint32_t format, ULONG forced)
{
    ULONG depth = aar_serve_display_depth(format);
    ULONG formats = format == AAR_FMT_BGRA8888 ? RGBFF_B8G8R8A8 :
                    (format == AAR_FMT_CLUT8 ? RGBFF_CLUT : RGBFF_R5G6B5);
    if (forced)
        return forced;
    return X_P96_BEST_MODE_ID(s,
        P96BIDTAG_NominalWidth, w,
        P96BIDTAG_NominalHeight, h,
        P96BIDTAG_Depth, depth,
        P96BIDTAG_FormatsAllowed, formats,
        TAG_DONE);
}

static struct Screen *open_p96_screen(struct aar_serve *s, ULONG mode,
                                      uint32_t w, uint32_t h, ULONG depth,
                                      ULONG rgb)
{
    struct Screen *scr = X_P96_OPEN_SCREEN(s,
        P96SA_DisplayID, mode,
        P96SA_Width, w,
        P96SA_Height, h,
        P96SA_Depth, depth,
        P96SA_RGBFormat, rgb,
        P96SA_Quiet, TRUE,
        P96SA_ShowTitle, FALSE,
        P96SA_AutoScroll, FALSE,
        P96SA_Title, (ULONG)s->title,
        TAG_DONE);

    if (scr && (ULONG)X_P96_BITMAP_ATTR(s, scr->RastPort.BitMap, P96BMA_RGBFORMAT) != rgb) {
        X_P96_CLOSE_SCREEN(s, scr);
        scr = NULL;
    }
    return scr;
}

/* Opens the P96 screen for a DISPLAY_OPEN and finds the tokens of its bitmap
 * memory (pages of one tall screen when it pans). */
static int open_screen(struct aar_serve *s, uint32_t w, uint32_t h,
                       uint32_t format, ULONG forced_mode, ULONG *pitch_out,
                       ULONG *front_out, ULONG *back_out, ULONG *spare_out)
{
    ULONG mode, depth, rgb, pitch = 0, front = 0, back = 0, spare = 0;
    if (w == 0 || h == 0 || w > AAR_SERVE_SCREEN_MAX || h > AAR_SERVE_SCREEN_MAX)
        return 0;
    if (format != AAR_FMT_RGB565 && format != AAR_FMT_BGRA8888 &&
        format != AAR_FMT_CLUT8)
        return 0;
    if (!s->gfx.p96 || !IntuitionBase || !GfxBase)
        return 0;
    if (!s->gfx.pub_name[0])
        X_GET_DEFAULT_PUB_SCREEN((STRPTR)s->gfx.pub_name);
    depth = aar_serve_display_depth(format);
    rgb = format == AAR_FMT_BGRA8888 ? RGBFB_B8G8R8A8 :
          (format == AAR_FMT_CLUT8 ? RGBFB_CLUT : RGBFB_R5G6B5);
    mode = mode_for(s, w, h, format, forced_mode);
    if (mode == (ULONG)INVALID_ID)
        return 0;
    if (!claim_check(s))
        return 0;
    s->gfx.screen = open_p96_screen(s, mode, w, h, depth, rgb);
    if (!s->gfx.screen)
        return 0;
    if (!bitmap_token(s, s->gfx.screen->RastPort.BitMap, &front, &pitch)) {
        launcher_say(s, "display unavailable (front bitmap address)");
        close_display(s);
        return 0;
    }
    if (s->cfg.display_strategy == AAR_SERVE_DISPLAY_BUFFERS) {
        s->gfx.buf_front = X_ALLOC_SCREEN_BUFFER(s->gfx.screen, NULL, SB_SCREEN_BITMAP);
        if (!s->gfx.buf_front) {
            launcher_say(s, "double buffering unavailable (front screen buffer)");
        } else {
            s->gfx.buf_back = X_ALLOC_SCREEN_BUFFER(s->gfx.screen, NULL, 0);
            if (!s->gfx.buf_back) {
                launcher_say(s, "double buffering unavailable (back screen buffer)");
            } else {
                ULONG back_pitch = 0;

                if (!bitmap_token(s, s->gfx.buf_back->sb_BitMap, &back, &back_pitch)) {
                    launcher_say(s, "double buffering unavailable (back bitmap address)");
                } else if (back_pitch != pitch) {
                    launcher_say(s, "double buffering unavailable (back bitmap pitch)");
                } else {
                    s->gfx.safe_port = X_CREATE_PORT();
                    if (!s->gfx.safe_port) {
                        launcher_say(s, "double buffering unavailable (safe message port)");
                    } else if (!s->gfx.buf_front->sb_DBufInfo || !s->gfx.buf_back->sb_DBufInfo) {
                        launcher_say(s, "double buffering unavailable (screen buffer info)");
                    } else {
                        s->gfx.buf_front->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort =
                            s->gfx.safe_port;
                        s->gfx.buf_back->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort =
                            s->gfx.safe_port;
                        s->gfx.have_two = 1;
                    }
                }
            }
        }
    }
    if (!s->gfx.have_two) {
        struct Screen *tall = NULL;
        ULONG tall_front = 0, tall_pitch = 0;

        if (s->gfx.safe_port) {
            X_DELETE_PORT(s->gfx.safe_port);
            s->gfx.safe_port = NULL;
        }
        if (s->gfx.buf_back && s->gfx.screen)
            X_FREE_SCREEN_BUFFER(s->gfx.screen, s->gfx.buf_back);
        if (s->gfx.buf_front && s->gfx.screen)
            X_FREE_SCREEN_BUFFER(s->gfx.screen, s->gfx.buf_front);
        s->gfx.buf_back = NULL;
        s->gfx.buf_front = NULL;
        back = 0;
        if (s->cfg.display_strategy <= AAR_SERVE_DISPLAY_PAN3 &&
            h <= AAR_SERVE_SCREEN_MAX / 3u)
            tall = open_p96_screen(s, mode, w, h * 3u, depth, rgb);
        if (tall && tall->Height >= (WORD)(h * 3u) &&
            bitmap_token(s, tall->RastPort.BitMap, &tall_front, &tall_pitch)) {
            X_P96_CLOSE_SCREEN(s, s->gfx.screen);
            s->gfx.screen = tall;
            front = tall_front;
            pitch = tall_pitch;
            back = front + pitch * h;
            spare = back + pitch * h;
            s->gfx.have_pan = 1;
            s->gfx.pan_count = 3;
            launcher_say(s, "triple buffering by panning");
        } else {
            if (tall)
                X_P96_CLOSE_SCREEN(s, tall);
            tall = NULL;
            if (s->cfg.display_strategy <= AAR_SERVE_DISPLAY_PAN2 &&
                h <= AAR_SERVE_SCREEN_MAX / 2u)
                tall = open_p96_screen(s, mode, w, h * 2u, depth, rgb);
            if (tall && tall->Height >= (WORD)(h * 2u) &&
                bitmap_token(s, tall->RastPort.BitMap, &tall_front, &tall_pitch)) {
                X_P96_CLOSE_SCREEN(s, s->gfx.screen);
                s->gfx.screen = tall;
                front = tall_front;
                pitch = tall_pitch;
                back = front + pitch * h;
                s->gfx.have_pan = 1;
                s->gfx.pan_count = 2;
                launcher_say(s, "double buffering by panning");
            } else {
                if (tall)
                    X_P96_CLOSE_SCREEN(s, tall);
                launcher_say(s, "single buffer; the platform copies each frame");
            }
        }
    }
    if (!(s->cfg.flags & AAR_SERVEF_NO_INPUT_WINDOW)) {
        s->gfx.input_win = X_OPEN_WINDOW(NULL,
            WA_CustomScreen, (ULONG)s->gfx.screen,
            WA_Left, 0, WA_Top, 0,
            WA_Width, s->gfx.screen->Width,
            WA_Height, s->gfx.screen->Height,
            WA_Borderless, TRUE,
            WA_Backdrop, TRUE,
            WA_RMBTrap, TRUE,
            WA_ReportMouse, TRUE,
            WA_Activate, TRUE,
            WA_NoCareRefresh, TRUE,
            WA_IDCMP, IDCMP_RAWKEY | IDCMP_MOUSEMOVE | IDCMP_DELTAMOVE |
                      IDCMP_MOUSEBUTTONS | IDCMP_CLOSEWINDOW,
            TAG_DONE);
        if (!s->gfx.input_win) {
            close_display(s);
            return 0;
        }
    }
    *pitch_out = pitch;
    *front_out = front;
    *back_out = back;
    *spare_out = spare;
    s->gfx.flip_pending = 0;
    s->gfx.safe_pending = 0;
    return 1;
}

/* A SafeMessage of a ChangeScreenBuffer flip came back: the old buffer may be
 * drawn into again, so the app's flip completes. */
static void service_display(struct aar_serve *s)
{
    while (s->gfx.safe_port && s->gfx.safe_pending && X_GET_MSG(s->gfx.safe_port)) {
        s->gfx.safe_pending = aar_serve_screen_safe_completed(s->gfx.safe_pending);
        aar_serve_display_flip_safe(&s->core.display);
    }
}

/* DISPLAY_OPEN: opens the screen, finds the bitmaps' tokens and reports them
 * in the completion. The card binds its scanout to them when it sees it. */
static void open_display(struct aar_serve *s, const struct aar_serve_req *req,
                         struct aar_serve_done *done)
{
    ULONG pitch = 0, front = 0, back = 0, spare = 0;

    if (!open_screen(s, req->arg[AAR_A_DISPLAY_OPEN_W],
                     req->arg[AAR_A_DISPLAY_OPEN_H],
                     req->arg[AAR_A_DISPLAY_OPEN_FORMAT], s->cfg.screen_mode,
                     &pitch, &front, &back, &spare)) {
        launcher_say(s, "cannot open the requested screen");
        s->display_failed = 1;
        want_stop(s, AAR_SERVE_WHY_DISPLAY);
        memset(done, 0, sizeof(*done));
        done->result = AAR_E_IO;
        return;
    }
    aar_serve_display_opened(&s->core.display, req, pitch, front, back, spare,
                             s->gfx.have_two, s->gfx.have_pan, (uint32_t)s->gfx.pan_count, done);
}

static void display_op_open(void *ctx, const struct aar_serve_req *req,
                            struct aar_serve_done *done)
{
    struct aar_serve *s = ctx;

    open_display(s, req, done);
}

static int display_op_change_buffer(void *ctx, int to_back)
{
    struct aar_serve *s = ctx;

    if (!s->gfx.screen || !s->gfx.buf_front || !s->gfx.buf_back ||
        !X_CHANGE_SCREEN_BUFFER(s->gfx.screen, to_back ? s->gfx.buf_back : s->gfx.buf_front))
        return 0;
    s->gfx.safe_pending = aar_serve_screen_safe_started(s->gfx.safe_pending);
    return 1;
}

static void display_op_pan(void *ctx, uint32_t page)
{
    struct aar_serve *s = ctx;

    s->gfx.screen->ViewPort.RasInfo->RyOffset = (WORD)(page * s->core.display.height);
    X_SCROLL_VPORT(&s->gfx.screen->ViewPort);
}

static void display_op_wait_tof(void *ctx)
{
    (void)ctx;
    X_WAIT_TOF();
}

static void display_op_load_rgb32(void *ctx, const uint32_t *table)
{
    struct aar_serve *s = ctx;

    X_LOAD_RGB32(&s->gfx.screen->ViewPort, (ULONG *)table);
}

static void display_op_close(void *ctx)
{
    struct aar_serve *s = ctx;

    close_display(s);
}

/* The display table of the built-in backend; the instance copies it and sets ctx. */
static const struct aar_serve_display_ops aar_serve_gfx_backend = {
    NULL, display_op_open, display_op_change_buffer, display_op_pan,
    display_op_wait_tof, display_op_load_rgb32, display_op_close
};
