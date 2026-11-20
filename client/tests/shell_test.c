/*
 * Host test of the AmigaOS shell (client/src/aar_shell.c) on the fake system of
 * shell_host.h, over the core and the reference platform.
 *
 *   shell_test GOOD_IMAGE
 *
 * GOOD_IMAGE is an accepted AAR image (client/tests/make_images.py writes one)
 * that the probe and file load tests use.
 *
 * Every check names its test, so that the mutation run (mutants.sh) can require
 * that a deliberately broken shell makes exactly that test fail.
 *
 * Build with -no-pie (or -m32).
 *
 * SPDX-License-Identifier: MIT
 */
#define AAR_SHELL_HOST 1
#include "../src/aar_shell.c"
#include "../src/refplat.h"

static char names[HX_TASKS][8];

static unsigned long checks, failures;
static const char *cur_test = "";

#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            printf("FAIL [%s] %s:%d: %s\n", cur_test, __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

/*
 * The host's dispatcher process: a coroutine of its own task. Its first half
 * (shell_disp_begin) runs when the task that started it waits; its last half
 * (shell_disp_end) when the task that stopped it waits, and only if the stop
 * was requested. A process must end under Forbid, which it keeps: the fake
 * aborts when it does not, and clears the Forbid when the process is gone.
 */
static struct AARBase *hx_disp_base;
static int hx_fail_spawn;          /* the next spawn fails */
static uint32_t hx_disp_sigs;      /* signal bits the dispatcher's task has allocated already */
static unsigned long hx_vblank_installs, hx_vblank_removes;
static int hx_vblank_stuck;        /* the interrupt cannot be removed */

static void hx_process_gone(void)
{
    if (hx_forbid <= 0)
        HX_FAIL("the dispatcher process ended without holding Forbid");
    hx_forbid = 0;
    hx_tasks[HX_DISP].listed = 0;
}

static void hx_disp_end(void)
{
    struct hx_task *caller = hx_cur;

    if (!hx_disp_base->disp_stop) {
        hx_bg = hx_disp_end; /* it keeps running until it is told to stop */
        return;
    }
    hx_cur = &hx_tasks[HX_DISP];
    hx_dispatcher_stops++;
    shell_disp_end(hx_disp_base);
    hx_process_gone();
    hx_cur = caller;
}

static void hx_disp_begin(void)
{
    struct hx_task *caller = hx_cur;

    hx_cur = &hx_tasks[HX_DISP];
    if (shell_disp_begin(hx_disp_base) == AAR_OK)
        hx_bg = hx_disp_end;
    else
        hx_process_gone();
    hx_cur = caller;
}

static int hx_spawn(struct AARBase *b)
{
    hx_dispatcher_starts++;
    if (hx_fail_spawn) {
        hx_fail_spawn--;
        return AAR_E_NOMEM;
    }
    hx_disp_base = b;
    memset(&hx_tasks[HX_DISP], 0, sizeof(hx_tasks[HX_DISP]));
    hx_tasks[HX_DISP].used = 1;
    hx_tasks[HX_DISP].listed = 1;
    hx_tasks[HX_DISP].name = &names[HX_DISP];
    hx_tasks[HX_DISP].kind = 1;
    hx_tasks[HX_DISP].sig_alloc = hx_disp_sigs;
    b->disp_process = &hx_tasks[HX_DISP];
    hx_bg = hx_disp_begin;
    return AAR_OK;
}

static void hx_vblank_install(struct AARBase *b)
{
    b->vblank_installed = 1;
    hx_vblank_installs++;
}

static void hx_vblank_remove(struct AARBase *b)
{
    if (!b->vblank_installed || hx_vblank_stuck)
        return;
    b->vblank_installed = 0;
    hx_vblank_removes++;
}

static struct AARBase base;
static struct MsgPort ports[3];
static const uint8_t *good_image;
static uint32_t good_len;

#define T(i) (&hx_tasks[i])
#define USE(i) (hx_cur = T(i))

static int popcount(uint32_t v)
{
    int n = 0;

    for (; v; v &= v - 1u)
        n++;
    return n;
}

static void on_release(void *user, uint32_t handle)
{
    (void)user;
    (void)handle;
    /* Nothing is held while the platform works: no lock, no Disable. */
    CHECK(base.lock.excl == 0 && base.lock.shared == 0);
    CHECK(hx_disabled == 0);
}

static void setup(const char *name)
{
    int i;

    cur_test = name;
    printf("== %s\n", name);
    fflush(stdout);
    refplat_reset();
    refplat.on_release = on_release;
    memset(hx_tasks, 0, sizeof(hx_tasks));
    for (i = 0; i < 4; i++) {
        hx_tasks[i].used = 1;
        hx_tasks[i].listed = 1;
        hx_tasks[i].name = &names[i];
        hx_tasks[i].kind = 1;
    }
    memset(hx_pool_used, 0, sizeof(hx_pool_used));
    hx_live_blocks = 0;
    hx_live_bytes = 0;
    hx_alloc_calls = 0;
    hx_fail_alloc = 0;
    hx_disabled = 0;
    hx_dispatcher_starts = hx_dispatcher_stops = 0;
    hx_forbid = 0;
    hx_waits = 0;
    hx_spurious = 0;
    hx_bg = NULL;
    hx_fail_spawn = 0;
    hx_disp_sigs = 0;
    hx_vblank_installs = hx_vblank_removes = 0;
    hx_vblank_stuck = 0;
    hx_delays = 0;
    memset(hx_ports, 0, sizeof(hx_ports));
    memset(ports, 0, sizeof(ports));
    for (i = 0; i < 3; i++)
        hx_port_register(&ports[i]);
    memset(hx_files, 0, sizeof(hx_files));
    hx_file_reads = hx_files_open = 0;
    hx_ctrl_c = 0;
    hx_ctrl_c_after_reads = 0;
    hx_fail_read_at = 0;
    memset(&base, 0, sizeof(base));
    USE(0);
    CHECK(shell_init(&base) == AAR_OK);
}

static int lib_open(void)
{
    int rc = shell_open(&base);

    if (rc == AAR_OK)
        base.library.lib_OpenCnt++;
    return rc;
}

static void lib_close(void)
{
    (void)shell_close(&base);
    if (--base.library.lib_OpenCnt == 0)
        shell_final_close(&base);
}

static void finish(void)
{
    CHECK(base.lock.excl == 0 && base.lock.shared == 0);
    CHECK(base.ctl.excl == 0 && base.ctl.shared == 0);
    CHECK(hx_disabled == 0);
    CHECK(hx_forbid == 0);
    CHECK(refplat_live() == 0);
    CHECK(hx_live_blocks == 0);
    CHECK(base.openers == NULL);
}

static AARHandle *open_h(struct MsgPort *port)
{
    AARHandle *h = NULL;
    ULONG reason = 77;
    LONG rc = v_open(&h, port, &reason);

    CHECK(rc == AAR_OK && h != NULL && reason == AAR_REASON_NONE);
    return h;
}

