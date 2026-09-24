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

/* the modern app look */
#define APP_BG      0xffffff        /* content background               */
#define APP_INK     0x1f2937        /* text                             */
#define APP_DIM     0x6b7280        /* secondary text                   */
#define APP_RULE    0xe5e7eb        /* separators                       */
#define APP_ACCENT  0x2563eb
#define APP_LINE_H  18              /* console line pitch               */
#define APP_PAD     16              /* content padding                  */

struct app_console {
    struct omni_client_conn conn;
    int    cols, rows;       /* console size in cells          */
    int    cur_row, cur_col; /* cursor position                */
    int    w, h;             /* window size in pixels          */
    int    ch;               /* content height (h - title bar) */
    int    top;              /* y of the first console row     */
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

/* gradient header band in the app's colour, with a 16 px title and an
 * optional subtitle; the console starts below it. Returns its height. */
int  app_header(struct app_win *a, uint32_t color, const char *title,
                const char *subtitle);
/* one "key   value" console row */
void app_kv(struct app_win *a, const char *key, const char *value);
/* sleep until an event or timeout_ms (-1 = forever); see omni_client_wait */
int  app_wait(struct app_win *a, int timeout_ms);

#endif /* OMNI_OS_APPS_APP_H */
