/*
 * aar_serve: the display backend (a Picasso96 screen, two screen buffers or a
 * panned tall screen, or a surface of its own shown in a window or scaled to
 * the screen; the palette, the input window, the pointer, the title, the mode
 * list and the vertical blank).
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

/* ---- The vertical blank: timer.device's VBLANK unit ------------------------------------- */

/* A timer request of the VBLANK unit with a time of one microsecond ends at the
 * next vertical blank: no interrupt server is needed, and the loop is woken by
 * the unit's reply port. */
static void vb_stop(struct aar_serve *s)
{
    if (!s->gfx.vb_pending)
        return;
    if (!X_CHECK_IO((struct IORequest *)s->gfx.vb_io))
        X_ABORT_IO((struct IORequest *)s->gfx.vb_io);
    X_WAIT_IO((struct IORequest *)s->gfx.vb_io);
    /* The reply set the port's signal and nothing is waiting for it. */
    X_SET_SIGNAL(0, 1UL << s->gfx.vb_port->mp_SigBit);
    s->gfx.vb_pending = 0;
}

static void vb_free(struct aar_serve *s)
{
    vb_stop(s);
    if (s->gfx.vb_io) {
        if (s->gfx.vb_io->tr_node.io_Device)
            X_CLOSE_DEVICE((struct IORequest *)s->gfx.vb_io);
        X_DELETE_IOREQ((struct IORequest *)s->gfx.vb_io);
        s->gfx.vb_io = NULL;
    }
    if (s->gfx.vb_port) {
        X_DELETE_PORT(s->gfx.vb_port);
        s->gfx.vb_port = NULL;
    }
}

static int vb_ready(struct aar_serve *s)
{
    if (s->gfx.vb_io)
        return 1;
    s->gfx.vb_port = X_CREATE_PORT();
    if (s->gfx.vb_port)
        s->gfx.vb_io = (struct timerequest *)X_CREATE_IOREQ(
            s->gfx.vb_port, sizeof(struct timerequest));
    if (!s->gfx.vb_io ||
        X_OPEN_DEVICE((CONST_STRPTR)TIMERNAME, UNIT_VBLANK,
                      (struct IORequest *)s->gfx.vb_io, 0)) {
        if (s->gfx.vb_io)
            s->gfx.vb_io->tr_node.io_Device = NULL;
        vb_free(s);
        return 0;
    }
    return 1;
}

static int display_op_vblank(void *ctx, int watch)
{
    struct aar_serve *s = ctx;

    if (!watch) {
        vb_stop(s);
        return 1;
    }
    if (!vb_ready(s))
        return 0;
    s->gfx.vb_io->tr_node.io_Command = TR_ADDREQUEST;
    s->gfx.vb_io->tr_time.tv_secs = 0;
    s->gfx.vb_io->tr_time.tv_micro = 1;
    X_SEND_IO((struct IORequest *)s->gfx.vb_io);
    s->gfx.vb_pending = 1;
    return 1;
}

static void service_vblank(struct aar_serve *s)
{
    if (!s->gfx.vb_pending || !X_CHECK_IO((struct IORequest *)s->gfx.vb_io))
        return;
    X_WAIT_IO((struct IORequest *)s->gfx.vb_io);
    s->gfx.vb_pending = 0;
    aar_serve_display_vblank_seen(&s->core.display);
}

/* ---- Windows, the pointer, the title ------------------------------------------------------ */

/* The window that shows or takes the input of the display: the display window
 * of a window display, else the backdrop input window. */
static struct Window *display_window(const struct aar_serve *s)
{
    return s->gfx.win ? s->gfx.win : s->gfx.input_win;
}

/* The drawing area of a display with a surface of its own: the inner area of
 * its window, or the whole screen. */
static struct RastPort *inner_area(const struct aar_serve *s, uint32_t *ox,
                                   uint32_t *oy, uint32_t *iw, uint32_t *ih)
{
    struct Window *win = display_window(s);

    *ox = *oy = *iw = *ih = 0;
    if (win && win->RPort) {
        int32_t w = (int32_t)win->Width - win->BorderLeft - win->BorderRight;
        int32_t h = (int32_t)win->Height - win->BorderTop - win->BorderBottom;

        *ox = (uint32_t)win->BorderLeft;
        *oy = (uint32_t)win->BorderTop;
        *iw = w > 0 ? (uint32_t)w : 0u;
        *ih = h > 0 ? (uint32_t)h : 0u;
        return win->RPort;
    }
    if (s->gfx.screen) {
        *iw = (uint32_t)s->gfx.screen->Width;
        *ih = (uint32_t)s->gfx.screen->Height;
        return &s->gfx.screen->RastPort;
    }
    return NULL;
}

