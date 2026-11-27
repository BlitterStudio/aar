/*
 * aar_serve: the input backend (the input window's events and the joystick).
 *
 * This file is part of aar_serve.c, which includes it after the services of the
 * loop and before the public calls; it is not compiled alone. State lives in the
 * instance (struct aar_serve in aar_serve_priv.h).
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */

/* The ReadJoyPort values the CD32 mapping in aar_serve_core.c assumes. */
_Static_assert(AAR_SERVE_JP_TYPE_GAMECTLR == JP_TYPE_GAMECTLR &&
               AAR_SERVE_JP_TYPE_JOYSTK == JP_TYPE_JOYSTK,
               "ReadJoyPort controller types");
_Static_assert(AAR_SERVE_JPF_BLUE == JPF_BUTTON_BLUE && AAR_SERVE_JPF_RED == JPF_BUTTON_RED &&
               AAR_SERVE_JPF_YELLOW == JPF_BUTTON_YELLOW &&
               AAR_SERVE_JPF_GREEN == JPF_BUTTON_GREEN &&
               AAR_SERVE_JPF_FORWARD == JPF_BUTTON_FORWARD &&
               AAR_SERVE_JPF_REVERSE == JPF_BUTTON_REVERSE &&
               AAR_SERVE_JPF_PLAY == JPF_BUTTON_PLAY,
               "ReadJoyPort buttons");
_Static_assert(AAR_SERVE_JPF_UP == JPF_JOY_UP && AAR_SERVE_JPF_DOWN == JPF_JOY_DOWN &&
               AAR_SERVE_JPF_LEFT == JPF_JOY_LEFT && AAR_SERVE_JPF_RIGHT == JPF_JOY_RIGHT,
               "ReadJoyPort directions");

static uint16_t break_signal_for_rawkey(uint16_t code, uint16_t qualifiers)
{
    if (!(qualifiers & IEQUALIFIER_CONTROL))
        return 0;
    switch (code) {
    case 0x22u: return AAR_BREAK_CTRL_D;
    case 0x12u: return AAR_BREAK_CTRL_E;
    case 0x23u: return AAR_BREAK_CTRL_F;
    default: return 0;
    }
}

static ULONG break_signal_bit(uint16_t signal)
{
    switch (signal) {
    case AAR_BREAK_CTRL_D: return SIGBREAKF_CTRL_D;
    case AAR_BREAK_CTRL_E: return SIGBREAKF_CTRL_E;
    case AAR_BREAK_CTRL_F: return SIGBREAKF_CTRL_F;
    default: return 0;
    }
}

/* The characters one key press makes, as AAR_EV_TEXT. The window's keymap is the
 * default one (MapRawKey with no keymap); dead keys are the previous keys that
 * Intuition keeps in the message's IAddress. */
static void push_text(struct aar_serve *s, uint16_t raw, uint16_t qual, APTR dead)
{
    struct InputEvent ie;
    UBYTE mapped[16];
    uint16_t chars[16];
    uint32_t i, count;
    LONG n;

    memset(&ie, 0, sizeof(ie));
    ie.ie_Class = IECLASS_RAWKEY;
    ie.ie_Code = raw;
    ie.ie_Qualifier = qual;
    ie.ie_EventAddress = dead;
    n = X_MAP_RAW_KEY(s, &ie, (STRPTR)mapped, (LONG)sizeof(mapped));
    if (n <= 0)
        return;
    count = aar_serve_text_chars(mapped, (int32_t)n, qual,
                                 IEQUALIFIER_LCOMMAND | IEQUALIFIER_RCOMMAND, chars, 16);
    for (i = 0; i < count; i++)
        push_event(s, AAR_EV_TEXT, chars[i], 0, 0, qual);
}

