/*
 * Host test of the aar.library headers: structure layouts, constants, the
 * prototypes the .fd file generates, the ZZ9000 extension and the platform
 * table.
 *
 * The layouts are checked against explicit offsets written down here from the
 * 68k rules of spec 1.3 (big-endian, LONG/ULONG/APTR of 4 bytes, alignment of
 * at most 2), not against what the host compiler happens to produce. Each
 * structure is also walked field by field: every offset must equal the end of
 * the previous field rounded to the 68k alignment, so an offset that was typed
 * wrong or a field that moved is caught even when the host would pad the same
 * way. The stub headers in client/tests/stubs give ULONG and APTR 32 bits, so
 * the host compiler lays the structures out the way the 68k does. The build
 * line is in client/tests/run_host.sh. Build it for 64-bit and for -m32.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include <proto/aar.h>
#include <libraries/aar_zz9000.h>

#include "../src/aar_plat.h"

static unsigned long checks;
static unsigned long failures;

#define CHECK_EQ(actual, expected)                                       \
    do {                                                                 \
        unsigned long long a_ = (unsigned long long)(actual);            \
        unsigned long long e_ = (unsigned long long)(expected);          \
        checks++;                                                        \
        if (a_ != e_) {                                                  \
            failures++;                                                  \
            printf("FAIL %s:%d: %s is %llu, expected %llu\n", __FILE__,  \
                   __LINE__, #actual, a_, e_);                           \
        }                                                                \
    } while (0)

/* ---- Offsets, from spec 1.3 and the design (5.3) ------------------------- */

/* AARInfo, 128 bytes. */
#define OFF_INFO_SIZE 0
#define OFF_INFO_API_MAJOR 4
#define OFF_INFO_API_MINOR 8
#define OFF_INFO_PLATFORM_ID 12
#define OFF_INFO_PROFILES 16
#define OFF_INFO_GROUPS 20
#define OFF_INFO_FLAGS 24
#define OFF_INFO_MAX_APPS 28
#define OFF_INFO_MAX_IMAGE_BYTES 32
#define OFF_INFO_MAX_ARGS 36
#define OFF_INFO_MAX_ARG_BYTES 40
#define OFF_INFO_SHARED_GRANULE 44
#define OFF_INFO_MSG_CAPACITY 48
#define OFF_INFO_STALL_LIMIT_MS 52
#define OFF_INFO_STOP_GRACE_MS 56
#define OFF_INFO_SHARED_DEFAULT_MIB 60
#define OFF_INFO_SHARED_MAX_MIB 64
#define OFF_INFO_WEIGHT_MAX 68
#define OFF_INFO_QUEUE_DEPTH 72
#define OFF_INFO_RESERVED0 76
#define OFF_INFO_PLATFORM 80
#define OFF_INFO_RESERVED1 112
#define SIZE_INFO 128

/* AARImageInfo, 48 bytes. */
#define OFF_IMG_SIZE 0
#define OFF_IMG_PROFILE 4
#define OFF_IMG_API_MAJOR 8
#define OFF_IMG_API_MINOR_REQUIRED 12
#define OFF_IMG_STACK_BYTES 16
#define OFF_IMG_MIN_HEAP_MIB 20
#define OFF_IMG_GROUPS_REQUIRED 24
#define OFF_IMG_GROUPS_OPTIONAL 28
#define OFF_IMG_NOTE_VERSION 32
#define OFF_IMG_IMAGE_BYTES 36
#define OFF_IMG_RESERVED 40
#define SIZE_IMG 48

