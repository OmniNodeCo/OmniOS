/*
 * OmniOS — os/apps/taskmgr.c
 *
 * Task Manager (App Store): running processes with their CPU and memory
 * use, CPU and memory history graphs, and End task. Refreshes every 2 s.
 * Click or Up/Down/PgUp/PgDn to select, Delete or "End task" to end the
 * selected process, C / M to sort by CPU / memory, Esc to close.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../gui/client.h"
#include "../gui/wm.h"          /* OMNI_WM_TITLE_H, OMNI_TASKBAR_H */

#define WIN_W    640
#define WIN_H    500
#define PAD      16
#define GRAPH_Y  48
#define GRAPH_H  64
#define COLS_Y   (GRAPH_Y + GRAPH_H + 14)
#define LIST_Y   (COLS_Y + 22)
#define ROW_H    22
#define FOOT_H   48
#define HIST     60
#define MAXP     512

#define C_BG     0xffffff
#define C_PANEL  0xf6f7f9
#define C_BORDER 0xe3e6eb
#define C_INK    0x1b1f24
#define C_DIM    0x5f6b7a
#define C_SEL    0xe6f0fe
#define C_CPU    0x14b8a6
#define C_MEM    0x8b5cf6

enum { K_ESC = 1, K_C = 46, K_M = 50, K_ENTER = 28, K_HOME = 102, K_UP = 103,
       K_PGUP = 104, K_END = 107, K_DOWN = 108, K_PGDN = 109, K_DELETE = 111 };

struct proc { int pid; char name[32]; long rss_kb; unsigned long ticks; double cpu; };

static struct proc g_p[MAXP];
static int g_np;
static struct { int pid; unsigned long ticks; } g_prev[MAXP];
static int g_nprev;
static unsigned long long g_tot_prev, g_idle_prev;
static double g_cpu_hist[HIST], g_mem_hist[HIST];
static int g_hist_n;
static double g_cpu_now;
static long g_mem_total, g_mem_used;
static int g_sel_pid = -1, g_top, g_sort, g_rows, g_h;
static char g_msg[80];

/* ------------------------------------------------------------------ */
/* data                                                               */
/* ------------------------------------------------------------------ */

static void cpu_totals(unsigned long long *tot, unsigned long long *idle)
{
    unsigned long long v[10] = { 0 };
    FILE *f = fopen("/proc/stat", "r");
    int i;
    *tot = *idle = 0;
    if (!f)
        return;
    if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4) {
        for (i = 0; i < 8; i++)
            *tot += v[i];
        *idle = v[3] + v[4];
    }
    fclose(f);
}

static long meminfo(const char *key)
{
    char line[128];
    size_t k = strlen(key);
    long v = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, key, k) == 0 && line[k] == ':') {
            v = atol(line + k + 1);
            break;
        }
    fclose(f);
    return v;
}

static int by_cpu(const void *a, const void *b)
{
    const struct proc *x = a, *y = b;
    if (x->cpu != y->cpu)
        return x->cpu < y->cpu ? 1 : -1;
    return (x->rss_kb < y->rss_kb) - (x->rss_kb > y->rss_kb);
}
static int by_mem(const void *a, const void *b)
{
    const struct proc *x = a, *y = b;
    if (x->rss_kb != y->rss_kb)
        return x->rss_kb < y->rss_kb ? 1 : -1;
    return (x->cpu < y->cpu) - (x->cpu > y->cpu);
}

static void scan(void)
{
    unsigned long long tot, idle, dt;
    DIR *d = opendir("/proc");
    struct dirent *de;
    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    int i, n = 0;

    cpu_totals(&tot, &idle);
    dt = tot - g_tot_prev;
    while (d && (de = readdir(d)) && n < MAXP) {
        char path[64], buf[512], *rp;
        unsigned long ut = 0, st = 0;
        long rss = 0;
        int pid;
        FILE *f;
        if (!isdigit((unsigned char)de->d_name[0]))
            continue;
        pid = atoi(de->d_name);
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        if (!(f = fopen(path, "r")))
            continue;
        if (!fgets(buf, sizeof(buf), f)) {
            fclose(f);
            continue;
        }
        fclose(f);
        rp = strrchr(buf, ')');
        if (!rp || !strchr(buf, '('))
            continue;
        /* after ")": state ppid pgrp session tty tpgid flags minflt cminflt
         * majflt cmajflt utime stime ... rss is the 22nd field after it */
        if (sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu "
                   "%*d %*d %*d %*d %*d %*d %*u %*u %ld", &ut, &st, &rss) != 3)
            continue;
        if (rss <= 0)
            continue;                           /* kernel threads */
        g_p[n].pid = pid;
        *rp = '\0';
        snprintf(g_p[n].name, sizeof(g_p[n].name), "%s", strchr(buf, '(') + 1);
        g_p[n].rss_kb = rss * page_kb;
        g_p[n].ticks = ut + st;
        g_p[n].cpu = 0;
        for (i = 0; i < g_nprev; i++)
            if (g_prev[i].pid == pid) {
                if (dt && g_tot_prev)
                    g_p[n].cpu = 100.0 * (double)(g_p[n].ticks - g_prev[i].ticks) / (double)dt;
                break;
            }
        n++;
    }
    if (d)
        closedir(d);
    g_np = n;
    for (i = 0; i < n; i++) {
        g_prev[i].pid = g_p[i].pid;
        g_prev[i].ticks = g_p[i].ticks;
    }
    g_nprev = n;
    g_cpu_now = (dt && g_tot_prev) ? 100.0 * (double)(dt - (idle - g_idle_prev)) / (double)dt : 0;
    g_tot_prev = tot;
    g_idle_prev = idle;
    g_mem_total = meminfo("MemTotal");
    g_mem_used = g_mem_total - meminfo("MemAvailable");
    memmove(g_cpu_hist, g_cpu_hist + 1, sizeof(double) * (HIST - 1));
    memmove(g_mem_hist, g_mem_hist + 1, sizeof(double) * (HIST - 1));
    g_cpu_hist[HIST - 1] = g_cpu_now;
    g_mem_hist[HIST - 1] = g_mem_total ? 100.0 * (double)g_mem_used / (double)g_mem_total : 0;
    if (g_hist_n < HIST)
        g_hist_n++;
    qsort(g_p, (size_t)n, sizeof(g_p[0]), g_sort ? by_mem : by_cpu);
}