static void vblanks(unsigned n)
{
    while (n--)
        shell_vblank(&base);
}

static uint32_t lease_of(AARHandle *h)
{
    return aar_get_be32(refplat_cb(refplat_slot(h->claim.handle)) + AAR_CB_LEASE_COUNTER);
}

/* ---- Tests ----------------------------------------------------------------------------- */

static void test_lifecycle(void)
{
    AARInfo info;
    AARHandle *hs[8], *x = (AARHandle *)(uintptr_t)1;
    ULONG reason = 0, mask;
    int i;

    setup("lifecycle");
    CHECK(lib_open() == AAR_OK);
    CHECK(hx_dispatcher_starts == 0); /* nothing yet: the dispatcher starts with the first call */
    info.size = sizeof(info);
    CHECK(v_query(&info) == AAR_OK && info.platform_id == AAR_PLATFORM_ZZ9000);
    CHECK(hx_dispatcher_starts == 1 && base.dispatcher_up == 1);
    CHECK(v_query(&info) == AAR_OK);
    CHECK(hx_dispatcher_starts == 1); /* one dispatcher for the life of the library */
    /* Eight handles in one task share ONE signal bit. */
    CHECK(popcount(hx_cur->sig_alloc) == 0);
    for (i = 0; i < 8; i++)
        hs[i] = open_h(NULL);
    CHECK(popcount(hx_cur->sig_alloc) == 1);
    mask = v_message_signal(hs[0]);
    CHECK(mask != 0 && popcount(mask) == 1 && mask == hx_cur->sig_alloc);
    for (i = 1; i < 8; i++)
        CHECK(v_message_signal(hs[i]) == mask);
    CHECK(v_open(&x, NULL, &reason) == AAR_E_NORESOURCE && x == NULL && reason == AAR_REASON_TABLES);
    for (i = 0; i < 8; i++)
        CHECK(v_close(hs[i]) == AAR_OK);
    CHECK(popcount(hx_cur->sig_alloc) == 1); /* the bit is the task's until its last CloseLibrary */
    CHECK(hx_dispatcher_stops == 0);
    lib_close();
    CHECK(hx_cur->sig_alloc == 0);
    CHECK(hx_dispatcher_stops == 1 && base.dispatcher_up == 0); /* stopped at the final close */
    CHECK(refplat.detaches == 1);
    /* A task with no free bit cannot open. */
    CHECK(lib_open() == AAR_OK);
    hx_cur->sig_alloc = 0xFFFFFFF0u;
    CHECK(v_open(&x, NULL, NULL) == AAR_E_NOMEM && x == NULL);
    hx_cur->sig_alloc = 0;
    lib_close();
    finish();
}

static void test_last_close(void)
{
    AARHandle *a[3], *b[2];
    int i;

    setup("last_close");
    USE(0);
    CHECK(lib_open() == AAR_OK && lib_open() == AAR_OK); /* two references */
    for (i = 0; i < 3; i++)
        CHECK((a[i] = open_h(NULL)) != NULL);
    USE(1);
    CHECK(lib_open() == AAR_OK);
    for (i = 0; i < 2; i++)
        b[i] = open_h(NULL);
    CHECK(refplat_live() == 5);
    CHECK(popcount(T(0)->sig_alloc) == 1 && popcount(T(1)->sig_alloc) == 1);
    USE(0);
    lib_close(); /* one reference left: nothing closes */
    CHECK(refplat_live() == 5 && T(0)->sig_alloc != 0);
    lib_close(); /* the last of task 0: its handles close, its bit goes */
    CHECK(refplat_live() == 2 && T(0)->sig_alloc == 0);
    CHECK(hx_dispatcher_stops == 0); /* task 1 still has the library */
    USE(1);
    CHECK(v_status(b[0], &(AARStatusInfo){ .size = sizeof(AARStatusInfo) }) == AAR_OK);
    lib_close();
    CHECK(refplat_live() == 0 && hx_dispatcher_stops == 1);
    finish();
}

static void test_owner_stale(void)
{
    AARHandle *h, *b;
    AARStatusInfo st;
    AARMessage m;
    AARServiceReq req;
    struct AARSharedMap map;
    aar_event ev;
    ULONG got;
    char buf[8];
    const char *argv[1] = { "x" };
    int pass;

    setup("owner_stale");
    USE(0);
    CHECK(lib_open() == AAR_OK);
    h = open_h(NULL);
    USE(1);
    CHECK(lib_open() == AAR_OK);
    b = open_h(NULL);
    memset(&st, 0, sizeof(st));
    memset(&req, 0, sizeof(req));
    memset(&map, 0, sizeof(map));
    memset(&ev, 0, sizeof(ev));
    st.size = sizeof(st);
    req.size = sizeof(req);
    map.size = sizeof(map);
    for (pass = 0; pass < 2; pass++) {
        int expect = pass == 0 ? AAR_E_OWNER : AAR_E_STALE;

        if (pass == 1) {
            USE(0);
            CHECK(v_close(h) == AAR_OK);
            USE(1);
        }
        CHECK(v_close(h) == expect);
        CHECK(v_status(h, &st) == expect);
        CHECK(v_stop(h) == expect);
        CHECK(v_start(h, argv, 1) == expect);
        CHECK(v_load_memory(h, buf, 4) == expect);
        CHECK(v_load_file(h, "nothing") == expect); /* before the missing file */
        CHECK(v_set_tag_list(h, NULL) == expect);
        {
            struct TagItem bad[2] = { { AAR_TAG_WEIGHT, 9999 }, { TAG_DONE, 0 } };

            CHECK(v_set_tag_list(h, bad) == expect); /* a bad handle before a bad tag */
        }
        CHECK(v_send_message(h, &m) == expect);
        CHECK(v_get_message(h, &m) == expect);
        CHECK(v_shared_map(h, 0x100000, 4, 0, &map) == expect);
        CHECK(v_shared_unmap(h, &map) == expect);
        CHECK(v_shared_publish(h, &map, 0, 1) == expect);
        CHECK(v_shared_acquire(h, &map, 0, 1) == expect);
        CHECK(v_service_next(h, &req) == expect);
        CHECK(v_service_reply(h, &req) == expect);
        CHECK(v_post_event(h, &ev) == expect);
        CHECK(v_audio_read(h, buf, 1, &got) == expect);
        CHECK(v_shared_token(h, buf, 4, &got) == expect);
        CHECK(v_message_signal(h) == (pass == 0 ? T(0)->sig_alloc : 0));
    }
    CHECK(v_status(b, &st) == AAR_OK);
    USE(1);
    lib_close();
    USE(0);
    lib_close();
    finish();
}

