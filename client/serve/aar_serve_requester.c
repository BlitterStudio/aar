/* Native file selection for aar_serve.c. The ASL process owns the modal
 * dialog; the serving task retains the claim, the payload and all replies.
 * SPDX-License-Identifier: MIT
 */

#ifndef AAR_SERVE_HOST
#include <dos/dostags.h>
#include <libraries/asl.h>
#include <proto/asl.h>

struct aar_serve_requester {
    struct Library *asl;
    struct FileRequester *dialog;
    struct MsgPort *port;
    struct Message finished;
    struct aar_serve_req req;
    struct Screen *screen;
    int result;
    int cancelled;
    char title[AAR_PATH_MAX + 1u];
    char drawer[REQUESTER_DRAWER_BYTES];
    char file[AAR_PATH_MAX + 1u];
};

static void requester_entry(void)
{
    struct aar_serve_requester *r = FindTask(NULL)->tc_UserData;
    struct Library *AslBase = r->asl;
    int save = r->req.arg[AAR_A_FILE_REQUEST_FLAGS] == AAR_FILE_REQUEST_SAVE;
    struct TagItem tags[] = {
        { ASLFR_TitleText, (ULONG)(r->title[0] ? r->title : (save ? "Save As" : "Open")) },
        { ASLFR_InitialDrawer, (ULONG)r->drawer },
        { ASLFR_InitialFile, (ULONG)r->file },
        { ASLFR_DoSaveMode, save },
        { r->screen ? ASLFR_Screen : TAG_IGNORE, (ULONG)r->screen },
        { TAG_DONE, 0 }
    };
    LONG error;

    SetIoErr(0);
    if (AslRequest(r->dialog, tags)) {
        r->result = 1;
    } else {
        error = IoErr();
        r->result = error == 0 || error == ERROR_BREAK ? 0 :
            error == ERROR_NO_FREE_STORE ? AAR_E_NOMEM : AAR_E_IO;
    }
    /* No requester storage or launcher code is used after this process exits.
     * Keep the owner from freeing them during the final return to DOS. */
    Forbid();
    PutMsg(r->port, &r->finished);
}

static void requester_free(struct aar_serve_requester *r)
{
    struct Library *AslBase = r->asl;

    if (r->dialog)
        FreeAslRequest(r->dialog);
    if (r->port)
        X_DELETE_PORT(r->port);
    if (AslBase)
        X_CLOSE_LIB(AslBase);
    X_FREE_VEC(r);
}

static int requester_start(struct aar_serve *s, const struct aar_serve_req *q,
                           const char *initial, uint32_t title_bytes)
{
    struct Library *AslBase;
    struct aar_serve_requester *r;
    const struct TagItem tags[] = {
        { NP_Entry, (ULONG)requester_entry },
        { NP_Name, (ULONG)"AAR file requester" },
        { NP_Input, 0 }, { NP_Output, 0 },
        { NP_CurrentDir, 0 }, { NP_HomeDir, 0 }, { NP_CopyVars, FALSE },
        { NP_WindowPtr, (ULONG)-1 }, { NP_StackSize, 16384 },
        { TAG_DONE, 0 }
    };
    struct Process *worker;
    int rc;

    if (s->requester)
        return AAR_E_BUSY;
    if (!file_transfer_allowed(s))
        return AAR_E_IO;
    AslBase = X_OPEN_LIB((CONST_STRPTR)"asl.library", 44);
    if (!AslBase)
        return AAR_E_NOSYS;
    r = X_ALLOC_VEC(sizeof(*r), MEMF_PUBLIC | MEMF_CLEAR);
    if (!r) {
        X_CLOSE_LIB(AslBase);
        return AAR_E_NOMEM;
    }
    r->asl = AslBase;
    r->req = *q;
    rc = requester_initial_path(s, initial, r->drawer, r->file);
    if (!file_transfer_allowed(s))
        rc = AAR_E_IO;
    if (rc != AAR_OK) {
        requester_free(r);
        return rc;
    }
    if (title_bytes) {
        uint32_t n = aar_serve_utf8_to_latin1(q->buf, title_bytes, (uint8_t *)r->title);
        r->title[n] = 0;
    }
    r->screen = s->gfx.input_win ? s->gfx.input_win->WScreen : s->gfx.screen;
    r->dialog = AllocAslRequest(ASL_FileRequest, NULL);
    r->port = X_CREATE_PORT();
    if (!r->dialog || !r->port) {
        requester_free(r);
        return AAR_E_NOMEM;
    }
    r->finished.mn_Node.ln_Type = NT_MESSAGE;
    r->finished.mn_Length = sizeof(r->finished);
    /* No inherited DOS streams, locks or variables to duplicate under Forbid. */
    Forbid();
    worker = CreateNewProc(tags);
    if (worker) {
        worker->pr_Task.tc_UserData = r;
        s->requester = r;
    }
    Permit();
    if (!worker) {
        requester_free(r);
        return AAR_E_NOMEM;
    }
    return AAR_OK;
}

