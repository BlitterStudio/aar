/*
 * SDL keyboard, mouse, wheel, text, window and game controller events become
 * AAR_EV_* with Amiga raw key codes. The up bit is not stored in code; the
 * event type distinguishes it. Qualifiers sit in buttons using the Amiga
 * IEQUALIFIER bits. Ctrl-D/E/F (raw 0x22/0x12/0x23) also enqueue AAR_EV_BREAK
 * with codes 4/5/6. Ctrl-C enqueues one AAR_EV_STOP and sets the stop flag; so
 * does a window close, unless the display was granted AAR_DISPCAP_CLOSE_REQUEST,
 * which turns it into AAR_EV_CLOSE_REQUEST. The queue holds 256 events and
 * drops the newest when full, except mouse motion, which merges as spec 8.3
 * describes: it sums the deltas (saturating at the int16 range) or, for an
 * absolute event, keeps the latest position, and joins the queue when there
 * is room. AAR_EV_STOP and AAR_EV_LOW_MEMORY are never dropped: they wait
 * outside the queue and aar_event_poll delivers them ahead of it. A stop is a
 * request and gives one event; a low-memory notification is one per crossing
 * of the low-water mark (spec 4.1), so it is a count. The poll delivers the
 * lifecycle events and, as this runner has the input group, the input events.
 * AAR_EV_TEXT, AAR_EV_RESIZE and AAR_EV_CLOSE_REQUEST leave the poll only while
 * the open display has the cap that asks for them; AAR_EV_WHEEL and
 * AAR_EV_ACTIVE always do.
 *
 * Game controllers (SDL_GameController) are ports 2 and up, in the order they
 * are opened. The d-pad, or else the left stick, gives dx and dy; A, B, X, Y,
 * start, right shoulder and left shoulder are AAR_JOY_BUTTON1 to BUTTON4,
 * PLAY, FORWARD and REVERSE. A raw SDL joystick that is not a game controller
 * keeps the old mapping: joystick 0 is port 1, joystick 1 is port 0.
 *
 * --input FILE injects scripted events before the app starts. Lines:
 *   key down|up <raw> [qualifiers]
 *   mouse <dx> <dy> [buttons]
 *   mousebtn <1|2|3> down|up
 *   joy <port> <dx> <dy> <buttons>
 *   break <4|5|6>
 *   wheel <dx> <dy> [qualifiers]
 *   text <code point> [qualifiers]
 *   active <0|1>
 *   close
 *   lowmem
 *   stop
 *   resize <w> <h>
 * Numbers accept a 0x prefix. '#' starts a comment. lowmem enqueues
 * AAR_EV_LOW_MEMORY at once (the heap raises it too, when its free pool falls
 * below the low-water mark, memory.c). mouse takes the buttons word as it is,
 * so 0x8000 (AAR_MOUSE_ABSOLUTE) makes an absolute event. text, close and
 * resize enqueue AAR_EV_TEXT, AAR_EV_CLOSE_REQUEST and AAR_EV_RESIZE; they
 * reach the app only while the display has the matching cap. Scripted events
 * are queued before the app starts, so an app that polls before it opens the
 * display loses those.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "host_internal.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST_QUAL_LSHIFT 0x0001u
#define HOST_QUAL_RSHIFT 0x0002u
#define HOST_QUAL_CAPS 0x0004u
#define HOST_QUAL_CTRL 0x0008u
#define HOST_QUAL_LALT 0x0010u
#define HOST_QUAL_RALT 0x0020u
#define HOST_QUAL_LCMD 0x0040u
#define HOST_QUAL_RCMD 0x0080u
#define HOST_QUAL_NUMPAD 0x0100u
#define HOST_QUAL_REPEAT 0x0200u

static aar_event queue[HOST_INPUT_CAP];
static uint32_t q_head, q_tail;
static uint32_t drops;
static uint16_t mouse_buttons;
static int stop_event_sent;
static int stop_pending;           /* AAR_EV_STOP waits here: one event per stop request */
static uint32_t low_pending;       /* AAR_EV_LOW_MEMORY waits here: one per crossing */
#define HOST_LOW_PENDING_MAX 255u  /* bounds the count, as the ring depth bounds the queue */
#define HOST_PADS 4
#define HOST_PAD_PORT_BASE 2u
#define HOST_PAD_NEUTRAL ((1u << 20) | (1u << 16))
struct pad {
    SDL_GameController *gc;
    SDL_JoystickID id;
    uint32_t last;                 /* packed state of the last event */
};
static struct pad pads[HOST_PADS];
static SDL_Joystick *joys[2];      /* raw joysticks that are not game controllers */
static uint32_t joy_last[2];
static int joy_valid[2];
static aar_event motion_pending;   /* merged mouse motion that waits for room */
static int motion_valid;
static int quit_follows_close;     /* the SDL_QUIT of a window close the app asked to hear of */

struct raw_map {
    SDL_Scancode scan;
    uint16_t raw;
    int keypad;
};