/* AARStatusInfo, 160 bytes. */
#define OFF_ST_SIZE 0
#define OFF_ST_STATE 4
#define OFF_ST_GENERATION 8
#define OFF_ST_END_REASON 12
#define OFF_ST_EXIT_CODE 16
#define OFF_ST_FLAGS 20
#define OFF_ST_PROFILE 24
#define OFF_ST_FAULT_CLASS 28
#define OFF_ST_FAULT_SYNDROME 32
#define OFF_ST_FAULT_PSTATE 36
#define OFF_ST_FAULT_PC 40
#define OFF_ST_FAULT_SP 48
#define OFF_ST_FAULT_LR 56
#define OFF_ST_FAULT_ADDR 64
#define OFF_ST_LOAD_BASE 72
#define OFF_ST_REASON 80
#define OFF_ST_LEASE_PERIOD_MS 84
#define OFF_ST_MSG_CONSUMED 88
#define OFF_ST_RESERVED 92
#define OFF_ST_PROGRESS 96
#define OFF_ST_SHARED_USED 104
#define OFF_ST_PRIVATE_COMMITTED 112
#define OFF_ST_SQ_FULL 120
#define OFF_ST_LOG_DROPPED 124
#define OFF_ST_EVENTS_DROPPED 128
#define OFF_ST_CQ_FULL 132
#define OFF_ST_STACK_SP 136
#define OFF_ST_STACK_BYTES 144
#define OFF_ST_PROFILE_HZ 148
#define OFF_ST_PROFILE_SAMPLES 152
#define OFF_ST_PROFILE_DROPPED 156
#define SIZE_ST 160

/* AAREvent, 188 bytes. struct Message is 20 bytes: a Node of 14 (succ 0,
 * pred 4, type 8, pri 9, name 10), the reply port at 14 and the length at 18. */
#define OFF_EV_MESSAGE 0
#define OFF_EV_TYPE 20
#define OFF_EV_HANDLE 24
#define OFF_EV_STATUS 28
#define SIZE_EV 188
#define SIZE_MESSAGE 20
#define OFF_MSG_NODE 0
#define SIZE_NODE 14
#define OFF_MSG_REPLY_PORT 14
#define OFF_MSG_LENGTH 18

/* AARSharedMap, 32 bytes. */
#define OFF_MAP_SIZE 0
#define OFF_MAP_BASE 4
#define OFF_MAP_BYTES 8
#define OFF_MAP_HADDR 12
#define OFF_MAP_FLAGS 16
#define OFF_MAP_GRANULE 20
#define OFF_MAP_RESERVED 24
#define SIZE_MAP 32

/* AARServiceReq, 96 bytes. */
#define OFF_REQ_SIZE 0
#define OFF_REQ_OP 4
#define OFF_REQ_ID 8
#define OFF_REQ_RESULT 12
#define OFF_REQ_ARG 16
#define OFF_REQ_OUT 40
#define OFF_REQ_BUF 64
#define OFF_REQ_BUF_OFF 68
#define OFF_REQ_BUF_LEN 72
#define OFF_REQ_FLAGS 76
#define OFF_REQ_REPLY_FLAGS 80
#define OFF_REQ_PTR 84
#define OFF_REQ_RESERVED 88
#define SIZE_REQ 96

/* The ZZ9000 structures. */
#define OFF_LIST_SIZE 0
#define OFF_LIST_HANDLE 4
#define OFF_LIST_STATE 8
#define OFF_LIST_COMMITTED 12
#define OFF_LIST_INDEX 16
#define OFF_LIST_CPU_MS 20
#define OFF_LIST_SERVICE_MS 24
#define OFF_LIST_BLOCK_REASON 28
#define OFF_LIST_END_REASON 32
#define OFF_LIST_FLAGS 36
#define OFF_LIST_NAME 40
#define SIZE_LIST 64
#define SIZE_STATS 68

/* ---- The field walk ------------------------------------------------------ */

/*
 * A field of a structure, with the offset the table above gives it. The walk
 * checks, for one structure: the first field is at 0, each field starts at the
 * end of the one before it (the alignment of every field here is 2 or less on
 * the 68k and every field size is even, so no padding appears), and the last
 * field ends at the structure size. It also checks the host's own offsetof.
 */
struct field {
    const char *name;
    unsigned long offset;      /* offsetof on this host */
    unsigned long expected;    /* the number written above */
    unsigned long size;        /* bytes of the field */
};

#define F(type, name, expected) \
    { #type "." #name, (unsigned long)offsetof(type, name), (expected), \
      (unsigned long)sizeof(((type *)0)->name) }

