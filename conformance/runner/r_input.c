/*
 * Runner side of C-INPUT (conformance/TESTS.md): the client posts the events. The peer
 * keywords of payloads/conf_input.h make the runner call AARPostEvent (key, mouse and
 * joystick events, a stop event, floods that fill the event queue) or signal its own
 * task Ctrl-D, Ctrl-E or Ctrl-F, which the serving loop turns into AAR_EV_BREAK events
 * (its break mask). C-INPUT.9 is judged here (variant overfull), as are the
 * interactive variants fed by the input events the app reports.
 *
 * Assumptions for the card run: AARPostEvent on a handle whose app does not poll
 * keeps events in the completion queue and the runtime until it does, and refuses
 * with AAR_E_BUSY (counting events_dropped) when they are full; the runner posts all the
 * events of a test before the app polls any, and a mouse event is folded into the one
 * before only when that one is still pending in a full queue.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "aar-conf.h"
#include "cd32_judge.h"
#include "../payloads/conf_input.h"

struct in_state {
    ULONG key_busy, mouse_busy, mouse_ok, drop_base;
    int over_done;
    ULONG over_accepted, over_busy, over_drop_base;
    int got_seen;
    ULONG got_n, got_ok, got_drops;
};

static struct in_state is;

static ULONG events_dropped(struct conf_run *run)
{
    AARStatusInfo st;

    memset(&st, 0, sizeof st);
    st.size = sizeof st;
    (void)AARStatus(run->h, &st);
    return st.events_dropped;
}

static void send_result(struct conf_run *run, ULONG ok, ULONG count, ULONG errors, ULONG x)
{
    AARMessage am;
    int tries;

    cm_make_result(am.bytes, ok, count, errors, CM_NONE, x, 0);
    for (tries = 0; tries < 100; tries++) {
        if (AARSendMessage(run->h, &am) != AAR_E_BUSY)
            return;
        Delay(1);
    }
}

static LONG post_raw(struct conf_run *run, UWORD type, UWORD code, WORD dx, WORD dy,
                     UWORD buttons, ULONG time_ms)
{
    aar_event ev;

    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.code = code;
    ev.dx = dx;
    ev.dy = dy;
    ev.buttons = buttons;
    ev.time_ms = time_ms;
    return AARPostEvent(run->h, &ev);
}

static void post_list(struct conf_run *run, const struct ci_ev *list, ULONG n)
{
    ULONG i;

    for (i = 0; i < n; i++)
        (void)post_raw(run, list[i].type, list[i].code, list[i].dx, list[i].dy,
                       list[i].buttons, list[i].time_ms);
}

static void peer_stopev(struct conf_run *run, const AARMessage *msg)
{
    (void)msg;
    (void)post_raw(run, AAR_EV_STOP, 0, 0, 0, 0, 0);
}

static void peer_keys(struct conf_run *run, const AARMessage *msg)
{
    (void)msg;
    post_list(run, ci_keys, CI_COUNT(ci_keys));
}

static void peer_mouse(struct conf_run *run, const AARMessage *msg)
{
    (void)msg;
    post_list(run, ci_mouse, CI_COUNT(ci_mouse));
}

static void peer_joy(struct conf_run *run, const AARMessage *msg)
{
    (void)msg;
    post_list(run, ci_joy, CI_COUNT(ci_joy));
}

/* The serving loop consumes Ctrl-D, Ctrl-E and Ctrl-F (its break mask) on its next pass. */
static void peer_break(struct conf_run *run, const AARMessage *msg)
{
    ULONG code = cm_get32(msg->bytes, 20);
    ULONG mask = code == AAR_BREAK_CTRL_D ? SIGBREAKF_CTRL_D :
                 code == AAR_BREAK_CTRL_E ? SIGBREAKF_CTRL_E : SIGBREAKF_CTRL_F;

    (void)run;
    Signal(FindTask(NULL), mask);
}

static void peer_flood(struct conf_run *run, const AARMessage *msg)
{
    ULONG n = cm_get32(msg->bytes, 20);
    ULONG accepted = 0;
    ULONG i;

    for (i = 0; i < n; i++) {
        int tries;
        LONG rc = AAR_E_BUSY;

        /* The runtime moves events out of the queue while the app sleeps: a refusal is
         * retried for a while. */
        for (tries = 0; tries < 30 && rc == AAR_E_BUSY; tries++) {
            rc = post_raw(run, AAR_EV_KEY_DOWN, (UWORD)(i + 1UL), 0, 0, 0, 0);
            if (rc == AAR_E_BUSY)
                Delay(1);
        }
        if (rc != AAR_OK)
            break;
        accepted++;
    }
    send_result(run, accepted == n, accepted, 0, 0);
}