/* Where a scaled surface is drawn. The next flip clears the area first. */
static void update_fit(struct aar_serve *s)
{
    uint32_t ox, oy, iw, ih;

    s->gfx.fit_w = 0;
    s->gfx.shown_w = 0;
    if (!s->gfx.scaled || !inner_area(s, &ox, &oy, &iw, &ih) || !iw || !ih)
        return;
    aar_serve_fit_rect(s->gfx.surf_w, s->gfx.surf_h, iw, ih, &s->gfx.fit_x,
                       &s->gfx.fit_y, &s->gfx.fit_w, &s->gfx.fit_h);
}

static void clear_inner(struct aar_serve *s)
{
    uint32_t ox, oy, iw, ih;
    struct RastPort *rp = inner_area(s, &ox, &oy, &iw, &ih);

    if (rp && iw && ih)
        X_P96_RECT_FILL(s, rp, ox, oy, ox + iw - 1u, oy + ih - 1u, 0UL);
}

/* The scaled rows are made and written a strip at a time. */
#define AAR_SERVE_STRIP_ROWS 16u

static void write_scaled(struct aar_serve *s, const struct RenderInfo *ri,
                         struct RastPort *rp, uint32_t dx, uint32_t dy,
                         uint32_t dw, uint32_t dh)
{
    uint32_t bpp = s->gfx.surf_bpp, row = dw * bpp, y;
    struct RenderInfo out;

    if (row == 0 || row > 32767u)
        return;
    if (s->gfx.strip_bytes < row * AAR_SERVE_STRIP_ROWS) {
        if (s->gfx.strip)
            X_FREE_MEM(s->gfx.strip, s->gfx.strip_bytes);
        s->gfx.strip_bytes = 0;
        s->gfx.strip = X_ALLOC_MEM(row * AAR_SERVE_STRIP_ROWS, MEMF_PUBLIC);
        if (!s->gfx.strip) {
            launcher_say(s, "no memory to scale the display");
            return;
        }
        s->gfx.strip_bytes = row * AAR_SERVE_STRIP_ROWS;
    }
    out.Memory = s->gfx.strip;
    out.BytesPerRow = (WORD)row;
    out.pad = 0;
    out.RGBFormat = (RGBFTYPE)s->gfx.surf_rgb;
    for (y = 0; y < dh; y += AAR_SERVE_STRIP_ROWS) {
        uint32_t n = dh - y < AAR_SERVE_STRIP_ROWS ? dh - y : AAR_SERVE_STRIP_ROWS;

        aar_serve_scale_nn((const uint8_t *)ri->Memory, (uint32_t)(UWORD)ri->BytesPerRow,
                           s->gfx.surf_w, s->gfx.surf_h, s->gfx.strip, row, dw, dh,
                           bpp, y, n);
        X_P96_WRITE_PIXEL_ARRAY(s, &out, 0, 0, rp, dx, dy + y, dw, n);
    }
}

/* A flip of a display with a surface of its own: draw the surface into the
 * window or the screen, scaled to the drawing area when it is a scaled display. */
static void display_op_present(void *ctx)
{
    struct aar_serve *s = ctx;
    uint32_t ox, oy, iw, ih, x = 0, y = 0, w, h;
    struct RastPort *rp;
    struct RenderInfo ri;
    LONG lock;

    if (!s->gfx.surface)
        return;
    rp = inner_area(s, &ox, &oy, &iw, &ih);
    if (!rp || !iw || !ih)
        return;
    if (s->gfx.scaled) {
        if (s->gfx.shown_w != iw || s->gfx.shown_h != ih || !s->gfx.fit_w) {
            update_fit(s);
            clear_inner(s);
            s->gfx.shown_w = iw;
            s->gfx.shown_h = ih;
        }
        x = s->gfx.fit_x;
        y = s->gfx.fit_y;
        w = s->gfx.fit_w;
        h = s->gfx.fit_h;
        if (!w || !h)
            return;
    } else {
        w = s->gfx.surf_w < iw ? s->gfx.surf_w : iw;
        h = s->gfx.surf_h < ih ? s->gfx.surf_h : ih;
    }
    memset(&ri, 0, sizeof(ri));
    lock = X_P96_LOCK_BITMAP(s, s->gfx.surface, &ri, sizeof(ri));
    if (lock) {
        if (s->gfx.scaled)
            write_scaled(s, &ri, rp, ox + x, oy + y, w, h);
        else
            X_P96_WRITE_PIXEL_ARRAY(s, &ri, 0, 0, rp, ox, oy, w, h);
        X_P96_UNLOCK_BITMAP(s, s->gfx.surface, lock);
    }
}

