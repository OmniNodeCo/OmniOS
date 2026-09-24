/*
 * OmniOS — os/apps/sysinfo.c
 *
 * System Information: OS, kernel, CPU, memory (with a usage bar) and
 * uptime, read from /proc and refreshed every two seconds.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>

#include "app.h"

#define COLOR 0x475569              /* slate, as its Start menu icon */

static int g_body_y;                /* first y below the header      */

static void release(char *out, size_t n)
{
    FILE *f = fopen("/etc/omnios-release", "r");
    char line[128], name[32], ver[32];
    snprintf(out, n, "OmniOS");
    if (!f)
        return;
    if (fgets(line, sizeof(line), f) &&
        sscanf(line, "%31s %31s", name, ver) == 2)
        snprintf(out, n, "%s %s", name, ver);
    fclose(f);
}

static void cpu_model(char *out, size_t n, int max_chars)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[256];
    int cpus = 0;

    snprintf(out, n, "unknown");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "model name", 10) != 0)
                continue;
            if (cpus++ == 0) {
                char *v = strchr(line, ':');
                if (v) {
                    v++;
                    while (*v == ' ')
                        v++;
                    v[strcspn(v, "\n")] = '\0';
                    snprintf(out, n, "%s", v);
                }
            }
        }
        fclose(f);
    }
    if (cpus > 1) {
        char tail[16];
        size_t room;
        snprintf(tail, sizeof(tail), " x%d", cpus);
        room = (size_t)max_chars > strlen(tail) ? (size_t)max_chars - strlen(tail) : 0;
        if (strlen(out) > room && room < n)
            out[room] = '\0';
        strncat(out, tail, n - strlen(out) - 1);
    } else if ((int)strlen(out) > max_chars && max_chars > 0 && (size_t)max_chars < n) {
        out[max_chars] = '\0';
    }
}

static long meminfo_kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    size_t k = strlen(key);
    long v = -1;

    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, key, k) == 0 && line[k] == ':') {
            v = atol(line + k + 1);
            break;
        }
    fclose(f);
    return v;
}

static void draw(struct app_win *a)
{
    struct utsname u;
    struct sysinfo si;
    char buf[160];
    long total = meminfo_kb("MemTotal"), avail = meminfo_kb("MemAvailable");
    int value_chars = (a->t.w - APP_PAD * 2 - 15 * 8) / 8;

    /* redraw the body from scratch: values change length */
    omni_client_fill(&a->t.conn, 0, g_body_y, a->t.w, a->t.h, APP_BG);
    app_setpos(a, 0, 0);

    release(buf, sizeof(buf));
    app_kv(a, "System", buf);
    if (uname(&u) == 0) {
        snprintf(buf, sizeof(buf), "%s %s", u.sysname, u.release);
        app_kv(a, "Kernel", buf);
        app_kv(a, "Architecture", u.machine);
        app_kv(a, "Computer name", u.nodename);
    }
    cpu_model(buf, sizeof(buf), value_chars);
    app_kv(a, "Processor", buf);
    if (a->t.conn.screen_w > 0) {
        snprintf(buf, sizeof(buf), "%d x %d", a->t.conn.screen_w,
                 a->t.conn.screen_h);
        app_kv(a, "Display", buf);
    }
    if (sysinfo(&si) == 0) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)si.procs);
        app_kv(a, "Processes", buf);
        snprintf(buf, sizeof(buf), "%.2f  %.2f  %.2f", si.loads[0] / 65536.0,
                 si.loads[1] / 65536.0, si.loads[2] / 65536.0);
        app_kv(a, "Load average", buf);
    }
    if (sysinfo(&si) == 0) {
        long d = si.uptime / 86400, h = (si.uptime / 3600) % 24;
        long m = (si.uptime / 60) % 60;
        if (d > 0)
            snprintf(buf, sizeof(buf), "%ld d %ld h %ld min", d, h, m);
        else if (h > 0)
            snprintf(buf, sizeof(buf), "%ld h %ld min", h, m);
        else
            snprintf(buf, sizeof(buf), "%ld min %ld s", m, si.uptime % 60);
        app_kv(a, "Uptime", buf);
    }

    /* memory: numbers plus a usage bar */
    if (total > 0) {
        long used = avail >= 0 ? total - avail : -1;
        int bar_y, bar_w = a->t.w - 2 * APP_PAD;
        if (used >= 0)
            snprintf(buf, sizeof(buf), "%ld MiB used of %ld MiB", used / 1024,
                     total / 1024);
        else
            snprintf(buf, sizeof(buf), "%ld MiB", total / 1024);
        app_kv(a, "Memory", buf);
        bar_y = a->t.top + a->t.cur_row * APP_LINE_H + 4;
        omni_client_rfill(&a->t.conn, APP_PAD, bar_y, bar_w, 10, 5, APP_RULE);
        if (used > 0) {
            int fw = (int)((long long)bar_w * used / total);
            if (fw < 10)
                fw = 10;
            omni_client_rfill(&a->t.conn, APP_PAD, bar_y, fw, 10, 5, APP_ACCENT);
        }
    }

    omni_client_textc(&a->t.conn, APP_PAD, a->t.ch - 26,
                      APP_DIM, APP_BG, "Updates every 2 seconds.");
}

int main(void)
{
    struct app_win a;

    app_init(&a, "System Information", 560, 400);
    if (a.t.conn.fd < 0)
        return 127;
    g_body_y = app_header(&a, COLOR, "System Information", "About this computer");
    draw(&a);

    for (;;) {
        struct omni_client_event e;
        int r = app_wait(&a, 2000), closed = 0;

        if (r < 0)
            break;
        if (r == 0) {                   /* timeout: refresh the numbers */
            draw(&a);
            continue;
        }
        while ((r = omni_client_poll(&a.t.conn, &e)) > 0) {
            if (e.type == 3 || (e.type == 1 && e.pressed && e.key == 1))
                closed = 1;
        }
        if (r < 0 || closed)
            break;
    }

    app_close(&a);
    return 0;
}