static void peer_mousefull(struct conf_run *run, const AARMessage *msg)
{
    ULONG i;

    (void)msg;
    is.drop_base = events_dropped(run);
    is.key_busy = 0;
    is.mouse_busy = 0;
    is.mouse_ok = 0;
    for (i = 0; i < 100UL; i++) {
        if (post_raw(run, AAR_EV_KEY_DOWN, (UWORD)(i + 1UL), 0, 0, 0, 0) == AAR_E_BUSY)
            is.key_busy++;
    }
    for (i = 0; i < 20UL; i++) {
        LONG rc = post_raw(run, AAR_EV_MOUSE, 0, 1, 0, 0, 0);

        if (rc == AAR_OK)
            is.mouse_ok++;
        else if (rc == AAR_E_BUSY)
            is.mouse_busy++;
    }
    send_result(run, 1, is.key_busy, is.mouse_busy, is.mouse_ok);
}

static void peer_drops(struct conf_run *run, const AARMessage *msg)
{
    ULONG total = events_dropped(run) - is.drop_base;
    ULONG mouse = total > is.key_busy ? total - is.key_busy : 0UL;

    (void)msg;
    send_result(run, 1, mouse, 0, 0);
}

static void peer_overfull(struct conf_run *run, const AARMessage *msg)
{
    ULONG i;

    (void)msg;
    is.over_drop_base = events_dropped(run);
    is.over_accepted = 0;
    is.over_busy = 0;
    for (i = 0; i < CI_OVERFULL_POSTS; i++) {
        LONG rc = post_raw(run, AAR_EV_KEY_DOWN, (UWORD)(i + 1UL), 0, 0, 0, 0);

        if (rc == AAR_OK)
            is.over_accepted++;
        else if (rc == AAR_E_BUSY)
            is.over_busy++;
    }
    is.over_done = 1;
}

static void peer_got(struct conf_run *run, const AARMessage *msg)
{
    is.got_seen = 1;
    is.got_n = cm_get32(msg->bytes, 20);
    is.got_ok = cm_get32(msg->bytes, 24);
    is.got_drops = events_dropped(run) - is.over_drop_base;
}

static void peer_newev(struct conf_run *run, const AARMessage *msg)
{
    (void)msg;
    post_list(run, ci_new, CI_COUNT(ci_new));
}

/* Absolute positions with no retry while the queue is full; the answer names the last post that was accepted. */
static void peer_absfull(struct conf_run *run, const AARMessage *msg)
{
    ULONG key_busy = 0, mouse_busy = 0, last_ok = 0;
    ULONG i;

    (void)msg;
    for (i = 0; i < 100UL; i++) {
        if (post_raw(run, AAR_EV_KEY_DOWN, (UWORD)(i + 1UL), 0, 0, 0, 0) == AAR_E_BUSY)
            key_busy++;
    }
    for (i = 1; i <= CI_ABS_POSTS; i++) {
        LONG rc = post_raw(run, AAR_EV_MOUSE, 0, ci_abs_x((uint32_t)i), ci_abs_y((uint32_t)i),
                           (UWORD)CI_MOUSE_ABSOLUTE, 0);

        if (rc == AAR_OK)
            last_ok = i;
        else if (rc == AAR_E_BUSY)
            mouse_busy++;
    }
    send_result(run, 1, key_busy, mouse_busy, last_ok);
}

static const struct conf_peer peers[] = {
    { CI_K_STOPEV, peer_stopev },
    { CI_K_KEYS, peer_keys },
    { CI_K_MOUSE, peer_mouse },
    { CI_K_JOY, peer_joy },
    { CI_K_BREAK, peer_break },
    { CI_K_FLOOD, peer_flood },
    { CI_K_MOUSEFULL, peer_mousefull },
    { CI_K_DROPS, peer_drops },
    { CI_K_OVERFULL, peer_overfull },
    { CI_K_GOT, peer_got },
    { CI_K_NEWEV, peer_newev },
    { CI_K_ABSFULL, peer_absfull },
};

static void prep_reset(struct conf_run *run)
{
    (void)run;
    memset(&is, 0, sizeof is);
}