static void test_attach_failure(void)
{
    AARInfo info;
    AARHandle *h = (AARHandle *)(uintptr_t)1;

    setup("attach_failure");
    refplat.attach_rc = AAR_E_NOSYS;
    CHECK(lib_open() == AAR_OK); /* the library opens; the platform is what is missing */
    info.size = sizeof(info);
    CHECK(v_query(&info) == AAR_E_NOSYS);
    CHECK(hx_dispatcher_starts == 1 && base.dispatcher_up == 0);
    CHECK(v_open(&h, NULL, NULL) == AAR_E_NOSYS && h == NULL);
    CHECK(popcount(hx_cur->sig_alloc) == 0); /* no bit taken for a failed open */
    refplat.attach_rc = AAR_OK;
    CHECK(v_query(&info) == AAR_OK && base.dispatcher_up == 1);
    CHECK(hx_dispatcher_starts == 3);
    lib_close();
    finish();
}

static void test_tag_list(void)
{
    AARHandle *h;
    static const struct TagItem good[] = {
        { AAR_TAG_PRIVATE_MIB, 64 }, { TAG_IGNORE, 5 }, { AAR_TAG_WEIGHT, 256 },
        { AAR_TAG_SHARED_MIB, 4095 }, { TAG_DONE, 0 }
    };
    static const struct TagItem more[] = { { AAR_TAG_WEIGHT, 7 }, { TAG_DONE, 0 } };
    struct TagItem chain[4];
    struct TagItem bad_last[4], bad_weight[3], bad_shared[3];

    setup("tag_list");
    CHECK(lib_open() == AAR_OK);
    h = open_h(NULL);
    CHECK(v_set_tag_list(h, good) == AAR_OK);
    CHECK(h->private_mib == 64 && h->weight == 256 && h->shared_mib == 4095);
    /* TAG_SKIP and TAG_MORE are walked by the utility.library binding. */
    chain[0] = (struct TagItem){ TAG_SKIP, 1 };
    chain[1] = (struct TagItem){ AAR_TAG_WEIGHT, 999 }; /* skipped */
    chain[2] = (struct TagItem){ TAG_MORE, (ULONG)(uintptr_t)more };
    chain[3] = (struct TagItem){ TAG_DONE, 0 };
    CHECK(v_set_tag_list(h, chain) == AAR_OK && h->weight == 7);
    CHECK(v_set_tag_list(h, NULL) == AAR_OK);
    /* The whole list is checked before any tag is applied. */
    h->private_mib = h->shared_mib = h->weight = 3;
    bad_last[0] = (struct TagItem){ AAR_TAG_PRIVATE_MIB, 100 };
    bad_last[1] = (struct TagItem){ AAR_TAG_WEIGHT, 50 };
    bad_last[2] = (struct TagItem){ AAR_TAG_BASE + 40, 1 }; /* unknown id, last */
    bad_last[3] = (struct TagItem){ TAG_DONE, 0 };
    CHECK(v_set_tag_list(h, bad_last) == AAR_E_INVAL);
    CHECK(h->private_mib == 3 && h->weight == 3 && h->shared_mib == 3);
    bad_weight[0] = (struct TagItem){ AAR_TAG_PRIVATE_MIB, 100 };
    bad_weight[1] = (struct TagItem){ AAR_TAG_WEIGHT, 257 };
    bad_weight[2] = (struct TagItem){ TAG_DONE, 0 };
    CHECK(v_set_tag_list(h, bad_weight) == AAR_E_INVAL && h->private_mib == 3);
    bad_shared[0] = (struct TagItem){ AAR_TAG_PRIVATE_MIB, 100 };
    bad_shared[1] = (struct TagItem){ AAR_TAG_SHARED_MIB, 4096 };
    bad_shared[2] = (struct TagItem){ TAG_DONE, 0 };
    CHECK(v_set_tag_list(h, bad_shared) == AAR_E_INVAL && h->private_mib == 3 && h->shared_mib == 3);
    bad_weight[1] = (struct TagItem){ AAR_TAG_WEIGHT, 0 };
    CHECK(v_set_tag_list(h, bad_weight) == AAR_OK && h->weight == 0 && h->private_mib == 100);
    /* Too late once the load began. */
    {
        static uint8_t img[16];

        CHECK(v_load_memory(h, img, sizeof(img)) == AAR_OK);
        CHECK(v_set_tag_list(h, good) == AAR_E_BUSY);
    }
    lib_close();
    finish();
}

static uint8_t big[200000];

static void test_load_file(void)
{
    AARHandle *h;
    uint32_t i, sum = 0;
    int slot;

    setup("load_file");
    for (i = 0; i < sizeof(big); i++) {
        big[i] = (uint8_t)(i * 13 + 1);
        sum += big[i];
    }
    hx_files[0] = (struct hx_file){ "Work:app.elf", big, 150000, 0, 0 };
    hx_files[1] = (struct hx_file){ "Work:empty.elf", big, 0, 0, 0 };
    hx_files[2] = (struct hx_file){ "Work:huge.elf", big, 0x01000001u, 0, 0 };
    hx_files[3] = (struct hx_file){ "Work:big.elf", big, 200000, 0, 0 };
    CHECK(lib_open() == AAR_OK);
    h = open_h(NULL);
    slot = refplat_slot(h->claim.handle);
    CHECK(v_load_file(h, NULL) == AAR_E_INVAL);
    CHECK(v_load_file(h, "Work:missing.elf") == AAR_E_NOENT);
    CHECK(v_load_file(h, "Work:empty.elf") == AAR_E_INVAL && hx_files_open == 0);
    hx_file_reads = 0;
    CHECK(v_load_file(h, "Work:huge.elf") == AAR_E_INVAL); /* over max_image_bytes */
    CHECK(hx_file_reads == 0 && hx_files_open == 0);
    CHECK(v_load_file(h, "Work:app.elf") == AAR_OK);
    CHECK(refplat.claims[slot].image_len == 150000);
    {
        uint32_t s2 = 0;

        for (i = 0; i < 150000; i++)
            s2 += big[i];
        CHECK(refplat.claims[slot].image_sum == s2);
    }
    CHECK(hx_file_reads == 3 && hx_files_open == 0);
    CHECK(v_load_file(h, "Work:app.elf") == AAR_E_BUSY);
    /* Not the owner: reported before the file is looked for. */
    USE(1);
    CHECK(v_load_file(h, "Work:missing.elf") == AAR_E_OWNER);
    USE(0);
    CHECK(v_close(h) == AAR_OK);

    /* Reads are 64 KiB at most, whatever chunk the platform would take. */
    h = open_h(NULL);
    slot = refplat_slot(h->claim.handle);
    refplat.chunk_max = 0x20000;
    hx_file_reads = 0;
    CHECK(v_load_file(h, "Work:big.elf") == AAR_OK);
    CHECK(hx_file_reads == 4); /* 3 x 64 KiB and the rest */
    CHECK(refplat.claims[slot].chunks == 4 && refplat.claims[slot].image_sum == sum);
    CHECK(v_close(h) == AAR_OK);
    refplat.chunk_max = 0;

    /* Ctrl-C between chunks: AAR_E_IO, the signal stays set, the file is closed. */
    h = open_h(NULL);
    hx_file_reads = 0;
    hx_ctrl_c = 0;
    hx_ctrl_c_after_reads = 1;
    CHECK(v_load_file(h, "Work:app.elf") == AAR_E_IO);
    CHECK(hx_file_reads == 1);       /* one chunk, then the check */
    CHECK(hx_ctrl_c == 1);           /* left set for the caller */
    CHECK(hx_files_open == 0);
    CHECK(v_load_file(h, "Work:app.elf") == AAR_E_BUSY); /* the load began and was never finished */
    hx_ctrl_c = 0;
    hx_ctrl_c_after_reads = 0;
    CHECK(v_close(h) == AAR_OK);

    /* A failing read. */
    h = open_h(NULL);
    hx_file_reads = 0;
    hx_fail_read_at = 2;
    CHECK(v_load_file(h, "Work:app.elf") == AAR_E_IO);
    CHECK(hx_files_open == 0);
    hx_fail_read_at = 0;
    CHECK(v_close(h) == AAR_OK);

    /* The platform refuses at the end. */
    h = open_h(NULL);
    refplat.load_end_rc = AAR_E_NORESOURCE;
    refplat.load_end_reason = AAR_REASON_MEMORY;
    CHECK(v_load_file(h, "Work:app.elf") == AAR_E_NORESOURCE);
    {
        AARStatusInfo st;

        st.size = sizeof(st);
        CHECK(v_status(h, &st) == AAR_OK && st.reason == AAR_REASON_MEMORY);
    }
    refplat.load_end_rc = AAR_OK;
    lib_close();
    finish();
}