static const struct raw_map raw_table[] = {
    {SDL_SCANCODE_GRAVE, 0x00u, 0},
    {SDL_SCANCODE_1, 0x01u, 0},
    {SDL_SCANCODE_2, 0x02u, 0},
    {SDL_SCANCODE_3, 0x03u, 0},
    {SDL_SCANCODE_4, 0x04u, 0},
    {SDL_SCANCODE_5, 0x05u, 0},
    {SDL_SCANCODE_6, 0x06u, 0},
    {SDL_SCANCODE_7, 0x07u, 0},
    {SDL_SCANCODE_8, 0x08u, 0},
    {SDL_SCANCODE_9, 0x09u, 0},
    {SDL_SCANCODE_0, 0x0au, 0},
    {SDL_SCANCODE_MINUS, 0x0bu, 0},
    {SDL_SCANCODE_EQUALS, 0x0cu, 0},
    {SDL_SCANCODE_BACKSLASH, 0x0du, 0},
    {SDL_SCANCODE_KP_0, 0x0fu, 1},
    {SDL_SCANCODE_Q, 0x10u, 0},
    {SDL_SCANCODE_W, 0x11u, 0},
    {SDL_SCANCODE_E, 0x12u, 0},
    {SDL_SCANCODE_R, 0x13u, 0},
    {SDL_SCANCODE_T, 0x14u, 0},
    {SDL_SCANCODE_Y, 0x15u, 0},
    {SDL_SCANCODE_U, 0x16u, 0},
    {SDL_SCANCODE_I, 0x17u, 0},
    {SDL_SCANCODE_O, 0x18u, 0},
    {SDL_SCANCODE_P, 0x19u, 0},
    {SDL_SCANCODE_LEFTBRACKET, 0x1au, 0},
    {SDL_SCANCODE_RIGHTBRACKET, 0x1bu, 0},
    {SDL_SCANCODE_KP_1, 0x1du, 1},
    {SDL_SCANCODE_KP_2, 0x1eu, 1},
    {SDL_SCANCODE_KP_3, 0x1fu, 1},
    {SDL_SCANCODE_A, 0x20u, 0},
    {SDL_SCANCODE_S, 0x21u, 0},
    {SDL_SCANCODE_D, 0x22u, 0},
    {SDL_SCANCODE_F, 0x23u, 0},
    {SDL_SCANCODE_G, 0x24u, 0},
    {SDL_SCANCODE_H, 0x25u, 0},
    {SDL_SCANCODE_J, 0x26u, 0},
    {SDL_SCANCODE_K, 0x27u, 0},
    {SDL_SCANCODE_L, 0x28u, 0},
    {SDL_SCANCODE_SEMICOLON, 0x29u, 0},
    {SDL_SCANCODE_APOSTROPHE, 0x2au, 0},
    {SDL_SCANCODE_KP_4, 0x2du, 1},
    {SDL_SCANCODE_KP_5, 0x2eu, 1},
    {SDL_SCANCODE_KP_6, 0x2fu, 1},
    {SDL_SCANCODE_NONUSBACKSLASH, 0x30u, 0},
    {SDL_SCANCODE_Z, 0x31u, 0},
    {SDL_SCANCODE_X, 0x32u, 0},
    {SDL_SCANCODE_C, 0x33u, 0},
    {SDL_SCANCODE_V, 0x34u, 0},
    {SDL_SCANCODE_B, 0x35u, 0},
    {SDL_SCANCODE_N, 0x36u, 0},
    {SDL_SCANCODE_M, 0x37u, 0},
    {SDL_SCANCODE_COMMA, 0x38u, 0},
    {SDL_SCANCODE_PERIOD, 0x39u, 0},
    {SDL_SCANCODE_SLASH, 0x3au, 0},
    {SDL_SCANCODE_KP_PERIOD, 0x3cu, 1},
    {SDL_SCANCODE_KP_7, 0x3du, 1},
    {SDL_SCANCODE_KP_8, 0x3eu, 1},
    {SDL_SCANCODE_KP_9, 0x3fu, 1},
    {SDL_SCANCODE_SPACE, 0x40u, 0},
    {SDL_SCANCODE_BACKSPACE, 0x41u, 0},
    {SDL_SCANCODE_TAB, 0x42u, 0},
    {SDL_SCANCODE_KP_ENTER, 0x43u, 1},
    {SDL_SCANCODE_RETURN, 0x44u, 0},
    {SDL_SCANCODE_ESCAPE, 0x45u, 0},
    {SDL_SCANCODE_DELETE, 0x46u, 0},
    {SDL_SCANCODE_KP_MINUS, 0x4au, 1},
    {SDL_SCANCODE_UP, 0x4cu, 0},
    {SDL_SCANCODE_DOWN, 0x4du, 0},
    {SDL_SCANCODE_RIGHT, 0x4eu, 0},
    {SDL_SCANCODE_LEFT, 0x4fu, 0},
    {SDL_SCANCODE_F1, 0x50u, 0},
    {SDL_SCANCODE_F2, 0x51u, 0},
    {SDL_SCANCODE_F3, 0x52u, 0},
    {SDL_SCANCODE_F4, 0x53u, 0},
    {SDL_SCANCODE_F5, 0x54u, 0},
    {SDL_SCANCODE_F6, 0x55u, 0},
    {SDL_SCANCODE_F7, 0x56u, 0},
    {SDL_SCANCODE_F8, 0x57u, 0},
    {SDL_SCANCODE_F9, 0x58u, 0},
    {SDL_SCANCODE_F10, 0x59u, 0},
    {SDL_SCANCODE_KP_DIVIDE, 0x5cu, 1},
    {SDL_SCANCODE_KP_MULTIPLY, 0x5du, 1},
    {SDL_SCANCODE_KP_PLUS, 0x5eu, 1},
    {SDL_SCANCODE_LSHIFT, 0x60u, 0},
    {SDL_SCANCODE_RSHIFT, 0x61u, 0},
    {SDL_SCANCODE_CAPSLOCK, 0x62u, 0},
    {SDL_SCANCODE_LCTRL, 0x63u, 0},
    {SDL_SCANCODE_RCTRL, 0x63u, 0},
    {SDL_SCANCODE_LALT, 0x64u, 0},
    {SDL_SCANCODE_RALT, 0x65u, 0},
    {SDL_SCANCODE_LGUI, 0x66u, 0},
    {SDL_SCANCODE_RGUI, 0x67u, 0},
};