static void drain_window(struct aar_serve *s, struct Window *win)
{
    struct IntuiMessage *im;
    int absolute = (s->gfx.caps & AAR_DISPCAP_POINTER_ABS) != 0;
    int extended_wheel;

    if (!win || !win->UserPort)
        return;
    extended_wheel = (win->IDCMPFlags & AAR_IDCMP_EXTENDEDMOUSE) != 0;
    while ((im = (struct IntuiMessage *)X_GET_MSG(win->UserPort)) != NULL) {
        ULONG class = im->Class;
        UWORD code = im->Code;
        UWORD qual = im->Qualifier;
        WORD mx = im->MouseX;
        WORD my = im->MouseY;
        APTR dead = NULL;

        if (class == AAR_IDCMP_EXTENDEDMOUSE) {
            int16_t delta[2] = {0, 0};

            if (extended_wheel && code == AAR_IMSGCODE_INTUIWHEELDATA && im->IAddress) {
                const uint8_t *data = im->IAddress;
                uint16_t version;

                /* Read the Version word before using the rest of IAddress.
                 * V1 has a reserved word followed by signed WheelX and WheelY. */
                memcpy(&version, data, sizeof(version));
                if (version == AAR_INTUIWHEELDATA_VERSION)
                    memcpy(delta, data + 2u * sizeof(version), sizeof(delta));
            }
            X_REPLY_MSG((struct Message *)im);
            if (delta[0] || delta[1])
                push_event(s, AAR_EV_WHEEL, 0, delta[0], delta[1], qual);
            continue;
        }

        /* Intuition keeps the previous keys of a dead key sequence in the
         * message's IAddress, which is gone once the message is replied. */
        if ((class & IDCMP_RAWKEY) && im->IAddress)
            dead = *(APTR *)im->IAddress;
        X_REPLY_MSG((struct Message *)im);
        if (class & IDCMP_ACTIVEWINDOW)
            push_event(s, AAR_EV_ACTIVE, 1, 0, 0, 0);
        if (class & IDCMP_INACTIVEWINDOW)
            push_event(s, AAR_EV_ACTIVE, 0, 0, 0, 0);
        if (class & IDCMP_NEWSIZE)
            window_resized(s);
        if (class & IDCMP_CLOSEWINDOW) {
            /* With AAR_DISPCAP_CLOSE_REQUEST the app decides what closing means. */
            if (s->gfx.caps & AAR_DISPCAP_CLOSE_REQUEST)
                push_event(s, AAR_EV_CLOSE_REQUEST, 0, 0, 0, 0);
            else
                want_stop(s, AAR_SERVE_WHY_WINDOW);
        }
        if (class & IDCMP_RAWKEY) {
            uint16_t raw = (uint16_t)(code & 0x7f);
            uint16_t type = (code & IECODE_UP_PREFIX) ? AAR_EV_KEY_UP
                                                     : AAR_EV_KEY_DOWN;
            uint16_t signal = type == AAR_EV_KEY_DOWN ?
                break_signal_for_rawkey(raw, qual) : 0;
            int16_t wheel_dx, wheel_dy;

            if (aar_serve_wheel_key(raw, &wheel_dx, &wheel_dy)) {
                /* Drop the release and use only one wheel source per window. */
                if (!extended_wheel && type == AAR_EV_KEY_DOWN)
                    push_event(s, AAR_EV_WHEEL, 0, wheel_dx, wheel_dy, qual);
                continue;
            }

            /* Ctrl-C and Ctrl-D/E/F are the loop's only while break_mask says so:
             * otherwise the key is the program's, an ordinary key event. */
            if (signal && !(s->cfg.break_mask & break_signal_bit(signal)))
                signal = 0;
            if (type == AAR_EV_KEY_DOWN &&
                (s->cfg.break_mask & SIGBREAKF_CTRL_C) &&
                aar_serve_rawkey_requests_stop(raw, qual, IEQUALIFIER_CONTROL)) {
                s->user_stop = 1;
                want_stop(s, AAR_SERVE_WHY_BREAK);
            }
            push_event(s, type, raw, 0, 0, qual);
            if (type == AAR_EV_KEY_DOWN && (s->gfx.caps & AAR_DISPCAP_TEXT_INPUT))
                push_text(s, raw, qual, dead);
            if (signal)
                push_event(s, AAR_EV_BREAK, signal, 0, 0, qual);
        }
        if (class & IDCMP_MOUSEBUTTONS) {
            uint16_t id = 0;
            int16_t px = 0, py = 0;

            if (code == SELECTDOWN) { s->in.mouse_buttons |= 1u; id = 1; }
            else if (code == SELECTUP) { s->in.mouse_buttons &= (uint16_t)~1u; id = 1; }
            else if (code == MENUDOWN) { s->in.mouse_buttons |= 2u; id = 2; }
            else if (code == MENUUP) { s->in.mouse_buttons &= (uint16_t)~2u; id = 2; }
            else if (code == MIDDLEDOWN) { s->in.mouse_buttons |= 4u; id = 3; }
            else if (code == MIDDLEUP) { s->in.mouse_buttons &= (uint16_t)~4u; id = 3; }
            if (absolute)
                surface_position(s, mx, my, &px, &py);
            push_event(s, AAR_EV_MOUSE, id, px, py,
                       (uint16_t)(s->in.mouse_buttons | (absolute ? AAR_MOUSE_ABSOLUTE : 0u)));
        }
        if (class & IDCMP_MOUSEMOVE) {
            if (absolute) {
                int16_t px, py;

                surface_position(s, mx, my, &px, &py);
                push_event(s, AAR_EV_MOUSE, 0, px, py,
                           (uint16_t)(s->in.mouse_buttons | AAR_MOUSE_ABSOLUTE));
            } else {
                int16_t dx = (int16_t)mx;
                int16_t dy = (int16_t)my;

                /* The library sums consecutive motion into one pending event. */
                if (dx || dy)
                    push_event(s, AAR_EV_MOUSE, 0, dx, dy, s->in.mouse_buttons);
            }
        }
    }
}

