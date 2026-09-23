/*
 * OmniOS — os/apps/sysinfo.c
 *
 * Reads /proc and friends and displays a System-Info sheet.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>

#include "app.h"

static void row(struct app_win *a, const char *k, const char *v)
{
    app_printf(a, "%s%s\n", k, v);
}

int main(void)
{
    struct app_win a;
    struct utsname u;
    struct sysinfo si;
    char line[256];
    FILE *f;

    app_init(&a, "System Information", 560, 420);
    if (a.t.conn.fd < 0)
        return 127;

    omni_client_clear(&a.t.conn, 0xf0f0f0);

    if (uname(&u) == 0) {
        row(&a, "OS:        ", u.sysname);
        row(&a, "Kernel:    ", u.release);
        row(&a, "Arch:      ", u.machine);
        row(&a, "Host:      ", u.nodename);
    }
    if (sysinfo(&si) == 0) {
        app_printf(&a, "RAM:       %lu MiB total\n",
                   (unsigned long)(si.totalram / 1024 / 1024));
        app_printf(&a, "Uptime:    %ld s\n", si.uptime);
    }

    f = fopen("/proc/meminfo", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "MemTotal", 8) ||
                !strncmp(line, "MemFree", 7) ||
                !strncmp(line, "Buffers", 7) ||
                !strncmp(line, "Cached", 6)) {
                line[strcspn(line, "\n")] = '\0';
                app_printf(&a, "%-12s %s\n", "", line);
            }
        }
        fclose(f);
    }

    app_printf(&a, "\nOmniOS — a lightweight OS built from source.\n");
    app_printf(&a, "GUI: OmniOS NanoX-style desktop shell.\n");

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
