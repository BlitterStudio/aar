/*
 * Host tests of the policy decisions the serving loop makes: the exit decision, the
 * reply backlog, the console writer's rules and the launcher queue, the SafeMessage
 * counts, the open plans and the DOS packet rules. No AmigaOS calls; the decisions
 * are the inline functions of aar_serve_core.h.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aar_serve_core.h"

/* The AAR file open flags, under the names the older tests use. */
#define ZZAPP_FILE_READ AAR_FILE_READ
#define ZZAPP_FILE_WRITE AAR_FILE_WRITE
#define ZZAPP_FILE_CREATE AAR_FILE_CREATE
#define ZZAPP_FILE_TRUNCATE AAR_FILE_TRUNCATE
#define ZZAPP_FILE_EXCL AAR_FILE_EXCL

static struct aar_serve_exit_in exit_in(uint32_t state, uint32_t end_reason,
                                        uint32_t elapsed, int stop_asked,
                                        int user_stop, int pending, int failed)
{
    struct aar_serve_exit_in in;

    memset(&in, 0, sizeof(in));
    in.state = state;
    in.end_reason = end_reason;
    in.elapsed_ms = elapsed;
    in.entered = 1;
    in.stop_asked = stop_asked;
    in.user_stop = user_stop;
    in.output_pending = pending;
    in.output_failed = failed;
    return in;
}

static void test_output_failure_does_not_block_exit(void)
{
    struct aar_serve_exit_in faulted =
        exit_in(AAR_STATE_FAULTED, 0u, 0u, 0, 0, 1, 1);
    struct aar_serve_exit_in idle = exit_in(AAR_STATE_IDLE, 0u, 0u, 0, 0, 1, 1);
    struct aar_serve_exit_in running =
        exit_in(AAR_STATE_RUNNING, 0u, 1000u, 1, 0, 1, 1);
    struct aar_serve_exit_in draining =
        exit_in(AAR_STATE_FAULTED, 0u, 0u, 0, 0, 1, 0);

    assert(aar_serve_output_blocks_exit(1, 1) == 0);
    assert(aar_serve_output_blocks_exit(1, 0) == 1);
    assert(aar_serve_run_should_exit(&faulted) == 1);
    assert(aar_serve_run_should_exit(&idle) == 1);
    assert(aar_serve_run_should_exit(&running) == 1);
    assert(aar_serve_run_should_exit(&draining) == 0);
}

static void test_ctrl_c_ends_even_if_output_remains(void)
{
    struct aar_serve_exit_in early =
        exit_in(AAR_STATE_FAULTED, 0u, 999u, 1, 1, 1, 0);
    struct aar_serve_exit_in due =
        exit_in(AAR_STATE_FAULTED, 0u, 1000u, 1, 1, 1, 0);
    struct aar_serve_exit_in running =
        exit_in(AAR_STATE_RUNNING, 0u, 1000u, 1, 1, 1, 0);
    struct aar_serve_exit_in not_yet =
        exit_in(AAR_STATE_RUNNING, 0u, 999u, 1, 1, 1, 0);

    assert(aar_serve_run_should_exit(&early) == 0);
    assert(aar_serve_run_should_exit(&due) == 1);
    assert(aar_serve_run_should_exit(&running) == 1);
    assert(aar_serve_run_should_exit(&not_yet) == 0);
    /* a program that was asked to stop, but not by Ctrl-C, is waited for while
     * its output is still going out */
    {
        struct aar_serve_exit_in out_pending =
            exit_in(AAR_STATE_RUNNING, 0u, 1000u, 1, 0, 1, 0);
        struct aar_serve_exit_in drained =
            exit_in(AAR_STATE_RUNNING, 0u, 1000u, 1, 0, 0, 0);

        assert(aar_serve_run_should_exit(&out_pending) == 0);
        assert(aar_serve_run_should_exit(&drained) == 1);
    }
}

static void test_clean_exit_still_drains(void)
{
    struct aar_serve_exit_in idle = exit_in(AAR_STATE_IDLE, 0u, 0u, 0, 0, 1, 0);
    struct aar_serve_exit_in done = exit_in(AAR_STATE_IDLE, 0u, 0u, 0, 0, 0, 0);
    struct aar_serve_exit_in watchdog =
        exit_in(AAR_STATE_RUNNING, AAR_END_RUNTIME_RESTART, 0u, 0, 0, 0, 0);

    assert(aar_serve_run_should_exit(&idle) == 0);
    assert(aar_serve_run_should_exit(&done) == 1);
    assert(aar_serve_run_should_exit(&watchdog) == 1);
}

static void test_lost_claim_ends_the_run(void)
{
    struct aar_serve_exit_in running =
        exit_in(AAR_STATE_RUNNING, 0u, 0u, 0, 0, 1, 0);
    struct aar_serve_exit_in starting =
        exit_in(AAR_STATE_STARTING, 0u, 0u, 0, 0, 0, 0);
    struct aar_serve_exit_in loading =
        exit_in(AAR_STATE_LOADING, 0u, 0u, 0, 0, 0, 0);

    /* A live handle with a running app keeps the loop going. */
    assert(aar_serve_run_should_exit(&running) == 0);
    running.lost = 1;
    starting.lost = 1;
    loading.lost = 1;
    assert(aar_serve_run_should_exit(&running) == 1);
    assert(aar_serve_run_should_exit(&starting) == 1);
    assert(aar_serve_run_should_exit(&loading) == 1);
}

