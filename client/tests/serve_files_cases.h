/*
 * Tests of serve_loop_test.c for the 0.6 requests of the files, time, locale,
 * environment and clipboard groups: FILE_DELETE, FILE_RENAME, DIR_CREATE,
 * FILE_SET_DATE, FILE_SET_PROTECT, FILE_TRUNCATE, VOLUME_INFO, FILE_STAT's
 * protection bits, TIME_UNIX, LOCALE_INFO, ENV_GET, CLIP_READ and CLIP_WRITE. It
 * is included into serve_loop_test.c (which has the helpers and the fake
 * system) and is not compiled alone.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */

/* ---- Helpers ----------------------------------------------------------------------------------- */

static uint32_t files_nid = 1000;

/* A request with entry flags; the payload is at off in the app's buffer. */
static int callf(uint32_t op, uint32_t flags, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                 uint32_t off, uint32_t len, struct comp *c)
{
    uint32_t arg[6];

    memset(arg, 0, sizeof(arg));
    arg[0] = a0;
    arg[1] = a1;
    arg[2] = a2;
    arg[3] = a3;
    if (refplat_app_request(SLOT, op, flags, ++files_nid, arg, len ? refplat_buf_token(SLOT, 0) : 0,
                            off, len) != AAR_OK)
        return 0;
    return wait_comp(c, 400);
}

/* An op whose payload is a path. */
static int pcall(uint32_t op, const char *path, uint32_t a0, uint32_t a1, uint32_t a2, struct comp *c)
{
    put_str(0x100, path);
    return callf(op, 0, a0, a1, a2, 0, 0x100, (uint32_t)strlen(path) + 1u, c);
}

static int rename_call(const char *from, const char *to, uint32_t flags, struct comp *c)
{
    size_t lf = strlen(from) + 1u, lt = strlen(to) + 1u;

    memcpy((void *)(uintptr_t)buf(0x100), from, lf);
    memcpy((void *)(uintptr_t)(buf(0x100) + lf), to, lt);
    return callf(AAR_OP_FILE_RENAME, 0, flags, 0, 0, 0, 0x100, (uint32_t)(lf + lt), c);
}

static void put_bytes(uint32_t off, const void *data, uint32_t n)
{
    memcpy((void *)(uintptr_t)buf(off), data, n);
}

static int in_data(const char *name)
{
    return hx_node_find(2, name);
}

/* Four directory entries below the root of data_dir(): "empty", "full" (with one entry),
 * "held" (locks of it fail with OBJECT_IN_USE). */
static void files_extra_nodes(void)
{
    int full = hx_node_add(2, "full", 1, NULL, 0);

    hx_node_add(2, "empty", 1, NULL, 0);
    hx_node_add(full, "x", 0, "x", 1);
    hx_nodes[hx_node_add(2, "held", 0, "held", 4)].held = 1;
}

/* ---- FILE_DELETE ----------------------------------------------------------------------------------- */

