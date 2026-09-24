/*
 * OmniOS — os/apps/calendar.c
 *
 * Calendar (App Store): a month at a time, today highlighted. Click a day
 * or use the arrow keys (a day / a week), PgUp/PgDn or the arrow buttons
 * for the month, Home or "Today" to come back, Esc to close.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../gui/client.h"
#include "../gui/wm.h"          /* OMNI_WM_TITLE_H */

#define WIN_W    420
#define CELL_W   54
#define CELL_H   44
#define GRID_X   21
#define GRID_Y   92
#define WIN_H    (GRID_Y + 6 * CELL_H + 72 + OMNI_WM_TITLE_H)

#define C_BG     0xffffff
#define C_INK    0x1b1f24
#define C_DIM    0x9aa3ae
#define C_WEEK   0x5f6b7a
#define C_RED    0xef4444
#define C_SEL    0xfde2e2
#define C_BTN    0xf3f4f6

enum { K_ESC = 1, K_T = 20, K_HOME = 102, K_UP = 103, K_PGUP = 104, K_LEFT = 105,
       K_RIGHT = 106, K_DOWN = 108, K_PGDN = 109 };

static int g_y, g_m, g_d;                /* selected date (m: 0..11)      */
static int g_ty, g_tm, g_td;             /* today                         */

static int days_in(int y, int m)
{
    static const int dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return dm[m] + (m == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0));
}

/* weekday (0 = Sunday) of y-m-d */
static int weekday(int y, int m, int d)
{
    struct tm t;
    time_t tt;
    memset(&t, 0, sizeof(t));
    t.tm_year = y - 1900; t.tm_mon = m; t.tm_mday = d; t.tm_hour = 12;
    tt = timegm(&t);
    gmtime_r(&tt, &t);
    return t.tm_wday;
}

static void today(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    g_ty = tm.tm_year + 1900; g_tm = tm.tm_mon; g_td = tm.tm_mday;
}

/* move the selection by `days` (may cross months and years) */
static void add_days(int days)
{
    struct tm t;
    time_t tt;
    memset(&t, 0, sizeof(t));
    t.tm_year = g_y - 1900; t.tm_mon = g_m; t.tm_mday = g_d + days; t.tm_hour = 12;
    tt = timegm(&t);
    gmtime_r(&tt, &t);
    g_y = t.tm_year + 1900; g_m = t.tm_mon; g_d = t.tm_mday;
}

static void add_months(int n)
{
    int m = g_m + n;
    g_y += (m < 0) ? (m - 11) / 12 : m / 12;
    g_m = ((m % 12) + 12) % 12;
    if (g_d > days_in(g_y, g_m))
        g_d = days_in(g_y, g_m);
}

static void button(struct omni_client_conn *c, int x, int y, int w, const char *s)
{
    omni_client_rfill(c, x, y, w, 30, 6, C_BTN);
    omni_client_textt(c, x + (w - (int)strlen(s) * 8) / 2, y + 11, C_INK, s);
}

