/*
 * OmniOS — os/apps/about.c
 *
 * About OmniOS: version and what the system is made of.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "app.h"

#define COLOR 0x7c3aed              /* violet, as its Start menu icon */

int main(void)
{
    struct app_win a;
    char sub[64] = "Development build";
    FILE *f;

    app_init(&a, "About OmniOS", 440, 330);
    if (a.t.conn.fd < 0)
        return 127;

    f = fopen("/etc/omnios-release", "r");
    if (f) {
        char line[128], name[32], ver[32];
        if (fgets(line, sizeof(line), f) &&
            sscanf(line, "%31s %31s", name, ver) == 2)
            snprintf(sub, sizeof(sub), "Version %s", ver);
        fclose(f);
    }
    app_header(&a, COLOR, "OmniOS", sub);

    app_put(&a, "A lightweight operating system whose desktop,\n");
    app_put(&a, "window manager and apps are built from scratch.\n\n");
    app_kv(&a, "Kernel", "Linux (static, x86_64)");
    app_kv(&a, "C library", "musl");
    app_kv(&a, "Shell", "ash (BusyBox)");
    app_kv(&a, "Desktop", "OmniOS desktop shell");
    omni_client_textc(&a.t.conn, APP_PAD, a.t.ch - 28,
                      APP_DIM, APP_BG, "(c) 2025-2026 OmniNodeCo - MIT License");

    for (;;) {
        struct omni_client_event e;
        int r = app_wait(&a, -1), closed = 0;
        if (r < 0)
            break;
        while ((r = omni_client_poll(&a.t.conn, &e)) > 0)
            if (e.type == 3 || (e.type == 1 && e.pressed && e.key == 1))
                closed = 1;
        if (r < 0 || closed)
            break;
    }

    app_close(&a);
    return 0;
}