static void test_console_read_gated_on_running(void)
{
    assert(aar_serve_console_read_allowed(AAR_STATE_RUNNING, 0) == 1);
    assert(aar_serve_console_read_allowed(AAR_STATE_FAULTED, 0) == 0);
    assert(aar_serve_console_read_allowed(AAR_STATE_STOPPING, 0) == 0);
    assert(aar_serve_console_read_allowed(AAR_STATE_IDLE, 0) == 0);
    assert(aar_serve_console_read_allowed(AAR_STATE_STARTING, 0) == 0);
    assert(aar_serve_console_read_allowed(AAR_STATE_RUNNING, 1) == 0);
}

static void test_finish_prompt_is_not_stdout(void)
{
    assert(aar_serve_finish_prompt(0, 1) == AAR_SERVE_FINISH_NONE);
    assert(aar_serve_finish_prompt(1, 1) == AAR_SERVE_FINISH_RETURN_ON_READ_HANDLE);
    assert(aar_serve_finish_prompt(1, 0) == AAR_SERVE_FINISH_WAIT_ON_USER_CONSOLE);
}

static void test_replies_are_never_dropped_for_want_of_room(void)
{
    uint32_t used = 0;
    uint32_t taken = 0;
    uint32_t deferred = 0;

    /* The SQ holds more requests than the backlog; the CQ stays full. Every
     * request taken adds at most one backlog entry; three deferred
     * completions (console write and read, flip) may follow. */
    while (aar_serve_reply_may_take_request(used)) {
        used++;
        taken++;
        assert(taken < 1000u);
    }
    assert(taken > 0u && taken < AAR_SERVE_REPLY_BACKLOG);
    for (deferred = 0; deferred < 3u; deferred++)
        used++;
    assert(used <= AAR_SERVE_REPLY_BACKLOG);
    assert(!aar_serve_reply_may_take_request(AAR_SERVE_REPLY_BACKLOG));
    assert(aar_serve_reply_may_take_request(0));
    /* Room returns as the backlog drains. */
    assert(aar_serve_reply_may_take_request(AAR_SERVE_REPLY_BACKLOG - AAR_SERVE_REPLY_RESERVE - 1u));
    assert(!aar_serve_reply_may_take_request(AAR_SERVE_REPLY_BACKLOG - AAR_SERVE_REPLY_RESERVE));
}

static void test_async_console_write_advances_only_on_completion(void)
{
    assert(aar_serve_console_write_advance(64u, 64) == 64u);
    assert(aar_serve_console_write_advance(64u, 17) == 17u);
    assert(aar_serve_console_write_advance(64u, 0) == 0u);
    assert(aar_serve_console_write_advance(64u, 65) == 0u);
    assert(!aar_serve_console_write_failed(64u, 64));
    assert(aar_serve_console_write_failed(64u, 17));
    assert(aar_serve_console_write_failed(64u, 0));
}

static void test_terminal_output_drains_before_exit(void)
{
    struct aar_serve_exit_in idle =
        exit_in(AAR_STATE_IDLE, 0u, 0u, 0, 0, 1, 0);
    struct aar_serve_exit_in faulted =
        exit_in(AAR_STATE_FAULTED, 0u, 0u, 0, 0, 1, 0);
    struct aar_serve_exit_in failed =
        exit_in(AAR_STATE_IDLE, 0u, 0u, 0, 0, 1, 1);

    assert(aar_serve_run_should_exit(&idle) == 0);
    assert(aar_serve_run_should_exit(&faulted) == 0);
    assert(aar_serve_run_should_exit(&failed) == 1);
}

static void test_raw_ctrl_c_requests_stop(void)
{
    assert(aar_serve_rawkey_requests_stop(0x33u, 0x0008u, 0x0008u));
    assert(!aar_serve_rawkey_requests_stop(0x20u, 0x0008u, 0x0008u)); /* Ctrl-A */
    assert(!aar_serve_rawkey_requests_stop(0x22u, 0x0008u, 0x0008u)); /* Ctrl-D */
    assert(!aar_serve_rawkey_requests_stop(0x33u, 0u, 0x0008u));
}

static void test_screen_buffer_safe_messages_are_all_reaped(void)
{
    uint32_t pending = 0;

    pending = aar_serve_screen_safe_started(pending);
    pending = aar_serve_screen_safe_started(pending);
    pending = aar_serve_screen_safe_completed(pending);
    assert(pending == 1u);
    pending = aar_serve_screen_safe_completed(pending);
    assert(pending == 0u);
    pending = aar_serve_screen_safe_completed(pending);
    assert(pending == 0u); /* nothing pending stays nothing */
}

static void test_launcher_output_reserves_status_room(void)
{
    uint32_t diagnostic_limit = AAR_SERVE_LAUNCHER_OUTPUT_BYTES -
                                AAR_SERVE_LAUNCHER_STATUS_RESERVE_BYTES;

    assert(aar_serve_launcher_output_accept(diagnostic_limit - 1u, 1u, 1));
    assert(!aar_serve_launcher_output_accept(diagnostic_limit, 1u, 1));
    assert(aar_serve_launcher_output_accept(diagnostic_limit, 1u, 0));
    assert(!aar_serve_launcher_output_accept(AAR_SERVE_LAUNCHER_OUTPUT_BYTES, 1u, 0));
}

