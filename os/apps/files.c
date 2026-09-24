/*
 * OmniOS — os/apps/files.c
 *
 * File Manager: browse the file system. Folders first, then files, with
 * sizes. Up/Down/PgUp/PgDn/Home/End select, Enter or a double click opens
 * a folder, Backspace goes to the parent folder, Esc closes.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "app.h"

#define COLOR     0xd97706          /* amber, as its Start menu icon */
#define ROW_H     24
#define STATUS_H  28
#define MAXE      1024
#define SEL_BG    0xdbeafe

struct entry {
    char name[256];
    int  dir;
    char kind;                      /* 'd' folder, 'f' file, 'c' device, 's' other */
    long long size;
};

static struct entry g_ents[MAXE];
static int  g_n, g_sel, g_top;      /* entries, selection, first shown */
static int  g_ndirs, g_nfiles;
static char g_cwd[1024] = "/";
static int  g_list_y, g_rows;       /* list geometry                   */

static int by_kind_then_name(const void *pa, const void *pb)
{
    const struct entry *a = pa, *b = pb;
    if (a->dir != b->dir)
        return b->dir - a->dir;         /* folders first */
    return strcasecmp(a->name, b->name);
}

static void load(void)
{
    DIR *d = opendir(g_cwd);
    struct dirent *de;
    int first = 0;

    g_n = g_sel = g_top = g_ndirs = g_nfiles = 0;
    if (strcmp(g_cwd, "/") != 0) {      /* ".." to go up */
        snprintf(g_ents[0].name, sizeof(g_ents[0].name), "..");
        g_ents[0].dir = 1;
        g_ents[0].kind = 'd';
        g_ents[0].size = 0;
        g_n = first = 1;
    }
    if (!d)
        return;
    while ((de = readdir(d)) != NULL && g_n < MAXE) {
        char path[1300];
        struct stat st;
        struct entry *e = &g_ents[g_n];

        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        snprintf(path, sizeof(path), "%s/%s",
                 strcmp(g_cwd, "/") ? g_cwd : "", de->d_name);
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        {
            int ok = stat(path, &st) == 0;       /* follows symlinks */
            e->dir = ok ? S_ISDIR(st.st_mode) : (de->d_type == DT_DIR);
            e->size = (ok && S_ISREG(st.st_mode)) ? (long long)st.st_size : 0;
            e->kind = e->dir ? 'd'
                    : !ok || S_ISREG(st.st_mode) ? 'f'
                    : (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) ? 'c' : 's';
        }
        if (e->dir) g_ndirs++; else g_nfiles++;
        g_n++;
    }
    closedir(d);
    qsort(g_ents + first, (size_t)(g_n - first), sizeof(g_ents[0]), by_kind_then_name);
}

static void human(long long b, char *out, size_t n)
{
    if (b < 1024)
        snprintf(out, n, "%lld B", b);
    else if (b < 1024 * 1024)
        snprintf(out, n, "%.1f KB", (double)b / 1024.0);
    else if (b < 1024LL * 1024 * 1024)
        snprintf(out, n, "%.1f MB", (double)b / (1024.0 * 1024.0));
    else if (b < 1024LL * 1024 * 1024 * 1024)
        snprintf(out, n, "%.1f GB", (double)b / (1024.0 * 1024.0 * 1024.0));
    else
        snprintf(out, n, "%.1f TB", (double)b / (1024.0 * 1024.0 * 1024.0 * 1024.0));
}

static void icon(struct app_win *a, int x, int y, int dir)
{
    if (dir) {                          /* folder: tab + body */
        omni_client_rfill(&a->t.conn, x, y + 1, 8, 5, 2, 0xf59e0b);
        omni_client_rfill(&a->t.conn, x, y + 3, 18, 13, 3, 0xfbbf24);
    } else {                            /* page with text lines */
        omni_client_rfill(&a->t.conn, x + 2, y, 14, 17, 2, 0xcbd5e1);
        omni_client_fill(&a->t.conn, x + 5, y + 5, 8, 1, 0x64748b);
        omni_client_fill(&a->t.conn, x + 5, y + 8, 8, 1, 0x64748b);
        omni_client_fill(&a->t.conn, x + 5, y + 11, 5, 1, 0x64748b);
    }
}

static void draw_list(struct app_win *a)
{
    int i, W = a->t.w;

    omni_client_fill(&a->t.conn, 0, g_list_y, W, g_rows * ROW_H + 4, APP_BG);
    for (i = 0; i < g_rows && g_top + i < g_n; i++) {
        const struct entry *e = &g_ents[g_top + i];
        int y = g_list_y + i * ROW_H;
        char right[32], name[64];
        int maxc = (W - 44 - 110) / 8;

        if (g_top + i == g_sel)
            omni_client_rfill(&a->t.conn, 8, y, W - 16, ROW_H, 6, SEL_BG);
        icon(a, 18, y + 3, e->dir);
        snprintf(name, sizeof(name), "%.*s", maxc > 0 && maxc < 63 ? maxc : 63, e->name);
        omni_client_textt(&a->t.conn, 46, y + 8, APP_INK, name);
        if (e->dir)
            snprintf(right, sizeof(right), "%s", strcmp(e->name, "..") ? "Folder" : "Up");
        else if (e->kind == 'c')
            snprintf(right, sizeof(right), "Device");
        else if (e->kind == 's')
            snprintf(right, sizeof(right), "Special");
        else
            human(e->size, right, sizeof(right));
        omni_client_textt(&a->t.conn, W - 24 - (int)strlen(right) * 8, y + 8,
                          APP_DIM, right);
    }
    if (g_n == 0)
        omni_client_textt(&a->t.conn, 46, g_list_y + 8, APP_DIM,
                          "This folder is empty or cannot be opened.");
}