static void draw(struct omni_client_conn *c, int h)
{
    static const char *mon[12] = { "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December" };
    static const char *wd[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
    char s[96];
    int first = weekday(g_y, g_m, 1), dim = days_in(g_y, g_m);
    int py = g_m ? g_y : g_y - 1, pm = g_m ? g_m - 1 : 11, pdim = days_in(py, pm);
    int i;
    struct tm t;
    time_t tt;

    omni_client_fill(c, 0, 0, WIN_W, h, C_BG);
    snprintf(s, sizeof(s), "%s %d", mon[g_m], g_y);
    omni_client_text2(c, GRID_X + 4, 22, C_INK, s);
    button(c, WIN_W - 21 - 34 - 4 - 34 - 8 - 64, 16, 64, "Today");
    button(c, WIN_W - 21 - 34 - 4 - 34, 16, 34, "<");
    button(c, WIN_W - 21 - 34, 16, 34, ">");
    for (i = 0; i < 7; i++)
        omni_client_textt(c, GRID_X + i * CELL_W + (CELL_W - 16) / 2, GRID_Y - 18,
                          (i == 0 || i == 6) ? C_RED : C_WEEK, wd[i]);
    for (i = 0; i < 42; i++) {
        int day = i - first + 1, cx = GRID_X + (i % 7) * CELL_W, cy = GRID_Y + (i / 7) * CELL_H;
        int in = day >= 1 && day <= dim, shown = in ? day : (day < 1 ? pdim + day : day - dim);
        int is_today = in && g_y == g_ty && g_m == g_tm && day == g_td;
        int is_sel = in && day == g_d;
        uint32_t fg = in ? C_INK : C_DIM;
        snprintf(s, sizeof(s), "%d", shown);
        if (is_sel && !is_today)
            omni_client_rfill(c, cx + (CELL_W - 36) / 2, cy + 4, 36, 36, 18, C_SEL);
        if (is_today) {
            omni_client_rfill(c, cx + (CELL_W - 36) / 2, cy + 4, 36, 36, 18, C_RED);
            fg = 0xffffff;
        }
        omni_client_textt(c, cx + (CELL_W - (int)strlen(s) * 8) / 2, cy + 18, fg, s);
    }
    memset(&t, 0, sizeof(t));
    t.tm_year = g_y - 1900; t.tm_mon = g_m; t.tm_mday = g_d; t.tm_hour = 12;
    tt = timegm(&t);
    gmtime_r(&tt, &t);
    omni_client_fill(c, GRID_X, GRID_Y + 6 * CELL_H + 8, 7 * CELL_W, 1, 0xe5e7eb);
    strftime(s, sizeof(s), "%A, %B %d, %Y", &t);
    omni_client_textt(c, GRID_X + 4, GRID_Y + 6 * CELL_H + 24, C_INK, s);
    strftime(s, sizeof(s), "Day %j of the year  -  week %V", &t);
    omni_client_textt(c, GRID_X + 4, GRID_Y + 6 * CELL_H + 42, C_WEEK, s);
}

#ifndef OMNI_CALENDAR_NO_MAIN
int main(void)
{
    struct omni_client_conn conn;
    int h, quit = 0, shown_day;

    memset(&conn, 0, sizeof(conn));
    if (omni_client_open(&conn, "Calendar") < 0)
        return 127;
    if (omni_client_window(&conn, "Calendar", WIN_W, WIN_H) < 0) {
        omni_client_close(&conn);
        return 127;
    }
    h = WIN_H - OMNI_WM_TITLE_H;
    today();
    g_y = g_ty; g_m = g_tm; g_d = g_td;
    shown_day = g_td;
    draw(&conn, h);

    while (!quit) {
        struct pollfd pf = { conn.fd, POLLIN, 0 };
        struct omni_client_event e;
        int r = poll(&pf, 1, 60000), changed = 0;   /* a minute: notice midnight */
        if (r < 0)
            continue;
        if (r == 0) {
            today();
            changed = g_td != shown_day;
            shown_day = g_td;
        }
        while (r > 0 && !quit && (r = omni_client_poll(&conn, &e)) > 0) {
            if (e.type == 3) {
                quit = 1;
            } else if (e.type == 2 && e.pressed) {
                int bx = WIN_W - 21 - 34;
                changed = 1;
                if (e.y >= 16 && e.y < 46) {
                    if (e.x >= bx)                          add_months(1);
                    else if (e.x >= bx - 38)                add_months(-1);
                    else if (e.x >= bx - 38 - 8 - 64 && e.x < bx - 46) {
                        today(); g_y = g_ty; g_m = g_tm; g_d = g_td;
                    }
                } else if (e.x >= GRID_X && e.x < GRID_X + 7 * CELL_W &&
                           e.y >= GRID_Y && e.y < GRID_Y + 6 * CELL_H) {
                    int i = (e.y - GRID_Y) / CELL_H * 7 + (e.x - GRID_X) / CELL_W;
                    int day = i - weekday(g_y, g_m, 1) + 1;
                    if (day >= 1 && day <= days_in(g_y, g_m))
                        g_d = day;
                    else
                        add_days(day - g_d);                /* a neighbour month */
                }
            } else if (e.type == 1 && e.pressed) {
                changed = 1;
                switch (e.key) {
                case K_ESC:   quit = 1; break;
                case K_LEFT:  add_days(-1); break;
                case K_RIGHT: add_days(1); break;
                case K_UP:    add_days(-7); break;
                case K_DOWN:  add_days(7); break;
                case K_PGUP:  add_months(-1); break;
                case K_PGDN:  add_months(1); break;
                case K_HOME: case K_T:
                    today(); g_y = g_ty; g_m = g_tm; g_d = g_td;
                    break;
                default:      changed = 0; break;
                }
            }
        }
        if (r < 0)
            quit = 1;
        if (changed && !quit)
            draw(&conn, h);
    }
    omni_client_close(&conn);
    return 0;
}
#endif