static void test_launcher_output_orders_queued_messages_before_app_output(void)
{
    assert(aar_serve_output_next_source(0u, 0u) == AAR_SERVE_OUTPUT_NONE);
    assert(aar_serve_output_next_source(12u, 0u) == AAR_SERVE_OUTPUT_LAUNCHER);
    assert(aar_serve_output_next_source(0u, 12u) == AAR_SERVE_OUTPUT_APP);
    assert(aar_serve_output_next_source(12u, 12u) == AAR_SERVE_OUTPUT_LAUNCHER);
}

/* The flags word carries AAR values: READ 1, WRITE 2, CREATE 4, TRUNCATE 8,
 * EXCL 16. Every valid combination has one plan; everything else, including
 * unknown bits, is refused before AmigaDOS is touched. */
static void test_open_flags_map_to_one_plan_each(void)
{
    static const struct {
        uint32_t flags;
        enum aar_serve_open_plan plan;
    } valid[] = {
        {1u, AAR_SERVE_OPEN_READ_EXISTING},
        {2u, AAR_SERVE_OPEN_UPDATE_EXISTING},
        {3u, AAR_SERVE_OPEN_UPDATE_EXISTING},
        {2u | 8u, AAR_SERVE_OPEN_TRUNCATE_EXISTING},
        {3u | 8u, AAR_SERVE_OPEN_TRUNCATE_EXISTING},
        {2u | 4u, AAR_SERVE_OPEN_CREATE_KEEP},
        {3u | 4u, AAR_SERVE_OPEN_CREATE_KEEP},
        {2u | 4u | 8u, AAR_SERVE_OPEN_CREATE_REPLACE},
        {3u | 4u | 8u, AAR_SERVE_OPEN_CREATE_REPLACE},
        {2u | 4u | 16u, AAR_SERVE_OPEN_CREATE_EXCLUSIVE},
        {3u | 4u | 16u, AAR_SERVE_OPEN_CREATE_EXCLUSIVE},
        {2u | 4u | 8u | 16u, AAR_SERVE_OPEN_CREATE_EXCLUSIVE},
        {3u | 4u | 8u | 16u, AAR_SERVE_OPEN_CREATE_EXCLUSIVE},
    };
    uint32_t flags;
    size_t i;

    assert(ZZAPP_FILE_READ == 1u && ZZAPP_FILE_WRITE == 2u &&
           ZZAPP_FILE_CREATE == 4u && ZZAPP_FILE_TRUNCATE == 8u &&
           ZZAPP_FILE_EXCL == 16u);
    for (flags = 0; flags < 256u; flags++) {
        enum aar_serve_open_plan expect = AAR_SERVE_OPEN_INVALID;

        for (i = 0; i < sizeof(valid) / sizeof(valid[0]); i++)
            if (valid[i].flags == flags)
                expect = valid[i].plan;
        assert(aar_serve_open_plan_for_flags(flags) == expect);
    }
    assert(aar_serve_open_plan_for_flags(0x80000000u | 3u) ==
           AAR_SERVE_OPEN_INVALID);
    assert(aar_serve_open_plan_for_flags(0xffffffffu) == AAR_SERVE_OPEN_INVALID);
}

static void test_open_flag_refusals_are_the_invalid_combinations(void)
{
    /* Neither READ nor WRITE. */
    assert(aar_serve_open_plan_for_flags(0u) == AAR_SERVE_OPEN_INVALID);
    assert(aar_serve_open_plan_for_flags(ZZAPP_FILE_CREATE) ==
           AAR_SERVE_OPEN_INVALID);
    /* CREATE or TRUNCATE without WRITE. */
    assert(aar_serve_open_plan_for_flags(ZZAPP_FILE_READ | ZZAPP_FILE_CREATE) ==
           AAR_SERVE_OPEN_INVALID);
    assert(aar_serve_open_plan_for_flags(ZZAPP_FILE_READ | ZZAPP_FILE_TRUNCATE) ==
           AAR_SERVE_OPEN_INVALID);
    /* EXCL without CREATE. */
    assert(aar_serve_open_plan_for_flags(ZZAPP_FILE_WRITE | ZZAPP_FILE_EXCL) ==
           AAR_SERVE_OPEN_INVALID);
    assert(aar_serve_open_plan_for_flags(ZZAPP_FILE_READ | ZZAPP_FILE_WRITE |
                                         ZZAPP_FILE_TRUNCATE |
                                         ZZAPP_FILE_EXCL) == AAR_SERVE_OPEN_INVALID);
    /* Unknown bit. */
    assert(aar_serve_open_plan_for_flags(ZZAPP_FILE_READ | 32u) ==
           AAR_SERVE_OPEN_INVALID);
    /* The old CREATE (create or replace) is CREATE|TRUNCATE now. */
    assert(aar_serve_open_plan_for_flags(ZZAPP_FILE_WRITE | ZZAPP_FILE_CREATE |
                                         ZZAPP_FILE_TRUNCATE) ==
           AAR_SERVE_OPEN_CREATE_REPLACE);
}

