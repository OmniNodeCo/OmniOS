/*
 * OmniOS — os/gui/catalog.c
 *
 * App catalog + installed-app state (see catalog.h).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "catalog.h"

/* Start menu order. */
const struct omni_app_info omni_catalog[] = {
    { "terminal", "Terminal",     "/usr/bin/omnios-term",    "System",
      "Command line with a real shell (ash).",                 1, 1, 0x334155, '>' },
    { "files",    "File Manager", "/usr/bin/omnios-files",   "System",
      "Browse the files on your system.",                      1, 1, 0xf59e0b, 'F' },
    { "edit",     "Text Editor",  "/usr/bin/omnios-edit",    "Productivity",
      "Write and edit plain text.",                            0, 1, 0x0ea5e9, 'E' },
    { "calc",     "Calculator",   "/usr/bin/omnios-calc",    "Utilities",
      "A four-function calculator.",                           0, 1, 0x10b981, '+' },
    { "sysinfo",  "System Info",  "/usr/bin/omnios-sysinfo", "Utilities",
      "Memory, uptime and CPU at a glance.",                   0, 1, 0x64748b, 'i' },
    { "clock",    "Clock",        "/usr/bin/omnios-clock",   "Utilities",
      "A big digital clock with today's date.",                0, 0, 0x6366f1, 'C' },
    { "snake",    "Snake",        "/usr/bin/omnios-snake",   "Games",
      "The classic: eat, grow, and don't bite your tail.",     0, 0, 0x22c55e, 'S' },
    { "store",    "App Store",    "/usr/bin/omnios-store",   "System",
      "Get and remove apps.",                                  1, 1, 0x3b82f6, 'A' },
    { "about",    "About OmniOS", "/usr/bin/omnios-about",   "System",
      "Version and credits.",                                  1, 1, 0x8b5cf6, 'O' },
};
const int omni_catalog_n = (int)(sizeof(omni_catalog) / sizeof(omni_catalog[0]));

const struct omni_app_info *omni_app_for_title(const char *title)
{
    int i;
    if (!title || !title[0])
        return NULL;
    for (i = 0; i < omni_catalog_n; i++)        /* "OmniOS Terminal" */
        if (strstr(title, omni_catalog[i].name))
            return &omni_catalog[i];
    for (i = 0; i < omni_catalog_n; i++) {      /* "System Information" */
        const char *n = omni_catalog[i].name;
        size_t k = strcspn(n, " ");
        if (n[k] == ' ' && strncmp(title, n, k) == 0 &&
            (title[k] == ' ' || title[k] == '\0'))
            return &omni_catalog[i];
    }
    return NULL;
}

int omni_app_find(const char *id)
{
    int i;
    for (i = 0; i < omni_catalog_n; i++)
        if (strcmp(omni_catalog[i].id, id) == 0)
            return i;
    return -1;
}

const char *omni_apps_state_path(void)
{
    const char *p = getenv("OMNI_APPS_STATE");
    return (p && *p) ? p : OMNI_APPS_STATE;
}

void omni_apps_load(unsigned char installed[OMNI_CATALOG_MAX])
{
    char line[128];
    FILE *f;
    int i;

    for (i = 0; i < OMNI_CATALOG_MAX; i++)
        installed[i] = 0;

    f = fopen(omni_apps_state_path(), "r");
    if (!f) {                               /* fresh system: defaults */
        for (i = 0; i < omni_catalog_n; i++)
            installed[i] = (unsigned char)(omni_catalog[i].preinstalled ||
                                           omni_catalog[i].system);
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *s = line, *e;
        while (*s == ' ' || *s == '\t')
            s++;
        e = s + strcspn(s, " \t\r\n#");
        *e = '\0';
        if (*s && (i = omni_app_find(s)) >= 0)
            installed[i] = 1;
    }
    fclose(f);
    for (i = 0; i < omni_catalog_n; i++)
        if (omni_catalog[i].system)
            installed[i] = 1;
}

/* mkdir -p for the directory part of path */
static void make_parent_dirs(const char *path)
{
    char dir[256];
    char *p;
    snprintf(dir, sizeof(dir), "%s", path);
    p = strrchr(dir, '/');
    if (!p || p == dir)
        return;
    *p = '\0';
    for (p = dir + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(dir, 0755);
            *p = '/';
        }
    }
    mkdir(dir, 0755);
}

int omni_apps_save(const unsigned char installed[OMNI_CATALOG_MAX])
{
    const char *path = omni_apps_state_path();
    char tmp[300];
    FILE *f;
    int i, err;

    make_parent_dirs(path);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f)
        return -1;
    fprintf(f, "# OmniOS installed apps, one id per line (managed by the App Store)\n");
    for (i = 0; i < omni_catalog_n; i++)
        if (installed[i] && !omni_catalog[i].system)
            fprintf(f, "%s\n", omni_catalog[i].id);
    if (fflush(f) != 0 || ferror(f)) {
        err = errno;
        fclose(f);
        unlink(tmp);
        errno = err;
        return -1;
    }
    fclose(f);
    if (rename(tmp, path) != 0) {
        err = errno;
        unlink(tmp);
        errno = err;
        return -1;
    }
    return 0;
}