static void prep_std(struct conf_run *run)
{
    prep_reset(run);
    run->serve_flags_off = AAR_SERVEF_NO_INPUT_WINDOW;
}

/* Variant show: the events the app saw, kept for the operator, and whether any of
 * them is evidence that a person used the input: a key, or a mouse or joystick event
 * with motion or buttons. Events of the activation kind (type and code, no motion, no
 * button) are not. */
#define SHOW_LINES 24

static char show_lines[SHOW_LINES][72];
static unsigned show_count;
static ULONG show_total;
static ULONG show_keys;
static ULONG show_moves;

static void show_message(struct conf_run *run, const AARMessage *m)
{
    ULONG type = cm_get32(m->bytes, 4);
    LONG dx = (LONG)cm_get32(m->bytes, 12);
    LONG dy = (LONG)cm_get32(m->bytes, 16);
    ULONG buttons = cm_get32(m->bytes, 20);

    (void)run;
    if (m->bytes[0] != 'S' || m->bytes[1] != 'H' || m->bytes[2] != 'O' || m->bytes[3] != 'W')
        return;
    show_total++;
    if (type == AAR_EV_KEY_DOWN || type == AAR_EV_KEY_UP)
        show_keys++;
    else if ((type == AAR_EV_MOUSE || type == AAR_EV_JOY) && (dx || dy || buttons))
        show_moves++;
    if (show_count < SHOW_LINES)
        snprintf(show_lines[show_count++], sizeof show_lines[0],
                 "type %lu code 0x%lx dx %ld dy %ld buttons 0x%lx", (unsigned long)type,
                 (unsigned long)cm_get32(m->bytes, 8), (long)dx, (long)dy,
                 (unsigned long)buttons);
}

static void prep_show(struct conf_run *run)
{
    memset(&is, 0, sizeof is);
    show_count = 0;
    show_total = 0;
    show_keys = 0;
    show_moves = 0;
    run->serve_flags_off = AAR_SERVEF_NO_INPUT_WINDOW;
    run->on_message = show_message;
    run->extra_args[run->extra_count++] = "conf:show=1";
    conf_emitf("# C-INPUT.11: for 12 seconds the app shows a grey screen: press keys, move the "
               "mouse, press its buttons and use a joystick");
}

/* After the show, before the question: what counts, and a repeat when nothing does. */
static int show_review(const struct conf_run *run)
{
    (void)run;
    conf_emitf("# C-INPUT.11: %lu key events and %lu mouse or joystick events with motion or "
               "buttons, among %lu events", (unsigned long)show_keys,
               (unsigned long)show_moves, (unsigned long)show_total);
    if (show_keys + show_moves)
        return 0;
    conf_emitf("# C-INPUT.11: the app saw no key press, mouse move, button or joystick movement, "
               "only events like activation. Click the grey screen first, then press keys and "
               "move the mouse.");
    return 1;
}

/* The person says yes; the app's evidence has the last word. */
static int show_accept(char *why)
{
    if (show_keys + show_moves)
        return 1;
    conf_detailf(why, "operator confirmed but the app saw no key or mouse event (%lu events, none "
                      "a key, mouse move or button)", (unsigned long)show_total);
    return 0;
}

/* Variant buttons (C-INPUT.17): the same list as show, and a check of the rules for what real devices send:
 * a mouse event's code is 0 to 3, a text event's code 1 to 0xFFFF, a focus event's code 0 or 1, and when a
 * wheel event was seen, the wheel's raw keys (0x7A to 0x7D) were not also sent as keys. The person's yes needs
 * at least one mouse button, wheel or text event as evidence. */
static ULONG bt_buttons, bt_wheel, bt_text, bt_rawwheel, bt_bad, bt_total;
static char bt_why[CONF_DETAIL_MAX];

static void buttons_message(struct conf_run *run, const AARMessage *m)
{
    ULONG type = cm_get32(m->bytes, 4);
    ULONG code = cm_get32(m->bytes, 8);

    if (m->bytes[0] != 'S' || m->bytes[1] != 'H' || m->bytes[2] != 'O' || m->bytes[3] != 'W')
        return;
    show_message(run, m);
    bt_total++;
    if (type == AAR_EV_MOUSE) {
        if (code > 3UL) {
            bt_bad++;
            conf_detailf(bt_why, "a mouse event with code %lu (0 to 3 expected)", (unsigned long)code);
        } else if (code != 0UL) {
            bt_buttons++;
        }
    } else if (type == AAR_EV_WHEEL) {
        bt_wheel++;
    } else if (type == AAR_EV_TEXT) {
        bt_text++;
        if (code < 1UL || code > 0xFFFFUL) {
            bt_bad++;
            conf_detailf(bt_why, "a text event with code %lu (1 to 65535 expected)", (unsigned long)code);
        }
    } else if (type == AAR_EV_ACTIVE) {
        if (code > 1UL) {
            bt_bad++;
            conf_detailf(bt_why, "a focus event with code %lu (0 or 1 expected)", (unsigned long)code);
        }
    } else if ((type == AAR_EV_KEY_DOWN || type == AAR_EV_KEY_UP) && code >= 0x7AUL && code <= 0x7DUL) {
        bt_rawwheel++;
    }
}