/* One CD32 pad or joystick port read with lowlevel.library: AAR_JOY_* buttons
 * and the axes. A port with nothing a pad or joystick in it reads as released. */
static void poll_cd32(struct aar_serve *s, ULONG port)
{
    uint32_t now = now_ms(s);
    uint16_t buttons = 0;
    int16_t dx = 0, dy = 0;
    ULONG state, jp;

    /* ReadJoyPort senses the controller by driving the port pins: about once a
     * frame is often enough. */
    if (s->in.joy_valid[port] && now - s->in.cd32_ms[port] < 16u)
        return;
    s->in.cd32_ms[port] = now;
    jp = X_READ_JOY_PORT(s, port);
    if (aar_serve_cd32_valid(jp)) {
        buttons = aar_serve_cd32_buttons(jp);
        aar_serve_cd32_axes(jp, &dx, &dy);
    }
    state = ((ULONG)buttons << 8) | ((ULONG)(dx + 1) << 4) | (ULONG)(dy + 1);
    if (s->in.joy_valid[port] && state == s->in.joy_last[port])
        return;
    s->in.joy_last[port] = state;
    s->in.joy_valid[port] = 1;
    push_event(s, AAR_EV_JOY, (uint16_t)port, dx, dy, buttons);
}

/*
 * Read-only joystick sampling: JOYxDAT and the CIA-A fire bits.  The launcher
 * this code comes from used lowlevel.library's ReadJoyPort(), whose controller
 * auto-sensing drives the port pins and left the left mouse button repeating
 * and releasing on its own after the launcher had exited.  Reading these
 * registers changes no state.
 * One fire button per port; a mouse in a polled port reads as movement.  The
 * second and third button of a stick are on the POT lines, which belong to
 * potgo.resource and the input handlers (a mouse's right and middle buttons are
 * on them too), so they are not read; only a port in cfg.cd32_ports, which is
 * read with ReadJoyPort, reports more than one button.
 */
static void poll_joysticks(struct aar_serve *s)
{
    ULONG port;

    if (!s->gfx.input_win || !(s->gfx.input_win->Flags & WFLG_WINDOWACTIVE))
        return;
    for (port = 0; port < 2; port++) {
        UWORD dat;
        int fire;
        ULONG state;
        int16_t dx = 0, dy = 0;

        if (s->cfg.cd32_ports & (1UL << port)) {
            poll_cd32(s, port);
            continue;
        }
        if (!(s->cfg.joy_ports & (1UL << port)))
            continue;
        dat = X_JOYDAT(port);
        fire = (X_CIAA_PRA() & (port ? 0x80u : 0x40u)) == 0;
        state = ((ULONG)(dat & 0x0303u) << 1) | (ULONG)fire;
        if (s->in.joy_valid[port] && state == s->in.joy_last[port])
            continue;
        s->in.joy_last[port] = state;
        s->in.joy_valid[port] = 1;
        if (dat & 0x0200u) dx = -1;
        else if (dat & 0x0002u) dx = 1;
        if (((dat >> 9) ^ (dat >> 8)) & 1u) dy = -1;
        else if (((dat >> 1) ^ dat) & 1u) dy = 1;
        push_event(s, AAR_EV_JOY, (uint16_t)port, dx, dy, fire ? 1u : 0u);
    }
}