/* The inner size of the window, for AAR_EV_RESIZE; a scaled surface follows it. */
static void window_resized(struct aar_serve *s)
{
    uint32_t ox, oy, iw, ih;

    if (!inner_area(s, &ox, &oy, &iw, &ih))
        return;
    if (s->gfx.caps & AAR_DISPCAP_RESIZE)
        push_event(s, AAR_EV_RESIZE, 0, (int16_t)iw, (int16_t)ih, 0);
    if (s->gfx.scaled) {
        update_fit(s);
        display_op_present(s);
    }
}

/* Where the pointer is on the surface, for an absolute mouse event. */
static void surface_position(const struct aar_serve *s, int32_t mx, int32_t my,
                             int16_t *dx, int16_t *dy)
{
    struct Window *win = display_window(s);
    int32_t px = mx, py = my, sx, sy;

    if (win) {
        px -= win->BorderLeft;
        py -= win->BorderTop;
    }
    aar_serve_surface_pos(px, py, s->gfx.surf_w, s->gfx.surf_h,
                          s->gfx.scaled ? s->gfx.fit_x : 0u,
                          s->gfx.scaled ? s->gfx.fit_y : 0u,
                          s->gfx.scaled ? s->gfx.fit_w : 0u,
                          s->gfx.scaled ? s->gfx.fit_h : 0u, &sx, &sy);
    *dx = (int16_t)(sx > 32767 ? 32767 : (sx < -32768 ? -32768 : sx));
    *dy = (int16_t)(sy > 32767 ? 32767 : (sy < -32768 ? -32768 : sy));
}

/* A custom pointer is a BOOPSI pointer object over a two plane bitmap; it
 * is released after the window has another pointer. */
static void pointer_release(struct aar_serve *s)
{
    if (s->gfx.pointer_obj) {
        X_DISPOSE_POINTER(s->gfx.pointer_obj);
        s->gfx.pointer_obj = NULL;
    }
    if (s->gfx.pointer_bm) {
        X_FREE_BITMAP(s->gfx.pointer_bm);
        s->gfx.pointer_bm = NULL;
    }
}

static void pointer_reset(struct aar_serve *s)
{
    struct Window *win = display_window(s);

    if (win)
        X_SET_WINDOW_POINTER(win, TAG_DONE);
    pointer_release(s);
}

/* The three pointer colours of Preferences (colours 17 to 19), 0x00RRGGBB. */
static void pointer_palette(struct aar_serve *s, uint32_t palette[3])
{
    struct Preferences prefs;
    UWORD c[3];
    unsigned k;

    (void)s;
    memset(&prefs, 0, sizeof(prefs));
    X_GET_PREFS(&prefs, sizeof(prefs));
    c[0] = prefs.color17;
    c[1] = prefs.color18;
    c[2] = prefs.color19;
    for (k = 0; k < 3; k++)
        palette[k] = ((uint32_t)((c[k] >> 8) & 15u) * 17u << 16) |
                     ((uint32_t)((c[k] >> 4) & 15u) * 17u << 8) |
                     ((uint32_t)(c[k] & 15u) * 17u);
}

