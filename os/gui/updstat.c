/*
 * OmniOS — os/gui/updstat.c
 *
 * OmniOS Update status file (see updstat.h).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "updstat.h"

static const char *g_state_names[] = {
    "idle", "checking", "uptodate", "available", "downloading", "ready", "error"
};

const char *omni_update_dir(void)
{
    const char *p = getenv("OMNI_UPDATE_DIR");
    return (p && *p) ? p : OMNI_UPDATE_DIR;
}

int omni_update_read(struct omni_update_status *st)
{
    char path[512], line[256];
    FILE *f;

    memset(st, 0, sizeof(*st));
    snprintf(st->current, sizeof(st->current), "%s", omni_update_running_version());
    snprintf(path, sizeof(path), "%s/status", omni_update_dir());
    f = fopen(path, "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '='), *v;
        int i;
        if (!eq)
            continue;
        *eq = '\0';
        v = eq + 1;
        v[strcspn(v, "\r\n")] = '\0';
        if (strcmp(line, "state") == 0) {
            for (i = 0; i < (int)(sizeof(g_state_names) / sizeof(g_state_names[0])); i++)
                if (strcmp(v, g_state_names[i]) == 0)
                    st->state = i;
        } else if (strcmp(line, "latest") == 0) {
            snprintf(st->latest, sizeof(st->latest), "%s", v);
        } else if (strcmp(line, "progress") == 0) {
            st->progress = atoi(v);
        } else if (strcmp(line, "message") == 0) {
            snprintf(st->message, sizeof(st->message), "%s", v);
        } else if (strcmp(line, "checked") == 0) {
            st->checked = atol(v);
        }
    }
    fclose(f);
    return 0;
}

int omni_update_write(const struct omni_update_status *st)
{
    char path[512], tmp[512];
    FILE *f;

    mkdir(omni_update_dir(), 0755);
    snprintf(path, sizeof(path), "%s/status", omni_update_dir());
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f)
        return -1;
    fprintf(f, "state=%s\ncurrent=%s\nlatest=%s\nprogress=%d\nmessage=%s\nchecked=%ld\n",
            g_state_names[(st->state >= 0 && st->state <= UPD_ERROR) ? st->state : 0],
            st->current, st->latest, st->progress, st->message, st->checked);
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

void omni_update_log(const char *fmt, ...)
{
    char path[512], when[32];
    time_t t = time(NULL);
    struct tm tmv;
    va_list ap;
    FILE *f;

    mkdir(omni_update_dir(), 0755);
    snprintf(path, sizeof(path), "%s/history", omni_update_dir());
    f = fopen(path, "a");
    if (!f)
        return;
    localtime_r(&t, &tmv);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tmv);
    fprintf(f, "%s  ", when);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

const char *omni_update_running_version(void)
{
    static char ver[32];
    char line[128], name[32];
    const char *p = getenv("OMNI_RELEASE_FILE");
    FILE *f;

    if (ver[0])
        return ver;
    f = fopen((p && *p) ? p : "/etc/omnios-release", "r");
    if (f) {
        if (fgets(line, sizeof(line), f))
            sscanf(line, "%31s %31s", name, ver);
        fclose(f);
    }
    return ver;
}