static void draw(struct app_win *a)
{
    char status[96];
    int ch = a->t.ch;                           /* content height */

    char where[96];
    int maxc = (a->t.w - 2 * APP_PAD) / 8;
    size_t l = strlen(g_cwd);

    if ((int)l > maxc && maxc > 3 && maxc < (int)sizeof(where))   /* ...tail */
        snprintf(where, sizeof(where), "...%s", g_cwd + l - (size_t)(maxc - 3));
    else
        snprintf(where, sizeof(where), "%s", g_cwd);
    omni_client_clear(&a->t.conn, APP_BG);
    g_list_y = app_header(a, COLOR, "File Manager", where) + 8;
    g_rows = (ch - STATUS_H - g_list_y - 4) / ROW_H;
    if (g_rows < 1)
        g_rows = 1;
    draw_list(a);

    omni_client_fill(&a->t.conn, 0, ch - STATUS_H, a->t.w, STATUS_H, 0xf8fafc);
    omni_client_fill(&a->t.conn, 0, ch - STATUS_H, a->t.w, 1, APP_RULE);
    snprintf(status, sizeof(status), "%d folder%s, %d file%s", g_ndirs,
             g_ndirs == 1 ? "" : "s", g_nfiles, g_nfiles == 1 ? "" : "s");
    omni_client_textt(&a->t.conn, APP_PAD, ch - STATUS_H + 10, APP_DIM, status);
    omni_client_textt(&a->t.conn, a->t.w - APP_PAD - 30 * 8, ch - STATUS_H + 10,
                      APP_DIM, "Enter: open   Backspace: up");
}

static void go(struct app_win *a, const char *name)
{
    char prev[256] = "";
    int i;

    if (strcmp(name, "..") == 0) {
        char *slash = strrchr(g_cwd, '/');
        snprintf(prev, sizeof(prev), "%s", slash ? slash + 1 : "");
        if (slash && slash != g_cwd)
            *slash = '\0';
        else
            snprintf(g_cwd, sizeof(g_cwd), "/");
    } else {
        size_t l = strlen(g_cwd);
        if (l + strlen(name) + 2 >= sizeof(g_cwd))
            return;
        snprintf(g_cwd + l, sizeof(g_cwd) - l, "%s%s",
                 strcmp(g_cwd, "/") ? "/" : "", name);
    }
    load();
    for (i = 0; prev[0] && i < g_n; i++)        /* back up: reselect */
        if (strcmp(g_ents[i].name, prev) == 0) {
            g_sel = i;
            if (g_sel >= g_rows)
                g_top = g_sel - g_rows / 2;
            break;
        }
    draw(a);
}

static void select_row(struct app_win *a, int sel)
{
    if (g_n == 0)
        return;
    if (sel < 0) sel = 0;
    if (sel >= g_n) sel = g_n - 1;
    g_sel = sel;
    if (g_sel < g_top)
        g_top = g_sel;
    if (g_sel >= g_top + g_rows)
        g_top = g_sel - g_rows + 1;
    draw_list(a);
}

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

int main(void)
{
    struct app_win a;
    long last_click = 0;
    int last_row = -1;

    app_init(&a, "File Manager", 560, 420);
    if (a.t.conn.fd < 0)
        return 127;
    load();
    draw(&a);

    for (;;) {
        struct omni_client_event e;
        int r = app_wait(&a, -1), quit = 0;

        if (r < 0)
            break;
        while ((r = omni_client_poll(&a.t.conn, &e)) > 0) {
            if (e.type == 3) {
                quit = 1;
            } else if (e.type == 1 && e.pressed) {
                switch (e.key) {
                case 1:   quit = 1; break;                         /* Esc   */
                case 103: select_row(&a, g_sel - 1); break;         /* Up    */
                case 108: select_row(&a, g_sel + 1); break;         /* Down  */
                case 104: select_row(&a, g_sel - g_rows); break;    /* PgUp  */
                case 109: select_row(&a, g_sel + g_rows); break;    /* PgDn  */
                case 102: select_row(&a, 0); break;                 /* Home  */
                case 107: select_row(&a, g_n - 1); break;           /* End   */
                case 14:  if (strcmp(g_cwd, "/")) go(&a, ".."); break;
                case 28: case 96:                                   /* Enter */
                    if (g_n > 0 && g_ents[g_sel].dir)
                        go(&a, g_ents[g_sel].name);
                    break;
                default: break;
                }
            } else if (e.type == 4 && (e.key == 4 || e.key == 5) && g_n > g_rows) {
                int top = g_top + (e.key == 4 ? -3 : 3), s = g_sel;    /* wheel */
                if (top > g_n - g_rows)
                    top = g_n - g_rows;
                if (top < 0)
                    top = 0;
                g_top = top;
                if (s < top)
                    s = top;
                if (s >= top + g_rows)
                    s = top + g_rows - 1;
                select_row(&a, s);
            } else if (e.type == 2 && e.pressed) {
                int row = e.y >= g_list_y ? (e.y - g_list_y) / ROW_H : -1;
                if (row >= 0 && row < g_rows && g_top + row < g_n) {
                    long t = now_ms();
                    int idx = g_top + row;
                    if (idx == last_row && t - last_click < 500 && g_ents[idx].dir) {
                        last_row = -1;
                        go(&a, g_ents[idx].name);           /* double click */
                    } else {
                        last_row = idx;
                        last_click = t;
                        select_row(&a, idx);
                    }
                }
            }
        }
        if (r < 0 || quit)
            break;
    }

    app_close(&a);
    return 0;
}
