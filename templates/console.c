/*
 * Shell console template: stdout, stderr, stdin and break events.
 * Prints a banner, then echoes each stdin line in upper case until EOF
 * (Ctrl-\ in a Shell window) and reports any Ctrl-D/E/F seen meanwhile. Break
 * events are input events, so the note lists AAR_GROUP_INPUT as optional.
 *
 * SPDX-License-Identifier: MIT
 */
#include <ctype.h>
#include <stdio.h>

#include "aar/app.h"
#include "aar/note.h"

AAR_NOTE(0, 0, 0, AAR_GROUP_INPUT)

static void report_breaks(void)
{
    aar_event events[8];
    uint32_t count = 0;
    uint32_t i;

    if (aar_event_poll(events, 8u, &count) != AAR_OK)
        return;
    for (i = 0; i < count; i++)
        if (events[i].type == AAR_EV_BREAK)
            printf("break: Ctrl-%c\n", 'A' + events[i].code - 1);
}

int aar_main(int argc, char **argv)
{
    char line[256];
    unsigned lines = 0;
    int i;

    printf("console: %d argument(s)", argc - 1);
    for (i = 1; i < argc; i++)
        printf(" [%s]", argv[i]);
    printf("\n");
    fprintf(stderr, "console: stderr works\n");
    fflush(stdout);
    while (!aar_should_stop() && fgets(line, sizeof line, stdin)) {
        char *p;

        for (p = line; *p; p++)
            *p = (char)toupper((unsigned char)*p);
        fputs(line, stdout);
        fflush(stdout);
        lines++;
        report_breaks();
    }
    report_breaks();
    printf("console: EOF after %u line(s)\n", lines);
    return 0;
}
