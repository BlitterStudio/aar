/*
 * Workbench project-path handling shared by aar-run and its host test.
 * Include after the Workbench/DOS declarations and <string.h>.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_RUN_WORKBENCH_H
#define AAR_RUN_WORKBENCH_H

/* The full path of a WBArg: its directory and its name. A WBArg with an empty
 * name is the directory itself (a drawer) and gives the directory's path. */
static int aar_run_project_path(const struct WBArg *project, char *path,
                                size_t capacity)
{
    char directory[256];

    if (!capacity)
        return 0;
    path[0] = 0;
    if (NameFromLock(project->wa_Lock, (STRPTR)directory,
                     (LONG)sizeof(directory)) != 0) {
        size_t n = strlen(directory);
        size_t m = strlen((const char *)project->wa_Name);

        if (n + 1 + m < capacity) {
            memcpy(path, directory, n);
            if (m && n && directory[n - 1] != ':')
                path[n++] = '/';
            memcpy(path + n, project->wa_Name, m + 1);
            return 1;
        }
    }
    return 0;
}

#endif
