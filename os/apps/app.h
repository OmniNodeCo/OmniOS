/*
 * OmniOS — os/apps/app.h
 *
 * Shared scaffold for the bundled desktop applications. Each app is a
 * separate process that opens a window on the desktop shell via the client
 * library and draws into it with a tiny console-style API (app_put /
 * app_printf) that remotes the text to the server.
 */
#ifndef OMNI_OS_APPS_APP_H
#define OMNI_OS_APPS_APP_H

#include "../gui/client.h"
#include "../lib/omni.h"

#define OMNI_APP_ROWS 24
#define OMNI_APP_COLS 78

struct app_console {
    struct omni_client_conn conn;
    int    cols, rows;       /* window size in cells           */
    int    cur_row, cur_col; /* cursor position                */
    int    w, h;             /* window size in pixels          */
};

struct app_win {
    struct app_console t;
};

void app_init(struct app_win *a, const char *title, int w, int h);
void app_close(struct app_win *a);

void app_put(struct app_win *a, const char *s);
void app_printf(struct app_win *a, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void app_newline(struct app_win *a);
void app_clear(struct app_win *a);
void app_setpos(struct app_win *a, int col, int row);

/* draw a framed box + prompt; returns after writing */
void app_box(struct app_win *a, int x, int y, int w, int h, const char *title);

#endif /* OMNI_OS_APPS_APP_H */