static void walk(const char *what, const struct field *f, unsigned n,
                 unsigned long total, unsigned long host_size)
{
    unsigned long end = 0;
    unsigned i;

    for (i = 0; i < n; i++) {
        unsigned long align = f[i].size >= 2 ? 2 : 1; /* 68k: at most 2 */

        end = (end + align - 1) & ~(align - 1);
        checks++;
        if (f[i].expected != end) {
            failures++;
            printf("FAIL %s: written offset %lu, 68k rules give %lu\n",
                   f[i].name, f[i].expected, end);
        }
        checks++;
        if (f[i].offset != f[i].expected) {
            failures++;
            printf("FAIL %s: header offset %lu, expected %lu\n", f[i].name,
                   f[i].offset, f[i].expected);
        }
        end += f[i].size;
    }
    checks++;
    if (end != total) {
        failures++;
        printf("FAIL %s: fields end at %lu, expected size %lu\n", what, end, total);
    }
    checks++;
    if (host_size != total) {
        failures++;
        printf("FAIL %s: sizeof is %lu, expected %lu\n", what, host_size, total);
    }
}

#define WALK(what, fields, total, type) \
    walk(what, fields, (unsigned)(sizeof(fields) / sizeof((fields)[0])), total, \
         (unsigned long)sizeof(type))