static int sel_index(void)
{
    int i;
    for (i = 0; i < g_np; i++)
        if (g_p[i].pid == g_sel_pid)
            return i;
    return -1;
}

static void select_index(int i)
{
    if (g_np == 0)
        return;
    if (i < 0) i = 0;
    if (i >= g_np) i = g_np - 1;
    g_sel_pid = g_p[i].pid;
    if (i < g_top)
        g_top = i;
    if (i >= g_top + g_rows)
        g_top = i - g_rows + 1;
}

/* ------------------------------------------------------------------ */
/* drawing                                                            */
/* ------------------------------------------------------------------ */

static void graph(struct omni_client_conn *c, int x, int w, const char *label,
                  const char *value, const double *hist, uint32_t col)
{
    int gx = x + 150, gw = w - 150 - 12, bw = gw / HIST, i;
    omni_client_rfill(c, x, GRAPH_Y, w, GRAPH_H, 8, C_BORDER);
    omni_client_rfill(c, x + 1, GRAPH_Y + 1, w - 2, GRAPH_H - 2, 7, C_PANEL);
    omni_client_textt(c, x + 14, GRAPH_Y + 16, C_DIM, label);
    omni_client_text2(c, x + 14, GRAPH_Y + 32, C_INK, value);
    omni_client_fill(c, gx, GRAPH_Y + 10, bw * HIST, GRAPH_H - 20, 0xffffff);
    for (i = 0; i < HIST; i++) {
        int bh = (int)(hist[i] * (GRAPH_H - 22) / 100.0 + 0.5);
        if (i < HIST - g_hist_n || bh <= 0)
            continue;
        omni_client_fill(c, gx + i * bw, GRAPH_Y + GRAPH_H - 11 - bh, bw - 1, bh, col);
    }
}

static void draw(struct omni_client_conn *c)
{
    char s[96], v[48];
    int i, si = sel_index();

    omni_client_fill(c, 0, 0, WIN_W, g_h, C_BG);
    omni_client_text2(c, PAD, 14, C_INK, "Processes");
    snprintf(s, sizeof(s), "%d running  -  sorted by %s (C/M)", g_np, g_sort ? "memory" : "CPU");
    omni_client_textt(c, PAD + 160, 20, C_DIM, s);

    snprintf(v, sizeof(v), "%.0f%%", g_cpu_now);
    graph(c, PAD, (WIN_W - 3 * PAD) / 2, "CPU", v, g_cpu_hist, C_CPU);
    snprintf(v, sizeof(v), "%ld MB", g_mem_used / 1024);
    snprintf(s, sizeof(s), "Memory of %ld MB", g_mem_total / 1024);
    graph(c, PAD * 2 + (WIN_W - 3 * PAD) / 2, (WIN_W - 3 * PAD) / 2, s, v, g_mem_hist, C_MEM);

    omni_client_fill(c, PAD, COLS_Y + 16, WIN_W - 2 * PAD, 1, C_BORDER);
    omni_client_textt(c, PAD + 8, COLS_Y + 2, C_DIM, "Name");
    omni_client_textt(c, 330, COLS_Y + 2, C_DIM, "PID");
    omni_client_textt(c, 420, COLS_Y + 2, g_sort ? C_DIM : C_INK, "CPU");
    omni_client_textt(c, 510, COLS_Y + 2, g_sort ? C_INK : C_DIM, "Memory");
    for (i = 0; i < g_rows && g_top + i < g_np; i++) {
        const struct proc *p = &g_p[g_top + i];
        int y = LIST_Y + i * ROW_H;
        if (g_top + i == si)
            omni_client_rfill(c, PAD, y, WIN_W - 2 * PAD, ROW_H, 5, C_SEL);
        omni_client_textt(c, PAD + 8, y + 7, C_INK, p->name);
        snprintf(s, sizeof(s), "%d", p->pid);
        omni_client_textt(c, 330, y + 7, C_DIM, s);
        snprintf(s, sizeof(s), "%4.1f%%", p->cpu);
        omni_client_textt(c, 420, y + 7, p->cpu >= 10 ? 0xb45309 : C_INK, s);
        snprintf(s, sizeof(s), "%.1f MB", (double)p->rss_kb / 1024.0);
        omni_client_textt(c, 510, y + 7, C_INK, s);
    }
    omni_client_fill(c, 0, g_h - FOOT_H, WIN_W, FOOT_H, C_PANEL);
    omni_client_fill(c, 0, g_h - FOOT_H, WIN_W, 1, C_BORDER);
    omni_client_textt(c, PAD, g_h - FOOT_H + 20, g_msg[0] ? 0xb91c1c : C_DIM,
                      g_msg[0] ? g_msg : "Select a process, then End task (or Delete)");
    omni_client_rfill(c, WIN_W - PAD - 110, g_h - FOOT_H + 9, 110, 30, 6, si >= 0 ? 0xdc2626 : 0xd1d5db);
    omni_client_textt(c, WIN_W - PAD - 110 + (110 - 8 * 8) / 2, g_h - FOOT_H + 20, 0xffffff, "End task");
}

