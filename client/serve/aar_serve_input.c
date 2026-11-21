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

static void drain_window(struct aar_serve *s, struct Window *win)
{
    struct IntuiMessage *im;
    if (!win || !win->UserPort)
        return;
    while ((im = (struct IntuiMessage *)X_GET_MSG(win->UserPort)) != NULL) {
        ULONG class = im->Class;
        UWORD code = im->Code;
        UWORD qual = im->Qualifier;
        WORD mx = im->MouseX;
        WORD my = im->MouseY;
        X_REPLY_MSG((struct Message *)im);
        if (class & IDCMP_CLOSEWINDOW)
            want_stop(s, AAR_SERVE_WHY_WINDOW);
        if (class & IDCMP_RAWKEY) {
            uint16_t raw = (uint16_t)(code & 0x7f);
            uint16_t type = (code & IECODE_UP_PREFIX) ? AAR_EV_KEY_UP
                                                     : AAR_EV_KEY_DOWN;
            uint16_t signal = type == AAR_EV_KEY_DOWN ?
                break_signal_for_rawkey(raw, qual) : 0;

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
            if (signal)
                push_event(s, AAR_EV_BREAK, signal, 0, 0, qual);
        }
        if (class & IDCMP_MOUSEBUTTONS) {
            uint16_t id = 0;
            if (code == SELECTDOWN) { s->in.mouse_buttons |= 1u; id = 1; }
            else if (code == SELECTUP) { s->in.mouse_buttons &= (uint16_t)~1u; id = 1; }
            else if (code == MENUDOWN) { s->in.mouse_buttons |= 2u; id = 2; }
            else if (code == MENUUP) { s->in.mouse_buttons &= (uint16_t)~2u; id = 2; }
            else if (code == MIDDLEDOWN) { s->in.mouse_buttons |= 4u; id = 3; }
            else if (code == MIDDLEUP) { s->in.mouse_buttons &= (uint16_t)~4u; id = 3; }
            push_event(s, AAR_EV_MOUSE, id, 0, 0, s->in.mouse_buttons);
        }
        if (class & IDCMP_MOUSEMOVE) {
            int16_t dx = (int16_t)mx;
            int16_t dy = (int16_t)my;

            /* The library sums consecutive motion into one pending event. */
            if (dx || dy)
                push_event(s, AAR_EV_MOUSE, 0, dx, dy, s->in.mouse_buttons);
        }
    }
}

/*
 * Read-only joystick sampling: JOYxDAT and the CIA-A fire bits.  The launcher
 * this code comes from used lowlevel.library's ReadJoyPort(), whose controller
 * auto-sensing drives the port pins and left the left mouse button repeating
 * and releasing on its own after the launcher had exited.  Reading these
 * registers changes no state.
 * One fire button per port; a mouse in a polled port reads as movement.
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