static void requester_abort(struct aar_serve_requester *r)
{
    struct Library *AslBase = r->asl;

    r->cancelled = 1;
    AbortAslRequest(r->dialog);
}

static void requester_pump(struct aar_serve *s, int stopping)
{
    struct aar_serve_requester *r = s->requester;
    struct aar_serve_done done = {0};
    char path[AAR_PATH_MAX + 1u];

    if (!r)
        return;
    if (!X_GET_MSG(r->port)) {
        if (stopping)
            requester_abort(r);
        return;
    }
    /* Detach before DOS policy checks, which may pump native file transfers. */
    s->requester = NULL;
    done.result = r->result;
    if (stopping || r->cancelled || !file_transfer_allowed(s)) {
        done.result = AAR_E_IO;
    } else if (done.result == 1) {
        done.result = requester_selected_path(s, (const char *)r->dialog->fr_Drawer,
                                               (const char *)r->dialog->fr_File,
                                               r->req.arg[AAR_A_FILE_REQUEST_FLAGS] ==
                                                   AAR_FILE_REQUEST_SAVE, path);
        if (!file_transfer_allowed(s)) {
            done.result = AAR_E_IO;
        } else if (done.result == AAR_OK) {
            uint32_t len = (uint32_t)strlen(path);

            if (len >= r->req.arg[AAR_A_FILE_REQUEST_CAP]) {
                done.result = AAR_E_NOSPC;
            } else {
                memcpy(r->req.buf, path, len + 1u);
                done.out[AAR_R_FILE_REQUEST_LEN] = len;
                done.result = 1;
            }
        }
    }
    reply_request(s, &r->req, &done);
    requester_free(r);
}

static void requester_destroy(struct aar_serve *s)
{
    /* An abort sent before ASL opens its window may have no effect. Keep the
     * request allocated and retry until the child has returned from ASL. */
    while (s->requester) {
        requester_pump(s, 1);
        if (s->requester)
            Delay(1);
    }
}

static ULONG requester_sigmask(const struct aar_serve *s)
{
    return s->requester ? 1UL << s->requester->port->mp_SigBit : 0UL;
}
#else
static void requester_pump(struct aar_serve *s, int stopping)
{
    (void)s;
    (void)stopping;
}

static void requester_destroy(struct aar_serve *s) { (void)s; }
static ULONG requester_sigmask(const struct aar_serve *s) { (void)s; return 0; }
#endif

static int requester_begin(struct aar_serve *s, const struct aar_serve_req *q,
                           struct aar_serve_done *done)
{
    uint32_t bytes = q->arg[AAR_A_FILE_REQUEST_IN_BYTES];
    uint32_t cap = q->arg[AAR_A_FILE_REQUEST_CAP];
    const uint8_t *end_title;
    uint32_t title_bytes, initial_bytes;

    done->result = AAR_E_INVAL;
    if (q->flags || q->arg[AAR_A_FILE_REQUEST_FLAGS] > AAR_FILE_REQUEST_SAVE ||
        !q->buf || bytes < 2u || bytes > 2u * (AAR_PATH_MAX + 1u) ||
        !cap || cap > AAR_PATH_MAX + 1u || q->buf_len != (bytes > cap ? bytes : cap))
        return AAR_SERVE_REQ_DONE;
    end_title = memchr(q->buf, 0, bytes - 1u);
    if (!end_title)
        return AAR_SERVE_REQ_DONE;
    title_bytes = (uint32_t)(end_title - q->buf);
    initial_bytes = bytes - title_bytes - 1u;
    if (title_bytes > AAR_PATH_MAX || initial_bytes > AAR_PATH_MAX + 1u ||
        memchr(end_title + 1, 0, initial_bytes) != q->buf + bytes - 1u)
        return AAR_SERVE_REQ_DONE;
#ifdef AAR_SERVE_HOST
    (void)s;
    done->result = AAR_E_NOSYS;
#else
    done->result = requester_start(s, q, (const char *)end_title + 1, title_bytes);
    if (done->result == AAR_OK)
        return AAR_SERVE_REQ_DEFERRED;
#endif
    return AAR_SERVE_REQ_DONE;
}