static void end_task(void)
{
    int si = sel_index();
    g_msg[0] = '\0';
    if (si < 0)
        return;
    if (g_p[si].pid == 1 || g_p[si].pid == getpid()) {
        snprintf(g_msg, sizeof(g_msg), "%s can't be ended.", g_p[si].name);
        return;
    }
    if (kill(g_p[si].pid, SIGTERM) != 0)
        snprintf(g_msg, sizeof(g_msg), "Couldn't end %s.", g_p[si].name);
    else
        snprintf(g_msg, sizeof(g_msg), "Ended %s (%d).", g_p[si].name, g_p[si].pid);
}

#ifndef OMNI_TASKMGR_NO_MAIN
int main(void)
{
    struct omni_client_conn conn;
    int h = WIN_H, quit = 0;

    memset(&conn, 0, sizeof(conn));
    if (omni_client_open(&conn, "Task Manager") < 0)
        return 127;
    if (conn.screen_h > 0 && h > conn.screen_h - OMNI_TASKBAR_H - 24)
        h = conn.screen_h - OMNI_TASKBAR_H - 24;
    if (omni_client_window(&conn, "Task Manager", WIN_W, h) < 0) {
        omni_client_close(&conn);
        return 127;
    }
    g_h = h - OMNI_WM_TITLE_H;
    g_rows = (g_h - LIST_Y - FOOT_H - 4) / ROW_H;
    if (g_rows < 1)
        g_rows = 1;
    scan();
    usleep(200000);                             /* a first CPU sample */
    scan();
    draw(&conn);

    while (!quit) {
        struct pollfd pf = { conn.fd, POLLIN, 0 };
        struct omni_client_event e;
        int r = poll(&pf, 1, 2000), changed = 0;
        if (r < 0)
            continue;
        if (r == 0) {
            scan();
            changed = 1;
        }
        while (r > 0 && !quit && (r = omni_client_poll(&conn, &e)) > 0) {
            if (e.type == 3) {
                quit = 1;
            } else if (e.type == 2 && e.pressed) {
                if (e.y >= LIST_Y && e.y < LIST_Y + g_rows * ROW_H) {
                    select_index(g_top + (e.y - LIST_Y) / ROW_H);
                    g_msg[0] = '\0';
                } else if (e.x >= WIN_W - PAD - 110 && e.y >= g_h - FOOT_H + 9 &&
                           e.y < g_h - FOOT_H + 39) {
                    end_task();
                    scan();
                }
                changed = 1;
            } else if (e.type == 1 && e.pressed) {
                int si = sel_index();
                changed = 1;
                switch (e.key) {
                case K_ESC:    quit = 1; break;
                case K_UP:     select_index(si < 0 ? 0 : si - 1); break;
                case K_DOWN:   select_index(si + 1); break;
                case K_PGUP:   select_index(si - g_rows); break;
                case K_PGDN:   select_index(si + g_rows); break;
                case K_HOME:   select_index(0); break;
                case K_END:    select_index(g_np - 1); break;
                case K_DELETE: end_task(); scan(); break;
                case K_C:      g_sort = 0; scan(); break;
                case K_M:      g_sort = 1; scan(); break;
                default:       changed = 0; break;
                }
            }
        }
        if (r < 0)
            quit = 1;
        if (changed && !quit)
            draw(&conn);
    }
    omni_client_close(&conn);
    return 0;
}
#endif