static int raw_from_scan(SDL_Scancode scan, uint16_t *raw, int *keypad)
{
    size_t i;

    for (i = 0; i < sizeof raw_table / sizeof raw_table[0]; i++) {
        if (raw_table[i].scan == scan) {
            *raw = raw_table[i].raw;
            *keypad = raw_table[i].keypad;
            return 1;
        }
    }
    return 0;
}

static uint16_t qualifiers(SDL_Keymod mod, int keypad, int repeat)
{
    uint16_t q = 0;

    if (mod & KMOD_LSHIFT)
        q |= HOST_QUAL_LSHIFT;
    if (mod & KMOD_RSHIFT)
        q |= HOST_QUAL_RSHIFT;
    if (mod & KMOD_CAPS)
        q |= HOST_QUAL_CAPS;
    if (mod & KMOD_CTRL)
        q |= HOST_QUAL_CTRL;
    if (mod & KMOD_LALT)
        q |= HOST_QUAL_LALT;
    if (mod & KMOD_RALT)
        q |= HOST_QUAL_RALT;
    if (mod & KMOD_LGUI)
        q |= HOST_QUAL_LCMD;
    if (mod & KMOD_RGUI)
        q |= HOST_QUAL_RCMD;
    if (keypad)
        q |= HOST_QUAL_NUMPAD;
    if (repeat)
        q |= HOST_QUAL_REPEAT;
    return q;
}

static uint16_t break_for_raw(uint16_t raw, uint16_t qual)
{
    if ((qual & HOST_QUAL_CTRL) == 0u)
        return 0;
    if (raw == 0x22u)
        return AAR_BREAK_CTRL_D;
    if (raw == 0x12u)
        return AAR_BREAK_CTRL_E;
    if (raw == 0x23u)
        return AAR_BREAK_CTRL_F;
    return 0;
}

static int ring_room(void)
{
    return q_head - q_tail < HOST_INPUT_CAP;
}

static void ring_put(const aar_event *ev)
{
    queue[q_head & (HOST_INPUT_CAP - 1u)] = *ev;
    q_head++;
}

/* The merged motion joins the queue once there is room. */
static void flush_motion(void)
{
    if (motion_valid && ring_room()) {
        ring_put(&motion_pending);
        motion_valid = 0;
    }
}

static int16_t sat16(int32_t v)
{
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return (int16_t)v;
}

static void push_event(uint16_t type, uint16_t code, int16_t dx, int16_t dy,
                       uint16_t buttons)
{
    aar_event ev;
    int motion = type == AAR_EV_MOUSE && code == 0u;

    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.code = code;
    ev.dx = dx;
    ev.dy = dy;
    ev.buttons = buttons;
    /* AAR_EV_LOW_MEMORY has no field but its type (spec 4.1). */
    ev.time_ms = type == AAR_EV_LOW_MEMORY ? 0u : host_time_ms();
    if (motion_valid) {
        if (motion && motion_pending.buttons == buttons) {
            if (buttons & AAR_MOUSE_ABSOLUTE) {
                motion_pending.dx = dx;
                motion_pending.dy = dy;
            } else {
                motion_pending.dx = sat16((int32_t)motion_pending.dx + dx);
                motion_pending.dy = sat16((int32_t)motion_pending.dy + dy);
            }
            motion_pending.time_ms = ev.time_ms;
            return;
        }
        /* Anything else keeps its place behind the motion. */
        flush_motion();
        if (motion_valid) {
            drops++;
            return;
        }
    }
    if (!ring_room()) {
        if (motion) {
            motion_pending = ev;
            motion_valid = 1;
        } else {
            drops++;
        }
        return;
    }
    ring_put(&ev);
}

static void note_stop(void)
{
    host_request_stop();
    if (!stop_event_sent) {
        stop_pending = 1;
        stop_event_sent = 1;
    }
}