static void test_dos_packet_transport_and_error_are_distinct(void)
{
    assert(aar_serve_dos_boolean_result(1, 1));
    assert(!aar_serve_dos_boolean_result(1, 0));
    assert(!aar_serve_dos_boolean_result(0, 1));
    assert(aar_serve_dos_error_code(1, 42, -1) == 42);
    assert(aar_serve_dos_error_code(0, 42, -1) == -1);
}

static void test_packet_wait_outlives_abandonment(void)
{
    assert(aar_serve_packet_abandoned(1, 1));
    assert(aar_serve_packet_abandoned(0, 0));
    assert(!aar_serve_packet_abandoned(0, 1));
    assert(!aar_serve_packet_keep_waiting(1));
    assert(aar_serve_packet_keep_waiting(0));
}

static void test_bstr_encoding_matches_dos_packets(void)
{
    uint8_t buf[16];

    assert(aar_serve_bstr_encode("Work:foo", buf, sizeof(buf)) == 10u);
    assert(buf[0] == 8);
    assert(memcmp(buf + 1, "Work:foo", 8) == 0);
    assert(buf[9] == 0);
    assert(aar_serve_bstr_encode("", buf, sizeof(buf)) == 2u);
    assert(buf[0] == 0);
    assert(aar_serve_bstr_encode("ab", buf, sizeof(buf)) == 4u);
    assert(buf[3] == 0);
    assert(aar_serve_bstr_encode("abc", buf, 2u) == 0u);
    assert(strcmp(aar_serve_dos_packet_path("Apps:demo.elf"), "demo.elf") == 0);
    assert(strcmp(aar_serve_dos_packet_path("Work:Apps/demo.elf"),
                  "Apps/demo.elf") == 0);
    assert(strcmp(aar_serve_dos_packet_path("relative/demo.elf"),
                  "relative/demo.elf") == 0);
    assert(aar_serve_dos_packet_path(NULL) == NULL);
}

static void test_examine_packet_names_are_bcpl_strings(void)
{
    uint8_t fib_name[108];
    char out[256];

    /* A short name with no NUL and stale bytes after its counted end. */
    memset(fib_name, 'x', sizeof(fib_name));
    fib_name[0] = 7;
    memcpy(fib_name + 1, "f00.bin", 7);
    assert(aar_serve_bstr_decode(fib_name, sizeof(fib_name), out, sizeof(out)) == 7u);
    assert(strcmp(out, "f00.bin") == 0);
    assert(aar_serve_bstr_decode(fib_name, sizeof(fib_name), out, 4u) == 3u);
    assert(strcmp(out, "f00") == 0);
    /* One byte, with and without a trailing NUL outside the counted name. */
    fib_name[0] = 1;
    fib_name[1] = 'a';
    fib_name[2] = 0;
    assert(aar_serve_bstr_decode(fib_name, sizeof(fib_name), out, sizeof(out)) == 1u);
    assert(strcmp(out, "a") == 0);
    fib_name[2] = 'x';
    assert(aar_serve_bstr_decode(fib_name, sizeof(fib_name), out, sizeof(out)) == 1u);
    assert(strcmp(out, "a") == 0);
    fib_name[0] = 0;
    assert(aar_serve_bstr_decode(fib_name, sizeof(fib_name), out, sizeof(out)) == 0u);
    assert(out[0] == 0);
    fib_name[0] = sizeof(fib_name) - 1u;
    assert(aar_serve_bstr_decode(fib_name, sizeof(fib_name), out, sizeof(out)) == 107u);
    assert(memcmp(out, fib_name + 1, 107) == 0 && out[107] == 0);
}

static void test_pending_packet_ctrl_c_still_requests_stop(void)
{
    assert(aar_serve_ctrl_c_must_request_stop(1));
    assert(!aar_serve_ctrl_c_must_request_stop(0));
}

static void test_user_break_is_pending_or_consumed(void)
{
    /* dos_packet_async() clears the signal and records user_stop. */
    assert(aar_serve_user_break(0, 1));
    assert(aar_serve_user_break(1, 0));
    assert(!aar_serve_user_break(0, 0));
}

struct fake_dos_path {
    struct aar_serve_dos_path_candidate candidates[2];
    struct aar_serve_dos_path_reply replies[2];
    uintptr_t handlers[2];
    uint32_t directories[2];
    uintptr_t bstr_addresses[2];
    uint32_t bstr_bptrs[2];
    int advances[3];
    uint32_t candidate_count;
    uint32_t candidate_index;
    uint32_t resolve_count;
    uint32_t send_count;
    uint32_t bstr_bytes;
    uint8_t bstr[258];
};

static int fake_dos_resolve(void *context, int advance,
                            struct aar_serve_dos_path_candidate *candidate)
{
    struct fake_dos_path *path = context;

    path->advances[path->resolve_count++] = advance;
    if (advance)
        path->candidate_index++;
    else
        path->candidate_index = 0;
    if (path->candidate_index >= path->candidate_count)
        return 0;
    *candidate = path->candidates[path->candidate_index];
    return 1;
}

static void fake_dos_send(void *context,
                          const struct aar_serve_dos_path_candidate *candidate,
                          const uint8_t *name_bstr, uint32_t name_bstr_bytes,
                          uint32_t name_bstr_bptr,
                          struct aar_serve_dos_path_reply *reply)
{
    struct fake_dos_path *path = context;
    uint32_t index = path->send_count++;