static void test_layouts(void)
{
    static const struct field info[] = {
        F(AARInfo, size, OFF_INFO_SIZE),
        F(AARInfo, api_major, OFF_INFO_API_MAJOR),
        F(AARInfo, api_minor, OFF_INFO_API_MINOR),
        F(AARInfo, platform_id, OFF_INFO_PLATFORM_ID),
        F(AARInfo, profiles, OFF_INFO_PROFILES),
        F(AARInfo, groups, OFF_INFO_GROUPS),
        F(AARInfo, flags, OFF_INFO_FLAGS),
        F(AARInfo, max_apps, OFF_INFO_MAX_APPS),
        F(AARInfo, max_image_bytes, OFF_INFO_MAX_IMAGE_BYTES),
        F(AARInfo, max_args, OFF_INFO_MAX_ARGS),
        F(AARInfo, max_arg_bytes, OFF_INFO_MAX_ARG_BYTES),
        F(AARInfo, shared_granule, OFF_INFO_SHARED_GRANULE),
        F(AARInfo, msg_capacity, OFF_INFO_MSG_CAPACITY),
        F(AARInfo, stall_limit_ms, OFF_INFO_STALL_LIMIT_MS),
        F(AARInfo, stop_grace_ms, OFF_INFO_STOP_GRACE_MS),
        F(AARInfo, shared_default_mib, OFF_INFO_SHARED_DEFAULT_MIB),
        F(AARInfo, shared_max_mib, OFF_INFO_SHARED_MAX_MIB),
        F(AARInfo, weight_max, OFF_INFO_WEIGHT_MAX),
        F(AARInfo, queue_depth, OFF_INFO_QUEUE_DEPTH),
        F(AARInfo, reserved0, OFF_INFO_RESERVED0),
        F(AARInfo, platform, OFF_INFO_PLATFORM),
        F(AARInfo, reserved1, OFF_INFO_RESERVED1),
    };
    static const struct field img[] = {
        F(AARImageInfo, size, OFF_IMG_SIZE),
        F(AARImageInfo, profile, OFF_IMG_PROFILE),
        F(AARImageInfo, api_major, OFF_IMG_API_MAJOR),
        F(AARImageInfo, api_minor_required, OFF_IMG_API_MINOR_REQUIRED),
        F(AARImageInfo, stack_bytes, OFF_IMG_STACK_BYTES),
        F(AARImageInfo, min_heap_mib, OFF_IMG_MIN_HEAP_MIB),
        F(AARImageInfo, groups_required, OFF_IMG_GROUPS_REQUIRED),
        F(AARImageInfo, groups_optional, OFF_IMG_GROUPS_OPTIONAL),
        F(AARImageInfo, note_version, OFF_IMG_NOTE_VERSION),
        F(AARImageInfo, image_bytes, OFF_IMG_IMAGE_BYTES),
        F(AARImageInfo, reserved, OFF_IMG_RESERVED),
    };
    static const struct field st[] = {
        F(AARStatusInfo, size, OFF_ST_SIZE),
        F(AARStatusInfo, state, OFF_ST_STATE),
        F(AARStatusInfo, generation, OFF_ST_GENERATION),
        F(AARStatusInfo, end_reason, OFF_ST_END_REASON),
        F(AARStatusInfo, exit_code, OFF_ST_EXIT_CODE),
        F(AARStatusInfo, flags, OFF_ST_FLAGS),
        F(AARStatusInfo, profile, OFF_ST_PROFILE),
        F(AARStatusInfo, fault_class, OFF_ST_FAULT_CLASS),
        F(AARStatusInfo, fault_syndrome, OFF_ST_FAULT_SYNDROME),
        F(AARStatusInfo, fault_pstate, OFF_ST_FAULT_PSTATE),
        F(AARStatusInfo, fault_pc, OFF_ST_FAULT_PC),
        F(AARStatusInfo, fault_sp, OFF_ST_FAULT_SP),
        F(AARStatusInfo, fault_lr, OFF_ST_FAULT_LR),
        F(AARStatusInfo, fault_addr, OFF_ST_FAULT_ADDR),
        F(AARStatusInfo, load_base, OFF_ST_LOAD_BASE),
        F(AARStatusInfo, reason, OFF_ST_REASON),
        F(AARStatusInfo, lease_period_ms, OFF_ST_LEASE_PERIOD_MS),
        F(AARStatusInfo, msg_consumed, OFF_ST_MSG_CONSUMED),
        F(AARStatusInfo, reserved, OFF_ST_RESERVED),
        F(AARStatusInfo, progress, OFF_ST_PROGRESS),
        F(AARStatusInfo, shared_used, OFF_ST_SHARED_USED),
        F(AARStatusInfo, private_committed, OFF_ST_PRIVATE_COMMITTED),
        F(AARStatusInfo, sq_full, OFF_ST_SQ_FULL),
        F(AARStatusInfo, log_dropped, OFF_ST_LOG_DROPPED),
        F(AARStatusInfo, events_dropped, OFF_ST_EVENTS_DROPPED),
        F(AARStatusInfo, cq_full, OFF_ST_CQ_FULL),
        F(AARStatusInfo, stack_sp, OFF_ST_STACK_SP),
        F(AARStatusInfo, stack_bytes, OFF_ST_STACK_BYTES),
        F(AARStatusInfo, profile_hz, OFF_ST_PROFILE_HZ),
        F(AARStatusInfo, profile_samples, OFF_ST_PROFILE_SAMPLES),
        F(AARStatusInfo, profile_dropped, OFF_ST_PROFILE_DROPPED),
    };
    static const struct field ev[] = {
        F(AAREvent, message, OFF_EV_MESSAGE),
        F(AAREvent, type, OFF_EV_TYPE),
        F(AAREvent, handle, OFF_EV_HANDLE),
        F(AAREvent, status, OFF_EV_STATUS),
    };
    static const struct field msg[] = {
        F(struct Message, mn_Node, OFF_MSG_NODE),
        F(struct Message, mn_ReplyPort, OFF_MSG_REPLY_PORT),
        F(struct Message, mn_Length, OFF_MSG_LENGTH),
    };
    static const struct field map[] = {
        F(struct AARSharedMap, size, OFF_MAP_SIZE),
        F(struct AARSharedMap, base, OFF_MAP_BASE),
        F(struct AARSharedMap, bytes, OFF_MAP_BYTES),
        F(struct AARSharedMap, haddr, OFF_MAP_HADDR),
        F(struct AARSharedMap, flags, OFF_MAP_FLAGS),
        F(struct AARSharedMap, granule, OFF_MAP_GRANULE),
        F(struct AARSharedMap, reserved, OFF_MAP_RESERVED),
    };
    static const struct field req[] = {
        F(AARServiceReq, size, OFF_REQ_SIZE),
        F(AARServiceReq, op, OFF_REQ_OP),
        F(AARServiceReq, id, OFF_REQ_ID),
        F(AARServiceReq, result, OFF_REQ_RESULT),
        F(AARServiceReq, arg, OFF_REQ_ARG),
        F(AARServiceReq, out, OFF_REQ_OUT),
        F(AARServiceReq, buf, OFF_REQ_BUF),
        F(AARServiceReq, buf_off, OFF_REQ_BUF_OFF),
        F(AARServiceReq, buf_len, OFF_REQ_BUF_LEN),
        F(AARServiceReq, flags, OFF_REQ_FLAGS),
        F(AARServiceReq, reply_flags, OFF_REQ_REPLY_FLAGS),
        F(AARServiceReq, ptr, OFF_REQ_PTR),
        F(AARServiceReq, reserved, OFF_REQ_RESERVED),
    };
    static const struct field list[] = {
        F(AARZZ9000List, size, OFF_LIST_SIZE),
        F(AARZZ9000List, handle, OFF_LIST_HANDLE),
        F(AARZZ9000List, state, OFF_LIST_STATE),
        F(AARZZ9000List, committed, OFF_LIST_COMMITTED),
        F(AARZZ9000List, index, OFF_LIST_INDEX),
        F(AARZZ9000List, cpu_ms, OFF_LIST_CPU_MS),
        F(AARZZ9000List, service_ms, OFF_LIST_SERVICE_MS),
        F(AARZZ9000List, block_reason, OFF_LIST_BLOCK_REASON),
        F(AARZZ9000List, end_reason, OFF_LIST_END_REASON),
        F(AARZZ9000List, flags, OFF_LIST_FLAGS),
        F(AARZZ9000List, name, OFF_LIST_NAME),
    };

    WALK("AARInfo", info, SIZE_INFO, AARInfo);
    WALK("AARImageInfo", img, SIZE_IMG, AARImageInfo);
    WALK("AARStatusInfo", st, SIZE_ST, AARStatusInfo);
    WALK("AAREvent", ev, SIZE_EV, AAREvent);
    WALK("struct Message", msg, SIZE_MESSAGE, struct Message);
    WALK("AARSharedMap", map, SIZE_MAP, struct AARSharedMap);
    WALK("AARServiceReq", req, SIZE_REQ, AARServiceReq);
    WALK("AARZZ9000List", list, SIZE_LIST, AARZZ9000List);
    CHECK_EQ(sizeof(AARZZ9000Stats), SIZE_STATS);
    CHECK_EQ(sizeof(AARMessage), 32);
    CHECK_EQ(sizeof(AARU64), 8);
    CHECK_EQ(sizeof(struct Node), SIZE_NODE);
    CHECK_EQ(offsetof(struct Node, ln_Type), 8);
    CHECK_EQ(offsetof(struct Node, ln_Pri), 9);
    CHECK_EQ(offsetof(struct Node, ln_Name), 10);
    /* The ZZ9000 extension table: two words, then the function pointers. */
    CHECK_EQ(offsetof(aar_client_ext_zz9000, list), 8);
    /* The members that follow lease_pause are appended in this order; the log
     * sink is AARSetLogSink, not a field. */
    CHECK_EQ(offsetof(aar_client_ext_zz9000, lease_pause), 8 + 3 * sizeof(void *));
    CHECK_EQ(offsetof(aar_client_ext_zz9000, surface_alloc), 8 + 4 * sizeof(void *));
    CHECK_EQ(offsetof(aar_client_ext_zz9000, surface_free), 8 + 5 * sizeof(void *));
    CHECK_EQ(sizeof(aar_client_ext_zz9000), 8 + 6 * sizeof(void *));
}