/* The heap pool fell below its low-water mark (memory.c). */
static void low_memory_event(void)
{
    if (low_pending < HOST_LOW_PENDING_MAX)
        low_pending++;
}

static void on_key(const SDL_KeyboardEvent *key)
{
    uint16_t raw = 0;
    int keypad = 0;
    uint16_t type;
    uint16_t qual;
    uint16_t brk;

    if (!raw_from_scan(key->keysym.scancode, &raw, &keypad))
        return;
    type = key->type == SDL_KEYUP ? AAR_EV_KEY_UP : AAR_EV_KEY_DOWN;
    qual = qualifiers(key->keysym.mod, keypad, key->repeat);
    push_event(type, raw, 0, 0, qual);
    brk = type == AAR_EV_KEY_DOWN ? break_for_raw(raw, qual) : 0;
    if (brk)
        push_event(AAR_EV_BREAK, brk, 0, 0, qual);
}

static int pointer_absolute(void)
{
    return (host_display_caps() & AAR_DISPCAP_POINTER_ABS) != 0u;
}

/* The event word for a window position, in surface pixels. */
static void surface_pos(int x, int y, int16_t *px, int16_t *py)
{
    int sx, sy;

    host_display_to_surface(x, y, &sx, &sy);
    *px = sat16(sx);
    *py = sat16(sy);
}

static void on_button(const SDL_MouseButtonEvent *btn)
{
    uint16_t bit = 0;
    uint16_t id = 0;

    if (btn->button == SDL_BUTTON_LEFT) {
        bit = 1u;
        id = 1u;
    } else if (btn->button == SDL_BUTTON_RIGHT) {
        bit = 2u;
        id = 2u;
    } else if (btn->button == SDL_BUTTON_MIDDLE) {
        bit = 4u;
        id = 3u;
    } else {
        return;
    }
    if (btn->type == SDL_MOUSEBUTTONDOWN)
        mouse_buttons |= bit;
    else
        mouse_buttons = (uint16_t)(mouse_buttons & ~bit);
    if (pointer_absolute()) {
        int16_t px, py;

        surface_pos(btn->x, btn->y, &px, &py);
        push_event(AAR_EV_MOUSE, id, px, py,
                   (uint16_t)(mouse_buttons | AAR_MOUSE_ABSOLUTE));
    } else {
        push_event(AAR_EV_MOUSE, id, 0, 0, mouse_buttons);
    }
}

static void on_motion(const SDL_MouseMotionEvent *m)
{
    if (pointer_absolute()) {
        int16_t px, py;

        surface_pos(m->x, m->y, &px, &py);
        push_event(AAR_EV_MOUSE, 0, px, py,
                   (uint16_t)(mouse_buttons | AAR_MOUSE_ABSOLUTE));
    } else if (m->xrel || m->yrel) {
        push_event(AAR_EV_MOUSE, 0, sat16(m->xrel), sat16(m->yrel),
                   mouse_buttons);
    }
}

/* dx is positive to the right and dy positive down, towards the user; SDL's
 * y is positive away from the user, and both flip with a flipped direction. */
static void on_wheel(const SDL_MouseWheelEvent *w)
{
    int x = w->x;
    int y = w->y;

    if (w->direction == SDL_MOUSEWHEEL_FLIPPED) {
        x = -x;
        y = -y;
    }
    if (!x && !y)
        return;
    push_event(AAR_EV_WHEEL, 0, sat16(x), sat16(-y),
               qualifiers(SDL_GetModState(), 0, 0));
}

/* SDL_TEXTINPUT carries UTF-8. A code point outside 1..0xFFFF, and anything
 * that is not valid UTF-8, is dropped. */
static void on_text(const SDL_TextInputEvent *t)
{
    const unsigned char *s = (const unsigned char *)t->text;
    uint16_t qual;

    if (!(host_display_caps() & AAR_DISPCAP_TEXT_INPUT))
        return;
    qual = qualifiers(SDL_GetModState(), 0, 0);
    while (*s) {
        uint32_t cp;
        int len;
        int k;

        if (*s < 0x80u) {
            cp = *s;
            len = 1;
        } else if ((*s & 0xe0u) == 0xc0u) {
            cp = *s & 0x1fu;
            len = 2;
        } else if ((*s & 0xf0u) == 0xe0u) {
            cp = *s & 0x0fu;
            len = 3;
        } else if ((*s & 0xf8u) == 0xf0u) {
            cp = *s & 0x07u;
            len = 4;
        } else {
            s++;
            continue;
        }
        for (k = 1; k < len; k++) {
            if ((s[k] & 0xc0u) != 0x80u)
                break;
            cp = (cp << 6) | (s[k] & 0x3fu);
        }
        if (k < len) {
            s++;
            continue;
        }
        s += len;
        if ((len == 2 && cp < 0x80u) || (len == 3 && cp < 0x800u) ||
            (cp >= 0xd800u && cp <= 0xdfffu))
            continue;
        if (cp >= 1u && cp <= 0xffffu)
            push_event(AAR_EV_TEXT, (uint16_t)cp, 0, 0, qual);
    }
}