static int display_op_set_pointer(void *ctx, uint32_t mode, uint32_t width,
                                  uint32_t height, int32_t hot_x, int32_t hot_y,
                                  const uint8_t *argb)
{
    struct aar_serve *s = ctx;
    struct Window *win = display_window(s);
    struct BitMap *bm;
    APTR obj;
    uint32_t palette[3];

    if (!win)
        return AAR_E_NOSYS;
    if (mode == AAR_POINTER_DEFAULT) {
        pointer_reset(s);
        return AAR_OK;
    }
    if (mode == AAR_POINTER_BUSY) {
        X_SET_WINDOW_POINTER(win, WA_BusyPointer, TRUE, WA_PointerDelay, TRUE, TAG_DONE);
        pointer_release(s);
        return AAR_OK;
    }
    /* A sprite image needs Intuition 39. */
    if (X_INTUITION_VERSION() < 39UL)
        return AAR_E_NOSYS;
    if (mode == AAR_POINTER_HIDDEN) {
        width = 16;
        height = 1;
        hot_x = hot_y = 0;
    }
    bm = X_ALLOC_BITMAP((width + 15u) & ~15u, height, 2, BMF_CLEAR | BMF_DISPLAYABLE, NULL);
    if (!bm)
        return AAR_E_NOMEM;
    if (mode == AAR_POINTER_CUSTOM) {
        pointer_palette(s, palette);
        aar_serve_pointer_planes(argb, width, height, palette, (uint8_t *)bm->Planes[0],
                                 (uint8_t *)bm->Planes[1], bm->BytesPerRow);
    }
    obj = X_NEW_POINTER(bm, -hot_x, -hot_y);
    if (!obj) {
        X_FREE_BITMAP(bm);
        return AAR_E_NOMEM;
    }
    X_SET_WINDOW_POINTER(win, WA_Pointer, (ULONG)obj, TAG_DONE);
    pointer_release(s);
    s->gfx.pointer_obj = obj;
    s->gfx.pointer_bm = bm;
    return AAR_OK;
}

static int display_op_set_title(void *ctx, const char *title)
{
    struct aar_serve *s = ctx;

    snprintf(s->gfx.title_buf, sizeof(s->gfx.title_buf), "%s", title);
    if (s->gfx.win) {
        X_SET_WINDOW_TITLES(s->gfx.win, (CONST_STRPTR)s->gfx.title_buf,
                            (CONST_STRPTR)~0UL);
    } else if (s->gfx.input_win) {
        X_SET_WINDOW_TITLES(s->gfx.input_win, (CONST_STRPTR)~0UL,
                            (CONST_STRPTR)s->gfx.title_buf);
    } else if (s->gfx.screen) {
        s->gfx.screen->DefaultTitle = (UBYTE *)s->gfx.title_buf;
        s->gfx.screen->Title = (UBYTE *)s->gfx.title_buf;
    } else {
        return AAR_E_NOSYS;
    }
    return AAR_OK;
}

/* ---- Information and modes ------------------------------------------------------------------ */

/* The refresh rate of a display mode in millihertz, 0 when the monitor does not
 * say. */
static uint32_t mode_refresh_mhz(struct aar_serve *s, ULONG id)
{
    struct MonitorInfo mi;

    (void)s;
    if (!id || id == (ULONG)INVALID_ID)
        return 0;
    memset(&mi, 0, sizeof(mi));
    if (X_GET_DISPLAY_INFO(&mi, sizeof(mi), DTAG_MNTR, id) == 0)
        return 0;
    return aar_serve_refresh_mhz(mi.TotalRows, mi.TotalColorClocks, 3571429UL);
}

static void display_op_info(void *ctx, aar_display_info *out)
{
    struct aar_serve *s = ctx;
    struct Window *win = display_window(s);

    out->refresh_mhz = mode_refresh_mhz(s, s->gfx.mode_id);
    if (s->gfx.scr_w && s->gfx.scr_h) {
        out->screen_width = s->gfx.scr_w;
        out->screen_height = s->gfx.scr_h;
    }
    if (win ? (win->Flags & WFLG_WINDOWACTIVE) != 0
            : (s->gfx.screen && IntuitionBase->ActiveScreen == s->gfx.screen))
        out->flags |= AAR_DISPINFO_ACTIVE;
}

static uint32_t mode_formats(ULONG compatible)
{
    return ((compatible & RGBFF_R5G6B5) ? (1u << AAR_FMT_RGB565) : 0u) |
           ((compatible & RGBFF_B8G8R8A8) ? (1u << AAR_FMT_BGRA8888) : 0u) |
           ((compatible & RGBFF_CLUT) ? (1u << AAR_FMT_CLUT8) : 0u);
}