static void prep_buttons(struct conf_run *run)
{
    memset(&is, 0, sizeof is);
    show_count = 0;
    show_total = 0;
    show_keys = 0;
    show_moves = 0;
    bt_buttons = bt_wheel = bt_text = bt_rawwheel = bt_bad = bt_total = 0;
    bt_why[0] = '\0';
    run->serve_flags_off = AAR_SERVEF_NO_INPUT_WINDOW;
    run->on_message = buttons_message;
    run->extra_args[run->extra_count++] = "conf:show=1";
    conf_emitf("# C-INPUT.17: for 20 seconds the app shows a window: press each mouse button, turn the "
               "wheel and type some letters in it");
}

static int buttons_review(const struct conf_run *run)
{
    (void)run;
    conf_emitf("# C-INPUT.17: %lu mouse button events, %lu wheel events, %lu text events, %lu raw wheel "
               "keys, among %lu events", (unsigned long)bt_buttons, (unsigned long)bt_wheel,
               (unsigned long)bt_text, (unsigned long)bt_rawwheel, (unsigned long)bt_total);
    if (bt_buttons + bt_wheel + bt_text)
        return 0;
    conf_emitf("# C-INPUT.17: the app saw no mouse button, wheel or text event. Click the window "
               "first, then press the buttons and type.");
    return 1;
}

static int buttons_accept(char *why)
{
    if (bt_bad) {
        conf_detailf(why, "%.150s", bt_why);
        return 0;
    }
    if (bt_wheel && bt_rawwheel) {
        conf_detailf(why, "the wheel was sent as wheel events and also as %lu raw keys",
                     (unsigned long)bt_rawwheel);
        return 0;
    }
    if (bt_buttons + bt_wheel + bt_text)
        return 1;
    conf_detailf(why, "operator confirmed but the app saw no mouse button, wheel or text event (%lu events)",
                 (unsigned long)bt_total);
    return 0;
}

static struct conf_cd32_judge cd32;
static int cd32_finished;

static void cd32_message(struct conf_run *run, const AARMessage *m)
{
    (void)run;
    if (memcmp(m->bytes, "SHOW", 4) == 0)
        conf_cd32_event(&cd32, cm_get32(m->bytes, 4), cm_get32(m->bytes, 8),
                       cm_get32(m->bytes, 20));
}

static void prep_cd32(struct conf_run *run, unsigned port)
{
    prep_std(run);
    cd32 = (struct conf_cd32_judge){ .port = port };
    cd32_finished = 0;
    run->cd32_ports = 1UL << port;
    run->on_message = cd32_message;
    run->extra_args[run->extra_count++] = "conf:show=1";
}

static void prep_cd32_port0(struct conf_run *run)
{
    prep_cd32(run, 0);
}

static void prep_cd32_port1(struct conf_run *run)
{
    prep_cd32(run, 1);
}

static void cd32_button_names(char out[64], uint32_t mask)
{
    static const struct {
        uint32_t bit;
        const char *name;
    } buttons[] = {
        { AAR_JOY_BUTTON1, "red" }, { AAR_JOY_BUTTON2, "blue" },
        { AAR_JOY_BUTTON3, "green" }, { AAR_JOY_BUTTON4, "yellow" },
        { AAR_JOY_PLAY, "play" }, { AAR_JOY_FORWARD, "forward" },
        { AAR_JOY_REVERSE, "reverse" },
    };
    size_t len = 0;
    unsigned i;

    for (i = 0; i < CONF_COUNT(buttons); ++i) {
        if (mask & buttons[i].bit)
            len += (size_t)snprintf(out + len, 64 - len, "%s%s", len ? ", " : "", buttons[i].name);
    }
    if (!len)
        strcpy(out, "none");
}