    assert(index < 2u);
    assert(name_bstr_bytes <= sizeof(path->bstr));
    assert(((uintptr_t)name_bstr & 3u) == 0u);
    assert(name_bstr_bptr == (uint32_t)((uintptr_t)name_bstr >> 2));
    path->handlers[index] = candidate->handler;
    path->directories[index] = candidate->directory;
    path->bstr_addresses[index] = (uintptr_t)name_bstr;
    path->bstr_bptrs[index] = name_bstr_bptr;
    path->bstr_bytes = name_bstr_bytes;
    memcpy(path->bstr, name_bstr, name_bstr_bytes);
    *reply = path->replies[index];
}

static void test_dos_path_dispatch_uses_assign_locks_and_retries(void)
{
    const int32_t not_found = 404;
    union {
        uintptr_t alignment;
        uint8_t bytes[260];
    } name_bstr;
    struct fake_dos_path path;
    int attempted;
    int usable;
    memset(&path, 0, sizeof(path));
    path.candidate_count = 1u;
    path.candidates[0].handler = 0x11110000u;
    path.candidates[0].directory = 0x1000u;
    path.replies[0].completed = 1;
    path.replies[0].result = 1;
    assert(aar_serve_dos_dispatch_path("Apps:demo.elf", name_bstr.bytes, 258u,
                                       not_found, fake_dos_resolve, &path,
                                       fake_dos_send, &path, &attempted, &usable,
                                       NULL));
    assert(attempted && usable);
    assert(path.send_count == 1u);
    assert(path.handlers[0] == 0x11110000u);
    assert(path.directories[0] == 0x1000u);
    assert(path.bstr_bytes == 10u);
    assert(path.bstr[0] == 8u);
    assert(memcmp(path.bstr + 1, "demo.elf", 8u) == 0);
    assert(path.bstr[9] == 0u);

    memset(&path, 0, sizeof(path));
    path.candidate_count = 2u;
    path.candidates[0].handler = 0x11110000u;
    path.candidates[0].directory = 0x1000u;
    path.candidates[1].handler = 0x22220000u;
    path.candidates[1].directory = 0x2000u;
    path.replies[0].completed = 1;
    path.replies[0].error = not_found;
    path.replies[1].completed = 1;
    path.replies[1].result = 1;
    assert(aar_serve_dos_dispatch_path("Apps:demo.elf", name_bstr.bytes, 258u,
                                       not_found, fake_dos_resolve, &path,
                                       fake_dos_send, &path, &attempted, &usable,
                                       NULL));
    assert(attempted && usable);
    assert(path.send_count == 2u);
    assert(path.handlers[0] == 0x11110000u);
    assert(path.handlers[1] == 0x22220000u);
    assert(path.directories[0] == 0x1000u);
    assert(path.directories[1] == 0x2000u);
    assert(path.advances[0] == 0);
    assert(path.advances[1] == 1);

    memset(&path, 0, sizeof(path));
    path.candidate_count = 2u;
    path.candidates[0].directory = 0x1000u;
    path.replies[0].completed = 1;
    path.replies[0].error = 5;
    assert(!aar_serve_dos_dispatch_path("Apps:demo.elf", name_bstr.bytes, 258u,
                                        not_found, fake_dos_resolve, &path,
                                        fake_dos_send, &path, &attempted, &usable,
                                        NULL));
    assert(attempted && usable);
    assert(path.send_count == 1u);

    memset(&path, 0, sizeof(path));
    path.candidate_count = 2u;
    path.candidates[0].directory = 0x1000u;
    path.candidates[1].directory = 0x2000u;
    path.replies[0].completed = 1;
    path.replies[0].error = not_found;
    path.replies[1].completed = 1;
    path.replies[1].abandoned = 1;
    path.replies[1].result = 1;
    assert(!aar_serve_dos_dispatch_path("Apps:demo.elf", name_bstr.bytes, 258u,
                                        not_found, fake_dos_resolve, &path,
                                        fake_dos_send, &path, &attempted, &usable,
                                        NULL));
    assert(attempted && !usable);
    assert(path.send_count == 2u);

    memset(&path, 0, sizeof(path));
    path.candidate_count = 1u;
    path.candidates[0].handler = 0x33330000u;
    path.candidates[0].directory = 0x3000u;
    path.replies[0].completed = 1;
    path.replies[0].result = 1;
    assert(aar_serve_dos_dispatch_path("relative/demo.elf", name_bstr.bytes,
                                       258u, not_found, fake_dos_resolve, &path,
                                       fake_dos_send, &path, &attempted, &usable,
                                       NULL));
    assert(attempted && usable);
    assert(path.handlers[0] == 0x33330000u);
    assert(path.directories[0] == 0x3000u);
    assert(path.bstr[0] == strlen("relative/demo.elf"));
    assert(memcmp(path.bstr + 1, "relative/demo.elf",
                  strlen("relative/demo.elf")) == 0);
    assert(path.bstr_addresses[0] == (uintptr_t)name_bstr.bytes);
    assert(path.bstr_bptrs[0] ==
           (uint32_t)((uintptr_t)name_bstr.bytes >> 2));

    memset(&path, 0, sizeof(path));
    path.candidate_count = 1u;
    path.replies[0].completed = 1;
    path.replies[0].result = 1;
    assert(!aar_serve_dos_dispatch_path("Apps:demo.elf", name_bstr.bytes + 1,
                                        258u, not_found, fake_dos_resolve, &path,
                                        fake_dos_send, &path, &attempted, &usable,
                                        NULL));
    assert(path.send_count == 0u);
}

