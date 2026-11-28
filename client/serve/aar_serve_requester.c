/* Native dialogs for aar_serve.c. The serving task owns all replies and
 * pumps the message dialog itself. Only the modal ASL call needs a process.
 * SPDX-License-Identifier: MIT
 */

struct aar_serve_requester {
    struct aar_serve_req req;
};

struct aar_serve_message_requester {
    struct aar_serve_requester active;
    struct Window *window;
    uint32_t choices;
    char title[];
};

static int message_requester_begin(struct aar_serve *s, const struct aar_serve_req *q,
                                  struct aar_serve_done *done)
{
    static const uint32_t limits[] = {
        AAR_REQUESTER_TITLE_MAX, AAR_REQUESTER_TEXT_MAX, AAR_REQUESTER_BUTTONS_MAX
    };
    char title[AAR_REQUESTER_TITLE_MAX + 1u];
    char text[AAR_REQUESTER_TEXT_MAX + 1u];
    char buttons[AAR_REQUESTER_BUTTONS_MAX + 1u];
    char *const fields[] = { title, text, buttons };
    char format[3u * AAR_REQUESTER_MAX_CHOICES];
    const char *args[1u + AAR_REQUESTER_MAX_CHOICES];
    const uint8_t *input = q->buf;
    uint32_t left = q->buf_len, choices = 0, title_bytes = 0;
    unsigned i;
    char *label, *end;
    struct aar_serve_message_requester *r;
    struct EasyStruct es = {0};

    done->result = AAR_E_INVAL;
    if (q->flags || !input || left > sizeof(title) + sizeof(text) + sizeof(buttons))
        return AAR_SERVE_REQ_DONE;
    for (i = 0; i < AAR_ENTRY_ARGS; i++)
        if (q->arg[i])
            return AAR_SERVE_REQ_DONE;
    for (i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
        const uint8_t *nul = memchr(input, 0, left);
        uint32_t bytes;
        int32_t n;

        if (!nul)
            return AAR_SERVE_REQ_DONE;
        bytes = (uint32_t)(nul - input);
        if (bytes > limits[i] || (!bytes && i != 0))
            return AAR_SERVE_REQ_DONE;
        n = aar_serve_utf8_to_latin1_checked(input, bytes, (uint8_t *)fields[i]);
        if (n < 0)
            return AAR_SERVE_REQ_DONE;
        fields[i][n] = '\0';
        if (i == 0)
            title_bytes = (uint32_t)n;
        input = nul + 1;
        left -= bytes + 1u;
    }
    if (left)
        return AAR_SERVE_REQ_DONE;

    /* RawDoFmt consumes the body argument followed by one argument per
     * label. Keep the '|' separators in the format and all app text in the
     * arguments so percent sequences are never interpreted as directives. */
    args[0] = text;
    label = buttons;
    for (;;) {
        end = strchr(label, '|');
        if (!*label || end == label || choices == AAR_REQUESTER_MAX_CHOICES)
            return AAR_SERVE_REQ_DONE;
        if (end)
            *end = '\0';
        args[1u + choices] = label;
        format[3u * choices] = '%';
        format[3u * choices + 1u] = 's';
        format[3u * choices + 2u] = end ? '|' : '\0';
        choices++;
        if (!end)
            break;
        label = end + 1;
    }

    if (s->requester)
        done->result = AAR_E_BUSY;
    else if (!file_transfer_allowed(s))
        done->result = AAR_E_IO;
    else if (!IntuitionBase || X_INTUITION_VERSION() < 36u)
        done->result = AAR_E_NOSYS;
    else {
        r = X_ALLOC_VEC(sizeof(*r) + title_bytes + 1u, MEMF_PUBLIC);
        if (!r) {
            done->result = AAR_E_NOMEM;
            return AAR_SERVE_REQ_DONE;
        }
        r->active.req = *q;
        r->choices = choices;
        /* Intuition formats the body and labels during Build, but may keep
         * the window's title pointer until FreeSysRequest. */
        memcpy(r->title, title, title_bytes + 1u);
        es.es_StructSize = sizeof(es);
        es.es_Title = (UBYTE *)(title_bytes ? r->title : "Message");
        es.es_TextFormat = (UBYTE *)"%s";
        es.es_GadgetFormat = (UBYTE *)format;
        r->window = X_BUILD_EASY_REQUEST(s->gfx.input_win ? s->gfx.input_win : s->gfx.win,
                                         &es, 0, args);
        /* BuildEasyRequestArgs may return 0 or 1 instead of a window. A
         * failed build is not a selection from this request's button row. */
        if ((uintptr_t)r->window <= 1u) {
            X_FREE_VEC(r);
            done->result = AAR_E_IO;
            return AAR_SERVE_REQ_DONE;
        }
        s->requester = &r->active;
        return AAR_SERVE_REQ_DEFERRED;
    }
    return AAR_SERVE_REQ_DONE;
}

static void message_requester_pump(struct aar_serve *s,
                                   struct aar_serve_message_requester *r, int stopping)
{
    struct aar_serve_done done = {0};

    done.result = AAR_E_IO;
    if (!stopping) {
        LONG choice = X_SYS_REQ_HANDLER(r->window, NULL, FALSE);

        if (choice == -2) /* no satisfying IDCMP message yet */
            return;
        if (choice >= 0 && (uint32_t)choice < r->choices) {
            done.result = AAR_OK;
            done.out[AAR_R_REQUESTER_CHOICE] = choice ? (uint32_t)choice : r->choices;
        }
    }
    s->requester = NULL;
    X_FREE_SYS_REQUEST(r->window);
    reply_request(s, &r->active.req, &done);
    X_FREE_VEC(r);
}