static void on_window(const SDL_WindowEvent *w)
{
    uint32_t sw = 0;
    uint32_t sh = 0;

    if (w->event == SDL_WINDOWEVENT_CLOSE) {
        if (host_display_caps() & AAR_DISPCAP_CLOSE_REQUEST) {
            push_event(AAR_EV_CLOSE_REQUEST, 0, 0, 0, 0);
            quit_follows_close = 1; /* SDL follows the last close with SDL_QUIT */
        } else {
            note_stop();
        }
    } else if (w->event == SDL_WINDOWEVENT_FOCUS_GAINED ||
               w->event == SDL_WINDOWEVENT_FOCUS_LOST) {
        int gained = w->event == SDL_WINDOWEVENT_FOCUS_GAINED;

        if (host_display_focus(w->windowID, gained))
            push_event(AAR_EV_ACTIVE, (uint16_t)gained, 0, 0, 0);
    } else if (w->event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        if (host_display_window_resized(w->windowID, w->data1, w->data2, &sw,
                                        &sh))
            push_event(AAR_EV_RESIZE, 0, (int16_t)sw, (int16_t)sh, 0);
    } else if (w->event == SDL_WINDOWEVENT_EXPOSED) {
        host_display_expose(w->windowID);
    }
}

static int axis_dir(Sint16 v)
{
    if (v < -16000)
        return -1;
    if (v > 16000)
        return 1;
    return 0;
}

static void poll_joysticks(void)
{
    int n;

    for (n = 0; n < 2; n++) {
        SDL_Joystick *joy = joys[n];
        int dx = 0;
        int dy = 0;
        int fire;
        uint32_t state;
        uint16_t port;

        if (!joy || !SDL_JoystickGetAttached(joy))
            continue;
        if (SDL_JoystickNumHats(joy) > 0) {
            Uint8 hat = SDL_JoystickGetHat(joy, 0);
            if (hat & SDL_HAT_LEFT)
                dx = -1;
            else if (hat & SDL_HAT_RIGHT)
                dx = 1;
            if (hat & SDL_HAT_UP)
                dy = -1;
            else if (hat & SDL_HAT_DOWN)
                dy = 1;
        } else if (SDL_JoystickNumAxes(joy) >= 2) {
            dx = axis_dir(SDL_JoystickGetAxis(joy, 0));
            dy = axis_dir(SDL_JoystickGetAxis(joy, 1));
        }
        fire = SDL_JoystickNumButtons(joy) > 0 &&
               SDL_JoystickGetButton(joy, 0);
        state = ((uint32_t)(dx + 1) << 8) | (uint32_t)(dy + 1) |
                (fire ? 0x10000u : 0u);
        if (joy_valid[n] && state == joy_last[n])
            continue;
        joy_last[n] = state;
        joy_valid[n] = 1;
        /* SDL joystick 0 is the Amiga game port (port 1). */
        port = n == 0 ? 1u : 0u;
        push_event(AAR_EV_JOY, port, (int16_t)dx, (int16_t)dy, fire ? 1u : 0u);
    }
}

/* Opens the game controllers that are present and not open yet. */
static void pads_scan(void)
{
    int count = SDL_NumJoysticks();
    int i;

    for (i = 0; i < count; i++) {
        SDL_JoystickID id;
        SDL_GameController *gc;
        int slot = -1;
        int s;
        int known = 0;

        if (!SDL_IsGameController(i))
            continue;
        id = SDL_JoystickGetDeviceInstanceID(i);
        for (s = 0; s < HOST_PADS; s++) {
            if (pads[s].gc && pads[s].id == id)
                known = 1;
            else if (!pads[s].gc && slot < 0)
                slot = s;
        }
        if (known)
            continue;
        if (slot < 0)
            return;
        gc = SDL_GameControllerOpen(i);
        if (!gc)
            continue;
        pads[slot].gc = gc;
        pads[slot].id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
        pads[slot].last = HOST_PAD_NEUTRAL;
    }
}

static void pad_close(int s)
{
    /* The app must not be left with a button held. */
    if (pads[s].last != HOST_PAD_NEUTRAL)
        push_event(AAR_EV_JOY, (uint16_t)(HOST_PAD_PORT_BASE + (uint32_t)s), 0,
                   0, 0);
    SDL_GameControllerClose(pads[s].gc);
    pads[s].gc = 0;
    pads[s].last = HOST_PAD_NEUTRAL;
}

static void poll_pads(void)
{
    int s;

    for (s = 0; s < HOST_PADS; s++) {
        SDL_GameController *gc = pads[s].gc;
        int dx = 0;
        int dy = 0;
        uint16_t buttons = 0;
        uint32_t state;

        if (!gc)
            continue;
        if (!SDL_GameControllerGetAttached(gc)) {
            pad_close(s);
            continue;
        }
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))
            dx = -1;
        else if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
            dx = 1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))
            dy = -1;
        else if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))
            dy = 1;
        if (dx == 0)
            dx = axis_dir(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX));
        if (dy == 0)
            dy = axis_dir(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY));
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A))
            buttons |= AAR_JOY_BUTTON1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B))
            buttons |= AAR_JOY_BUTTON2;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X))
            buttons |= AAR_JOY_BUTTON3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y))
            buttons |= AAR_JOY_BUTTON4;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START))
            buttons |= AAR_JOY_PLAY;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))
            buttons |= AAR_JOY_FORWARD;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))
            buttons |= AAR_JOY_REVERSE;
        state = ((uint32_t)(dx + 1) << 20) | ((uint32_t)(dy + 1) << 16) |
                buttons;
        if (state == pads[s].last)
            continue;
        pads[s].last = state;
        push_event(AAR_EV_JOY, (uint16_t)(HOST_PAD_PORT_BASE + (uint32_t)s),
                   (int16_t)dx, (int16_t)dy, buttons);
    }
}