/* The probe of an absolute name walks a multi-directory assign and finds the
 * file in the second directory. Resolving the name again for the open would
 * restart at the first directory (where FINDUPDATE creates a new file); the
 * open must instead go to the candidate the probe reported. */
static void test_probe_candidate_is_reported_and_reused_for_the_open(void)
{
    const int32_t not_found = 404;
    union {
        uintptr_t alignment;
        uint8_t bytes[260];
    } name_bstr;
    struct aar_serve_dos_path_candidate selected;
    struct fake_dos_path probe;
    struct fake_dos_path open;
    int attempted;
    int usable;

    memset(&probe, 0, sizeof(probe));
    probe.candidate_count = 2u;
    probe.candidates[0].handler = 0x11110000u;
    probe.candidates[0].directory = 0x1000u;
    probe.candidates[1].handler = 0x22220000u;
    probe.candidates[1].directory = 0x2000u;
    probe.replies[0].completed = 1;
    probe.replies[0].error = not_found;
    probe.replies[1].completed = 1;
    probe.replies[1].result = 1;
    memset(&selected, 0, sizeof(selected));
    assert(aar_serve_dos_dispatch_path("Apps:data/save.dat", name_bstr.bytes,
                                       258u, not_found, fake_dos_resolve, &probe,
                                       fake_dos_send, &probe, &attempted, &usable,
                                       &selected));
    assert(selected.handler == 0x22220000u);
    assert(selected.directory == 0x2000u);

    /* A pinned resolver offers only that candidate. */
    memset(&open, 0, sizeof(open));
    open.candidate_count = 1u;
    open.candidates[0] = selected;
    open.replies[0].completed = 1;
    open.replies[0].result = 1;
    assert(aar_serve_dos_dispatch_path("Apps:data/save.dat", name_bstr.bytes,
                                       258u, not_found, fake_dos_resolve, &open,
                                       fake_dos_send, &open, &attempted, &usable,
                                       NULL));
    assert(open.send_count == 1u);
    assert(open.handlers[0] == 0x22220000u);
    assert(open.directories[0] == 0x2000u);

    /* An in-use answer decides at its own candidate: no later directory is
     * tried, and that candidate is the one reported. */
    memset(&probe, 0, sizeof(probe));
    probe.candidate_count = 2u;
    probe.candidates[0].handler = 0x11110000u;
    probe.candidates[0].directory = 0x1000u;
    probe.candidates[1].handler = 0x22220000u;
    probe.candidates[1].directory = 0x2000u;
    probe.replies[0].completed = 1;
    probe.replies[0].error = not_found;
    probe.replies[1].completed = 1;
    probe.replies[1].error = 202;
    memset(&selected, 0, sizeof(selected));
    assert(!aar_serve_dos_dispatch_path("Apps:data/save.dat", name_bstr.bytes,
                                        258u, not_found, fake_dos_resolve, &probe,
                                        fake_dos_send, &probe, &attempted,
                                        &usable, &selected));
    assert(usable);
    assert(probe.send_count == 2u);
    assert(selected.handler == 0x22220000u);
    assert(selected.directory == 0x2000u);
}

static void test_probe_result_distinguishes_missing_in_use_and_errors(void)
{
    const int32_t not_found = 205;
    const int32_t in_use = 202;

    assert(aar_serve_probe_result_for_lock(1, 0, not_found, in_use) ==
           AAR_SERVE_PROBE_EXISTS);
    assert(aar_serve_probe_result_for_lock(0, not_found, not_found, in_use) ==
           AAR_SERVE_PROBE_MISSING);
    assert(aar_serve_probe_result_for_lock(0, in_use, not_found, in_use) ==
           AAR_SERVE_PROBE_EXISTS);
    assert(aar_serve_probe_result_for_lock(0, 218, not_found, in_use) ==
           AAR_SERVE_PROBE_ERROR);
    assert(aar_serve_probe_result_for_lock(0, 0, not_found, in_use) ==
           AAR_SERVE_PROBE_ERROR);
}

static void test_plans_that_act_on_an_existing_file_probe_the_target(void)
{
    assert(!aar_serve_plan_probes_target(AAR_SERVE_OPEN_READ_EXISTING, 0));
    assert(!aar_serve_plan_probes_target(AAR_SERVE_OPEN_READ_EXISTING, 1));
    assert(aar_serve_plan_probes_target(AAR_SERVE_OPEN_UPDATE_EXISTING, 0));
    assert(aar_serve_plan_probes_target(AAR_SERVE_OPEN_UPDATE_EXISTING, 1));
    assert(aar_serve_plan_probes_target(AAR_SERVE_OPEN_TRUNCATE_EXISTING, 0));
    assert(aar_serve_plan_probes_target(AAR_SERVE_OPEN_TRUNCATE_EXISTING, 1));
    assert(aar_serve_plan_probes_target(AAR_SERVE_OPEN_CREATE_EXCLUSIVE, 0));
    assert(aar_serve_plan_probes_target(AAR_SERVE_OPEN_CREATE_EXCLUSIVE, 1));
    /* CREATE acts on an existing file too (keep contents, or replace it);
     * only an absolute name needs the probe to find its directory. */
    assert(!aar_serve_plan_probes_target(AAR_SERVE_OPEN_CREATE_KEEP, 0));
    assert(aar_serve_plan_probes_target(AAR_SERVE_OPEN_CREATE_KEEP, 1));
    assert(!aar_serve_plan_probes_target(AAR_SERVE_OPEN_CREATE_REPLACE, 0));
    assert(aar_serve_plan_probes_target(AAR_SERVE_OPEN_CREATE_REPLACE, 1));
    assert(!aar_serve_plan_probes_target(AAR_SERVE_OPEN_INVALID, 1));
}