static void test_probe(void)
{
    AARImageInfo info, mem;
    uint8_t *half;

    setup("probe");
    CHECK(good_image != NULL && good_len > 64);
    if (!good_image)
        return;
    hx_files[0] = (struct hx_file){ "Work:good.elf", good_image, good_len, 0, 0 };
    half = malloc(good_len / 2);
    memcpy(half, good_image, good_len / 2);
    hx_files[1] = (struct hx_file){ "Work:half.elf", half, good_len / 2, 0, 0 };
    hx_files[2] = (struct hx_file){ "Work:text", (const uint8_t *)"not an elf at all, just text\n", 29, 0, 0 };
    hx_files[3] = (struct hx_file){ "Work:none", good_image, 0, 0, 0 };
    memset(&info, 0xEE, sizeof(info));
    info.size = sizeof(info);
    CHECK(v_probe_file("Work:good.elf", &info) == AAR_OK);
    memset(&mem, 0xEE, sizeof(mem));
    mem.size = sizeof(mem);
    CHECK(v_probe_memory(good_image, good_len, &mem) == AAR_OK);
    CHECK(memcmp(&info, &mem, sizeof(info)) == 0 && info.image_bytes == good_len);
    CHECK(hx_files_open == 0);
    CHECK(v_probe_file("Work:missing", &info) == AAR_E_NOENT);
    CHECK(v_probe_file("Work:half.elf", &info) == AAR_E_INVAL && hx_files_open == 0);
    CHECK(v_probe_file("Work:text", &info) == AAR_E_INVAL);
    CHECK(v_probe_file("Work:none", &info) == AAR_E_INVAL);
    CHECK(v_probe_file(NULL, &info) == AAR_E_INVAL && v_probe_file("Work:good.elf", NULL) == AAR_E_INVAL);
    hx_file_reads = 0;
    hx_fail_read_at = 1;
    CHECK(v_probe_file("Work:good.elf", &info) == AAR_E_IO && hx_files_open == 0);
    hx_fail_read_at = 0;
    memset(&info, 0xEE, sizeof(info));
    info.size = 16; /* a small structure: the call fills that much */
    CHECK(v_probe_file("Work:good.elf", &info) == AAR_OK && info.size == 16);
    CHECK(info.profile == mem.profile && info.groups_required == 0xEEEEEEEEu);
    CHECK(v_probe_memory(NULL, 100, &mem) == AAR_E_INVAL);
    free(half);
}

static void put_keys(AARHandle *h, int n)
{
    aar_event ev;
    int i;

    memset(&ev, 0, sizeof(ev));
    ev.type = AAR_EV_KEY_DOWN;
    for (i = 0; i < n; i++) {
        ev.code = (uint16_t)i;
        CHECK(v_post_event(h, &ev) == AAR_OK);
    }
}

static void test_notify(void)
{
    AARHandle *h1, *h2, *h3;
    uint8_t raw[AAR_MSG_BYTES] = { 9 };
    uint32_t arg[6] = { 0 };
    uint32_t mask;
    struct Message *got;
    unsigned long posts;
    uint8_t entry[AAR_ENTRY_BYTES];
    aar_event ev, ev2;
    int s1, i;

    setup("notify");
    CHECK(lib_open() == AAR_OK);
    h1 = open_h(&ports[0]);
    h2 = open_h(&ports[0]); /* one port may serve several handles */
    s1 = refplat_slot(h1->claim.handle);
    mask = T(0)->sig_alloc;
    shell_pass(&base);
    CHECK(ports[0].n == 0 && T(0)->signalled == 0);
    CHECK(refplat_app_message(s1, raw) == AAR_OK);
    shell_pass(&base);
    CHECK(ports[0].n == 1 && ports[0].queue[0] == &h1->event.message);
    CHECK(T(0)->signals == mask && T(0)->signalled == 1);
    CHECK(h1->event.type == AAR_NOTIFY_MESSAGE && h1->event.handle == (APTR)(uintptr_t)h1);
    CHECK(h1->event.status.size == sizeof(AARStatusInfo) && h1->event.status.state == AAR_STATE_CLAIMED);
    CHECK(h1->event.message.mn_Node.ln_Type == NT_MESSAGE && h1->event.message.mn_Length == sizeof(AAREvent));
    CHECK(h1->event.message.mn_ReplyPort == 0);
    /* One event at a time: while it is on the port the library posts no second
     * (a second PutMsg of the node would abort the fake system). The task is
     * still signalled, so a lost wake cannot stick. */
    posts = ports[0].posts;
    vblanks(5);
    shell_pass(&base);
    CHECK(ports[0].posts == posts && ports[0].n == 1);
    CHECK(T(0)->signalled >= 2);
    /* The client takes it; the next one follows for work still pending. */
    got = hx_getmsg(&ports[0]);
    CHECK(got == &h1->event.message && ports[0].n == 0);
    vblanks(5);
    shell_pass(&base);
    CHECK(ports[0].n == 1);
    got = hx_getmsg(&ports[0]);
    CHECK(aar_core_get_message(&base.core, h1, &(AARMessage){ { 0 } }) == AAR_OK);
    /* Two handles, one port, two events. */
    CHECK(refplat_app_request(refplat_slot(h2->claim.handle), AAR_OP_CONSOLE_WRITE, 0, 1, arg, 0, 0, 0) == AAR_OK);
    shell_pass(&base);
    CHECK(ports[0].n == 1 && ports[0].queue[0] == &h2->event.message);
    CHECK(h2->event.type == AAR_NOTIFY_REQUEST);
    (void)hx_getmsg(&ports[0]);
    /* The end of the app. */
    h3 = open_h(&ports[1]);
    refplat_app_set_state(refplat_slot(h3->claim.handle), AAR_STATE_IDLE, AAR_END_EXIT);
    vblanks(1);
    shell_pass(&base);
    CHECK(ports[1].n == 1 && h3->event.type == AAR_NOTIFY_COMPLETE);
    (void)hx_getmsg(&ports[1]);
    shell_pass(&base);
    CHECK(ports[1].n == 0); /* reported once */
    /* Closing a handle takes its event back off the port. */
    refplat_app_set_state(refplat_slot(h3->claim.handle), AAR_STATE_FAULTED, AAR_END_USER_FAULT);
    vblanks(1);
    shell_pass(&base);
    CHECK(ports[1].n == 0); /* the end reason did not change: still reported */
    refplat_app_set_state(refplat_slot(h3->claim.handle), AAR_STATE_RUNNING, AAR_END_NONE);
    shell_pass(&base);
    refplat_app_set_state(refplat_slot(h3->claim.handle), AAR_STATE_FAULTED, AAR_END_USER_FAULT);
    shell_pass(&base);
    CHECK(ports[1].n == 1 && h3->event.type == AAR_NOTIFY_FAULT);
    CHECK(v_close(h3) == AAR_OK);
    CHECK(ports[1].n == 0); /* the node left the port before its memory went */
    /* The dispatcher flushes a coalesced mouse event the CQ had no room for. */
    put_keys(h2, 64);
    memset(&ev, 0, sizeof(ev));
    ev.type = AAR_EV_MOUSE;
    ev.dx = 4;
    CHECK(v_post_event(h2, &ev) == AAR_OK);
    ev.dx = 5;
    CHECK(v_post_event(h2, &ev) == AAR_OK);
    CHECK(h2->mouse_pending == 1);
    CHECK(refplat_app_cq_pop(refplat_slot(h2->claim.handle), entry) == 1);
    shell_pass(&base);
    CHECK(h2->mouse_pending == 0);
    for (i = 0; i < 64; i++)
        CHECK(refplat_app_cq_pop(refplat_slot(h2->claim.handle), entry) == 1);
    aar_entry_get_event(entry, &ev2);
    CHECK(ev2.type == AAR_EV_MOUSE && ev2.dx == 9);
    lib_close();
    finish();
}