/* Every Picasso96 mode that can show one of the surface formats, and the one
 * of the Workbench screen's size marked as the preferred one. */
static uint32_t display_op_modes(void *ctx, aar_display_mode *out, uint32_t max)
{
    struct aar_serve *s = ctx;
    ULONG id = (ULONG)INVALID_ID;
    uint32_t total = 0;
    struct Screen *wb;

    if (!s->gfx.p96 || !GfxBase)
        return 0;
    while ((id = X_NEXT_DISPLAY_INFO(id)) != (ULONG)INVALID_ID) {
        struct DimensionInfo dim;
        struct DisplayInfo disp;
        uint32_t formats;

        if (!X_P96_MODE_ATTR(s, id, P96IDA_ISP96))
            continue;
        formats = mode_formats(X_P96_MODE_ATTR(s, id, P96IDA_COMPATIBLEFORMATS));
        if (!formats)
            continue;
        memset(&disp, 0, sizeof(disp));
        memset(&dim, 0, sizeof(dim));
        if (X_GET_DISPLAY_INFO(&disp, sizeof(disp), DTAG_DISP, id) == 0 ||
            disp.NotAvailable ||
            X_GET_DISPLAY_INFO(&dim, sizeof(dim), DTAG_DIMS, id) == 0)
            continue;
        if (total < max) {
            memset(&out[total], 0, sizeof(out[total]));
            out[total].width = (uint32_t)(dim.Nominal.MaxX - dim.Nominal.MinX + 1);
            out[total].height = (uint32_t)(dim.Nominal.MaxY - dim.Nominal.MinY + 1);
            out[total].refresh_mhz = mode_refresh_mhz(s, id);
            out[total].formats = formats;
        }
        total++;
    }
    wb = X_LOCK_PUB_SCREEN(NULL);
    if (wb) {
        aar_serve_modes_prefer(out, total < max ? total : max, (uint32_t)wb->Width,
                               (uint32_t)wb->Height);
        X_UNLOCK_PUB_SCREEN(NULL, wb);
    }
    return total;
}

/* ---- Opening and closing ----------------------------------------------------------------------- */

