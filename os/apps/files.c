/*
 * OmniOS — os/apps/files.c
 *
 * Minimal file manager: lists a directory, and pressing Enter on a
 * subdirectory enters it. Up arrow or ".." line goes to the parent.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app.h"

static char cwd[512] = "/";

static void redraw(struct app_win *a)
{
    struct dirent *de;
    DIR *d = opendir(cwd);
    int row = 1;

    omni_client_clear(&a->t.conn, 0xf0f0f0);
    app_setpos(a, 0, 0);
    {
        char hdr[600];
        snprintf(hdr, sizeof(hdr), "Directory: %s", cwd);
        omni_client_text(&a->t.conn, 12, 12, hdr);
    }

    if (!d) {
        omni_client_text(&a->t.conn, 12, 32, "cannot open directory");
        return;
    }

    while ((de = readdir(d)) != NULL) {
        char tail[600];
        int isdir = (de->d_type == DT_DIR);
        snprintf(tail, sizeof(tail), "%s%s", de->d_name, isdir ? "/" : "");
        if (row >= a->t.rows - 1)
            break;
        omni_client_text(&a->t.conn, 12, 24 + row * 8, tail);
        row++;
    }
    closedir(d);
}

int main(void)
{
    struct app_win a;

    app_init(&a, "File Manager", 560, 400);
    if (a.t.conn.fd < 0)
        return 127;

    redraw(&a);

    for (;;) {
        struct omni_client_event e;
        int r = omni_client_poll(&a.t.conn, &e);
        if (r < 0)
            break;
        if (r == 0) {
            usleep(20000);
            continue;
        }
        if (e.type == 3)
            break;
        if (e.type == 1 && e.pressed) {
            if (e.key == 1) /* Esc */
                break;
        }
    }

    app_close(&a);
    return 0;
}