static int parse_i(const char *s, long *out)
{
    char *end = 0;
    long v;

    if (!s || !*s)
        return 0;
    v = strtol(s, &end, 0);
    if (!end || *end)
        return 0;
    *out = v;
    return 1;
}

static int load_input(const char *path)
{
    FILE *fp;
    char line[160];
    uint32_t lineno = 0;

    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "aar-host: cannot open input script %s\n", path);
        return -1;
    }
    while (fgets(line, sizeof line, fp)) {
        char *tok[6];
        int ntok = 0;
        char *p = line;
        size_t n = strlen(p);
        long a = 0, b = 0, c = 0, d = 0;

        lineno++;
        while (n && (p[n - 1u] == '\n' || p[n - 1u] == '\r'))
            p[--n] = 0;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == 0 || *p == '#')
            continue;
        while (ntok < 6 && *p) {
            tok[ntok++] = p;
            while (*p && *p != ' ' && *p != '\t')
                p++;
            if (*p) {
                *p++ = 0;
                while (*p == ' ' || *p == '\t')
                    p++;
            }
        }
        if (ntok >= 3 && strcmp(tok[0], "key") == 0 && parse_i(tok[2], &a)) {
            uint16_t qual = 0;
            if (ntok >= 4 && parse_i(tok[3], &b))
                qual = (uint16_t)b;
            if (strcmp(tok[1], "down") == 0)
                push_event(AAR_EV_KEY_DOWN, (uint16_t)a, 0, 0, qual);
            else if (strcmp(tok[1], "up") == 0)
                push_event(AAR_EV_KEY_UP, (uint16_t)a, 0, 0, qual);
            else
                goto bad;
        } else if (ntok >= 3 && strcmp(tok[0], "mouse") == 0 &&
                   parse_i(tok[1], &a) && parse_i(tok[2], &b)) {
            uint16_t buttons = 0;
            if (ntok >= 4 && parse_i(tok[3], &c))
                buttons = (uint16_t)c;
            push_event(AAR_EV_MOUSE, 0, (int16_t)a, (int16_t)b, buttons);
        } else if (ntok >= 3 && strcmp(tok[0], "mousebtn") == 0 &&
                   parse_i(tok[1], &a)) {
            uint16_t bit = a == 1 ? 1u : a == 2 ? 2u : a == 3 ? 4u : 0u;
            if (!bit || (strcmp(tok[2], "down") != 0 && strcmp(tok[2], "up") != 0))
                goto bad;
            if (strcmp(tok[2], "down") == 0)
                mouse_buttons |= bit;
            else
                mouse_buttons = (uint16_t)(mouse_buttons & ~bit);
            push_event(AAR_EV_MOUSE, (uint16_t)a, 0, 0, mouse_buttons);
        } else if (ntok >= 5 && strcmp(tok[0], "joy") == 0 &&
                   parse_i(tok[1], &a) && parse_i(tok[2], &b) &&
                   parse_i(tok[3], &c) && parse_i(tok[4], &d)) {
            push_event(AAR_EV_JOY, (uint16_t)a, (int16_t)b, (int16_t)c,
                       (uint16_t)d);
        } else if (ntok >= 2 && strcmp(tok[0], "break") == 0 &&
                   parse_i(tok[1], &a)) {
            push_event(AAR_EV_BREAK, (uint16_t)a, 0, 0, 0);
        } else if (ntok >= 3 && strcmp(tok[0], "resize") == 0 &&
                   parse_i(tok[1], &a) && parse_i(tok[2], &b)) {
            push_event(AAR_EV_RESIZE, 0, (int16_t)a, (int16_t)b, 0);
        } else if (ntok >= 3 && strcmp(tok[0], "wheel") == 0 &&
                   parse_i(tok[1], &a) && parse_i(tok[2], &b)) {
            uint16_t qual = 0;
            if (ntok >= 4 && parse_i(tok[3], &c))
                qual = (uint16_t)c;
            push_event(AAR_EV_WHEEL, 0, (int16_t)a, (int16_t)b, qual);
        } else if (ntok >= 2 && strcmp(tok[0], "text") == 0 &&
                   parse_i(tok[1], &a)) {
            uint16_t qual = 0;
            if (ntok >= 3 && parse_i(tok[2], &c))
                qual = (uint16_t)c;
            push_event(AAR_EV_TEXT, (uint16_t)a, 0, 0, qual);
        } else if (ntok >= 2 && strcmp(tok[0], "active") == 0 &&
                   parse_i(tok[1], &a)) {
            push_event(AAR_EV_ACTIVE, a ? 1u : 0u, 0, 0, 0);
        } else if (ntok == 1 && strcmp(tok[0], "close") == 0) {
            push_event(AAR_EV_CLOSE_REQUEST, 0, 0, 0, 0);
        } else if (ntok == 1 && strcmp(tok[0], "lowmem") == 0) {
            low_memory_event();
        } else if (ntok == 1 && strcmp(tok[0], "stop") == 0) {
            note_stop();
        } else {
            goto bad;
        }
        continue;
    bad:
        fprintf(stderr, "aar-host: bad input script line %u\n", lineno);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