#ifndef AAR_SERVE_HOST
#include <dos/dostags.h>
#include <libraries/asl.h>
#include <proto/asl.h>

struct aar_serve_file_requester {
    struct aar_serve_requester active;
    struct Library *asl;
    struct FileRequester *dialog;
    struct MsgPort *port;
    struct Message finished;
    struct Screen *screen;
    int result;
    int cancelled;
    char title[AAR_PATH_MAX + 1u];
    char drawer[REQUESTER_DRAWER_BYTES];
    char file[AAR_PATH_MAX + 1u];
};

static void requester_entry(void)
{
    struct aar_serve_file_requester *r = FindTask(NULL)->tc_UserData;
    struct Library *AslBase = r->asl;
    int save = r->active.req.arg[AAR_A_FILE_REQUEST_FLAGS] == AAR_FILE_REQUEST_SAVE;
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

static void file_requester_free(struct aar_serve_file_requester *r)
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

static int file_requester_start(struct aar_serve *s, const struct aar_serve_req *q,
                                const char *initial, uint32_t title_bytes)
{
    struct Library *AslBase;
    struct aar_serve_file_requester *r;
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
    r->active.req = *q;
    rc = requester_initial_path(s, initial, r->drawer, r->file);
    if (!file_transfer_allowed(s))
        rc = AAR_E_IO;
    if (rc != AAR_OK) {
        file_requester_free(r);
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
        file_requester_free(r);
        return AAR_E_NOMEM;
    }
    r->finished.mn_Node.ln_Type = NT_MESSAGE;
    r->finished.mn_Length = sizeof(r->finished);
    /* No inherited DOS streams, locks or variables to duplicate under Forbid. */
    Forbid();
    worker = CreateNewProc(tags);
    if (worker) {
        worker->pr_Task.tc_UserData = r;
        s->requester = &r->active;
    }
    Permit();
    if (!worker) {
        file_requester_free(r);
        return AAR_E_NOMEM;
    }
    return AAR_OK;
}

static void file_requester_abort(struct aar_serve_file_requester *r)
{
    struct Library *AslBase = r->asl;

    r->cancelled = 1;
    AbortAslRequest(r->dialog);
}

static void file_requester_pump(struct aar_serve *s, struct aar_serve_file_requester *r,
                                int stopping)
{
    struct aar_serve_done done = {0};
    char path[AAR_PATH_MAX + 1u];

    if (!X_GET_MSG(r->port)) {
        if (stopping)
            file_requester_abort(r);
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
                                               r->active.req.arg[AAR_A_FILE_REQUEST_FLAGS] ==
                                                   AAR_FILE_REQUEST_SAVE, path);
        if (!file_transfer_allowed(s)) {
            done.result = AAR_E_IO;
        } else if (done.result == AAR_OK) {
            uint32_t len = (uint32_t)strlen(path);

            if (len >= r->active.req.arg[AAR_A_FILE_REQUEST_CAP]) {
                done.result = AAR_E_NOSPC;
            } else {
                memcpy(r->active.req.buf, path, len + 1u);
                done.out[AAR_R_FILE_REQUEST_LEN] = len;
                done.result = 1;
            }
        }
    }
    reply_request(s, &r->active.req, &done);
    file_requester_free(r);
}
#endif

static void requester_pump(struct aar_serve *s, int stopping)
{
    struct aar_serve_requester *r = s->requester;

    if (!r)
        return;
    if (r->req.op == AAR_OP_REQUESTER)
        message_requester_pump(s, (struct aar_serve_message_requester *)r, stopping);
#ifndef AAR_SERVE_HOST
    else
        file_requester_pump(s, (struct aar_serve_file_requester *)r, stopping);
#endif
}

static void requester_close_message_on_screen(struct aar_serve *s, const struct Screen *screen)
{
    if (s->requester && s->requester->req.op == AAR_OP_REQUESTER) {
        struct aar_serve_message_requester *r =
            (struct aar_serve_message_requester *)s->requester;

        if (r->window->WScreen == screen)
            message_requester_pump(s, r, 1);
    }
}

static void requester_destroy(struct aar_serve *s)
{
    /* An abort sent before ASL opens its window may have no effect. Keep the
     * request allocated and retry until the child has returned from ASL. */
    while (s->requester) {
        requester_pump(s, 1);
#ifndef AAR_SERVE_HOST
        if (s->requester)
            Delay(1);
#endif
    }
}

static ULONG requester_sigmask(const struct aar_serve *s)
{
    const struct aar_serve_requester *r = s->requester;
    const struct MsgPort *port;

    if (!r)
        return 0;
    if (r->req.op == AAR_OP_REQUESTER)
        port = ((const struct aar_serve_message_requester *)r)->window->UserPort;
#ifndef AAR_SERVE_HOST
    else
        port = ((const struct aar_serve_file_requester *)r)->port;
#else
    else
        return 0;
#endif
    return 1UL << port->mp_SigBit;
}

static int file_requester_begin(struct aar_serve *s, const struct aar_serve_req *q,
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
    if (s->requester) {
        done->result = AAR_E_BUSY;
        return AAR_SERVE_REQ_DONE;
    }
#ifdef AAR_SERVE_HOST
    done->result = AAR_E_NOSYS;
#else
    done->result = file_requester_start(s, q, (const char *)end_title + 1, title_bytes);
    if (done->result == AAR_OK)
        return AAR_SERVE_REQ_DEFERRED;
#endif
    return AAR_SERVE_REQ_DONE;
}
