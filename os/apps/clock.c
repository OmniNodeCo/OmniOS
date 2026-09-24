/*
 * OmniOS — os/apps/clock.c
 *
 * Clock (App Store app): a large seven-segment HH:MM:SS display plus the
 * date. Only digits that changed are redrawn; the loop wakes on each new
 * second. Esc closes.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../gui/client.h"
#include "../gui/wm.h"          /* OMNI_WM_TITLE_H */

#define WIN_W   300
#define WIN_H   (120 + OMNI_WM_TITLE_H)
#define DIG_W   28
#define DIG_H   50
#define SEG_T   6
#define PAIR_G  6               /* gap between the two digits of a pair */
#define COLON_W 18
#define TOP_Y   16

#define C_BG    0x0b1a2e
#define C_ON    0x39d0ff
#define C_OFF   0x132b44
#define C_DATE  0x9fb8d0

/* segments a..g as bits 0..6 */
static const unsigned char g_digit_segs[10] = {
    0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f
};

/* x of digit slot 0..5 */
static int slot_x(int slot)
{
    int total = 6 * DIG_W + 3 * PAIR_G + 2 * COLON_W;
    int x = (WIN_W - total) / 2;
    int pair = slot / 2;
    return x + pair * (2 * DIG_W + PAIR_G + COLON_W) + (slot % 2) * (DIG_W + PAIR_G);
}

static void draw_digit(struct omni_client_conn *c, int x, int y, int d)
{
    /* rectangles for a, b, c, d, e, f, g */
    const int half = (DIG_H - 3 * SEG_T) / 2;              /* vertical length */
    const int r[7][4] = {
        { x + SEG_T, y,                         DIG_W - 2 * SEG_T, SEG_T },
        { x + DIG_W - SEG_T, y + SEG_T,         SEG_T, half },
        { x + DIG_W - SEG_T, y + 2 * SEG_T + half, SEG_T, half },
        { x + SEG_T, y + DIG_H - SEG_T,         DIG_W - 2 * SEG_T, SEG_T },
        { x, y + 2 * SEG_T + half,              SEG_T, half },
        { x, y + SEG_T,                         SEG_T, half },
        { x + SEG_T, y + SEG_T + half,          DIG_W - 2 * SEG_T, SEG_T },
    };
    int s;
    for (s = 0; s < 7; s++)
        omni_client_fill(c, r[s][0], r[s][1], r[s][2], r[s][3],
                         (g_digit_segs[d] >> s) & 1 ? C_ON : C_OFF);
}

static void draw_colons(struct omni_client_conn *c)
{
    int k;
    for (k = 0; k < 2; k++) {
        int x = slot_x(2 * k + 1) + DIG_W + (COLON_W - SEG_T) / 2;
        omni_client_fill(c, x, TOP_Y + 14, SEG_T, SEG_T, C_ON);
        omni_client_fill(c, x, TOP_Y + DIG_H - 20, SEG_T, SEG_T, C_ON);
    }
}

int main(void)
{
    struct omni_client_conn conn;
    char shown[7] = "      ", date_shown[64] = "";
    int running = 1;

    memset(&conn, 0, sizeof(conn));
    if (omni_client_open(&conn, "Clock") < 0)
        return 127;
    if (omni_client_window(&conn, "Clock", WIN_W, WIN_H) < 0) {
        omni_client_close(&conn);
        return 127;
    }
    omni_client_clear(&conn, C_BG);
    draw_colons(&conn);

    while (running) {
        struct timespec now;
        struct tm tmv;
        struct pollfd pf = { conn.fd, POLLIN, 0 };
        struct omni_client_event e;
        char digits[7], date[64];
        int i, r, wait_ms;

        clock_gettime(CLOCK_REALTIME, &now);
        localtime_r(&now.tv_sec, &tmv);
        strftime(digits, sizeof(digits), "%H%M%S", &tmv);
        for (i = 0; i < 6; i++)
            if (digits[i] != shown[i]) {
                draw_digit(&conn, slot_x(i), TOP_Y, digits[i] - '0');
                shown[i] = digits[i];
            }
        strftime(date, sizeof(date), "%A, %d %B %Y", &tmv);
        if (strcmp(date, date_shown) != 0) {
            int x = (WIN_W - (int)strlen(date) * 8) / 2;
            omni_client_fill(&conn, 0, TOP_Y + DIG_H + 14, WIN_W, 12, C_BG);
            omni_client_textc(&conn, x < 4 ? 4 : x, TOP_Y + DIG_H + 16,
                              C_DATE, C_BG, date);
            snprintf(date_shown, sizeof(date_shown), "%s", date);
        }

        wait_ms = 1000 - (int)(now.tv_nsec / 1000000L) + 5;   /* next second */
        if (poll(&pf, 1, wait_ms) > 0) {
            while ((r = omni_client_poll(&conn, &e)) > 0)
                if (e.type == 3 || (e.type == 1 && e.pressed && e.key == 1))
                    running = 0;
            if (r < 0)
                running = 0;
        }
    }
    omni_client_close(&conn);
    return 0;
}
