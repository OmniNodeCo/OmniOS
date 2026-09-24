/*
 * OmniOS — os/apps/app.c
 *
 * App scaffold: connects to the desktop shell, opens a window, and exposes
 * a stream-oriented "console" the apps print into.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "../gui/wm.h"          /* OMNI_WM_TITLE_H */

void app_init(struct app_win *a, const char *title, int w, int h)
{
    memset(a, 0, sizeof(*a));
    if (omni_client_open(&a->t.conn, title) < 0)
        return;
    if (omni_client_window(&a->t.conn, title, w, h) < 0) {
        omni_client_close(&a->t.conn);
        return;
    }
    a->t.w = w;
    a->t.h = h;
    a->t.ch = h - OMNI_WM_TITLE_H;
    a->t.top = APP_PAD;
    a->t.cols = (w - 2 * APP_PAD) / 8;
    a->t.rows = (h - OMNI_WM_TITLE_H - 2 * APP_PAD) / APP_LINE_H;
    if (a->t.cols < 16) a->t.cols = 16;
    if (a->t.rows < 4)  a->t.rows = 4;
    omni_client_clear(&a->t.conn, APP_BG);
    a->t.cur_row = 0;
    a->t.cur_col = 0;
}

void app_close(struct app_win *a)
{
    omni_client_close(&a->t.conn);
}

void app_setpos(struct app_win *a, int col, int row)
{
    a->t.cur_col = col;
    a->t.cur_row = row;
}

static int row_y(const struct app_win *a)
{
    return a->t.top + a->t.cur_row * APP_LINE_H + (APP_LINE_H - 8) / 2;
}

/* send a run of characters as one message (not one per character) */
static void flush_run(struct app_win *a, char *run, int *n, int col0)
{
    if (*n == 0)
        return;
    run[*n] = '\0';
    omni_client_textc(&a->t.conn, APP_PAD + col0 * 8, row_y(a), APP_INK,
                      APP_BG, run);
    *n = 0;
}

void app_put(struct app_win *a, const char *s)
{
    char run[128];
    int n = 0, col0 = a->t.cur_col;

    for (; *s; s++) {
        if (*s == '\n') {
            flush_run(a, run, &n, col0);
            app_newline(a);
            col0 = a->t.cur_col;
            continue;
        }
        if (a->t.cur_col >= a->t.cols) {
            flush_run(a, run, &n, col0);
            app_newline(a);
            col0 = a->t.cur_col;
        }
        run[n++] = *s;
        a->t.cur_col++;
        if (n >= (int)sizeof(run) - 1) {
            flush_run(a, run, &n, col0);
            col0 = a->t.cur_col;
        }
    }
    flush_run(a, run, &n, col0);
}

void app_printf(struct app_win *a, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    app_put(a, buf);
}

void app_newline(struct app_win *a)
{
    a->t.cur_col = 0;
    a->t.cur_row++;
    if (a->t.cur_row >= a->t.rows) {
        /* scroll by redrawing the buffer is app-specific; simple wrap */
        a->t.cur_row = a->t.rows - 1;
    }
}

void app_clear(struct app_win *a)
{
    omni_client_clear(&a->t.conn, APP_BG);
    a->t.cur_col = 0;
    a->t.cur_row = 0;
}

void app_box(struct app_win *a, int x, int y, int w, int h, const char *title)
{
    omni_client_rfill(&a->t.conn, x, y, w, h, 8, APP_RULE);
    omni_client_rfill(&a->t.conn, x + 1, y + 1, w - 2, h - 2, 7, APP_BG);
    if (title)
        omni_client_textt(&a->t.conn, x + 12, y + 10, APP_INK, title);
}

static uint32_t mix(uint32_t a, uint32_t b, int t, int n)   /* a->b, t/n */
{
    uint32_t r = (((a >> 16) & 255) * (uint32_t)(n - t) + ((b >> 16) & 255) * (uint32_t)t) / (uint32_t)n;
    uint32_t g = (((a >> 8) & 255) * (uint32_t)(n - t) + ((b >> 8) & 255) * (uint32_t)t) / (uint32_t)n;
    uint32_t bl = ((a & 255) * (uint32_t)(n - t) + (b & 255) * (uint32_t)t) / (uint32_t)n;
    return (r << 16) | (g << 8) | bl;
}

int app_header(struct app_win *a, uint32_t color, const char *title,
               const char *subtitle)
{
    int hh = subtitle ? 68 : 52;
    uint32_t right = mix(color, 0x000000, 7, 20);    /* tone on tone */

    omni_client_grad(&a->t.conn, 0, 0, a->t.w, hh, color, right, 0);
    omni_client_text2(&a->t.conn, APP_PAD + 2, subtitle ? 16 : 18, 0xffffff, title);
    if (subtitle)
        omni_client_textt(&a->t.conn, APP_PAD + 2, 42, 0xe0e7ff, subtitle);
    a->t.top = hh + 12;
    a->t.rows = (a->t.ch - a->t.top - APP_PAD) / APP_LINE_H;
    if (a->t.rows < 1)
        a->t.rows = 1;
    a->t.cur_row = 0;
    a->t.cur_col = 0;
    return hh;
}

void app_kv(struct app_win *a, const char *key, const char *value)
{
    int y = row_y(a);
    omni_client_textc(&a->t.conn, APP_PAD, y, APP_DIM, APP_BG, key);
    omni_client_textc(&a->t.conn, APP_PAD + 15 * 8, y, APP_INK, APP_BG, value);
    app_newline(a);
}

int app_wait(struct app_win *a, int timeout_ms)
{
    return omni_client_wait(&a->t.conn, timeout_ms);
}