/* An event that needs a cap of the display leaves the poll only with it. */
static int event_wanted(uint16_t type)
{
    uint32_t caps = host_display_caps();

    if (type == AAR_EV_RESIZE)
        return (caps & AAR_DISPCAP_RESIZE) != 0u;
    if (type == AAR_EV_TEXT)
        return (caps & AAR_DISPCAP_TEXT_INPUT) != 0u;
    if (type == AAR_EV_CLOSE_REQUEST)
        return (caps & AAR_DISPCAP_CLOSE_REQUEST) != 0u;
    return 1;
}

/* The lifecycle events are latches, not queue entries: a full queue cannot drop them (spec
 * 8.3). They are delivered ahead of the queue, each once per occurrence. */
static int api_event_poll(aar_event *out, uint32_t max, uint32_t *out_n)
{
    uint32_t n = 0;

    if (!out_n || (max && !out))
        return AAR_E_INVAL;
    if (n < max && stop_pending) {
        memset(&out[n], 0, sizeof out[n]);
        out[n].type = AAR_EV_STOP;
        out[n].time_ms = host_time_ms();
        n++;
        stop_pending = 0;
    }
    while (n < max && low_pending) {
        memset(&out[n], 0, sizeof out[n]);
        out[n].type = AAR_EV_LOW_MEMORY; /* no field but its type (spec 4.1) */
        n++;
        low_pending--;
    }
    for (;;) {
        while (n < max && q_tail != q_head) {
            const aar_event *ev = &queue[q_tail & (HOST_INPUT_CAP - 1u)];

            q_tail++;
            if (event_wanted(ev->type))
                out[n++] = *ev;
        }
        /* Merged motion joins the queue as soon as the app made room. */
        if (n < max && motion_valid) {
            flush_motion();
            if (q_tail != q_head)
                continue;
        }
        break;
    }
    *out_n = n;
    return AAR_OK;
}

void host_input_pump(void)
{
    SDL_Event ev;

    if (host_stop_event_pending())
        note_stop();
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            if (quit_follows_close && (host_display_caps() & AAR_DISPCAP_CLOSE_REQUEST))
                quit_follows_close = 0;
            else
                note_stop();
        } else if (ev.type == SDL_WINDOWEVENT) {
            on_window(&ev.window);
        } else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
            on_key(&ev.key);
        } else if (ev.type == SDL_MOUSEBUTTONDOWN ||
                   ev.type == SDL_MOUSEBUTTONUP) {
            on_button(&ev.button);
        } else if (ev.type == SDL_MOUSEMOTION) {
            on_motion(&ev.motion);
        } else if (ev.type == SDL_MOUSEWHEEL) {
            on_wheel(&ev.wheel);
        } else if (ev.type == SDL_TEXTINPUT) {
            on_text(&ev.text);
        } else if (ev.type == SDL_CONTROLLERDEVICEADDED) {
            pads_scan();
        } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
            int s;

            for (s = 0; s < HOST_PADS; s++) {
                if (pads[s].gc && pads[s].id == ev.cdevice.which)
                    pad_close(s);
            }
        }
    }
    quit_follows_close = 0;
    poll_joysticks();
    poll_pads();
    (void)drops;
}