static void test_dead_owner_no_signal(void)
{
    AARHandle *h;
    uint8_t raw[AAR_MSG_BYTES] = { 2 };
    unsigned long posts;

    setup("dead_owner_no_signal");
    USE(1);
    CHECK(lib_open() == AAR_OK);
    h = open_h(&ports[2]);
    posts = ports[2].posts;
    /* The task dies; the reap has not run yet. The dispatcher must neither
     * signal it nor write to its port (the fake system aborts on either). */
    T(1)->listed = 0;
    CHECK(refplat_app_message(refplat_slot(h->claim.handle), raw) == AAR_OK);
    shell_pass(&base);
    vblanks(10);
    shell_pass(&base);
    CHECK(ports[2].posts == posts && T(1)->signalled == 0);
    /* The reap then takes the handle. */
    vblanks(60);
    shell_pass(&base);
    CHECK(refplat_live() == 0 && hx_live_blocks == 0);
    CHECK(base.openers == NULL); /* its record was freed by the reap */
    base.library.lib_OpenCnt = 0;
    USE(0); /* the task that stops it is alive */
    shell_dispatcher_stop(&base);
    finish();
}

static void test_reap(void)
{
    AARHandle *a1, *a2, *b1;
    AARStatusInfo st;
    static char other_name[8];
    struct AAROpener *o;

    setup("reap");
    st.size = sizeof(st);
    USE(1);
    CHECK(lib_open() == AAR_OK);
    a1 = open_h(NULL);
    a2 = open_h(NULL);
    USE(2);
    CHECK(lib_open() == AAR_OK);
    b1 = open_h(NULL);
    USE(0);
    CHECK(lib_open() == AAR_OK);
    shell_pass(&base);
    CHECK(refplat_live() == 3);
    /* Nothing is reaped before the period is over, nor while the owner lives. */
    vblanks(49);
    shell_pass(&base);
    CHECK(refplat_live() == 3);
    vblanks(60);
    shell_pass(&base);
    CHECK(refplat_live() == 3);
    /* Address reuse: another task now has the owner's address. */
    T(1)->name = other_name;
    vblanks(49);
    shell_pass(&base);
    CHECK(refplat_live() == 3); /* the next pass is at 50 */
    vblanks(1);
    shell_pass(&base);
    CHECK(refplat_live() == 1); /* a1 and a2 are reaped: STOP-less release of idle claims */
    CHECK(a1->state == AAR_H_DEAD && a2->state == AAR_H_DEAD);
    CHECK(hx_live_blocks > 3); /* the tombstones are still there */
    /* The new occupant of the address cannot use the old pointers: they are stale, not "other's". */
    USE(1);
    CHECK(v_status(a1, &st) == AAR_E_STALE);
    CHECK(v_close(a2) == AAR_E_STALE);
    for (o = base.openers; o; o = o->next)
        CHECK(o->stamp.id != T(1) || o->stamp.name != other_name);
    /* Live owners go on. */
    USE(2);
    CHECK(v_status(b1, &st) == AAR_OK);
    /* The task is gone for good: the tombstones are freed at the next pass. */
    T(1)->listed = 0;
    vblanks(50);
    shell_pass(&base);
    CHECK(hx_live_blocks == 3); /* b1's handle and the opener records of tasks 0 and 2 */
    USE(2);
    lib_close();
    USE(0);
    lib_close();
    /* The owner of the reaped handles never closed the library: its count stays
     * (as zz9k.library's does), so the dispatcher runs on. */
    CHECK(base.dispatcher_up == 1);
    CHECK(base.library.lib_OpenCnt == 1);
    base.library.lib_OpenCnt = 0;
    shell_final_close(&base);
    finish();
}

static void test_rename(void)
{
    AARHandle *h;
    AARStatusInfo st;
    static char new_name[8];

    setup("rename");
    st.size = sizeof(st);
    CHECK(lib_open() == AAR_OK);
    USE(1);
    CHECK(lib_open() == AAR_OK); /* another task keeps the library open */
    USE(0);
    h = open_h(NULL);
    shell_pass(&base);
    /* A client must not rename its task while it owns a handle: the library
     * takes the new name for another task, and loses the app (spec, T11). */
    T(0)->name = new_name;
    vblanks(50);
    shell_pass(&base);
    CHECK(refplat_live() == 0);
    CHECK(v_status(h, &st) == AAR_E_STALE);
    /* Its last close frees the tombstone. */
    T(0)->name = &names[0];
    CHECK(hx_live_blocks == 2); /* the tombstone and task 1's record; the reap dropped task 0's */
    lib_close();
    CHECK(hx_live_blocks == 1); /* the task's last close freed the tombstone */
    USE(1);
    lib_close();
    finish();
}