static void test_file_delete_and_its_errors(void)
{
    struct comp c;
    int n;

    setup("file_delete");
    data_dir();
    files_extra_nodes();
    make_serve();
    CHECK(pcall(AAR_OP_FILE_DELETE, "second", 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(in_data("second") == 0);
    CHECK(pcall(AAR_OP_FILE_DELETE, "second", 0, 0, 0, &c) && c.result == AAR_E_NOENT);
    CHECK(pcall(AAR_OP_FILE_DELETE, "empty", 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(in_data("empty") == 0);
    /* a directory with an entry stays */
    CHECK(pcall(AAR_OP_FILE_DELETE, "full", 0, 0, 0, &c) && c.result == AAR_E_EXIST);
    CHECK(in_data("full") != 0 && hx_node_find(in_data("full"), "x") != 0);
    /* delete-protected */
    n = in_data("hello.txt");
    hx_nodes[n].protect = 1;
    CHECK(pcall(AAR_OP_FILE_DELETE, "hello.txt", 0, 0, 0, &c) && c.result == AAR_E_ACCES);
    CHECK(in_data("hello.txt") == n);
    hx_nodes[n].protect = 0;
    /* in use: the handler refuses the delete of an open file */
    CHECK(pcall(AAR_OP_FILE_OPEN, "hello.txt", 3, AAR_FILE_READ, 0, &c) && c.result == AAR_OK);
    CHECK(pcall(AAR_OP_FILE_DELETE, "hello.txt", 0, 0, 0, &c) && c.result == AAR_E_BUSY);
    CHECK(in_data("hello.txt") == n);
    CHECK(call(AAR_OP_FILE_CLOSE, 90, 3, 0, 0, 0, 0, 0, &c) && c.result == AAR_OK);
    /* held by another program: the lock is refused with the same error */
    CHECK(pcall(AAR_OP_FILE_DELETE, "held", 0, 0, 0, &c) && c.result == AAR_E_BUSY);
    CHECK(in_data("held") != 0);
    CHECK(pcall(AAR_OP_FILE_DELETE, "hello.txt", 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(in_data("hello.txt") == 0);
    finish();
}

static void test_file_delete_path_policy(void)
{
    struct comp c;
    int secret;

    setup("file_delete_policy");
    data_dir();
    make_serve();
    secret = hx_node_find(1, "secret");
    CHECK(secret != 0);
    CHECK(pcall(AAR_OP_FILE_DELETE, "../secret", 0, 0, 0, &c) && c.result == AAR_E_ACCES);
    CHECK(pcall(AAR_OP_FILE_DELETE, "/secret", 0, 0, 0, &c) && c.result == AAR_E_ACCES);
    CHECK(pcall(AAR_OP_FILE_DELETE, "RAM:secret", 0, 0, 0, &c) && c.result == AAR_E_ACCES);
    CHECK(pcall(AAR_OP_FILE_DELETE, ".", 0, 0, 0, &c) && c.result == AAR_E_ACCES);
    CHECK(pcall(AAR_OP_FILE_DELETE, "..", 0, 0, 0, &c) && c.result == AAR_E_ACCES);
    CHECK(pcall(AAR_OP_FILE_DELETE, "", 0, 0, 0, &c) && c.result == AAR_E_INVAL);
    CHECK(hx_node_find(1, "secret") == secret && hx_nodes[2].used);
    /* a path with no NUL in the payload */
    memset((void *)(uintptr_t)buf(0x100), 'a', 300);
    CHECK(callf(AAR_OP_FILE_DELETE, 0, 0, 0, 0, 0, 0x100, 300, &c) && c.result == AAR_E_INVAL);
    CHECK(callf(AAR_OP_FILE_DELETE, 0, 0, 0, 0, 0, 0, 0, &c) && c.result == AAR_E_INVAL);
    finish();
}

static void test_file_delete_with_a_volume_path_needs_abs(void)
{
    struct comp c;

    setup("file_delete_abs");
    data_dir();
    cfg.flags |= AAR_SERVEF_ABS_PATHS;
    make_serve();
    CHECK(pcall(AAR_OP_FILE_DELETE, "RAM:data/second", 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(in_data("second") == 0);
    CHECK(pcall(AAR_OP_FILE_DELETE, "RAM:data/second", 0, 0, 0, &c) && c.result == AAR_E_NOENT);
    CHECK(pcall(AAR_OP_FILE_DELETE, "RAM:", 0, 0, 0, &c) && c.result == AAR_E_ACCES);
    CHECK(pcall(AAR_OP_FILE_DELETE, "NOSUCH:x", 0, 0, 0, &c) && c.result == AAR_E_IO);
    finish();
}

static void test_a_write_protected_volume_refuses_every_change_with_acces(void)
{
    struct comp c;
    int n;

    setup("write_protected");
    data_dir();
    make_serve();
    hx_vol_readonly = 1;
    CHECK(pcall(AAR_OP_FILE_DELETE, "second", 0, 0, 0, &c) && c.result == AAR_E_ACCES);
    CHECK(rename_call("second", "third", 0, &c) && c.result == AAR_E_ACCES);
    CHECK(pcall(AAR_OP_DIR_CREATE, "newdir", 0, 0, 0, &c) && c.result == AAR_E_ACCES);
    CHECK(pcall(AAR_OP_FILE_SET_DATE, "second", 0, 1700000000u, 0, &c) && c.result == AAR_E_ACCES);
    CHECK(pcall(AAR_OP_FILE_SET_PROTECT, "second", 5, 0, 0, &c) && c.result == AAR_E_ACCES);
    n = in_data("second");
    CHECK(n != 0 && hx_nodes[n].protect == 0 && in_data("third") == 0 && in_data("newdir") == 0);
    /* the volume says so, too */
    CHECK(pcall(AAR_OP_VOLUME_INFO, "second", 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK((c.out[AAR_R_VOLUME_FLAGS] & AAR_VOLF_READONLY) != 0);
    finish();
}

/* ---- FILE_RENAME ----------------------------------------------------------------------------------- */

static void test_file_rename_and_its_errors(void)
{
    struct comp c;
    int n, sub;

    setup("file_rename");
    data_dir();
    files_extra_nodes();
    make_serve();
    n = in_data("second");
    CHECK(rename_call("second", "third", 0, &c) && c.result == AAR_OK);
    CHECK(in_data("second") == 0 && in_data("third") == n);
    /* the target exists */
    CHECK(rename_call("third", "hello.txt", 0, &c) && c.result == AAR_E_EXIST);
    CHECK(in_data("third") == n && in_data("hello.txt") != 0);
    /* a missing source */
    CHECK(rename_call("nothing", "x", 0, &c) && c.result == AAR_E_NOENT);
    /* the source is held by another program */
    CHECK(rename_call("held", "x", 0, &c) && c.result == AAR_E_BUSY);
    /* and an open file is in use */
    CHECK(pcall(AAR_OP_FILE_OPEN, "third", 3, AAR_FILE_READ, 0, &c) && c.result == AAR_OK);
    CHECK(rename_call("third", "fourth", 0, &c) && c.result == AAR_E_BUSY);
    CHECK(call(AAR_OP_FILE_CLOSE, 91, 3, 0, 0, 0, 0, 0, &c) && c.result == AAR_OK);
    /* into a directory */
    sub = in_data("empty");
    CHECK(rename_call("third", "empty/third", 0, &c) && c.result == AAR_OK);
    CHECK(hx_node_find(sub, "third") == n);
    /* flags that no constant names */
    CHECK(rename_call("hello.txt", "h2", 2, &c) && c.result == AAR_E_INVAL);
    CHECK(rename_call("hello.txt", "h2", 3, &c) && c.result == AAR_E_INVAL);
    CHECK(in_data("hello.txt") != 0 && in_data("h2") == 0);
    /* the payload is two paths, each with its NUL */
    put_bytes(0x100, "hello.txt\0", 10);
    CHECK(callf(AAR_OP_FILE_RENAME, 0, 0, 0, 0, 0, 0x100, 10, &c) && c.result == AAR_E_INVAL);
    CHECK(rename_call("", "x", 0, &c) && c.result == AAR_E_INVAL);
    CHECK(rename_call("hello.txt", "", 0, &c) && c.result == AAR_E_INVAL);
    finish();
}

static void test_file_rename_replace_deletes_the_target_first(void)
{
    struct comp c;
    int full;

    setup("file_rename_replace");
    data_dir();
    files_extra_nodes();
    make_serve();
    CHECK(rename_call("second", "hello.txt", AAR_RENAME_REPLACE, &c) && c.result == AAR_OK);
    CHECK(in_data("second") == 0);
    CHECK(in_data("hello.txt") != 0 && hx_nodes[in_data("hello.txt")].size == 3 &&
          memcmp(hx_nodes[in_data("hello.txt")].data, "two", 3) == 0);
    /* a target that is not there is no error */
    CHECK(rename_call("hello.txt", "fresh", AAR_RENAME_REPLACE, &c) && c.result == AAR_OK);
    CHECK(in_data("fresh") != 0 && in_data("hello.txt") == 0);
    /* the source itself is left alone */
    CHECK(rename_call("fresh", "fresh", AAR_RENAME_REPLACE, &c) && c.result == AAR_OK);
    CHECK(rename_call("fresh", "fresh", 0, &c) && c.result == AAR_OK);
    CHECK(rename_call("fresh", "./fresh", 0, &c) && c.result == AAR_OK);
    CHECK(in_data("fresh") != 0 && hx_nodes[in_data("fresh")].size == 3);
    CHECK(in_data("fresh") != 0 && hx_nodes[in_data("fresh")].size == 3);
    /* a directory with entries cannot be the target */
    full = in_data("full");
    CHECK(rename_call("fresh", "full", AAR_RENAME_REPLACE, &c) && c.result == AAR_E_EXIST);
    CHECK(in_data("fresh") != 0 && in_data("full") == full);
    /* a target held by another program */
    CHECK(rename_call("fresh", "held", AAR_RENAME_REPLACE, &c) && c.result == AAR_E_BUSY);
    CHECK(in_data("fresh") != 0 && in_data("held") != 0);
    finish();
}

static void test_file_rename_stays_under_the_root_and_on_one_volume(void)
{
    struct comp c;

    setup("file_rename_policy");
    data_dir();
    hx_devs[1].name = "WORK";
    hx_devs[1].port[0] = &hx_vol2_port;
    hx_devs[1].node[0] = 1;
    hx_devs[1].count = 1;
    hx_ndevs = 2;
    make_serve();
    CHECK(rename_call("second", "../moved", 0, &c) && c.result == AAR_E_ACCES);
    CHECK(rename_call("../secret", "mine", 0, &c) && c.result == AAR_E_ACCES);
    CHECK(rename_call("second", "RAM:data/x", 0, &c) && c.result == AAR_E_ACCES);
    CHECK(in_data("second") != 0 && hx_node_find(1, "moved") == 0 && in_data("mine") == 0);
    finish();

    setup("file_rename_abs");
    data_dir();
    hx_devs[1].name = "WORK";
    hx_devs[1].port[0] = &hx_vol2_port;
    hx_devs[1].node[0] = 1;
    hx_devs[1].count = 1;
    hx_ndevs = 2;
    cfg.flags |= AAR_SERVEF_ABS_PATHS;
    make_serve();
    CHECK(rename_call("RAM:data/second", "RAM:data/other", 0, &c) && c.result == AAR_OK);
    CHECK(in_data("other") != 0 && in_data("second") == 0);
    /* one name relative, one with a volume */
    CHECK(rename_call("other", "RAM:data/third", 0, &c) && c.result == AAR_OK);
    CHECK(in_data("third") != 0 && in_data("other") == 0);
    /* two volumes: refused, and with REPLACE the target on the other volume is not deleted */
    {
        int third = in_data("third");
        int target = hx_node_add(1, "target", 0, "keep me", 7);

        CHECK(third != 0 && target != 0);
        CHECK(rename_call("RAM:data/third", "WORK:data/fourth", 0, &c) && c.result == AAR_E_INVAL);
        CHECK(in_data("third") == third && in_data("fourth") == 0);
        CHECK(rename_call("RAM:data/third", "WORK:target", AAR_RENAME_REPLACE, &c) &&
              c.result == AAR_E_INVAL);
        CHECK(in_data("third") == third && hx_nodes[third].size == 3 &&
              memcmp(hx_nodes[third].data, "two", 3) == 0);
        CHECK(hx_node_find(1, "target") == target && hx_nodes[target].size == 7 &&
              memcmp(hx_nodes[target].data, "keep me", 7) == 0);
        /* a relative source and a volume target */
        CHECK(rename_call("third", "WORK:target", AAR_RENAME_REPLACE, &c) && c.result == AAR_E_INVAL);
        CHECK(rename_call("WORK:target", "RAM:data/third", AAR_RENAME_REPLACE, &c) &&
              c.result == AAR_E_INVAL);
        CHECK(in_data("third") == third && hx_node_find(1, "target") == target &&
              hx_nodes[target].size == 7);
        /* the same object is still a no-op, on either volume */
        CHECK(rename_call("RAM:data/third", "RAM:data/third", AAR_RENAME_REPLACE, &c) &&
              c.result == AAR_OK);
        CHECK(rename_call("third", "RAM:data/third", 0, &c) && c.result == AAR_OK);
        CHECK(rename_call("WORK:target", "WORK:target", AAR_RENAME_REPLACE, &c) && c.result == AAR_OK);
        CHECK(rename_call("WORK:target", "WORK:target", 0, &c) && c.result == AAR_OK);
        CHECK(in_data("third") == third && hx_node_find(1, "target") == target &&
              hx_nodes[target].size == 7 && memcmp(hx_nodes[target].data, "keep me", 7) == 0);
    }
    {
        int source = in_data("third");
        int second_volume = hx_node_add(1, "vol2", 1, NULL, 0);
        int separate_target;
        int later_dir = hx_node_add(1, "later", 1, NULL, 0);
        int later_target = hx_node_add(later_dir, "target", 0, "later", 5);

        /* The same handler serves another volume. Its port is not proof
         * that deleting the target before sending a rename is safe. */
        hx_nodes[second_volume].volume = 1;
        separate_target = hx_node_add(second_volume, "target", 0, "separate", 8);
        CHECK(rename_call("third", "RAM:vol2/target", AAR_RENAME_REPLACE, &c) &&
              c.result == AAR_E_INVAL);
        CHECK(hx_node_find(second_volume, "target") == separate_target &&
              hx_nodes[separate_target].size == 8 &&
              memcmp(hx_nodes[separate_target].data, "separate", 8) == 0);

        /* The first assign directory does not contain the target. No
         * destructive replacement can use a later directory. */
        hx_devs[2] = (struct hx_dev){"MULTI", {&hx_vol_port, &hx_vol_port}, {2, later_dir}, 2};
        hx_ndevs = 3;
        CHECK(rename_call("third", "MULTI:target", AAR_RENAME_REPLACE, &c) &&
              c.result == AAR_E_INVAL);
        CHECK(hx_node_find(later_dir, "target") == later_target &&
              hx_nodes[later_target].size == 5 &&
              memcmp(hx_nodes[later_target].data, "later", 5) == 0);
        /* No deletion is required when two names identify the same object. */
        CHECK(rename_call("MULTI:target", "RAM:later/target", AAR_RENAME_REPLACE, &c) &&
              c.result == AAR_OK);
        CHECK(in_data("third") == source && hx_nodes[source].size == 3 &&
              memcmp(hx_nodes[source].data, "two", 3) == 0);
    }
    CHECK(rename_call("RAM:data/third", "RAM:data/hello.txt", 0, &c) && c.result == AAR_E_EXIST);
    CHECK(rename_call("RAM:data/third", "RAM:data/hello.txt", AAR_RENAME_REPLACE, &c) &&
          c.result == AAR_OK);
    CHECK(in_data("third") == 0 && in_data("hello.txt") != 0);
    finish();
}

/* ---- DIR_CREATE -------------------------------------------------------------------------------------- */

static void test_dir_create_and_its_errors(void)
{
    struct comp c;

    setup("dir_create");
    data_dir();
    files_extra_nodes();
    make_serve();
    CHECK(pcall(AAR_OP_DIR_CREATE, "made", 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(in_data("made") != 0 && hx_nodes[in_data("made")].is_dir);
    CHECK(pcall(AAR_OP_DIR_CREATE, "made", 0, 0, 0, &c) && c.result == AAR_E_EXIST);
    CHECK(pcall(AAR_OP_DIR_CREATE, "hello.txt", 0, 0, 0, &c) && c.result == AAR_E_EXIST);
    CHECK(pcall(AAR_OP_DIR_CREATE, "held", 0, 0, 0, &c) && c.result == AAR_E_EXIST);
    /* a missing parent */
    CHECK(pcall(AAR_OP_DIR_CREATE, "nope/child", 0, 0, 0, &c) && c.result == AAR_E_NOENT);
    CHECK(pcall(AAR_OP_DIR_CREATE, "made/child", 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(hx_node_find(in_data("made"), "child") != 0);
    CHECK(pcall(AAR_OP_DIR_CREATE, "../outside", 0, 0, 0, &c) && c.result == AAR_E_ACCES);
    CHECK(pcall(AAR_OP_DIR_CREATE, "/outside", 0, 0, 0, &c) && c.result == AAR_E_ACCES);
    CHECK(pcall(AAR_OP_DIR_CREATE, "RAM:outside", 0, 0, 0, &c) && c.result == AAR_E_ACCES);
    CHECK(pcall(AAR_OP_DIR_CREATE, "", 0, 0, 0, &c) && c.result == AAR_E_INVAL);
    CHECK(hx_node_find(1, "outside") == 0);
    finish();
}

/* ---- Dates: FILE_SET_DATE and FILE_STAT ------------------------------------------------------------- */

/* 2023-11-14 22:13:20 UTC. */
#define T_UTC 1700000000u

static void check_set_date_for(const char *name, int west, int locale_present, uint32_t minute)
{
    struct comp c;
    int n;

    setup(name);
    data_dir();
    hx_locale_present = locale_present;
    hx_loc_gmt_west = west;
    make_serve();
    CHECK(pcall(AAR_OP_FILE_SET_DATE, "hello.txt", 0, T_UTC, 0, &c) && c.result == AAR_OK);
    n = in_data("hello.txt");
    /* The DateStamp is the user's local clock: UTC less the minutes west. */
    CHECK(hx_nodes[n].mtime_days == 16753u && hx_nodes[n].mtime_min == minute &&
          hx_nodes[n].mtime_tick == 1000u);
    CHECK(pcall(AAR_OP_FILE_STAT, "hello.txt", 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_STAT_MTIME_HI] == 0 && c.out[AAR_R_STAT_MTIME_LO] == T_UTC);
    finish();
}

static void test_set_date_and_stat_are_unix_seconds_utc(void)
{
    /* local 23:13:20 east of UTC, 22:13:20 with no locale, 17:13:20 west of it */
    check_set_date_for("set_date_east", -60, 1, 1393u);
    check_set_date_for("set_date_utc", 0, 1, 1333u);
    check_set_date_for("set_date_west", 300, 1, 1033u);
    check_set_date_for("set_date_no_locale", 0, 0, 1333u);
}

static void test_stat_of_a_file_uses_the_offset_too(void)
{
    struct comp c;

    setup("stat_offset");
    data_dir();
    make_serve();
    /* The fixture's files are 9000 days after 1978-01-01, local. */
    CHECK(pcall(AAR_OP_FILE_STAT, "hello.txt", 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_STAT_MTIME_LO] == 252460800u + 9000u * 86400u);
    finish();

    setup("stat_offset_east");
    data_dir();
    hx_loc_gmt_west = -60;
    make_serve();
    CHECK(pcall(AAR_OP_FILE_STAT, "hello.txt", 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_STAT_MTIME_LO] == 252460800u + 9000u * 86400u - 3600u);
    finish();

    /* An unset stamp (no date at all) stays 0, whatever the offset. */
    setup("stat_unset");
    data_dir();
    hx_loc_gmt_west = -60;
    make_serve();
    hx_nodes[in_data("hello.txt")].mtime_days = 0;
    CHECK(pcall(AAR_OP_FILE_STAT, "hello.txt", 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_STAT_MTIME_HI] == 0 && c.out[AAR_R_STAT_MTIME_LO] == 0);
    finish();
}

static void test_set_date_errors(void)
{
    struct comp c;
    int n;

    setup("set_date_errors");
    data_dir();
    make_serve();
    n = in_data("hello.txt");
    CHECK(pcall(AAR_OP_FILE_SET_DATE, "missing", 0, T_UTC, 0, &c) && c.result == AAR_E_NOENT);
    /* before 1978, and after what a DateStamp counts here (2106) */
    CHECK(pcall(AAR_OP_FILE_SET_DATE, "hello.txt", 0, 100, 0, &c) && c.result == AAR_E_INVAL);
    CHECK(pcall(AAR_OP_FILE_SET_DATE, "hello.txt", 0xffffffffu, 0xfffffff0u, 0, &c) &&
          c.result == AAR_E_INVAL);
    CHECK(pcall(AAR_OP_FILE_SET_DATE, "hello.txt", 2, 0, 0, &c) && c.result == AAR_E_INVAL);
    CHECK(hx_nodes[n].mtime_days == 9000u);
    CHECK(pcall(AAR_OP_FILE_SET_DATE, "../secret", 0, T_UTC, 0, &c) && c.result == AAR_E_ACCES);
    CHECK(pcall(AAR_OP_FILE_SET_DATE, "RAM:data/hello.txt", 0, T_UTC, 0, &c) &&
          c.result == AAR_E_ACCES);
    CHECK(pcall(AAR_OP_FILE_SET_DATE, "", 0, T_UTC, 0, &c) && c.result == AAR_E_INVAL);
    CHECK(hx_nodes[n].mtime_days == 9000u);
    /* the root of the app has no leaf to give a date to */
    CHECK(pcall(AAR_OP_FILE_SET_DATE, ".", 0, T_UTC, 0, &c) && c.result == AAR_E_ACCES);
    finish();
}

/* ---- FILE_SET_PROTECT and the bits of FILE_STAT ------------------------------------------------------ */

static void test_protection_bits_go_both_ways(void)
{
    struct comp c;
    int n;

    setup("protect");
    data_dir();
    make_serve();
    n = in_data("hello.txt");
    CHECK(pcall(AAR_OP_FILE_STAT, "hello.txt", 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_STAT_PROTECT] == 0);
    CHECK(pcall(AAR_OP_FILE_SET_PROTECT, "hello.txt", AAR_PROT_WRITE | AAR_PROT_DELETE | AAR_PROT_SCRIPT,
                0, 0, &c) && c.result == AAR_OK);
    CHECK(hx_nodes[n].protect == (AAR_PROT_WRITE | AAR_PROT_DELETE | AAR_PROT_SCRIPT));
    CHECK(pcall(AAR_OP_FILE_STAT, "hello.txt", 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_STAT_PROTECT] == (AAR_PROT_WRITE | AAR_PROT_DELETE | AAR_PROT_SCRIPT));
    /* all eight bits are valid; the others are not */
    CHECK(pcall(AAR_OP_FILE_SET_PROTECT, "hello.txt", 0xffu, 0, 0, &c) && c.result == AAR_OK);
    CHECK(pcall(AAR_OP_FILE_SET_PROTECT, "hello.txt", 0x100u, 0, 0, &c) && c.result == AAR_E_INVAL);
    CHECK(pcall(AAR_OP_FILE_SET_PROTECT, "hello.txt", 0x8000u, 0, 0, &c) && c.result == AAR_E_INVAL);
    CHECK(hx_nodes[n].protect == 0xffu);
    /* a stat shows no more than the eight bits (the group and other bits of the file system are not) */
    hx_nodes[n].protect = 0x1234u;
    CHECK(pcall(AAR_OP_FILE_STAT, "hello.txt", 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_STAT_PROTECT] == 0x34u);
    CHECK(pcall(AAR_OP_FILE_SET_PROTECT, "nothing", 0, 0, 0, &c) && c.result == AAR_E_NOENT);
    CHECK(pcall(AAR_OP_FILE_SET_PROTECT, "../secret", 0, 0, 0, &c) && c.result == AAR_E_ACCES);
    CHECK(pcall(AAR_OP_FILE_SET_PROTECT, "RAM:data/hello.txt", 0, 0, 0, &c) &&
          c.result == AAR_E_ACCES);
    CHECK(pcall(AAR_OP_FILE_SET_PROTECT, "", 0, 0, 0, &c) && c.result == AAR_E_INVAL);
    /* a delete-protected file is protected for real */
    CHECK(pcall(AAR_OP_FILE_SET_PROTECT, "hello.txt", AAR_PROT_DELETE, 0, 0, &c) &&
          c.result == AAR_OK);
    CHECK(pcall(AAR_OP_FILE_DELETE, "hello.txt", 0, 0, 0, &c) && c.result == AAR_E_ACCES);
    finish();
}

/* ---- FILE_TRUNCATE ------------------------------------------------------------------------------- */

static void test_truncate_shortens_and_extends_with_zeros(void)
{
    struct comp c;
    int n, i;

    setup("truncate");
    data_dir();
    make_serve();
    n = in_data("second");
    CHECK(pcall(AAR_OP_FILE_OPEN, "second", 3, AAR_FILE_WRITE, 0, &c) && c.result == AAR_OK);
    /* the bytes past the end of the file are not zero in the fake volume */
    for (i = 3; i < 12; i++)
        hx_nodes[n].data[i] = 0xAA;
    CHECK(call(AAR_OP_FILE_SEEK, 80, 3, 0, 2, 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(call(AAR_OP_FILE_TRUNCATE, 81, 3, 0, 10, 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(hx_nodes[n].size == 10 && memcmp(hx_nodes[n].data, "two", 3) == 0);
    for (i = 3; i < 10; i++)
        CHECK(hx_nodes[n].data[i] == 0);
    /* the position stays where it was */
    CHECK(call(AAR_OP_FILE_SEEK, 82, 3, 0, 0, 1, 0, 0, &c) && c.result == AAR_OK &&
          c.out[AAR_R_FILE_POS_LO] == 2);
    CHECK(call(AAR_OP_FILE_TRUNCATE, 83, 3, 0, 1, 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(hx_nodes[n].size == 1);
    /* the same size changes nothing */
    CHECK(call(AAR_OP_FILE_TRUNCATE, 84, 3, 0, 1, 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(hx_nodes[n].size == 1);
    CHECK(call(AAR_OP_FILE_TRUNCATE, 85, 3, 0, 0, 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(hx_nodes[n].size == 0);
    /* more than 2 GiB is more than a file here */
    CHECK(call(AAR_OP_FILE_TRUNCATE, 86, 3, 0, 0x80000000u, 0, 0, 0, &c) && c.result == AAR_E_INVAL);
    CHECK(call(AAR_OP_FILE_TRUNCATE, 87, 3, 1, 0, 0, 0, 0, &c) && c.result == AAR_E_INVAL);
    CHECK(call(AAR_OP_FILE_CLOSE, 88, 3, 0, 0, 0, 0, 0, &c) && c.result == AAR_OK);
    finish();
}

static void test_truncate_needs_an_id_opened_for_writing(void)
{
    struct comp c;

    setup("truncate_badf");
    data_dir();
    make_serve();
    CHECK(pcall(AAR_OP_FILE_OPEN, "hello.txt", 3, AAR_FILE_READ, 0, &c) && c.result == AAR_OK);
    CHECK(call(AAR_OP_FILE_TRUNCATE, 90, 3, 0, 1, 0, 0, 0, &c) && c.result == AAR_E_BADF);
    CHECK(hx_nodes[in_data("hello.txt")].size == 13);
    CHECK(call(AAR_OP_FILE_TRUNCATE, 91, 4, 0, 1, 0, 0, 0, &c) && c.result == AAR_E_BADF);
    CHECK(call(AAR_OP_FILE_TRUNCATE, 92, 1, 0, 1, 0, 0, 0, &c) && c.result == AAR_E_BADF);
    CHECK(call(AAR_OP_FILE_TRUNCATE, 93, 99, 0, 1, 0, 0, 0, &c) && c.result == AAR_E_BADF);
    CHECK(call(AAR_OP_FILE_CLOSE, 94, 3, 0, 0, 0, 0, 0, &c) && c.result == AAR_OK);
    /* the id is free again, and may come back with another direction */
    CHECK(pcall(AAR_OP_FILE_OPEN, "second", 3, AAR_FILE_WRITE, 0, &c) && c.result == AAR_OK);
    CHECK(call(AAR_OP_FILE_TRUNCATE, 95, 3, 0, 1, 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(call(AAR_OP_FILE_CLOSE, 96, 3, 0, 0, 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(pcall(AAR_OP_FILE_OPEN, "second", 3, AAR_FILE_READ, 0, &c) && c.result == AAR_OK);
    CHECK(call(AAR_OP_FILE_TRUNCATE, 97, 3, 0, 0, 0, 0, 0, &c) && c.result == AAR_E_BADF);
    CHECK(call(AAR_OP_FILE_CLOSE, 98, 3, 0, 0, 0, 0, 0, &c) && c.result == AAR_OK);
    finish();
}

/* Another handle's position keeps SetFileSize from cutting as short as asked: the handler answers
 * the size it kept, and that is not a truncation. */
static void test_truncate_below_another_handles_position_is_busy(void)
{
    struct comp c;
    int n;

    setup("truncate_held");
    data_dir();
    make_serve();
    n = in_data("hello.txt");
    CHECK(pcall(AAR_OP_FILE_OPEN, "hello.txt", 3, AAR_FILE_WRITE, 0, &c) && c.result == AAR_OK);
    CHECK(pcall(AAR_OP_FILE_OPEN, "hello.txt", 4, AAR_FILE_READ, 0, &c) && c.result == AAR_OK);
    CHECK(call(AAR_OP_FILE_SEEK, 100, 4, 0, 8, 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(call(AAR_OP_FILE_TRUNCATE, 101, 3, 0, 2, 0, 0, 0, &c) && c.result == AAR_E_BUSY);
    CHECK(hx_nodes[n].size == 8);
    CHECK(call(AAR_OP_FILE_SEEK, 102, 4, 0, 0, 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(call(AAR_OP_FILE_TRUNCATE, 103, 3, 0, 2, 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(hx_nodes[n].size == 2);
    CHECK(call(AAR_OP_FILE_CLOSE, 104, 3, 0, 0, 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(call(AAR_OP_FILE_CLOSE, 105, 4, 0, 0, 0, 0, 0, &c) && c.result == AAR_OK);
    finish();
}

static void test_a_full_volume_stops_an_extension(void)
{
    struct comp c;

    setup("truncate_full");
    data_dir();
    make_serve();
    CHECK(pcall(AAR_OP_FILE_OPEN, "second", 3, AAR_FILE_WRITE, 0, &c) && c.result == AAR_OK);
    hx_vol_full = 1;
    CHECK(call(AAR_OP_FILE_TRUNCATE, 90, 3, 0, 20000, 0, 0, 0, &c) && c.result == AAR_E_NOSPC);
    hx_vol_full = 0;
    CHECK(call(AAR_OP_FILE_CLOSE, 91, 3, 0, 0, 0, 0, 0, &c) && c.result == AAR_OK);
    finish();
}

/* ---- VOLUME_INFO ------------------------------------------------------------------------------------- */

static void test_volume_info_reports_blocks_and_protection(void)
{
    struct comp c;

    setup("volume_info");
    data_dir();
    cfg.flags |= AAR_SERVEF_ABS_PATHS;
    make_serve();
    CHECK(pcall(AAR_OP_VOLUME_INFO, "hello.txt", 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_VOLUME_BLOCKS_TOTAL] == 1000 && c.out[AAR_R_VOLUME_BLOCKS_USED] == 250 &&
          c.out[AAR_R_VOLUME_BLOCK_BYTES] == 512 && c.out[AAR_R_VOLUME_FLAGS] == 0);
    CHECK(pcall(AAR_OP_VOLUME_INFO, "RAM:data", 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_VOLUME_BLOCKS_TOTAL] == 1000);
    hx_vol_blocks = 5000;
    hx_vol_blocks_used = 4999;
    CHECK(pcall(AAR_OP_VOLUME_INFO, ".", 0, 0, 0, &c) && c.result == AAR_OK &&
          c.out[AAR_R_VOLUME_BLOCKS_TOTAL] == 5000);
    hx_vol_readonly = 1;
    CHECK(pcall(AAR_OP_VOLUME_INFO, "second", 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_VOLUME_BLOCKS_TOTAL] == 5000 && c.out[AAR_R_VOLUME_BLOCKS_USED] == 4999 &&
          c.out[AAR_R_VOLUME_FLAGS] == AAR_VOLF_READONLY);
    CHECK(pcall(AAR_OP_VOLUME_INFO, "missing", 0, 0, 0, &c) && c.result == AAR_E_NOENT);
    CHECK(pcall(AAR_OP_VOLUME_INFO, "", 0, 0, 0, &c) && c.result == AAR_E_INVAL);
    CHECK(pcall(AAR_OP_VOLUME_INFO, "../secret", 0, 0, 0, &c) && c.result == AAR_E_ACCES);
    finish();

    setup("volume_info_policy");
    data_dir();
    make_serve();
    CHECK(pcall(AAR_OP_VOLUME_INFO, "RAM:data", 0, 0, 0, &c) && c.result == AAR_E_ACCES);
    finish();
}

/* ---- A stop cancels what changes the volume ---------------------------------------------------------- */

static void test_a_stop_cancels_the_new_file_requests_and_not_the_reads(void)
{
    struct comp c;
    int n, sub;

    setup("stop_new_files");
    data_dir();
    make_serve();
    CHECK(pcall(AAR_OP_FILE_OPEN, "second", 3, AAR_FILE_WRITE, 0, &c) && c.result == AAR_OK);
    n = in_data("second");
    aar_serve_stop(S);
    CHECK(pcall(AAR_OP_FILE_DELETE, "hello.txt", 0, 0, 0, &c) && c.result == AAR_E_IO);
    CHECK(rename_call("hello.txt", "x", 0, &c) && c.result == AAR_E_IO);
    CHECK(rename_call("hello.txt", "x", AAR_RENAME_REPLACE, &c) && c.result == AAR_E_IO);
    CHECK(pcall(AAR_OP_DIR_CREATE, "made", 0, 0, 0, &c) && c.result == AAR_E_IO);
    CHECK(pcall(AAR_OP_FILE_SET_DATE, "hello.txt", 0, T_UTC, 0, &c) && c.result == AAR_E_IO);
    CHECK(pcall(AAR_OP_FILE_SET_PROTECT, "hello.txt", 5, 0, 0, &c) && c.result == AAR_E_IO);
    CHECK(call(AAR_OP_FILE_TRUNCATE, 70, 3, 0, 1, 0, 0, 0, &c) && c.result == AAR_E_IO);
    CHECK(call(AAR_OP_FILE_TRUNCATE, 71, 3, 0, 20, 0, 0, 0, &c) && c.result == AAR_E_IO);
    sub = in_data("hello.txt");
    CHECK(sub != 0 && hx_nodes[sub].mtime_days == 9000u && hx_nodes[sub].protect == 0);
    CHECK(in_data("x") == 0 && in_data("made") == 0 && hx_nodes[n].size == 3);
    /* what only reads still runs */
    CHECK(pcall(AAR_OP_FILE_STAT, "hello.txt", 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(pcall(AAR_OP_VOLUME_INFO, "hello.txt", 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(call(AAR_OP_FILE_CLOSE, 72, 3, 0, 0, 0, 0, 0, &c) && c.result == AAR_OK);
    finish();
}

static void test_the_platform_ending_the_app_cancels_the_new_file_requests(void)
{
    struct comp c;

    setup("stop_new_files_platform");
    data_dir();
    make_serve();
    app_state(H, AAR_STATE_STOPPING, 0);
    CHECK(pcall(AAR_OP_FILE_DELETE, "hello.txt", 0, 0, 0, &c) && c.result == AAR_E_IO);
    CHECK(pcall(AAR_OP_DIR_CREATE, "made", 0, 0, 0, &c) && c.result == AAR_E_IO);
    CHECK(in_data("hello.txt") != 0 && in_data("made") == 0);
    finish();
}

/* ---- TIME_UNIX --------------------------------------------------------------------------------------- */

/* The clock shows 23:13:20.5 on 2023-11-14 and the user is one hour east of UTC. */
static void set_clock(void)
{
    hx_clock.ds_Days = 16753;
    hx_clock.ds_Minute = 1393;
    hx_clock.ds_Tick = 1025;
}

static void test_time_unix_is_utc_microseconds(void)
{
    struct comp c;

    setup("time_unix");
    set_clock();
    hx_loc_gmt_west = -60;
    make_serve();
    CHECK(hx_host_lib_opens == 0); /* the libraries are opened by their first use */
    CHECK(callf(AAR_OP_TIME_UNIX, 0, 0, 0, 0, 0, 0, 0, &c) && c.result == AAR_OK);
    {
        uint64_t us = ((uint64_t)c.out[AAR_R_TIME_UNIX_US_HI] << 32) | c.out[AAR_R_TIME_UNIX_US_LO];

        CHECK(us == 1700000000ull * 1000000ull + 500000ull);
    }
    CHECK(c.out[AAR_R_TIME_UNIX_FLAGS] == AAR_LOCALEF_OFFSET_KNOWN);
    CHECK(hx_host_lib_opens == 1 && hx_host_libs_open == 1 && hx_locale_open_now == 0);
    /* the library stays open for the next call: one OpenLibrary in all */
    CHECK(callf(AAR_OP_TIME_UNIX, 0, 0, 0, 0, 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(hx_host_lib_opens == 1);
    finish();

    /* west of UTC */
    setup("time_unix_west");
    set_clock();
    hx_clock.ds_Minute = 1393 - 600; /* 13:13:20 local, UTC-10 (Hawaii): 23:13:20 UTC */
    hx_loc_gmt_west = 600;
    make_serve();
    CHECK(callf(AAR_OP_TIME_UNIX, 0, 0, 0, 0, 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_TIME_UNIX_US_HI] == (uint32_t)((1700003600ull * 1000000ull + 500000ull) >> 32) &&
          c.out[AAR_R_TIME_UNIX_US_LO] == (uint32_t)(1700003600ull * 1000000ull + 500000ull));
    finish();

    /* no locale.library: the clock as it is, and the flag says the offset is not known */
    setup("time_unix_no_locale");
    set_clock();
    hx_locale_present = 0;
    make_serve();
    CHECK(callf(AAR_OP_TIME_UNIX, 0, 0, 0, 0, 0, 0, 0, &c) && c.result == AAR_OK);
    {
        uint64_t us = ((uint64_t)c.out[AAR_R_TIME_UNIX_US_HI] << 32) | c.out[AAR_R_TIME_UNIX_US_LO];

        CHECK(us == 1700003600ull * 1000000ull + 500000ull);
    }
    CHECK(c.out[AAR_R_TIME_UNIX_FLAGS] == 0);
    CHECK(hx_host_lib_opens == 0);
    /* it does not try again for every call */
    CHECK(callf(AAR_OP_TIME_UNIX, 0, 0, 0, 0, 0, 0, 0, &c) && c.result == AAR_OK);
    finish();
}

/* ---- LOCALE_INFO ------------------------------------------------------------------------------------- */

static const char *rec_str(uint32_t off)
{
    return (const char *)(uintptr_t)buf(0x400 + off);
}

static int rec_zero_from(uint32_t from, uint32_t to)
{
    uint32_t i;

    for (i = from; i < to; i++)
        if (buf(0x400)[i] != 0)
            return 0;
    return 1;
}

static void test_locale_info_fills_the_record(void)
{
    struct comp c;

    setup("locale_info");
    hx_loc_gmt_west = -60;
    make_serve();
    memset((void *)(uintptr_t)buf(0x400), 0x5a, 64);
    CHECK(callf(AAR_OP_LOCALE_INFO, 0, 0, 0, 0, 0, 0x400, AAR_LOCALE_REC_BYTES, &c) && c.result == AAR_OK);
    CHECK(aar_get_be32((const uint8_t *)(uintptr_t)buf(0x400 + AAR_LOCALE_REC_UTC_OFFSET_MIN)) == 60u);
    CHECK(aar_get_be32((const uint8_t *)(uintptr_t)buf(0x400 + AAR_LOCALE_REC_FLAGS)) ==
          AAR_LOCALEF_OFFSET_KNOWN);
    CHECK(strcmp(rec_str(AAR_LOCALE_REC_LANGUAGE), "english") == 0);
    CHECK(strcmp(rec_str(AAR_LOCALE_REC_COUNTRY), "GB") == 0);
    CHECK(strcmp(rec_str(AAR_LOCALE_REC_DECIMAL_POINT), ".") == 0);
    CHECK(strcmp(rec_str(AAR_LOCALE_REC_GROUP_SEPARATOR), ",") == 0);
    CHECK(rec_zero_from(0x38, 0x40));
    /* the fields are zero padded, not left as they were */
    CHECK(rec_zero_from(AAR_LOCALE_REC_LANGUAGE + 8, AAR_LOCALE_REC_COUNTRY));
    CHECK(rec_zero_from(AAR_LOCALE_REC_COUNTRY + 3, AAR_LOCALE_REC_DECIMAL_POINT));
    /* a room smaller than the record is not enough */
    CHECK(callf(AAR_OP_LOCALE_INFO, 0, 0, 0, 0, 0, 0x400, 63, &c) && c.result == AAR_E_INVAL);
    finish();

    /* German, with a comma and a Latin-1 group separator (a no-break space) */
    setup("locale_info_german");
    strcpy(hx_loc_language, "deutsch.language");
    hx_loc_country = 'D';
    hx_loc_decimal[0] = ',';
    hx_loc_decimal[1] = 0;
    hx_loc_group[0] = (char)0xA0;
    hx_loc_group[1] = 0;
    hx_loc_gmt_west = 0;
    make_serve();
    CHECK(callf(AAR_OP_LOCALE_INFO, 0, 0, 0, 0, 0, 0x400, AAR_LOCALE_REC_BYTES, &c) && c.result == AAR_OK);
    CHECK(strcmp(rec_str(AAR_LOCALE_REC_LANGUAGE), "deutsch") == 0);
    CHECK(strcmp(rec_str(AAR_LOCALE_REC_COUNTRY), "D") == 0);
    CHECK(strcmp(rec_str(AAR_LOCALE_REC_DECIMAL_POINT), ",") == 0);
    CHECK(memcmp(rec_str(AAR_LOCALE_REC_GROUP_SEPARATOR), "\xC2\xA0", 3) == 0);
    CHECK(aar_get_be32((const uint8_t *)(uintptr_t)buf(0x400 + AAR_LOCALE_REC_UTC_OFFSET_MIN)) == 0u);
    CHECK(aar_get_be32((const uint8_t *)(uintptr_t)buf(0x400 + AAR_LOCALE_REC_FLAGS)) ==
          AAR_LOCALEF_OFFSET_KNOWN);
    finish();

    /* a locale that is long: three bytes of UTF-8 at most, only whole characters, a 31 character name */
    setup("locale_info_long");
    strcpy(hx_loc_language, "abcdefghijklmnopqrstuvwxyzabcdefghij.language");
    hx_loc_country = 0x41424344UL;
    memcpy(hx_loc_decimal, "\xB7\xB7\xB7", 4);
    hx_loc_gmt_west = 90;
    make_serve();
    CHECK(callf(AAR_OP_LOCALE_INFO, 0, 0, 0, 0, 0, 0x400, AAR_LOCALE_REC_BYTES, &c) && c.result == AAR_OK);
    CHECK(strlen(rec_str(AAR_LOCALE_REC_LANGUAGE)) == 31);
    CHECK(strcmp(rec_str(AAR_LOCALE_REC_COUNTRY), "ABCD") == 0);
    CHECK(strcmp(rec_str(AAR_LOCALE_REC_DECIMAL_POINT), "\xC2\xB7") == 0);
    CHECK(aar_get_be32((const uint8_t *)(uintptr_t)buf(0x400 + AAR_LOCALE_REC_UTC_OFFSET_MIN)) ==
          (uint32_t)-90);
    finish();
}

static void test_locale_info_without_locale_library_is_zeros_and_ok(void)
{
    struct comp c;

    setup("locale_info_none");
    hx_locale_present = 0;
    make_serve();
    memset((void *)(uintptr_t)buf(0x400), 0x5a, 64);
    CHECK(callf(AAR_OP_LOCALE_INFO, 0, 0, 0, 0, 0, 0x400, AAR_LOCALE_REC_BYTES, &c) && c.result == AAR_OK);
    CHECK(rec_zero_from(0, 64));
    finish();
}

/* ---- ENV_GET ----------------------------------------------------------------------------------------- */

/* The name goes in at 0x400 and the value comes back there; room is the whole payload. */
static int env_call(const char *name, uint32_t room, struct comp *c)
{
    memset((void *)(uintptr_t)buf(0x400), 0x5a, room > 300 ? room : 300);
    memcpy((void *)(uintptr_t)buf(0x400), name, strlen(name) + 1u);
    return callf(AAR_OP_ENV_GET, 0, 0, 0, 0, 0, 0x400, room, c);
}

static void test_env_get_returns_the_stored_bytes_and_the_full_length(void)
{
    struct comp c;
    static const uint8_t binary[7] = { 'a', 0, 'b', 0xff, 0, 0, 'z' };
    uint8_t big[5000];
    unsigned i;

    setup("env_get");
    hx_env_set("wb", "3.2", 3);
    hx_env_set("bin", binary, 7);
    hx_env_set("empty", "", 0);
    for (i = 0; i < sizeof(big); i++)
        big[i] = (uint8_t)('a' + i % 26u);
    hx_env_set("big", big, sizeof(big));
    make_serve();
    /* it fits: the value and a NUL, and the length without the NUL */
    CHECK(env_call("wb", 64, &c) && c.result == AAR_OK && c.out[AAR_R_ENV_LEN] == 3);
    CHECK(memcmp((const void *)(uintptr_t)buf(0x400), "3.2\0", 4) == 0);
    /* the bytes as they are stored */
    CHECK(env_call("bin", 64, &c) && c.result == AAR_OK && c.out[AAR_R_ENV_LEN] == 7);
    CHECK(memcmp((const void *)(uintptr_t)buf(0x400), binary, 7) == 0 && buf(0x400)[7] == 0);
    CHECK(env_call("empty", 64, &c) && c.result == AAR_OK && c.out[AAR_R_ENV_LEN] == 0);
    CHECK(buf(0x400)[0] == 0);
    /* a room as long as the value: no NUL after it, and the length is still the value's */
    CHECK(env_call("wb", 3, &c) && c.result == AAR_OK && c.out[AAR_R_ENV_LEN] == 3);
    CHECK(memcmp((const void *)(uintptr_t)buf(0x400), "3.2", 3) == 0 && buf(0x400)[3] == 0x5a);
    CHECK(env_call("bin", 7, &c) && c.result == AAR_OK && c.out[AAR_R_ENV_LEN] == 7);
    CHECK(memcmp((const void *)(uintptr_t)buf(0x400), binary, 7) == 0 && buf(0x400)[7] == 0x5a);
    /* a room too small for the value (it holds the name, so at least that long): the first bytes,
     * no NUL, and the full length */
    CHECK(env_call("bin", 4, &c) && c.result == AAR_OK && c.out[AAR_R_ENV_LEN] == 7);
    CHECK(memcmp((const void *)(uintptr_t)buf(0x400), binary, 4) == 0 && buf(0x400)[4] == 0x5a);
    /* the first call reports the full length: nothing grows for bytes the request cannot take */
    hx_getvar_calls = 0;
    CHECK(env_call("big", 5, &c) && c.result == AAR_OK && c.out[AAR_R_ENV_LEN] == 5000);
    CHECK(memcmp((const void *)(uintptr_t)buf(0x400), big, 5) == 0);
    CHECK(hx_getvar_calls == 1 && hx_getvar_max_size == 1024);
    /* a value the request can hold that did not fit the first buffer: GetVar cut it and put its
     * full length in IoErr, so one more call has a buffer of that size */
    hx_getvar_calls = 0;
    CHECK(env_call("big", 6000, &c) && c.result == AAR_OK && c.out[AAR_R_ENV_LEN] == 5000);
    CHECK(memcmp((const void *)(uintptr_t)buf(0x400), big, 5000) == 0 && buf(0x400)[5000] == 0);
    CHECK(hx_getvar_calls == 2 && hx_getvar_max_size == 5001);
    /* an unset variable */
    CHECK(env_call("nothing", 64, &c) && c.result == AAR_E_NOENT);
    finish();
}

/* GetVar's result as documented: the bytes copied (cut to the buffer less the NUL) and, in
 * IoErr(), the full size of the variable. */
static void test_env_get_uses_the_full_length_getvar_reports(void)
{
    struct comp c;
    static uint8_t huge[2u << 20];
    uint8_t mid[1500];
    unsigned i;

    for (i = 0; i < sizeof(mid); i++)
        mid[i] = (uint8_t)('a' + i % 26u);
    /* a value far longer than the request can take: the first call counts it, and no buffer
     * grows to it */
    setup("env_get_count_only");
    for (i = 0; i < sizeof(huge); i++)
        huge[i] = (uint8_t)('a' + i % 26u);
    hx_env_set("huge", huge, sizeof(huge));
    make_serve();
    CHECK(env_call("huge", 64, &c) && c.result == AAR_OK && c.out[AAR_R_ENV_LEN] == sizeof(huge));
    CHECK(memcmp((const void *)(uintptr_t)buf(0x400), huge, 64) == 0);
    CHECK(hx_getvar_calls == 1 && hx_getvar_max_size == 1024);
    finish();

    /* the first buffer holds 1023 bytes and the NUL: a value of 1023 is whole, one of 1024 is not */
    setup("env_get_edges");
    hx_env_set("fit", mid, 1023);
    hx_env_set("edge", mid, 1024);
    make_serve();
    CHECK(env_call("fit", 2000, &c) && c.result == AAR_OK && c.out[AAR_R_ENV_LEN] == 1023);
    CHECK(memcmp((const void *)(uintptr_t)buf(0x400), mid, 1023) == 0 && buf(0x400)[1023] == 0);
    CHECK(hx_getvar_calls == 1);
    hx_getvar_calls = 0;
    CHECK(env_call("edge", 2000, &c) && c.result == AAR_OK && c.out[AAR_R_ENV_LEN] == 1024);
    CHECK(memcmp((const void *)(uintptr_t)buf(0x400), mid, 1024) == 0 && buf(0x400)[1024] == 0);
    CHECK(hx_getvar_calls == 2 && hx_getvar_max_size == 1025);
    finish();

    /* the variable grows between the calls: it is fetched again, with the new length */
    setup("env_get_grows");
    hx_env_set("mid", mid, 1500);
    hx_getvar_grow_by = 100;
    hx_getvar_grow_times = 1;
    make_serve();
    CHECK(env_call("mid", 6000, &c) && c.result == AAR_OK && c.out[AAR_R_ENV_LEN] == 1600);
    CHECK(memcmp((const void *)(uintptr_t)buf(0x400), mid, 1500) == 0);
    CHECK(buf(0x400)[1500] == 'g' && buf(0x400)[1599] == 'g' && buf(0x400)[1600] == 0);
    CHECK(hx_getvar_calls == 3);
    finish();

    /* one that never settles is given up after a few calls */
    setup("env_get_never_settles");
    hx_env_set("mid", mid, 1500);
    hx_getvar_grow_by = 100;
    hx_getvar_grow_times = 100;
    make_serve();
    CHECK(env_call("mid", 6000, &c) && c.result == AAR_E_IO);
    CHECK(hx_getvar_calls == 4);
    finish();

    /* a dos.library that does not report the full size cannot be told from a cut value */
    setup("env_get_no_size");
    hx_env_set("mid", mid, 1500);
    hx_getvar_no_ioerr = 1;
    make_serve();
    CHECK(env_call("mid", 6000, &c) && c.result == AAR_E_IO);
    CHECK(hx_getvar_calls == 1);
    finish();
}

static void test_env_get_checks_the_name(void)
{
    struct comp c;
    char name[300];
    unsigned calls;

    setup("env_get_names");
    hx_env_set("x", "1", 1);
    make_serve();
    CHECK(env_call("", 64, &c) && c.result == AAR_E_INVAL);
    /* 255 bytes is the longest name; 256 is too long */
    memset(name, 'n', 255);
    name[255] = 0;
    CHECK(env_call(name, 400, &c) && c.result == AAR_E_NOENT);
    memset(name, 'n', 256);
    name[256] = 0;
    CHECK(env_call(name, 400, &c) && c.result == AAR_E_INVAL);
    /* no NUL inside the buffer */
    memset((void *)(uintptr_t)buf(0x400), 'x', 8);
    CHECK(callf(AAR_OP_ENV_GET, 0, 0, 0, 0, 0, 0x400, 8, &c) && c.result == AAR_E_INVAL);
    /* a name that leaves ENV: a volume, a leading slash, an empty component (in AmigaDOS each
     * slash after the first of a run is a parent) or a parent: none of them reaches GetVar */
    calls = hx_getvar_calls;
    CHECK(env_call("../x", 64, &c) && c.result == AAR_E_ACCES);
    CHECK(env_call("a/../x", 64, &c) && c.result == AAR_E_ACCES);
    CHECK(env_call("/x", 64, &c) && c.result == AAR_E_ACCES);
    CHECK(env_call("SYS:x", 64, &c) && c.result == AAR_E_ACCES);
    CHECK(env_call("dir//x", 64, &c) && c.result == AAR_E_ACCES);
    CHECK(env_call("prefs///x", 64, &c) && c.result == AAR_E_ACCES);
    CHECK(env_call("//x", 64, &c) && c.result == AAR_E_ACCES);
    CHECK(env_call("x/", 64, &c) && c.result == AAR_E_ACCES);
    CHECK(env_call("dir/", 64, &c) && c.result == AAR_E_ACCES);
    CHECK(hx_getvar_calls == calls);
    CHECK(env_call("dir/x", 64, &c) && c.result == AAR_E_NOENT);
    CHECK(env_call("x", 64, &c) && c.result == AAR_OK);
    finish();
}

static void test_a_config_flag_refuses_the_environment(void)
{
    struct comp c;

    setup("env_policy");
    hx_env_set("x", "1", 1);
    cfg.flags |= AAR_SERVEF_NO_ENV;
    make_serve();
    CHECK(env_call("x", 64, &c) && c.result == AAR_E_ACCES);
    CHECK(env_call("", 64, &c) && c.result == AAR_E_ACCES);
    CHECK(hx_getvar_calls == 0);
    /* time and locale are not the environment */
    CHECK(callf(AAR_OP_TIME_UNIX, 0, 0, 0, 0, 0, 0, 0, &c) && c.result == AAR_OK);
    finish();
}

/* ---- The clipboard ------------------------------------------------------------------------------------ */

static void be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* FORM FTXT with an optional CSET chunk (cset < 0: none) and the CHRS chunks given. Returns the size. */
static uint32_t make_ftxt(uint8_t *out, int cset, const void *a, uint32_t na, const void *b, uint32_t nb)
{
    uint32_t pos = 12;

    memcpy(out, "FORM", 4);
    memcpy(out + 8, "FTXT", 4);
    if (cset >= 0) {
        memcpy(out + pos, "CSET", 4);
        be(out + pos + 4, 32);
        memset(out + pos + 8, 0, 32);
        be(out + pos + 8, (uint32_t)cset);
        pos += 8 + 32;
    }
    memcpy(out + pos, "CHRS", 4);
    be(out + pos + 4, na);
    memcpy(out + pos + 8, a, na);
    pos += 8 + na;
    if (na & 1u)
        out[pos++] = 0;
    if (b) {
        memcpy(out + pos, "CHRS", 4);
        be(out + pos + 4, nb);
        memcpy(out + pos + 8, b, nb);
        pos += 8 + nb;
        if (nb & 1u)
            out[pos++] = 0;
    }
    be(out + 4, pos - 8);
    return pos;
}

/* One CLIP_READ of room bytes at offset; the data lands at 0x400. */
static int clip_read_call(uint32_t unit, uint32_t type, uint32_t offset, uint32_t room, struct comp *c)
{
    memset((void *)(uintptr_t)buf(0x400), 0x5a, room);
    return callf(AAR_OP_CLIP_READ, 0, unit, type, offset, 0, 0x400, room, c);
}

/* One CLIP_WRITE chunk of n bytes. */
static int clip_write_call(uint32_t unit, uint32_t type, uint32_t offset, uint32_t total, const void *data,
                           uint32_t n, int last, struct comp *c)
{
    put_bytes(0x400, data, n);
    return callf(AAR_OP_CLIP_WRITE, last ? AAR_CLIPF_LAST : 0u, unit, type, offset, total, 0x400, n, c);
}

static void test_clip_read_text_converts_ftxt_to_utf8(void)
{
    struct comp c;
    uint8_t form[256];
    uint32_t n;

    setup("clip_read_text");
    make_serve();
    /* the clipboard is empty */
    CHECK(clip_read_call(0, AAR_CLIP_TEXT, 0, 64, &c) && c.result == AAR_E_NOENT);
    CHECK(clip_read_call(0, AAR_CLIP_IFF, 0, 64, &c) && c.result == AAR_E_NOENT);
    /* ISO-8859-1 by default */
    n = make_ftxt(form, -1, "Caf\xE9 \xA4!", 7, NULL, 0);
    hx_clip_set(0, form, n);
    CHECK(clip_read_call(0, AAR_CLIP_TEXT, 0, 64, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_CLIP_READ_SIZE] == 9 && c.out[AAR_R_CLIP_READ_COUNT] == 9);
    CHECK(memcmp((const void *)(uintptr_t)buf(0x400), "Caf\xC3\xA9 \xC2\xA4!", 9) == 0);
    CHECK(buf(0x400)[9] == 0x5a); /* no NUL: the text is not a C string */
    /* two CHRS chunks make one text */
    n = make_ftxt(form, -1, "ab", 2, "c\xFC", 2);
    hx_clip_set(0, form, n);
    CHECK(clip_read_call(0, AAR_CLIP_TEXT, 0, 64, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_CLIP_READ_SIZE] == 5 && memcmp((const void *)(uintptr_t)buf(0x400), "abc\xC3\xBC", 5) == 0);
    /* a CSET chunk says UTF-8 (106): the bytes go through */
    n = make_ftxt(form, 106, "\xE2\x82\xAC", 3, NULL, 0);
    hx_clip_set(0, form, n);
    CHECK(clip_read_call(0, AAR_CLIP_TEXT, 0, 64, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_CLIP_READ_SIZE] == 3 && memcmp((const void *)(uintptr_t)buf(0x400), "\xE2\x82\xAC", 3) == 0);
    /* a CSET that says ISO-8859-1 (0, and 4) changes nothing */
    n = make_ftxt(form, 0, "\xE9", 1, NULL, 0);
    hx_clip_set(0, form, n);
    CHECK(clip_read_call(0, AAR_CLIP_TEXT, 0, 64, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_CLIP_READ_SIZE] == 2 && memcmp((const void *)(uintptr_t)buf(0x400), "\xC3\xA9", 2) == 0);
    /* the whole stream is the IFF type, as it is */
    CHECK(clip_read_call(0, AAR_CLIP_IFF, 0, 64, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_CLIP_READ_SIZE] == n && c.out[AAR_R_CLIP_READ_COUNT] == n);
    CHECK(memcmp((const void *)(uintptr_t)buf(0x400), form, n) == 0);
    CHECK(hx_clip_handles == 0);
    finish();
}

static void test_clip_read_units_and_types(void)
{
    struct comp c;
    uint8_t form[64];
    uint32_t n;

    setup("clip_read_units");
    make_serve();
    n = make_ftxt(form, -1, "u255", 4, NULL, 0);
    hx_clip_set(255, form, n);
    CHECK(clip_read_call(255, AAR_CLIP_TEXT, 0, 16, &c) && c.result == AAR_OK && c.out[AAR_R_CLIP_READ_SIZE] == 4);
    CHECK(hx_clip_last_unit == 255);
    CHECK(clip_read_call(0, AAR_CLIP_TEXT, 0, 16, &c) && c.result == AAR_E_NOENT);
    CHECK(clip_read_call(256, AAR_CLIP_TEXT, 0, 16, &c) && c.result == AAR_E_INVAL);
    CHECK(clip_read_call(0xffffffffu, AAR_CLIP_TEXT, 0, 16, &c) && c.result == AAR_E_INVAL);
    CHECK(clip_read_call(255, 0, 0, 16, &c) && c.result == AAR_E_INVAL);
    CHECK(clip_read_call(255, 3, 0, 16, &c) && c.result == AAR_E_INVAL);
    /* not text: a picture form is an IFF clip and no text */
    memcpy(form, "FORM", 4);
    be(form + 4, 8);
    memcpy(form + 8, "ILBM", 4);
    memcpy(form + 12, "XXXX", 4);
    hx_clip_set(1, form, 16);
    CHECK(clip_read_call(1, AAR_CLIP_TEXT, 0, 32, &c) && c.result == AAR_E_NOENT);
    CHECK(clip_read_call(1, AAR_CLIP_IFF, 0, 32, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_CLIP_READ_SIZE] == 16 && memcmp((const void *)(uintptr_t)buf(0x400), form, 16) == 0);
    /* bytes that are no form at all */
    hx_clip_set(2, "not a form at all", 17);
    CHECK(clip_read_call(2, AAR_CLIP_TEXT, 0, 32, &c) && c.result == AAR_E_NOENT);
    CHECK(clip_read_call(2, AAR_CLIP_IFF, 0, 32, &c) && c.result == AAR_E_NOENT);
    /* an offset past the end */
    CHECK(clip_read_call(1, AAR_CLIP_IFF, 17, 32, &c) && c.result == AAR_E_INVAL);
    CHECK(clip_read_call(1, AAR_CLIP_IFF, 16, 32, &c) && c.result == AAR_OK && c.out[AAR_R_CLIP_READ_COUNT] == 0);
    finish();
}

/* A clip is read in chunks: one consistent clip, from offset 0 until offset 0 is asked again. */
static void test_a_chunked_clip_read_is_one_consistent_clip(void)
{
    struct comp c;
    static uint8_t latin[20000];
    static uint8_t form[20100];
    static uint8_t got[40000];
    static uint8_t form2[64];
    uint32_t n, off = 0, size = 0, i;

    setup("clip_read_chunks");
    make_serve();
    for (i = 0; i < sizeof(latin); i++)
        latin[i] = (uint8_t)(i % 3u == 0 ? 0xE9 : 'a' + i % 26u);
    n = make_ftxt(form, -1, latin, sizeof(latin), NULL, 0);
    hx_clip_set(0, form, n);
    /* the full size is in the first answer, and 4096 bytes of it */
    CHECK(clip_read_call(0, AAR_CLIP_TEXT, 0, 4096, &c) && c.result == AAR_OK);
    size = c.out[AAR_R_CLIP_READ_SIZE];
    CHECK(size == 20000u + 6667u); /* every third byte is a two byte character */
    CHECK(c.out[AAR_R_CLIP_READ_COUNT] == 4096);
    memcpy(got, (const void *)(uintptr_t)buf(0x400), 4096);
    off = 4096;
    /* another program replaces the clip in the middle of the sequence */
    n = make_ftxt(form2, -1, "other", 5, NULL, 0);
    hx_clip_set(0, form2, n);
    while (off < size) {
        CHECK(clip_read_call(0, AAR_CLIP_TEXT, off, 4096, &c) && c.result == AAR_OK);
        CHECK(c.out[AAR_R_CLIP_READ_SIZE] == size);
        if (c.result != AAR_OK || c.out[AAR_R_CLIP_READ_COUNT] == 0)
            break;
        memcpy(got + off, (const void *)(uintptr_t)buf(0x400), c.out[AAR_R_CLIP_READ_COUNT]);
        off += c.out[AAR_R_CLIP_READ_COUNT];
    }
    CHECK(off == size);
    {
        /* the sequence read the first clip to its end: every third byte is U+00E9, the rest letters */
        uint32_t pos = 0;

        for (i = 0; i < 20000u; i++) {
            if (i % 3u == 0) {
                CHECK(got[pos] == 0xC3 && got[pos + 1] == 0xA9);
                pos += 2;
            } else {
                CHECK(got[pos] == (uint8_t)('a' + i % 26u));
                pos++;
            }
        }
        CHECK(pos == size);
    }
    /* offset 0 again takes the clip as it is now */
    CHECK(clip_read_call(0, AAR_CLIP_TEXT, 0, 4096, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_CLIP_READ_SIZE] == 5 && memcmp((const void *)(uintptr_t)buf(0x400), "other", 5) == 0);
    CHECK(hx_clip_handles == 0);
    finish();
}

static void test_a_chunked_text_write_replaces_the_clip_at_the_last_chunk(void)
{
    struct comp c;
    uint8_t form[64];
    uint32_t n;
    const uint8_t *clip;
    static const char text[] = "h\xC3\xA9llo \xE2\x82\xAC \xF0\x9F\x98\x80 \xFF!";
    /* h, e-acute, l, l, o, space, euro, space, emoji, space, an invalid byte, ! */

    setup("clip_write_text");
    make_serve();
    n = make_ftxt(form, -1, "old", 3, NULL, 0);
    hx_clip_set(0, form, n);
    /* the first two chunks change nothing */
    CHECK(clip_write_call(0, AAR_CLIP_TEXT, 0, sizeof(text) - 1, text, 6, 0, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_CLIP_WRITE_COUNT] == 6);
    CHECK(clip_write_call(0, AAR_CLIP_TEXT, 6, sizeof(text) - 1, text + 6, 8, 0, &c) && c.result == AAR_OK);
    CHECK(hx_clip_updates == 0 && hx_clips[0].len == n && memcmp(hx_clips[0].data, form, n) == 0);
    CHECK(clip_write_call(0, AAR_CLIP_TEXT, 14, sizeof(text) - 1, text + 14, (uint32_t)sizeof(text) - 1 - 14, 1, &c) &&
          c.result == AAR_OK);
    CHECK(c.out[AAR_R_CLIP_WRITE_COUNT] == sizeof(text) - 1 - 14);
    CHECK(hx_clip_updates == 1);
    /* Latin-1 text of 12 characters; what is not Latin-1 is a '?' (the broken byte, too) */
    clip = hx_clips[0].data;
    CHECK(hx_clips[0].len == 8 + 4 + 8 + 12);
    CHECK(memcmp(clip, "FORM", 4) == 0 && memcmp(clip + 8, "FTXT", 4) == 0 && memcmp(clip + 12, "CHRS", 4) == 0);
    CHECK(aar_get_be32(clip + 16) == 12 && aar_get_be32(clip + 4) == 4 + 8 + 12);
    CHECK(memcmp(clip + 20, "h\xE9llo ? ? ?!", 12) == 0);
    /* it reads back as the same text, as far as Latin-1 reaches */
    CHECK(clip_read_call(0, AAR_CLIP_TEXT, 0, 64, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_CLIP_READ_SIZE] == 13 && memcmp((const void *)(uintptr_t)buf(0x400), "h\xC3\xA9llo ? ? ?!", 13) == 0);
    CHECK(hx_clip_handles == 0);
    finish();
}

static void test_clip_write_sequence_errors_change_nothing(void)
{
    struct comp c;
    uint8_t form[64];
    uint32_t n;

    setup("clip_write_errors");
    make_serve();
    n = make_ftxt(form, -1, "old", 3, NULL, 0);
    hx_clip_set(0, form, n);
    /* a chunk with no first chunk before it */
    CHECK(clip_write_call(0, AAR_CLIP_TEXT, 5, 10, "world", 5, 1, &c) && c.result == AAR_E_INVAL);
    /* a gap */
    CHECK(clip_write_call(0, AAR_CLIP_TEXT, 0, 10, "hello", 5, 0, &c) && c.result == AAR_OK);
    CHECK(clip_write_call(0, AAR_CLIP_TEXT, 6, 10, "world", 4, 1, &c) && c.result == AAR_E_INVAL);
    /* another unit, another type or another total in the middle */
    CHECK(clip_write_call(0, AAR_CLIP_TEXT, 0, 10, "hello", 5, 0, &c) && c.result == AAR_OK);
    CHECK(clip_write_call(1, AAR_CLIP_TEXT, 5, 10, "world", 5, 1, &c) && c.result == AAR_E_INVAL);
    CHECK(clip_write_call(0, AAR_CLIP_TEXT, 0, 10, "hello", 5, 0, &c) && c.result == AAR_OK);
    CHECK(clip_write_call(0, AAR_CLIP_TEXT, 5, 11, "world", 5, 1, &c) && c.result == AAR_E_INVAL);
    /* more bytes than the total */
    CHECK(clip_write_call(0, AAR_CLIP_TEXT, 0, 4, "hello", 5, 1, &c) && c.result == AAR_E_INVAL);
    /* the last chunk with bytes missing */
    CHECK(clip_write_call(0, AAR_CLIP_TEXT, 0, 10, "hello", 5, 1, &c) && c.result == AAR_E_INVAL);
    /* units and types */
    CHECK(clip_write_call(256, AAR_CLIP_TEXT, 0, 5, "hello", 5, 1, &c) && c.result == AAR_E_INVAL);
    CHECK(clip_write_call(0, 7, 0, 5, "hello", 5, 1, &c) && c.result == AAR_E_INVAL);
    /* an IFF clip that is no FORM */
    CHECK(clip_write_call(0, AAR_CLIP_IFF, 0, 12, "no form here", 12, 1, &c) && c.result == AAR_E_INVAL);
    CHECK(clip_write_call(0, AAR_CLIP_IFF, 0, 4, "FORM", 4, 1, &c) && c.result == AAR_E_INVAL);
    /* nothing of it reached the clipboard */
    CHECK(hx_clip_updates == 0 && hx_clips[0].len == n && memcmp(hx_clips[0].data, form, n) == 0);
    /* a sequence that is not finished is dropped by the next first chunk, which is a new clip */
    CHECK(clip_write_call(0, AAR_CLIP_TEXT, 0, 10, "hello", 5, 0, &c) && c.result == AAR_OK);
    CHECK(clip_write_call(0, AAR_CLIP_TEXT, 0, 3, "new", 3, 1, &c) && c.result == AAR_OK);
    CHECK(hx_clip_updates == 1 && memcmp(hx_clips[0].data + 20, "new\0", 4) == 0);
    /* a clip that is too large */
    CHECK(clip_write_call(0, AAR_CLIP_TEXT, 0, 0x7fffffffu, "x", 1, 0, &c) && c.result == AAR_E_NOMEM);
    CHECK(hx_clip_updates == 1);
    finish();
}

static void test_clip_write_empty_text_clears_the_text(void)
{
    struct comp c;
    uint8_t form[64];
    uint32_t n;

    setup("clip_write_empty");
    make_serve();
    n = make_ftxt(form, -1, "old", 3, NULL, 0);
    hx_clip_set(0, form, n);
    CHECK(clip_write_call(0, AAR_CLIP_TEXT, 0, 0, "", 0, 1, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_CLIP_WRITE_COUNT] == 0 && hx_clip_updates == 1);
    CHECK(hx_clips[0].len == 20 && aar_get_be32(hx_clips[0].data + 16) == 0);
    CHECK(clip_read_call(0, AAR_CLIP_TEXT, 0, 16, &c) && c.result == AAR_E_NOENT);
    finish();
}

static void test_clip_write_iff_passes_one_form_through(void)
{
    struct comp c;
    uint8_t form[64];

    setup("clip_write_iff");
    make_serve();
    memcpy(form, "FORM", 4);
    be(form + 4, 11); /* odd: a pad byte follows */
    memcpy(form + 8, "ILBM", 4);
    memcpy(form + 12, "abcdefg", 7);
    form[19] = 0;
    memset(form + 20, 'Z', 10); /* bytes after the form are not part of it */
    CHECK(clip_write_call(3, AAR_CLIP_IFF, 0, 30, form, 20, 0, &c) && c.result == AAR_OK);
    CHECK(clip_write_call(3, AAR_CLIP_IFF, 20, 30, form + 20, 10, 1, &c) && c.result == AAR_OK);
    CHECK(hx_clip_updates == 1 && hx_clips[3].len == 20 && memcmp(hx_clips[3].data, form, 20) == 0);
    /* the pad byte is added when the sender left it out */
    CHECK(clip_write_call(3, AAR_CLIP_IFF, 0, 19, form, 19, 1, &c) && c.result == AAR_OK);
    CHECK(hx_clips[3].len == 20 && hx_clips[3].data[19] == 0 && memcmp(hx_clips[3].data, form, 19) == 0);
    /* a form that says it is longer than the clip is no clip */
    be(form + 4, 100);
    CHECK(clip_write_call(3, AAR_CLIP_IFF, 0, 20, form, 20, 1, &c) && c.result == AAR_E_INVAL);
    CHECK(hx_clip_updates == 2);
    /* the form is read back as it is */
    CHECK(clip_read_call(3, AAR_CLIP_IFF, 0, 64, &c) && c.result == AAR_OK && c.out[AAR_R_CLIP_READ_SIZE] == 20);
    finish();
}

static void test_clip_without_the_system_libraries(void)
{
    struct comp c;

    setup("clip_nosys");
    hx_iffparse_present = 0;
    make_serve();
    CHECK(clip_read_call(0, AAR_CLIP_TEXT, 0, 16, &c) && c.result == AAR_E_NOSYS);
    CHECK(clip_write_call(0, AAR_CLIP_TEXT, 0, 3, "abc", 3, 1, &c) && c.result == AAR_E_NOSYS);
    /* the arguments are checked first */
    CHECK(clip_read_call(256, AAR_CLIP_TEXT, 0, 16, &c) && c.result == AAR_E_INVAL);
    finish();

    /* clipboard.device does not open */
    setup("clip_nodevice");
    hx_clip_fail_open = 1;
    make_serve();
    CHECK(clip_read_call(0, AAR_CLIP_TEXT, 0, 16, &c) && c.result == AAR_E_NOSYS);
    CHECK(clip_write_call(0, AAR_CLIP_TEXT, 0, 3, "abc", 3, 1, &c) && c.result == AAR_E_NOSYS);
    finish();

    /* the device fails */
    setup("clip_ioerror");
    hx_clip_set(0, "FORM\0\0\0\4FTXT", 12);
    hx_clip_fail_io = 1;
    make_serve();
    CHECK(clip_read_call(0, AAR_CLIP_TEXT, 0, 16, &c) && c.result == AAR_E_IO);
    CHECK(clip_write_call(0, AAR_CLIP_TEXT, 0, 3, "abc", 3, 1, &c) && c.result == AAR_E_IO);
    CHECK(hx_clip_updates == 0);
    finish();
}

static void test_a_config_flag_refuses_the_clipboard(void)
{
    struct comp c;

    setup("clip_policy");
    hx_clip_set(0, "FORM\0\0\0\4FTXT", 12);
    cfg.flags |= AAR_SERVEF_NO_CLIPBOARD;
    make_serve();
    CHECK(clip_read_call(0, AAR_CLIP_TEXT, 0, 16, &c) && c.result == AAR_E_ACCES);
    CHECK(clip_read_call(256, AAR_CLIP_TEXT, 0, 16, &c) && c.result == AAR_E_ACCES);
    CHECK(clip_write_call(0, AAR_CLIP_TEXT, 0, 3, "abc", 3, 1, &c) && c.result == AAR_E_ACCES);
    CHECK(hx_clip_updates == 0 && hx_host_lib_opens == 0);
    /* the environment is not the clipboard */
    hx_env_set("x", "1", 1);
    CHECK(env_call("x", 64, &c) && c.result == AAR_OK);
    finish();
}

/* A clipboard request that is posted, not waited for; a write's payload is put at 0x400 first. */
static int clip_post(uint32_t op, uint32_t flags, uint32_t unit, uint32_t n)
{
    uint32_t arg[6];

    memset(arg, 0, sizeof(arg));
    arg[0] = unit;
    arg[1] = AAR_CLIP_TEXT;
    arg[3] = op == AAR_OP_CLIP_WRITE ? n : 0;
    return refplat_app_request(SLOT, op, flags, ++files_nid, arg, refplat_buf_token(SLOT, 0), 0x400, n) ==
           AAR_OK;
}

/* clipboard.device keeps a request back while another program has a read or write sequence
 * open on the unit. The wait is the DOS packet wait's: it takes Ctrl-C and the heartbeat's check of
 * the claim, and the request is aborted and its handle closed, not leaked. */
static void test_ctrl_c_during_a_clipboard_write_aborts_it_and_keeps_the_clip(void)
{
    struct comp c;
    struct aar_serve_result res;
    uint8_t form[64];
    uint32_t n;

    setup("clip_ctrl_c_write");
    n = make_ftxt(form, -1, "old", 3, NULL, 0);
    hx_clip_set(0, form, n);
    make_serve();
    hx_clip_hold = 1;
    put_bytes(0x400, "new", 3);
    CHECK(clip_post(AAR_OP_CLIP_WRITE, AAR_CLIPF_LAST, 0, 3));
    hx_sched(5000, ctrl_c_cb, NULL);
    ticks(4); /* the write waits at the device; Ctrl-C arrives while it does */
    CHECK(hx_now >= 5000);
    CHECK(hx_clip_aborts == 1 && hx_clip_held == NULL);
    memset(&res, 0, sizeof(res));
    res.size = sizeof(res);
    (void)aar_serve_result(S, &res);
    CHECK((res.flags & AAR_SERVE_R_USER_STOP) && (res.flags & AAR_SERVE_R_STOP_ASKED));
    CHECK(wait_comp(&c, 100));
    CHECK(c.result == AAR_E_IO);
    CHECK(hx_clip_updates == 0 && hx_clips[0].len == n && memcmp(hx_clips[0].data, form, n) == 0);
    CHECK(hx_clip_handles == 0);
    ticks(5);
    finish();
}

/* A device that does not grant AbortIO keeps the request and the handle until it replies: they are
 * not freed under it, and nothing is reported as done (the result is E_IO, no CMD_UPDATE follows). */
static void test_a_clipboard_device_that_ignores_the_abort_is_waited_for(void)
{
    struct comp c;
    uint8_t form[64];
    uint32_t n;

    setup("clip_abort_ignored");
    n = make_ftxt(form, -1, "old", 3, NULL, 0);
    hx_clip_set(0, form, n);
    make_serve();
    hx_clip_hold = 1;
    hx_clip_abort_ignored = 1;
    put_bytes(0x400, "new", 3);
    CHECK(clip_post(AAR_OP_CLIP_WRITE, AAR_CLIPF_LAST, 0, 3));
    hx_sched(5000, ctrl_c_cb, NULL);
    hx_sched(30000, hx_clip_release, NULL);
    ticks(4);
    CHECK(hx_now >= 30000); /* not given up while the device had the request */
    CHECK(hx_clip_aborts == 1 && hx_clip_held == NULL);
    CHECK(wait_comp(&c, 100));
    CHECK(c.result == AAR_E_IO);
    CHECK(hx_clip_updates == 0 && hx_clips[0].len == n && memcmp(hx_clips[0].data, form, n) == 0);
    CHECK(hx_clip_handles == 0);
    ticks(5);
    finish();
}

/* The other program ends its sequence: the request that waited is served, with no abort. */
static void test_a_clipboard_write_waits_for_the_other_program_and_completes(void)
{
    struct comp c;
    uint8_t form[64];
    uint32_t n;

    setup("clip_wait_released");
    n = make_ftxt(form, -1, "old", 3, NULL, 0);
    hx_clip_set(0, form, n);
    make_serve();
    hx_clip_hold = 1;
    put_bytes(0x400, "new", 3);
    CHECK(clip_post(AAR_OP_CLIP_WRITE, AAR_CLIPF_LAST, 0, 3));
    hx_sched(20000, hx_clip_release, NULL);
    CHECK(wait_comp(&c, 400));
    CHECK(c.result == AAR_OK && c.out[AAR_R_CLIP_WRITE_COUNT] == 3);
    CHECK(hx_now >= 20000 && hx_clip_aborts == 0 && hx_clip_updates == 1);
    CHECK(memcmp(hx_clips[0].data + 20, "new\0", 4) == 0);
    CHECK(hx_clip_handles == 0);
    finish();
}

/* The heartbeat finds the claim revoked while a read waits: the read is aborted and the loop ends as lost. */
static void test_a_claim_revoked_during_a_clipboard_read_aborts_it(void)
{
    uint8_t form[64];
    uint32_t n;

    setup("clip_revoked");
    n = make_ftxt(form, -1, "old", 3, NULL, 0);
    hx_clip_set(0, form, n);
    make_serve();
    hx_clip_hold = 1;
    CHECK(clip_post(AAR_OP_CLIP_READ, 0, 0, 64));
    hx_sched(3000, revoke_notice_cb, NULL); /* the next library call finds the claim gone */
    CHECK(ticks(20) == AAR_SERVE_LOST);
    CHECK(hx_clip_aborts == 1 && hx_clip_held == NULL && hx_clip_handles == 0);
    CHECK(hx_clip_updates == 0);
    finish();
}

/* FORM FTXT with one CHRS chunk of n bytes: ASCII letters, or (latin) the byte 0xE9. Returns the size. */
static uint32_t make_ftxt_run(uint8_t *out, uint32_t n, int latin)
{
    uint32_t i;
    uint32_t size = 4u + 8u + n + (n & 1u);

    memcpy(out, "FORM", 4);
    be(out + 4, size);
    memcpy(out + 8, "FTXT", 4);
    memcpy(out + 12, "CHRS", 4);
    be(out + 16, n);
    for (i = 0; i < n; i++)
        out[20 + i] = latin ? 0xE9 : (uint8_t)('a' + i % 26u);
    if (n & 1u)
        out[20 + n] = 0;
    return 8u + size;
}

/* A text of exactly the limit is accepted by CLIP_WRITE (its FTXT wrapper is 20 bytes more) and must
 * be read back whole: the limit is on the text, not on the stream that holds it. */
static void test_a_text_clip_of_the_limit_round_trips(void)
{
    struct comp c;
    uint8_t chunk[4096];
    uint32_t off, i;
    int ok = 1;

    setup("clip_limit_round_trip");
    make_serve();
    for (off = 0; ok && off < AAR_SERVE_CLIP_MAX; off += sizeof(chunk)) {
        for (i = 0; i < sizeof(chunk); i++)
            chunk[i] = (uint8_t)('a' + (off + i) % 26u);
        ok = clip_write_call(0, AAR_CLIP_TEXT, off, AAR_SERVE_CLIP_MAX, chunk, sizeof(chunk),
                             off + sizeof(chunk) >= AAR_SERVE_CLIP_MAX, &c) && c.result == AAR_OK;
    }
    CHECK(ok);
    CHECK(hx_clip_updates == 1 && hx_clips[0].len == 20u + AAR_SERVE_CLIP_MAX);
    for (off = 0; ok && off < AAR_SERVE_CLIP_MAX; off += sizeof(chunk)) {
        ok = clip_read_call(0, AAR_CLIP_TEXT, off, sizeof(chunk), &c) && c.result == AAR_OK &&
             c.out[AAR_R_CLIP_READ_SIZE] == AAR_SERVE_CLIP_MAX &&
             c.out[AAR_R_CLIP_READ_COUNT] == sizeof(chunk);
        for (i = 0; ok && i < sizeof(chunk); i++)
            ok = buf(0x400)[i] == (uint8_t)('a' + (off + i) % 26u);
    }
    CHECK(ok);
    CHECK(hx_clip_handles == 0);
    finish();
}

/* The limits of a read: the UTF-8 text (counted after the conversion, so Latin-1 text counts twice for
 * every byte of 0x80 and up), and the IFF form for AAR_CLIP_IFF. What is taken from the device has room
 * for the wrapper and no more. */
static void test_clip_read_limits_are_on_the_text_and_on_the_form(void)
{
    struct comp c;
    static uint8_t big[AAR_SERVE_CLIP_MAX + 256u];
    uint32_t n;

    setup("clip_read_limits");
    make_serve();
    /* ASCII text of the limit: read back whole as text; as an IFF clip it is longer than the limit */
    n = make_ftxt_run(big, AAR_SERVE_CLIP_MAX, 0);
    hx_clip_set(0, big, n);
    CHECK(clip_read_call(0, AAR_CLIP_TEXT, 0, 64, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_CLIP_READ_SIZE] == AAR_SERVE_CLIP_MAX && c.out[AAR_R_CLIP_READ_COUNT] == 64);
    CHECK(clip_read_call(0, AAR_CLIP_IFF, 0, 64, &c) && c.result == AAR_E_NOMEM);
    /* one byte more is not (the pad byte follows it) */
    n = make_ftxt_run(big, AAR_SERVE_CLIP_MAX + 1u, 0);
    hx_clip_set(0, big, n);
    CHECK(clip_read_call(0, AAR_CLIP_TEXT, 0, 64, &c) && c.result == AAR_E_NOMEM);
    /* a stream longer than the text with its wrapper can be is not taken at all */
    n = make_ftxt_run(big, AAR_SERVE_CLIP_MAX + 100u, 0);
    hx_clip_set(0, big, n);
    CHECK(clip_read_call(0, AAR_CLIP_TEXT, 0, 64, &c) && c.result == AAR_E_NOMEM);
    CHECK(clip_read_call(0, AAR_CLIP_IFF, 0, 64, &c) && c.result == AAR_E_NOMEM);
    /* Latin-1 text: two UTF-8 bytes for every 0xE9, so half the limit is the longest */
    n = make_ftxt_run(big, AAR_SERVE_CLIP_MAX / 2u, 1);
    hx_clip_set(0, big, n);
    CHECK(clip_read_call(0, AAR_CLIP_TEXT, 0, 64, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_CLIP_READ_SIZE] == AAR_SERVE_CLIP_MAX);
    CHECK(memcmp((const void *)(uintptr_t)buf(0x400), "\xC3\xA9\xC3\xA9", 4) == 0);
    n = make_ftxt_run(big, AAR_SERVE_CLIP_MAX / 2u + 1u, 1);
    hx_clip_set(0, big, n);
    CHECK(clip_read_call(0, AAR_CLIP_TEXT, 0, 64, &c) && c.result == AAR_E_NOMEM);
    /* an IFF clip of exactly the limit is read, one of two bytes more is not */
    memset(big, 0, AAR_SERVE_CLIP_MAX + 2u);
    memcpy(big, "FORM", 4);
    be(big + 4, AAR_SERVE_CLIP_MAX - 8u);
    memcpy(big + 8, "ILBM", 4);
    hx_clip_set(1, big, AAR_SERVE_CLIP_MAX);
    CHECK(clip_read_call(1, AAR_CLIP_IFF, 0, 64, &c) && c.result == AAR_OK);
    CHECK(c.out[AAR_R_CLIP_READ_SIZE] == AAR_SERVE_CLIP_MAX);
    be(big + 4, AAR_SERVE_CLIP_MAX - 6u);
    hx_clip_set(1, big, AAR_SERVE_CLIP_MAX + 2u);
    CHECK(clip_read_call(1, AAR_CLIP_IFF, 0, 64, &c) && c.result == AAR_E_NOMEM);
    CHECK(hx_clip_handles == 0);
    finish();
}

static void test_the_config_flags_of_the_host_ops_are_known(void)
{
    aar_serve *s = NULL;

    setup("host_flags");
    cfg.flags |= AAR_SERVEF_NO_ENV | AAR_SERVEF_NO_CLIPBOARD;
    app_state(H, AAR_STATE_RUNNING, AAR_END_NONE);
    CHECK(aar_serve_create(H, &cfg, &s) == AAR_OK && s != NULL);
    if (s)
        aar_serve_destroy(s);
    cfg.flags |= 1UL << 20;
    CHECK(aar_serve_create(H, &cfg, &s) == AAR_E_INVAL && s == NULL);
    finish();
}

/* The libraries are open from their first use to the destroy of the loop. */
static void test_the_host_libraries_are_closed_at_destroy(void)
{
    struct comp c;

    setup("host_libs_close");
    make_serve();
    CHECK(hx_host_libs_open == 0);
    CHECK(clip_read_call(0, AAR_CLIP_TEXT, 0, 16, &c) && c.result == AAR_E_NOENT);
    CHECK(hx_host_libs_open == 1); /* iffparse.library */
    CHECK(callf(AAR_OP_TIME_UNIX, 0, 0, 0, 0, 0, 0, 0, &c) && c.result == AAR_OK);
    CHECK(hx_host_libs_open == 2); /* and locale.library */
    finish(); /* checks that both are closed again */
}