/* ---- Constants ------------------------------------------------------------- */

static void test_constants(void)
{
    CHECK_EQ(strcmp(AAR_LIB_NAME, "aar.library"), 0);
    CHECK_EQ(AAR_LIB_VERSION, AAR_API_MAJOR);
    CHECK_EQ(AAR_LIB_VERSION, 0);
    CHECK_EQ(AAR_LIB_REVISION_READ, 2);
    CHECK_EQ(AAR_PROFILEF_A32, 1);
    CHECK_EQ(AAR_PROFILEF_A64, 2);
    CHECK_EQ(AAR_NOTIFY_MESSAGE, 1);
    CHECK_EQ(AAR_NOTIFY_COMPLETE, 2);
    CHECK_EQ(AAR_NOTIFY_FAULT, 3);
    CHECK_EQ(AAR_NOTIFY_REQUEST, 4);
    CHECK_EQ(AAR_MAPF_UNCACHED, 1);
    CHECK_EQ(AAR_MAPF_COHERENT, 2);
    CHECK_EQ(AAR_CFLAG_FLIP_SUBMITTED, 1);
    CHECK_EQ(AAR_CCAP_DISPLAY_CLUT8, 1);
    CHECK_EQ(AAR_CCAP_DISPLAY_PAN_ACK, 2);
    CHECK_EQ(AAR_CCAP_DISPLAY_LATEST, 4);
    CHECK_EQ(AAR_CCAP_KNOWN, 7);
    CHECK_EQ(AAR_TAG_BASE, 0x80415200UL);
    CHECK_EQ(AAR_TAG_PRIVATE_MIB, 0x80415201UL);
    CHECK_EQ(AAR_TAG_SHARED_MIB, 0x80415202UL);
    CHECK_EQ(AAR_TAG_WEIGHT, 0x80415203UL);
    CHECK_EQ(AAR_TAG_LEASE_EXTENDED, 0x80415204UL);
    CHECK_EQ(AAR_TAG_AUDIO_ROUTE, 0x80415205UL);
    CHECK_EQ(AAR_TAG_CLIENT_CAPS, 0x80415206UL);
    CHECK_EQ(AAR_TAG_PROFILE_HZ, 0x80415207UL);
    CHECK_EQ(AAR_PROFILE_HZ_MAX, 10000UL);
    CHECK_EQ(AAR_TAG_PLATFORM_BASE(AAR_PLATFORM_ZZ9000), 0x80415300UL);
    /* The names the header reuses from the app side exist with the spec values. */
    CHECK_EQ(AAR_OK, 0);
    CHECK_EQ(AAR_E_VERSION, -18);
    CHECK_EQ(AAR_E_OWNER, -17);
    CHECK_EQ(AAR_E_STALE, -16);
    CHECK_EQ(AAR_E_NORESOURCE, -15);
    CHECK_EQ(AAR_OP_DISPLAY_OPEN, 0x30);
    CHECK_EQ(AAR_AUDIO_ROUTE_AHI, 1);
    CHECK_EQ(AAR_PLATFORM_ZZ9000, 1);
    CHECK_EQ(AAR_EXT_ZZ9000_CLIENT, 1);
    CHECK_EQ(AAR_ZZ9000_INFO_CAPS, 7);
}