static void test_lease_gate(void)
{
    AARHandle *h;
    uint32_t before, after;
    int i, moved;

    setup("lease_gate");
    CHECK(lib_open() == AAR_OK);
    h = open_h(NULL);
    shell_pass(&base);
    /* The owner is blocked for a long time (it never runs): the interrupt
     * renews, the dispatcher keeps stamping, the claim stays alive. */
    before = lease_of(h);
    moved = 0;
    for (i = 0; i < 500; i++) {
        shell_vblank(&base);
        shell_pass(&base);
        if (lease_of(h) != before) {
            moved++;
            before = lease_of(h);
        }
    }
    CHECK(moved >= 99); /* one store per 5 ticks */
    /* The dispatcher stalls: the interrupt stops after about 5 s (250 ticks). */
    shell_pass(&base);
    vblanks(255);
    after = lease_of(h);
    vblanks(40);
    CHECK(lease_of(h) == after);
    /* It comes back. */
    shell_pass(&base);
    vblanks(5);
    CHECK(lease_of(h) != after);
    lib_close();
    finish();
}

static uint8_t *slurp(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *p;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = malloc((size_t)n);
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        p = NULL;
    }
    fclose(f);
    *len = (uint32_t)n;
    return p;
}

static void test_ext(void)
{
    const aar_client_ext_zz9000 *ext;
    AARHandle *h;
    AARZZ9000Stats stats;

    setup("ext");
    CHECK(lib_open() == AAR_OK);
    refplat.attach_rc = AAR_E_NOSYS;
    CHECK(v_ext(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_CLIENT) == 0); /* no platform: no table */
    refplat.attach_rc = AAR_OK;
    h = open_h(NULL);
    ext = (const aar_client_ext_zz9000 *)(uintptr_t)v_ext(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_CLIENT);
    CHECK(ext != NULL && AAR_CLIENT_EXT_ZZ9000_COVERS(ext, set_log_sink));
    CHECK(v_ext(AAR_PLATFORM_ZZ9000, 2) == 0 && v_ext(AAR_PLATFORM_NONE, 1) == 0);
    stats.size = sizeof(stats);
    CHECK(ext && ext->stats(h, &stats) == AAR_OK && stats.maps >= 2);
    USE(1);
    CHECK(ext && ext->stats(h, &stats) == AAR_E_OWNER);
    USE(0);
    lib_close();
    finish();
}

/*
 * What the shell allocates it gives back: 500 open and close pairs in one task
 * (some with a notify port, some armed), 300 probes, and 200 tasks that open the
 * library, open and close a handle and close the library again. The allocator
 * is net zero in bytes at each end (the card test T3 measured AvailMem).
 */
static void test_alloc_balance(void)
{
    AARHandle *h;
    AARImageInfo info;
    unsigned long bytes0;
    unsigned i;

    setup("alloc_balance");
    hx_files[0] = (struct hx_file){ "Work:good.elf", good_image, good_len, 0, 0 };
    CHECK(lib_open() == AAR_OK);
    h = open_h(NULL); /* the warm-up: the dispatcher, the task's record and its signal bit */
    CHECK(v_close(h) == AAR_OK);
    bytes0 = hx_live_bytes;
    for (i = 0; i < 500; i++) {
        h = open_h(i % 3 == 0 ? &ports[0] : NULL);
        if (i % 2)
            CHECK(v_message_signal(h) != 0);
        if (i % 7 == 0) {
            vblanks(1);
            shell_pass(&base);
        }
        CHECK(v_close(h) == AAR_OK);
    }
    CHECK(hx_live_bytes == bytes0);
    info.size = sizeof(info);
    for (i = 0; i < 300; i++)
        CHECK(v_probe_file("Work:good.elf", &info) == AAR_OK);
    CHECK(hx_live_bytes == bytes0 && hx_files_open == 0);
    lib_close();
    CHECK(hx_live_bytes == 0 && hx_live_blocks == 0);
    for (i = 0; i < 200; i++) {
        USE(i % 4);
        CHECK(lib_open() == AAR_OK);
        h = open_h(NULL);
        CHECK(v_close(h) == AAR_OK);
        lib_close();
        CHECK(T(i % 4)->sig_alloc == 0);
    }
    CHECK(hx_live_bytes == 0 && hx_live_blocks == 0);
    finish();
}

/*
 * A handle the platform has revoked (administrative release, lease lapse) is
 * still in the registry until AARClose. Every call answers AAR_E_STALE before
 * it looks at arguments, opens a file or asks the platform. AARClose is the
 * exception: it finishes the release and returns AAR_OK. Another task still
 * gets AAR_E_OWNER, which is checked first.
 */