static ULONG format_rgb(uint32_t format)
{
    return format == AAR_FMT_BGRA8888 ? RGBFB_B8G8R8A8 :
           (format == AAR_FMT_CLUT8 ? RGBFB_CLUT : RGBFB_R5G6B5);
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
    vb_stop(s);
    pointer_reset(s);
    if (s->gfx.win && s->gfx.win != s->gfx.input_win)
        X_CLOSE_WINDOW(s->gfx.win);
    if (s->gfx.input_win) {
        X_CLOSE_WINDOW(s->gfx.input_win);
        s->gfx.input_win = NULL;
    }
    s->gfx.win = NULL;
    if (s->gfx.surface) {
        X_P96_FREE_BITMAP(s, s->gfx.surface);
        s->gfx.surface = NULL;
    }
    if (s->gfx.strip) {
        X_FREE_MEM(s->gfx.strip, s->gfx.strip_bytes);
        s->gfx.strip = NULL;
        s->gfx.strip_bytes = 0;
    }
    if (s->gfx.pub) {
        X_UNLOCK_PUB_SCREEN(NULL, s->gfx.pub);
        s->gfx.pub = NULL;
    }
    s->gfx.scaled = 0;
    s->gfx.caps = 0;
    s->gfx.fit_w = 0;
    s->gfx.shown_w = 0;
    s->gfx.scr_w = s->gfx.scr_h = 0;
    s->gfx.mode_id = 0;
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

/* A bitmap of the surface's format on the board, named by a token: the surface
 * of a window display or a scaled one. NULL when it cannot be made or lies
 * outside what the platform can see. */
static struct BitMap *make_surface(struct aar_serve *s, uint32_t w, uint32_t h,
                                   uint32_t format, ULONG *token, ULONG *pitch)
{
    ULONG rgb = format_rgb(format);
    struct BitMap *bm;

    if (w == 0 || h == 0 || w > AAR_SERVE_SCREEN_MAX || h > AAR_SERVE_SCREEN_MAX ||
        !claim_check(s))
        return NULL;
    bm = X_P96_ALLOC_BITMAP(s, w, h, aar_serve_display_depth(format), BMF_CLEAR, NULL, rgb);
    if (!bm)
        return NULL;
    if ((ULONG)X_P96_BITMAP_ATTR(s, bm, P96BMA_RGBFORMAT) != rgb ||
        !bitmap_token(s, bm, token, pitch)) {
        X_P96_FREE_BITMAP(s, bm);
        return NULL;
    }
    return bm;
}

static void adopt_surface(struct aar_serve *s, struct BitMap *bm, uint32_t w,
                          uint32_t h, uint32_t format)
{
    s->gfx.surface = bm;
    s->gfx.surf_rgb = format_rgb(format);
    s->gfx.surf_w = w;
    s->gfx.surf_h = h;
    s->gfx.surf_bpp = aar_serve_display_depth(format) / 8u;
}

/* The IDCMP classes of the input window. The pointer position is a delta unless
 * the display asked for absolute positions. */
static ULONG input_idcmp(uint32_t flags, int newsize)
{
    return IDCMP_RAWKEY | IDCMP_MOUSEMOVE | IDCMP_MOUSEBUTTONS | IDCMP_CLOSEWINDOW |
           IDCMP_ACTIVEWINDOW | IDCMP_INACTIVEWINDOW |
           ((flags & AAR_DISPLAY_POINTER_ABS) ? 0UL : IDCMP_DELTAMOVE) |
           (newsize ? IDCMP_NEWSIZE : 0UL);
}

/* The caps the open could give besides the window's: the pointer position
 * needs an input window, text input also the keymap. Sets gfx.caps. */
static uint32_t finish_features(struct aar_serve *s, uint32_t flags, uint32_t features)
{
    if (s->gfx.input_win) {
        features |= AAR_DISPCAP_POINTER_ABS;
        if ((flags & AAR_DISPLAY_TEXT_INPUT) && !s->in.keymap)
            s->in.keymap = X_OPEN_LIB((CONST_STRPTR)"keymap.library", 37);
        if (s->in.keymap)
            features |= AAR_DISPCAP_TEXT_INPUT;
    }
    s->gfx.caps = aar_serve_display_caps(flags, features);
    return features;
}

/* AAR_DISPLAY_WINDOW: the surface in a window on the default public screen. It
 * needs a Picasso96 screen that shows at least 16 bits per pixel; CLUT8 surfaces
 * keep a screen of their own (their palette would change the public screen's). */
static int open_window_display(struct aar_serve *s, uint32_t w, uint32_t h,
                               uint32_t format, uint32_t flags, ULONG *pitch,
                               ULONG *front, uint32_t *features_out)
{
    int input = !(s->cfg.flags & AAR_SERVEF_NO_INPUT_WINDOW);
    int size = input && (flags & AAR_DISPLAY_RESIZABLE);
    struct Screen *pub;
    struct BitMap *bm;
    struct Window *win;
    uint32_t features;

    if (format == AAR_FMT_CLUT8 || !s->gfx.p96 || !IntuitionBase || !GfxBase)
        return 0;
    pub = X_LOCK_PUB_SCREEN(NULL);
    if (!pub)
        return 0;
    if (!X_P96_BITMAP_ATTR(s, pub->RastPort.BitMap, P96BMA_ISP96) ||
        X_P96_BITMAP_ATTR(s, pub->RastPort.BitMap, P96BMA_BYTESPERPIXEL) < 2) {
        X_UNLOCK_PUB_SCREEN(NULL, pub);
        return 0;
    }
    bm = make_surface(s, w, h, format, front, pitch);
    if (!bm) {
        X_UNLOCK_PUB_SCREEN(NULL, pub);
        return 0;
    }
    snprintf(s->gfx.title_buf, sizeof(s->gfx.title_buf), "%s", s->title);
    win = X_OPEN_WINDOW(NULL,
        WA_PubScreen, (ULONG)pub,
        WA_Title, (ULONG)s->gfx.title_buf,
        WA_InnerWidth, w,
        WA_InnerHeight, h,
        WA_DragBar, TRUE,
        WA_DepthGadget, TRUE,
        WA_CloseGadget, input ? TRUE : FALSE,
        WA_SizeGadget, size ? TRUE : FALSE,
        WA_SizeBRight, size ? TRUE : FALSE,
        WA_SizeBBottom, size ? TRUE : FALSE,
        WA_MinWidth, 64,
        WA_MinHeight, 64,
        WA_Activate, TRUE,
        WA_SmartRefresh, TRUE,
        WA_NoCareRefresh, TRUE,
        WA_RMBTrap, TRUE,
        WA_ReportMouse, TRUE,
        WA_IDCMP, input ? input_idcmp(flags, size) : 0UL,
        TAG_DONE);
    if (!win) {
        X_P96_FREE_BITMAP(s, bm);
        X_UNLOCK_PUB_SCREEN(NULL, pub);
        return 0;
    }
    s->gfx.pub = pub;
    s->gfx.win = win;
    if (input)
        s->gfx.input_win = win;
    adopt_surface(s, bm, w, h, format);
    s->gfx.scaled = (flags & AAR_DISPLAY_SCALE) != 0;
    s->gfx.scr_w = (uint32_t)pub->Width;
    s->gfx.scr_h = (uint32_t)pub->Height;
    s->gfx.mode_id = X_GET_VP_MODE_ID(&pub->ViewPort);
    update_fit(s);
    clear_inner(s);
    features = AAR_DISPCAP_WINDOW | (input ? AAR_DISPCAP_CLOSE_REQUEST : 0u) |
               (size ? AAR_DISPCAP_RESIZE : 0u) | (s->gfx.scaled ? AAR_DISPCAP_SCALED : 0u);
    *features_out = finish_features(s, flags, features);
    return 1;
}

/* Opens the P96 screen for a DISPLAY_OPEN and finds the tokens of its bitmap
 * memory (pages of one tall screen when it pans), or opens a window display. A
 * scaled display on a screen has a surface of its own and a screen the size of
 * the Workbench screen. */
static int open_screen(struct aar_serve *s, uint32_t w, uint32_t h,
                       uint32_t format, uint32_t flags, ULONG forced_mode,
                       ULONG *pitch_out, ULONG *front_out, ULONG *back_out,
                       ULONG *spare_out, uint32_t *features_out)
{
    ULONG mode, depth, rgb, pitch = 0, front = 0, back = 0, spare = 0;
    uint32_t screen_w = w, screen_h = h, features = 0;
    int scale_screen = (flags & AAR_DISPLAY_SCALE) != 0;
    if (w == 0 || h == 0 || w > AAR_SERVE_SCREEN_MAX || h > AAR_SERVE_SCREEN_MAX)
        return 0;
    if (format != AAR_FMT_RGB565 && format != AAR_FMT_BGRA8888 &&
        format != AAR_FMT_CLUT8)
        return 0;
    if (!s->gfx.p96 || !IntuitionBase || !GfxBase)
        return 0;
    *features_out = 0;
    if ((flags & AAR_DISPLAY_WINDOW) &&
        open_window_display(s, w, h, format, flags, &pitch, &front, features_out)) {
        *pitch_out = pitch;
        *front_out = front;
        *back_out = 0;
        *spare_out = 0;
        s->gfx.flip_pending = 0;
        s->gfx.safe_pending = 0;
        return 1;
    }
    if (!s->gfx.pub_name[0])
        X_GET_DEFAULT_PUB_SCREEN((STRPTR)s->gfx.pub_name);
    depth = aar_serve_display_depth(format);
    rgb = format_rgb(format);
    if (scale_screen) {
        struct Screen *wb = X_LOCK_PUB_SCREEN(NULL);
        uint32_t wb_w = 0, wb_h = 0;
        struct BitMap *bm = NULL;

        if (wb) {
            wb_w = (uint32_t)wb->Width;
            wb_h = (uint32_t)wb->Height;
            X_UNLOCK_PUB_SCREEN(NULL, wb);
        }
        if (wb_w && wb_h && wb_w <= AAR_SERVE_SCREEN_MAX && wb_h <= AAR_SERVE_SCREEN_MAX)
            bm = make_surface(s, w, h, format, &front, &pitch);
        if (bm) {
            adopt_surface(s, bm, w, h, format);
            screen_w = wb_w;
            screen_h = wb_h;
        } else {
            scale_screen = 0;
        }
    }
    mode = mode_for(s, screen_w, screen_h, format, forced_mode);
    if (mode == (ULONG)INVALID_ID) {
        close_display(s);
        return 0;
    }
    if (!claim_check(s)) {
        close_display(s);
        return 0;
    }
    s->gfx.screen = open_p96_screen(s, mode, screen_w, screen_h, depth, rgb);
    if (!s->gfx.screen) {
        close_display(s);
        return 0;
    }
    s->gfx.mode_id = mode;
    s->gfx.scr_w = screen_w;
    s->gfx.scr_h = screen_h;
    if (scale_screen) {
        /* The surface is its own bitmap and every flip draws it scaled. */
        s->gfx.scaled = 1;
        features |= AAR_DISPCAP_SCALED;
    } else if (!bitmap_token(s, s->gfx.screen->RastPort.BitMap, &front, &pitch)) {
        launcher_say(s, "display unavailable (front bitmap address)");
        close_display(s);
        return 0;
    }
    if (!scale_screen && s->cfg.display_strategy == AAR_SERVE_DISPLAY_BUFFERS) {
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
    if (!scale_screen && !s->gfx.have_two) {
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
            WA_IDCMP, input_idcmp(flags, 0),
            TAG_DONE);
        if (!s->gfx.input_win) {
            close_display(s);
            return 0;
        }
    }
    if (s->gfx.scaled)
        update_fit(s);
    *features_out = finish_features(s, flags, features);
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
    service_vblank(s);
}

/* DISPLAY_OPEN: opens the screen, finds the bitmaps' tokens and reports them
 * in the completion. The card binds its scanout to them when it sees it. */
static void open_display(struct aar_serve *s, const struct aar_serve_req *req,
                         struct aar_serve_done *done)
{
    ULONG pitch = 0, front = 0, back = 0, spare = 0;
    uint32_t features = 0;

    if (!open_screen(s, req->arg[AAR_A_DISPLAY_OPEN_W],
                     req->arg[AAR_A_DISPLAY_OPEN_H],
                     req->arg[AAR_A_DISPLAY_OPEN_FORMAT],
                     req->arg[AAR_A_DISPLAY_OPEN_FLAGS], s->cfg.screen_mode,
                     &pitch, &front, &back, &spare, &features)) {
        launcher_say(s, "cannot open the requested screen");
        s->display_failed = 1;
        want_stop(s, AAR_SERVE_WHY_DISPLAY);
        memset(done, 0, sizeof(*done));
        done->result = AAR_E_IO;
        return;
    }
    aar_serve_display_opened(&s->core.display, req, pitch, front, back, spare,
                             s->gfx.have_two, s->gfx.have_pan, (uint32_t)s->gfx.pan_count,
                             features, done);
}

static void display_op_open(void *ctx, const struct aar_serve_req *req,
                            struct aar_serve_done *done)
{
    struct aar_serve *s = ctx;

    open_display(s, req, done);
}

/* DISPLAY_RESIZE of a window display: a new surface bitmap of the new size. The
 * old one stays until the new one exists, so a failure leaves the display as it
 * was. The core records the new size from the completion. */
static void display_op_resize(void *ctx, const struct aar_serve_req *req,
                              struct aar_serve_done *done)
{
    struct aar_serve *s = ctx;
    uint32_t w = req->arg[AAR_A_DISPLAY_OPEN_W];
    uint32_t h = req->arg[AAR_A_DISPLAY_OPEN_H];
    uint32_t format = req->arg[AAR_A_DISPLAY_OPEN_FORMAT];
    ULONG pitch = 0, token = 0;
    struct BitMap *bm;

    memset(done, 0, sizeof(*done));
    if (!s->gfx.win || !s->gfx.surface) {
        done->result = AAR_E_NOSYS;
        return;
    }
    bm = make_surface(s, w, h, format, &token, &pitch);
    if (!bm) {
        done->result = AAR_E_NOMEM;
        return;
    }
    X_P96_FREE_BITMAP(s, s->gfx.surface);
    adopt_surface(s, bm, w, h, format);
    update_fit(s);
    done->result = AAR_OK;
    done->out[AAR_R_DISPLAY_OPEN_PITCH] = pitch;
    done->out[AAR_R_DISPLAY_OPEN_FRONT] = token;
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
    display_op_wait_tof, display_op_load_rgb32, display_op_close,
    display_op_present, display_op_info, display_op_modes, display_op_resize,
    display_op_set_title, display_op_set_pointer, display_op_vblank
};
