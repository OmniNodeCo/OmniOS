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
    a->t.cols = (w - 20) / 8;
    a->t.rows = (h - 24) / 8;
    if (a->t.cols < 16) a->t.cols = 16;
    if (a->t.rows < 4)  a->t.rows = 4;
    omni_client_clear(&a->t.conn, 0xf0f0f0);
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

void app_put(struct app_win *a, const char *s)
{
    const char *p = s;
    while (*p) {
        char ch = *p++;
        if (ch == '\n') {
            app_newline(a);
            continue;
        }
        if (a->t.cur_col >= a->t.cols)
            app_newline(a);
        {
            char cbuf[2] = { ch, 0 };
            omni_client_text(&a->t.conn,
                             12 + a->t.cur_col * 8,
                             12 + a->t.cur_row * 8, cbuf);
        }
        a->t.cur_col++;
    }
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
    omni_client_clear(&a->t.conn, 0xf0f0f0);
    a->t.cur_col = 0;
    a->t.cur_row = 0;
}

void app_box(struct app_win *a, int x, int y, int w, int h, const char *title)
{
    uint32_t ink  = 0x101010;
    uint32_t face = 0xf0f0f0;
    omni_client_fill(&a->t.conn, x, y, w, h, face);
    omni_client_rect(&a->t.conn, x, y, w, h, ink);
    if (title) {
        omni_client_text(&a->t.conn, x + 4, y + 2, title);
    }
}