/* ---- Prototypes -------------------------------------------------------------- */

/*
 * Every call has the type the .fd file's C line gives it. _Generic picks 1
 * only when the function has exactly this type. The library is not linked: the
 * names are declared by clib/aar_protos.h and only their types are used.
 */
#define IS_TYPE(fn, type) _Generic(&fn, type: 1, default: 0)

_Static_assert(IS_TYPE(AARQuery, LONG (*)(AARInfo *)), "AARQuery");
_Static_assert(IS_TYPE(AAROpen, LONG (*)(AARHandle **, struct MsgPort *, ULONG *)), "AAROpen");
_Static_assert(IS_TYPE(AARClose, LONG (*)(AARHandle *)), "AARClose");
_Static_assert(IS_TYPE(AARProbeFile, LONG (*)(const char *, AARImageInfo *)), "AARProbeFile");
_Static_assert(IS_TYPE(AARProbeMemory, LONG (*)(const void *, ULONG, AARImageInfo *)), "AARProbeMemory");
_Static_assert(IS_TYPE(AARSetTagList, LONG (*)(AARHandle *, const struct TagItem *)), "AARSetTagList");
_Static_assert(IS_TYPE(AARLoadFile, LONG (*)(AARHandle *, const char *)), "AARLoadFile");
_Static_assert(IS_TYPE(AARLoadMemory, LONG (*)(AARHandle *, const void *, ULONG)), "AARLoadMemory");
_Static_assert(IS_TYPE(AARStart, LONG (*)(AARHandle *, const char *const *, ULONG)), "AARStart");
_Static_assert(IS_TYPE(AARStop, LONG (*)(AARHandle *)), "AARStop");
_Static_assert(IS_TYPE(AARStatus, LONG (*)(AARHandle *, AARStatusInfo *)), "AARStatus");
_Static_assert(IS_TYPE(AARSendMessage, LONG (*)(AARHandle *, const AARMessage *)), "AARSendMessage");
_Static_assert(IS_TYPE(AARGetMessage, LONG (*)(AARHandle *, AARMessage *)), "AARGetMessage");
_Static_assert(IS_TYPE(AARMessageSignal, ULONG (*)(AARHandle *)), "AARMessageSignal");
_Static_assert(IS_TYPE(AARSharedMap, LONG (*)(AARHandle *, ULONG, ULONG, ULONG, struct AARSharedMap *)), "AARSharedMap");
_Static_assert(IS_TYPE(AARSharedUnmap, LONG (*)(AARHandle *, struct AARSharedMap *)), "AARSharedUnmap");
_Static_assert(IS_TYPE(AARSharedPublish, LONG (*)(AARHandle *, struct AARSharedMap *, ULONG, ULONG)), "AARSharedPublish");
_Static_assert(IS_TYPE(AARSharedAcquire, LONG (*)(AARHandle *, struct AARSharedMap *, ULONG, ULONG)), "AARSharedAcquire");
_Static_assert(IS_TYPE(AARServiceNext, LONG (*)(AARHandle *, AARServiceReq *)), "AARServiceNext");
_Static_assert(IS_TYPE(AARServiceReply, LONG (*)(AARHandle *, const AARServiceReq *)), "AARServiceReply");
_Static_assert(IS_TYPE(AARPostEvent, LONG (*)(AARHandle *, const aar_event *)), "AARPostEvent");
_Static_assert(IS_TYPE(AARAudioRead, LONG (*)(AARHandle *, void *, ULONG, ULONG *)), "AARAudioRead");
_Static_assert(IS_TYPE(AARSharedToken, LONG (*)(AARHandle *, const void *, ULONG, ULONG *)), "AARSharedToken");
_Static_assert(IS_TYPE(AARExt, APTR (*)(ULONG, ULONG)), "AARExt");
_Static_assert(IS_TYPE(AARSetLogSink, LONG (*)(AARHandle *, AARLogSink, APTR)), "AARSetLogSink");
_Static_assert(IS_TYPE(AARReadStack, LONG (*)(AARHandle *, void *, ULONG, ULONG *)), "AARReadStack");
_Static_assert(IS_TYPE(AARReadProfile, LONG (*)(AARHandle *, void *, ULONG, ULONG *, ULONG *)), "AARReadProfile");