int host_input_selftest(void)
{
    uint16_t raw = 0;
    int keypad = 0;
    SDL_Event ev;
    uint32_t low_seen;
    aar_event got[8];
    uint32_t n = 0;
    int saw_key = 0;
    int saw_break = 0;
    uint32_t i;

    if (!raw_from_scan(SDL_SCANCODE_D, &raw, &keypad) || raw != 0x22u)
        return 1;
    if (!raw_from_scan(SDL_SCANCODE_E, &raw, &keypad) || raw != 0x12u)
        return 2;
    if (!raw_from_scan(SDL_SCANCODE_F, &raw, &keypad) || raw != 0x23u)
        return 3;
    if (!raw_from_scan(SDL_SCANCODE_ESCAPE, &raw, &keypad) || raw != 0x45u)
        return 4;
    if (!raw_from_scan(SDL_SCANCODE_UP, &raw, &keypad) || raw != 0x4cu)
        return 5;
    if (!raw_from_scan(SDL_SCANCODE_KP_4, &raw, &keypad) || raw != 0x2du ||
        !keypad)
        return 6;
    if (!raw_from_scan(SDL_SCANCODE_KP_5, &raw, &keypad) || raw != 0x2eu ||
        !keypad)
        return 7;
    if (!raw_from_scan(SDL_SCANCODE_KP_6, &raw, &keypad) || raw != 0x2fu ||
        !keypad)
        return 8;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_KEYDOWN;
    ev.key.type = SDL_KEYDOWN;
    ev.key.state = SDL_PRESSED;
    ev.key.keysym.scancode = SDL_SCANCODE_D;
    ev.key.keysym.mod = KMOD_LCTRL;
    if (SDL_PushEvent(&ev) < 0)
        return 9;
    host_input_pump();
    if (api_event_poll(got, 4u, &n) != AAR_OK)
        return 10;
    for (i = 0; i < n; i++) {
        if (got[i].type == AAR_EV_KEY_DOWN && got[i].code == 0x22u)
            saw_key = 1;
        if (got[i].type == AAR_EV_BREAK && got[i].code == AAR_BREAK_CTRL_D)
            saw_break = 1;
    }
    if (!saw_key || !saw_break)
        return 11;
    /* Spec 8.3: the runtime never discards AAR_EV_LOW_MEMORY. With the queue full of other
     * events it still arrives, first and once. */
    n = 1;
    while (n) {
        if (api_event_poll(got, 4u, &n) != AAR_OK)
            return 12;
    }
    for (i = 0; i < HOST_INPUT_CAP; i++)
        push_event(AAR_EV_KEY_DOWN, 0x45u, 0, 0, 0);
    low_memory_event();
    if (api_event_poll(got, 1u, &n) != AAR_OK || n != 1u || got[0].type != AAR_EV_LOW_MEMORY)
        return 13;
    low_seen = 0;
    for (;;) {
        if (api_event_poll(got, 4u, &n) != AAR_OK)
            return 14;
        if (n == 0)
            break;
        for (i = 0; i < n; i++) {
            if (got[i].type == AAR_EV_LOW_MEMORY)
                low_seen++;
        }
    }
    if (low_seen != 0u)
        return 15;
    /* Spec 4.1: one AAR_EV_LOW_MEMORY per crossing of the low-water mark, queued again only
     * after the pool has risen above the high-water mark. Two crossings give two events, a
     * poll takes them both and a second poll finds none. */
    {
        aar_api heap_api;
        aar_heap_info hi;
        void *brk = 0;
        uint64_t all;

        memset(&heap_api, 0, sizeof heap_api);
        host_memory_bind(&heap_api);
        if (heap_api.heap_info(&hi) != AAR_OK)
            return 16;
        all = hi.limit - hi.committed;
        if (heap_api.heap_grow(all, &brk) != AAR_OK)
            return 17;
        if (heap_api.heap_shrink(all / 2u, &brk) != AAR_OK)
            return 18;
        if (heap_api.heap_grow(all / 2u, &brk) != AAR_OK)
            return 19;
        if (heap_api.heap_shrink(all, &brk) != AAR_OK)
            return 20;
        if (api_event_poll(got, 8u, &n) != AAR_OK)
            return 21;
        low_seen = 0;
        for (i = 0; i < n; i++) {
            if (got[i].type == AAR_EV_LOW_MEMORY)
                low_seen++;
        }
        if (low_seen != 2u)
            return 22;
        if (api_event_poll(got, 8u, &n) != AAR_OK || n != 0u)
            return 23;
    }
    /* A stop is a request, not a crossing: one event for it, however often it is made. */
    note_stop();
    note_stop();
    if (api_event_poll(got, 8u, &n) != AAR_OK || n != 1u || got[0].type != AAR_EV_STOP)
        return 24;
    if (api_event_poll(got, 8u, &n) != AAR_OK || n != 0u)
        return 25;
    return 0;
}

int host_input_init(void)
{
    const struct host_options *opt = host_options();
    int i;
    int n = 0;
    int count;

    q_head = q_tail = 0;
    drops = 0;
    mouse_buttons = 0;
    stop_event_sent = 0;
    stop_pending = 0;
    low_pending = 0;
    motion_valid = 0;
    quit_follows_close = 0;
    host_memory_set_low_notify(low_memory_event);
    memset(pads, 0, sizeof pads);
    /* A failure leaves the runner without game controllers, not without a
     * window. */
    (void)SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    pads_scan();
    memset(joys, 0, sizeof joys);
    memset(joy_valid, 0, sizeof joy_valid);
    count = SDL_NumJoysticks();
    for (i = 0; i < count && n < 2; i++) {
        if (SDL_IsGameController(i))
            continue;
        joys[n++] = SDL_JoystickOpen(i);
    }
    if (opt->input_path && load_input(opt->input_path) != 0)
        return -1;
    return 0;
}

void host_input_shutdown(void)
{
    int i;

    host_memory_set_low_notify(0);
    for (i = 0; i < 2; i++) {
        if (joys[i])
            SDL_JoystickClose(joys[i]);
        joys[i] = 0;
    }
    for (i = 0; i < HOST_PADS; i++) {
        if (pads[i].gc)
            SDL_GameControllerClose(pads[i].gc);
        pads[i].gc = 0;
    }
}

void host_input_bind(aar_api *api)
{
    api->event_poll = api_event_poll;
}