static int cd32_accept(char *why)
{
    uint32_t presses = conf_cd32_missing_presses(&cd32);
    uint32_t releases = conf_cd32_missing_releases(&cd32);
    char down[64];
    char up[64];

    cd32_button_names(down, presses);
    cd32_button_names(up, releases);
    conf_detailf(why, "CD32 port %u: missing presses: %s; missing releases: %s%s",
                 (unsigned)cd32.port, down, up,
                 cd32_finished ? "" : "; input payload did not finish successfully");
    return cd32_finished && !presses && !releases;
}

static int cd32_review(const struct conf_run *run)
{
    char detail[CONF_DETAIL_MAX];

    (void)run;
    (void)cd32_accept(detail);
    conf_emitf("# %s", detail);
    return 0;
}

static const struct conf_operator operators[] = {
    { "C-INPUT.11", "show", show_review, show_accept, 0 },
    { "C-INPUT.17", "buttons", buttons_review, buttons_accept, 0 },
    { "C-INPUT.20", "cd32-port0", cd32_review, cd32_accept, 1 },
    { "C-INPUT.21", "cd32-port1", cd32_review, cd32_accept, 1 },
};

static const struct conf_variant variants[] = {
    /* std ends in a stop (C-INPUT.2), so it runs last. */
    { "overfull", prep_reset },
    { "show", prep_show },
    { "buttons", prep_buttons },
    { "cd32-port0", prep_cd32_port0 },
    { "cd32-port1", prep_cd32_port1 },
    { "std", prep_std },
};

/* C-INPUT.9: with the queue overfull the new events are dropped, never the old ones. */
static void on_end(const struct conf_run *run, const struct conf_outcome *out,
                   const struct conf_parsed *pr)
{
    char d[CONF_DETAIL_MAX];
    int ok;

    if (strcmp(run->area, "C-INPUT") != 0 || !run->variant)
        return;
    if (run->cd32_ports) {
        cd32_finished = out->ran && !out->timed_out && pr->have_end &&
                        out->end_reason == AAR_END_EXIT && out->exit_code == 0;
        return;
    }
    if (strcmp(run->variant, "show") == 0) {
        unsigned i;

        if (!conf_g.interactive)
            return;
        conf_emitf("# C-INPUT.11: the app saw %lu events%s", (unsigned long)show_total,
                   show_total > SHOW_LINES ? " (the first ones follow)" : "");
        for (i = 0; i < show_count; i++)
            conf_emitf("#   %s", show_lines[i]);
        return;
    }
    if (strcmp(run->variant, "overfull") != 0)
        return;
    if (!is.over_done || !is.got_seen) {
        conf_detailf(d, "the overfull test did not complete (%s)",
                     out->ran ? "the app did not report" : out->why);
        conf_record("C-INPUT.9", 0, d);
        return;
    }
    /* The text guarantees that at least 64 events are kept and that the new ones are dropped,
     * not the old. Events the runtime drops after the queue took them are not in the client's
     * counter [spec question 63]: the counter must equal the posts the client was refused. */
    ok = is.got_ok && is.got_n >= 64UL && is.got_n <= is.over_accepted &&
         is.got_drops == is.over_busy && is.over_accepted + is.over_busy == CI_OVERFULL_POSTS;
    conf_detailf(d, "%lu posted: %lu accepted, %lu refused; the app polled %lu, first events in order %lu; "
                    "events_dropped rose by %lu%s",
                 (unsigned long)CI_OVERFULL_POSTS, (unsigned long)is.over_accepted,
                 (unsigned long)is.over_busy, (unsigned long)is.got_n, (unsigned long)is.got_ok,
                 (unsigned long)is.got_drops,
                 is.got_n < is.over_accepted ? " (the runtime kept the first ones, the rest are not counted)"
                                             : "");
    conf_record("C-INPUT.9", ok, d);
}

/* C-INPUT.9 is judged by on_end while the variant overfull runs; that variant carries no
 * payload ids of its own. */
static const struct conf_needs needs[] = {
    { "C-INPUT.9", "overfull", 1 },
};

const struct conf_area conf_area_input = {
    .name = "C-INPUT",
    .peers = peers,
    .peer_count = CONF_COUNT(peers),
    .variants = variants,
    .variant_count = CONF_COUNT(variants),
    .operators = operators,
    .operator_count = CONF_COUNT(operators),
    .needs = needs,
    .need_count = CONF_COUNT(needs),
    .on_end = on_end,
};
