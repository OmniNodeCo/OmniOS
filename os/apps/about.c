/*
 * OmniOS — os/apps/about.c
 *
 * About box for the OmniOS desktop.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <unistd.h>

#include "app.h"

int main(void)
{
    struct app_win a;

    app_init(&a, "About OmniOS", 420, 300);
    if (a.t.conn.fd < 0)
        return 127;

    omni_client_clear(&a.t.conn, 0xf0f0f0);
    omni_client_text(&a.t.conn, 24, 24,  "OmniOS");
    omni_client_text(&a.t.conn, 24, 48,  "a lightweight operating system");
    omni_client_text(&a.t.conn, 24, 72,  "built from scratch.");
    omni_client_text(&a.t.conn, 24, 112, "Kernel: Linux (static, x86_64)");
    omni_client_text(&a.t.conn, 24, 128, "libc: musl / shell: ash (BusyBox)");
    omni_client_text(&a.t.conn, 24, 144, "GUI: OmniOS desktop shell");
    omni_client_text(&a.t.conn, 24, 184, "(c) 2025-2026 OmniNodeCo — MIT License");

    for (;;) {
        struct omni_client_event e;
        int r = omni_client_poll(&a.t.conn, &e);
        if (r < 0)
            break;
        if (r == 0) {
            usleep(50000);
            continue;
        }
        if (e.type == 3)
            break;
    }

    app_close(&a);
    return 0;
}