/* The sink is a plain C function of three arguments: the type a function that
 * has them is accepted as, and no other. */
static void sample_sink(APTR user, const void *data, ULONG len)
{
    (void)user;
    (void)data;
    (void)len;
}
_Static_assert(IS_TYPE(sample_sink, void (*)(APTR, const void *, ULONG)), "log sink shape");
_Static_assert(_Generic(&sample_sink, AARLogSink: 1, default: 0), "AARLogSink");

/* ---- The platform table ----------------------------------------------------------- */

/* The function every platform object file defines. */
_Static_assert(IS_TYPE(aar_plat_get, struct aar_plat *(*)(void)), "aar_plat_get");

static int plat_attach(struct aar_plat *p) { (void)p; return AAR_OK; }

static const struct aar_plat_ops sample_ops = {
    .size = sizeof(struct aar_plat_ops),
    .attach = plat_attach,
};

static void test_platform_table(void)
{
    struct aar_plat p;

    memset(&p, 0, sizeof(p));
    p.version = AAR_PLAT_VERSION;
    p.ops = &sample_ops;
    CHECK_EQ(p.ops->attach(&p), AAR_OK);
    CHECK_EQ(p.ops->size, sizeof(struct aar_plat_ops));
    CHECK_EQ(AAR_PLAT_KICK_CQ, 1);
    CHECK_EQ(AAR_PLAT_KICK_SQ, 2);
}

int main(void)
{
    test_layouts();
    test_constants();
    test_platform_table();
    if (failures) {
        printf("header_test: %lu of %lu checks FAILED\n", failures, checks);
        return 1;
    }
    printf("header_test: %lu checks passed\n", checks);
    return 0;
}