/* A relative exclusive create whose target another program holds open: the
 * handler refuses the lock with ERROR_OBJECT_IN_USE. The file exists, so the
 * answer is EEXIST, not the EBUSY that any other plan reports. */
static void test_leaf_lock_failure_answers_by_plan(void)
{
    const int32_t not_found = 205;
    const int32_t in_use = 202;
    const enum aar_serve_open_plan creates[] = {
        AAR_SERVE_OPEN_CREATE_KEEP, AAR_SERVE_OPEN_CREATE_REPLACE,
        AAR_SERVE_OPEN_CREATE_EXCLUSIVE
    };
    const enum aar_serve_open_plan existing[] = {
        AAR_SERVE_OPEN_READ_EXISTING, AAR_SERVE_OPEN_UPDATE_EXISTING,
        AAR_SERVE_OPEN_TRUNCATE_EXISTING
    };
    unsigned i;

    assert(aar_serve_leaf_lock_failure(AAR_SERVE_OPEN_CREATE_EXCLUSIVE, in_use,
                                       not_found, in_use) == AAR_SERVE_LEAF_EXISTS);
    assert(aar_serve_leaf_lock_failure(AAR_SERVE_OPEN_CREATE_KEEP, in_use, not_found,
                                       in_use) == AAR_SERVE_LEAF_REFUSE);
    assert(aar_serve_leaf_lock_failure(AAR_SERVE_OPEN_CREATE_REPLACE, in_use,
                                       not_found, in_use) == AAR_SERVE_LEAF_REFUSE);
    for (i = 0; i < sizeof(creates) / sizeof(creates[0]); i++) {
        assert(aar_serve_leaf_lock_failure(creates[i], not_found, not_found,
                                           in_use) == AAR_SERVE_LEAF_PROCEED);
        assert(aar_serve_leaf_lock_failure(creates[i], 218, not_found, in_use) ==
               AAR_SERVE_LEAF_REFUSE);
        assert(aar_serve_leaf_lock_failure(creates[i], 0, not_found, in_use) ==
               AAR_SERVE_LEAF_REFUSE);
    }
    for (i = 0; i < sizeof(existing) / sizeof(existing[0]); i++) {
        assert(aar_serve_leaf_lock_failure(existing[i], not_found, not_found,
                                           in_use) == AAR_SERVE_LEAF_REFUSE);
        assert(aar_serve_leaf_lock_failure(existing[i], in_use, not_found,
                                           in_use) == AAR_SERVE_LEAF_REFUSE);
    }
}

/* A packet from a lost generation or after Ctrl-C never reaches the handler,
 * except releases, which give back what an earlier packet acquired. */
static void test_lost_operation_packets_are_not_sent_but_releases_are(void)
{
    assert(aar_serve_packet_may_send(0, 1, 0));
    assert(!aar_serve_packet_may_send(0, 0, 0));
    assert(!aar_serve_packet_may_send(0, 1, 1));
    assert(!aar_serve_packet_may_send(0, 0, 1));
    assert(aar_serve_packet_may_send(1, 0, 0));
    assert(aar_serve_packet_may_send(1, 1, 1));
    assert(aar_serve_packet_may_send(1, 0, 1));
}

/* The NEWFILE fallback after a handler without ACTION_SET_FILE_SIZE follows
 * an ACTION_END wait that Ctrl-C or a generation change may have abandoned.
 * Ownership, a Ctrl-C still pending, and a Ctrl-C an earlier wait already
 * consumed (user_stop) each stop the destructive step. */
static void test_destructive_open_steps_stop_when_the_request_is_cancelled(void)
{
    assert(aar_serve_open_step_allowed(1, 0, 0));
    assert(!aar_serve_open_step_allowed(0, 0, 0));
    assert(!aar_serve_open_step_allowed(1, 1, 0));
    assert(!aar_serve_open_step_allowed(1, 0, 1));
    assert(!aar_serve_open_step_allowed(0, 1, 1));
}

struct fake_release {
    int delivered;
    int32_t result;
    int sends;
    int freed;
    int fallback;
    int forgotten;
};

static int fake_send_release(void *context, int32_t *result)
{
    struct fake_release *release = context;

    release->sends++;
    *result = release->result;
    return release->delivered;
}

static void fake_free_local(void *context)
{
    ((struct fake_release *)context)->freed++;
}

static void fake_fallback_release(void *context)
{
    ((struct fake_release *)context)->fallback++;
}

static void fake_forget_slot(void *context)
{
    ((struct fake_release *)context)->forgotten++;
}

static struct aar_serve_dos_release_ops fake_release_ops(
    struct fake_release *release)
{
    struct aar_serve_dos_release_ops ops;