static void test_revoked_codes(void)
{
    AARHandle *h;
    AARStatusInfo st;
    AARMessage msg;
    AARServiceReq req;
    AARImageInfo image;
    struct AARSharedMap map;
    aar_event ev;
    struct TagItem tags[2];
    const char *argv[1] = { "x" };
    const aar_client_ext_zz9000 *ext;
    ULONG got = 0, token = 0;
    char buf[8];
    static uint8_t bytes[8];
    int slot;

    setup("revoked_codes");
    CHECK(lib_open() == AAR_OK);
    h = open_h(&ports[0]);
    slot = refplat_slot(h->claim.handle);
    ext = (const aar_client_ext_zz9000 *)(uintptr_t)v_ext(AAR_PLATFORM_ZZ9000,
                                                         AAR_EXT_ZZ9000_CLIENT);
    CHECK(ext != NULL);
    refplat_revoke(slot);
    /* The first call that reaches the platform learns the claim is gone. */
    CHECK(v_stop(h) == AAR_E_STALE);
    memset(&st, 0, sizeof(st));
    memset(&req, 0, sizeof(req));
    memset(&map, 0, sizeof(map));
    memset(&image, 0, sizeof(image));
    memset(&ev, 0, sizeof(ev));
    st.size = sizeof(st);
    req.size = sizeof(req);
    map.size = sizeof(map);
    image.size = sizeof(image);
    tags[0] = (struct TagItem){ AAR_TAG_WEIGHT, 9999 }; /* a bad tag must not win */
    tags[1] = (struct TagItem){ TAG_DONE, 0 };
    CHECK(v_set_tag_list(h, tags) == AAR_E_STALE);
    CHECK(v_load_file(h, "no-such-file") == AAR_E_STALE); /* not AAR_E_NOENT */
    CHECK(v_load_memory(h, NULL, 0) == AAR_E_STALE);      /* not AAR_E_INVAL */
    CHECK(v_start(h, NULL, 1) == AAR_E_STALE);
    CHECK(v_status(h, NULL) == AAR_E_STALE);
    CHECK(v_send_message(h, NULL) == AAR_E_STALE);
    CHECK(v_get_message(h, NULL) == AAR_E_STALE);
    CHECK(v_shared_map(h, 0, 0, 1, NULL) == AAR_E_STALE);
    CHECK(v_shared_unmap(h, NULL) == AAR_E_STALE);
    CHECK(v_shared_publish(h, NULL, 0, 0) == AAR_E_STALE);
    CHECK(v_shared_acquire(h, NULL, 0, 0) == AAR_E_STALE);
    CHECK(v_service_next(h, NULL) == AAR_E_STALE);
    CHECK(v_service_reply(h, NULL) == AAR_E_STALE);
    CHECK(v_post_event(h, NULL) == AAR_E_STALE);
    CHECK(v_audio_read(h, NULL, 1, NULL) == AAR_E_STALE); /* not AAR_E_INVAL */
    CHECK(v_shared_token(h, NULL, 0, NULL) == AAR_E_STALE);
    CHECK(v_probe_file("no-such", &image) == AAR_E_NOENT); /* no handle: unchanged */
    CHECK(v_probe_memory(bytes, 8, &image) == AAR_E_INVAL);
    CHECK(ext->stats(h, &(AARZZ9000Stats){ .size = sizeof(AARZZ9000Stats) }) == AAR_E_STALE);
    CHECK(ext->lease_pause(h, 0) == AAR_E_STALE);
    CHECK(ext->set_log_sink(h, NULL, 0) == AAR_E_STALE);
    /* Owner is checked before stale. */
    USE(1);
    CHECK(v_load_file(h, "no-such-file") == AAR_E_OWNER);
    CHECK(v_audio_read(h, buf, 1, &got) == AAR_E_OWNER);
    CHECK(v_shared_token(h, buf, 4, &token) == AAR_E_OWNER);
    CHECK(ext->stats(h, &(AARZZ9000Stats){ .size = sizeof(AARZZ9000Stats) }) == AAR_E_OWNER);
    USE(0);
    /* Close finishes the release. */
    CHECK(v_close(h) == AAR_OK);
    CHECK(v_status(h, &st) == AAR_E_STALE);
    CHECK(v_load_file(h, "no-such-file") == AAR_E_STALE);
    lib_close();
    finish();
    (void)argv;
    (void)req;
    (void)msg;
}

/*
 * The rule: a notification counts as done only when it has been delivered. The
 * end of the app is announced by an event that carries the terminal status; if
 * the port still holds an earlier event, nothing is posted, and the end stays
 * pending until an event with the terminal status has gone onto the port.
 */
static void test_notify_terminal(void)
{
    AARHandle *h, *f;
    AARMessage m;
    uint8_t raw[AAR_MSG_BYTES] = { 7 };
    struct Message *got;
    unsigned long posts;
    int s, sf;

    setup("notify_terminal");
    CHECK(lib_open() == AAR_OK);
    h = open_h(&ports[0]);
    s = refplat_slot(h->claim.handle);
    CHECK(refplat_app_message(s, raw) == AAR_OK);
    shell_pass(&base);
    CHECK(ports[0].n == 1 && h->event.type == AAR_NOTIFY_MESSAGE);
    CHECK(h->event.status.end_reason == AAR_END_NONE);
    /* The app ends while that event is still on the port: nothing can be posted. */
    refplat_app_set_state(s, AAR_STATE_IDLE, AAR_END_EXIT);
    posts = ports[0].posts;
    vblanks(1);
    shell_pass(&base);
    CHECK(ports[0].posts == posts && ports[0].n == 1);
    CHECK(h->ended_notified == 0); /* not done: nothing carrying the end has been posted */
    /* The client reads the message and takes the old event: the end follows. */
    CHECK(v_get_message(h, &m) == AAR_OK);
    got = hx_getmsg(&ports[0]);
    CHECK(got == &h->event.message && ports[0].n == 0);
    vblanks(1);
    shell_pass(&base);
    CHECK(ports[0].n == 1 && h->event.type == AAR_NOTIFY_COMPLETE);
    CHECK(h->event.status.end_reason == AAR_END_EXIT && h->event.status.state == AAR_STATE_IDLE);
    CHECK(h->ended_notified == 1);
    (void)hx_getmsg(&ports[0]);
    vblanks(10);
    shell_pass(&base);
    CHECK(ports[0].n == 0); /* once */
    /* A fault end, the same way: an event of another kind is on the port. */
    f = open_h(&ports[1]);
    sf = refplat_slot(f->claim.handle);
    CHECK(refplat_app_message(sf, raw) == AAR_OK);
    shell_pass(&base);
    CHECK(ports[1].n == 1 && f->event.type == AAR_NOTIFY_MESSAGE);
    refplat_app_set_state(sf, AAR_STATE_FAULTED, AAR_END_USER_FAULT);
    CHECK(v_get_message(f, &m) == AAR_OK);
    shell_pass(&base);
    CHECK(ports[1].n == 1 && f->ended_notified == 0);
    (void)hx_getmsg(&ports[1]);
    shell_pass(&base);
    CHECK(ports[1].n == 1 && f->event.type == AAR_NOTIFY_FAULT);
    CHECK(f->event.status.end_reason == AAR_END_USER_FAULT && f->ended_notified == 1);
    (void)hx_getmsg(&ports[1]);
    CHECK(v_close(h) == AAR_OK);
    CHECK(v_close(f) == AAR_OK);
    lib_close();
    finish();
}

/*
 * Lifecycle states are not reused for two meanings. The records of a task that
 * is gone are reclaimed whatever state they were left in: an opener record
 * hidden because its task address was reused, one left "closing" by a closer
 * that died, a handle left closing by one that died in AARClose.
 */
static unsigned openers_of(const void *id)
{
    const struct AAROpener *o;
    unsigned n = 0;

    for (o = base.openers; o; o = o->next)
        n += o->stamp.id == id;
    return n;
}