    ops.context = release;
    ops.send_release = fake_send_release;
    ops.free_local = fake_free_local;
    ops.fallback_release = fake_fallback_release;
    ops.forget_slot = fake_forget_slot;
    return ops;
}

static void test_async_release_preserves_or_frees_resources_by_delivery(void)
{
    struct fake_release release;
    struct aar_serve_dos_release_ops ops;
    int delivered;

    memset(&release, 0, sizeof(release));
    release.delivered = 1;
    release.result = 1;
    ops = fake_release_ops(&release);
    assert(aar_serve_dos_close_slot(&ops, &delivered));
    assert(delivered && release.sends == 1 && release.freed == 1 &&
           release.forgotten == 1 && release.fallback == 0);

    memset(&release, 0, sizeof(release));
    release.delivered = 1;
    ops = fake_release_ops(&release);
    assert(!aar_serve_dos_close_slot(&ops, &delivered));
    assert(delivered && release.freed == 1 && release.forgotten == 1 &&
           release.fallback == 0);

    memset(&release, 0, sizeof(release));
    ops = fake_release_ops(&release);
    assert(!aar_serve_dos_close_slot(&ops, &delivered));
    assert(!delivered && release.freed == 0 && release.forgotten == 0 &&
           release.fallback == 0);
    aar_serve_dos_end_or_close(&ops);
    assert(release.sends == 2 && release.freed == 0 && release.fallback == 1);

    memset(&release, 0, sizeof(release));
    release.delivered = 1;
    ops = fake_release_ops(&release);
    aar_serve_dos_release_abandoned(&ops, 1);
    assert(release.sends == 1 && release.freed == 1 && release.fallback == 0);

    memset(&release, 0, sizeof(release));
    ops = fake_release_ops(&release);
    aar_serve_dos_release_abandoned(&ops, 1);
    assert(release.sends == 1 && release.freed == 0 && release.fallback == 1);
    aar_serve_dos_release_abandoned(&ops, 0);
    assert(release.sends == 1);
}

/* The time an app gets to honour a stop is the platform's (AARInfo.stop_grace_ms);
 * the default applies only when the platform says none. */
static void test_the_stop_grace_is_the_platforms(void)
{
    struct aar_serve_exit_in in = exit_in(AAR_STATE_STOPPING, 0u, 1500u, 1, 0, 0, 0);

    in.grace_ms = 2000u;
    assert(aar_serve_run_should_exit(&in) == 0);
    in.elapsed_ms = 2000u;
    assert(aar_serve_run_should_exit(&in) == 1);
    in.grace_ms = 100u;
    in.elapsed_ms = 99u;
    assert(aar_serve_run_should_exit(&in) == 0);
    in.elapsed_ms = 100u;
    assert(aar_serve_run_should_exit(&in) == 1);
    /* A user stop ends the run after two graces even if output is pending. */
    in = exit_in(AAR_STATE_RUNNING, 0u, 399u, 1, 1, 1, 0);
    in.grace_ms = 200u;
    assert(aar_serve_run_should_exit(&in) == 0);
    in.elapsed_ms = 400u;
    assert(aar_serve_run_should_exit(&in) == 1);
    /* None reported: the default. */
    in = exit_in(AAR_STATE_STOPPING, 0u, AAR_SERVE_GRACE_MS - 1u, 1, 0, 0, 0);
    assert(aar_serve_run_should_exit(&in) == 0);
    in.elapsed_ms = AAR_SERVE_GRACE_MS;
    assert(aar_serve_run_should_exit(&in) == 1);
}

int main(void)
{
    test_output_failure_does_not_block_exit();
    test_ctrl_c_ends_even_if_output_remains();
    test_clean_exit_still_drains();
    test_lost_claim_ends_the_run();
    test_console_read_gated_on_running();
    test_finish_prompt_is_not_stdout();
    test_replies_are_never_dropped_for_want_of_room();
    test_async_console_write_advances_only_on_completion();
    test_terminal_output_drains_before_exit();
    test_raw_ctrl_c_requests_stop();
    test_screen_buffer_safe_messages_are_all_reaped();
    test_launcher_output_reserves_status_room();
    test_launcher_output_orders_queued_messages_before_app_output();
    test_the_stop_grace_is_the_platforms();
    test_open_flags_map_to_one_plan_each();
    test_open_flag_refusals_are_the_invalid_combinations();
    test_dos_packet_transport_and_error_are_distinct();
    test_packet_wait_outlives_abandonment();
    test_bstr_encoding_matches_dos_packets();
    test_examine_packet_names_are_bcpl_strings();
    test_pending_packet_ctrl_c_still_requests_stop();
    test_user_break_is_pending_or_consumed();
    test_dos_path_dispatch_uses_assign_locks_and_retries();
    test_probe_candidate_is_reported_and_reused_for_the_open();
    test_probe_result_distinguishes_missing_in_use_and_errors();
    test_plans_that_act_on_an_existing_file_probe_the_target();
    test_leaf_lock_failure_answers_by_plan();
    test_lost_operation_packets_are_not_sent_but_releases_are();
    test_destructive_open_steps_stop_when_the_request_is_cancelled();
    test_async_release_preserves_or_frees_resources_by_delivery();
    puts("serve_policy_test: ok");
    return 0;
}