static void test_opener_states(void)
{
    AARHandle *a, *c;
    AARStatusInfo st;
    static char other_name[8];

    setup("opener_states");
    st.size = sizeof(st);
    /* 1. A task is gone and another has its address; the new one opens. */
    USE(1);
    CHECK(lib_open() == AAR_OK);
    a = open_h(NULL);
    shell_pass(&base);
    T(1)->name = other_name;
    CHECK(lib_open() == AAR_OK);
    CHECK(openers_of(T(1)) == 1); /* the old record is reclaimed, not hidden */
    CHECK(refplat_live() == 1);
    vblanks(50);
    shell_pass(&base);
    CHECK(refplat_live() == 0 && a->state == AAR_H_DEAD); /* its handle is reaped as a dead owner's */
    CHECK(v_status(a, &st) == AAR_E_STALE);
    /* The new task closes the library: nothing of the old task is left. */
    base.library.lib_OpenCnt = 1;
    (void)shell_close(&base);
    CHECK(openers_of(T(1)) == 0);
    CHECK(v_close(a) == AAR_E_STALE);
    T(1)->listed = 0;
    vblanks(50);
    shell_pass(&base);
    CHECK(hx_live_blocks == 0);
    /* 2. A closer died after it marked its record closing: the record goes
     * with its task. */
    setup("opener_states");
    USE(2);
    CHECK(lib_open() == AAR_OK);
    CHECK(openers_of(T(2)) == 1);
    base.openers->closing = 1;
    T(2)->listed = 0;
    vblanks(50);
    shell_pass(&base);
    CHECK(openers_of(T(2)) == 0);
    /* 3. A closer died inside AARClose: the handle is closing for good. It is
     * taken like any other handle of a dead owner. */
    USE(3);
    CHECK(lib_open() == AAR_OK);
    c = open_h(NULL);
    CHECK(refplat_live() == 1);
    c->state = AAR_H_CLOSING;
    T(3)->listed = 0;
    vblanks(50);
    shell_pass(&base);
    CHECK(refplat_live() == 0); /* released; the task is absent, so the tombstone went too */
    CHECK(hx_live_blocks == 0 && base.openers == NULL);
    base.library.lib_OpenCnt = 0;
    USE(0); /* the closer of the library is alive */
    shell_final_close(&base);
    finish();
}

/*
 * The dispatcher process must be gone, with its interrupt, whenever the library
 * is closed for the last time or expunged, whatever the closing task has left:
 * the stop does not need a signal bit of its own (SIGF_SINGLE), and the start
 * does not either. A wake without a reason, or a break from outside, changes
 * nothing.
 */
static void test_dispatcher(void)
{
    AARInfo info;
    AARHandle *h;
    unsigned long attaches, installs;

    setup("dispatcher");
    info.size = sizeof(info);
    /* 1. The last closer started the dispatcher through AARQuery: it has
     * opened no handle and has no free signal bit. */
    CHECK(lib_open() == AAR_OK);
    CHECK(v_query(&info) == AAR_OK);
    CHECK(base.dispatcher_up == 1 && base.disp_process != NULL && base.vblank_installed == 1);
    CHECK(base.disp_bit >= 0 && hx_tasks[HX_DISP].sig_alloc == (1u << base.disp_bit));
    CHECK(hx_cur->sig_alloc == 0); /* neither start nor stop took a bit of the caller */
    hx_cur->sig_alloc = 0xFFFFFFFFu;
    lib_close();
    CHECK(base.disp_process == NULL && base.dispatcher_up == 0 && base.vblank_installed == 0);
    CHECK(refplat.detaches == 1); /* the dispatcher let go of the platform before it ended */
    CHECK(hx_tasks[HX_DISP].sig_alloc == 0 && hx_tasks[HX_DISP].listed == 0);
    CHECK(hx_forbid == 0 && hx_dispatcher_stops == 1);
    CHECK(hx_cur->sig_alloc == 0xFFFFFFFFu); /* its bits are untouched */
    hx_cur->sig_alloc = 0;
    /* Stopping what is not running waits for nothing. */
    {
        unsigned long waits = hx_waits;

        shell_dispatcher_stop(&base);
        CHECK(hx_waits == waits);
    }
    /* 2. A wake without a reason does not end either wait early. */
    hx_spurious = 2;
    CHECK(lib_open() == AAR_OK);
    CHECK(v_query(&info) == AAR_OK && base.dispatcher_up == 1);
    hx_spurious = 1;
    lib_close();
    CHECK(base.disp_process == NULL && base.dispatcher_up == 0 && base.vblank_installed == 0);
    CHECK(hx_spurious == 0 && hx_dispatcher_stops == 2);
    /* 3. A handle held to the end: the final close stops the dispatcher first,
     * then closes what is left. */
    CHECK(lib_open() == AAR_OK);
    h = open_h(NULL);
    CHECK(h != NULL && base.dispatcher_up == 1);
    CHECK(hx_cur->sig_alloc != 0);
    base.library.lib_OpenCnt = 0;
    shell_final_close(&base);
    CHECK(base.disp_process == NULL && refplat_live() == 0);
    (void)shell_close(&base);
    CHECK(hx_cur->sig_alloc == 0); /* the task's own bit went at its close */
    /* 4. The dispatcher cannot start: no process, or no signal bit for it. */
    attaches = refplat.attaches;
    installs = hx_vblank_installs;
    hx_fail_spawn = 1;
    CHECK(lib_open() == AAR_OK);
    CHECK(v_query(&info) == AAR_E_NOMEM && base.disp_process == NULL && base.dispatcher_up == 0);
    hx_disp_sigs = 0xFFFFFFFFu;
    CHECK(v_query(&info) == AAR_E_NOMEM);
    CHECK(base.disp_process == NULL && base.dispatcher_up == 0 && hx_forbid == 0);
    CHECK(refplat.attaches == attaches && base.vblank_installed == 0 && hx_vblank_installs == installs);
    hx_disp_sigs = 0;
    CHECK(v_query(&info) == AAR_OK && base.dispatcher_up == 1);
    /* 5. A break from outside ends the process with nobody waiting for it:
     * nothing is signalled, and the next call starts it again. */
    {
        struct hx_task *self = hx_cur;

        hx_bg = NULL;
        hx_cur = &hx_tasks[HX_DISP];
        shell_disp_end(&base);
        hx_process_gone();
        hx_cur = self;
    }
    CHECK(base.disp_process == NULL && base.dispatcher_up == 0 && base.vblank_installed == 0);
    CHECK(v_query(&info) == AAR_OK && base.dispatcher_up == 1 && hx_vblank_installs == installs + 2);
    /* 6. Expunge: a dispatcher that is running when the base is about to be
     * freed is stopped first; an interrupt that cannot be removed stops the
     * expunge. */
    base.library.lib_OpenCnt = 0;
    (void)shell_close(&base);
    hx_vblank_stuck = 1;
    CHECK(shell_expunge_ready(&base) == 0);
    CHECK(base.disp_process == NULL && base.vblank_installed == 1); /* the process is gone, the interrupt is not */
    hx_vblank_stuck = 0;
    base.vblank_installed = 0;
    CHECK(shell_expunge_ready(&base) == 1);
    finish();
}

int main(int argc, char **argv)
{
    uint32_t len = 0;
    uint8_t *img = argc > 1 ? slurp(argv[1], &len) : NULL;

    if (!img) {
        printf("shell_test: needs GOOD_IMAGE as its argument\n");
        return 2;
    }
    good_image = img;
    good_len = len;
    test_lifecycle();
    test_dispatcher();
    test_last_close();
    test_owner_stale();
    test_attach_failure();
    test_tag_list();
    test_load_file();
    test_probe();
    test_notify();
    test_notify_terminal();
    test_dead_owner_no_signal();
    test_reap();
    test_opener_states();
    test_rename();
    test_lease_gate();
    test_ext();
    test_revoked_codes();
    test_alloc_balance();
    printf("shell_test: %lu checks, %lu failures\n", checks, failures);
    free(img);
    return failures ? 1 : 0;
}
