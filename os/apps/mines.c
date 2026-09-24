/*
 * OmniOS — os/apps/mines.c
 *
 * Minesweeper (App Store). Left click reveals, right click flags; or move
 * with the arrow keys, Space/Enter to reveal, F to flag. The first click is
 * always safe. N or the face button starts a new game, Esc closes.
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

#define GW      12
#define GH      12
#define MINES   20
#define CELL    28
#define BX      16
#define BY      64
#define WIN_W   (BX * 2 + GW * CELL)
#define WIN_H   (BY + GH * CELL + BX + OMNI_WM_TITLE_H)

#define C_BG     0xf3f4f6
#define C_HIDDEN 0x9fb3c8
#define C_HID_HI 0xc3d2e2
#define C_OPEN   0xfbfcfd
#define C_LINE   0xe2e6ec
#define C_INK    0x1b1f24
#define C_CURSOR 0x2563eb

enum { K_ESC = 1, K_F = 33, K_N = 49, K_ENTER = 28, K_SPACE = 57, K_UP = 103,
       K_LEFT = 105, K_RIGHT = 106, K_DOWN = 108 };
enum { PLAYING, WON, LOST };

struct cell { unsigned char mine, adj, open, flag; };
static struct cell g_c[GH][GW];
static int g_state, g_placed, g_flags, g_opened, g_cx, g_cy, g_boom_x = -1, g_boom_y = -1;
static time_t g_t0;
static int g_secs;

static void new_game(void)
{
    memset(g_c, 0, sizeof(g_c));
    g_state = PLAYING;
    g_placed = g_flags = g_opened = g_secs = 0;
    g_boom_x = g_boom_y = -1;
}

/* mines go in after the first click, never on or next to it */
static void place(int sx, int sy)
{
    int n = 0, x, y, dx, dy;
    while (n < MINES) {
        x = rand() % GW;
        y = rand() % GH;
        if (g_c[y][x].mine || (abs(x - sx) <= 1 && abs(y - sy) <= 1))
            continue;
        g_c[y][x].mine = 1;
        n++;
    }
    for (y = 0; y < GH; y++)
        for (x = 0; x < GW; x++)
            for (dy = -1; dy <= 1; dy++)
                for (dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if ((dx || dy) && nx >= 0 && ny >= 0 && nx < GW && ny < GH &&
                        g_c[ny][nx].mine)
                        g_c[y][x].adj++;
                }
    g_placed = 1;
    g_t0 = time(NULL);
}

static void reveal(int x, int y)
{
    static int stack[GW * GH * 2];
    int sp = 0;
    if (g_state != PLAYING || g_c[y][x].open || g_c[y][x].flag)
        return;
    if (!g_placed)
        place(x, y);
    if (g_c[y][x].mine) {
        g_state = LOST;
        g_boom_x = x;
        g_boom_y = y;
        return;
    }
    stack[sp++] = y * GW + x;
    while (sp) {
        int p = stack[--sp], px = p % GW, py = p / GW, dx, dy;
        struct cell *c = &g_c[py][px];
        if (c->open || c->flag)
            continue;
        c->open = 1;
        g_opened++;
        if (c->adj)
            continue;
        for (dy = -1; dy <= 1; dy++)
            for (dx = -1; dx <= 1; dx++) {
                int nx = px + dx, ny = py + dy;
                if ((dx || dy) && nx >= 0 && ny >= 0 && nx < GW && ny < GH &&
                    !g_c[ny][nx].open && !g_c[ny][nx].mine && sp < GW * GH * 2)
                    stack[sp++] = ny * GW + nx;
            }
    }
    if (g_opened == GW * GH - MINES)
        g_state = WON;
}

static void flag(int x, int y)
{
    if (g_state != PLAYING || g_c[y][x].open)
        return;
    g_c[y][x].flag = !g_c[y][x].flag;
    g_flags += g_c[y][x].flag ? 1 : -1;
}

/* ------------------------------------------------------------------ */
/* drawing                                                            */
/* ------------------------------------------------------------------ */

static void draw_mine(struct omni_client_conn *c, int x, int y)
{
    int m = x + CELL / 2, n = y + CELL / 2;
    omni_client_fill(c, m - 9, n - 1, 18, 2, 0x111827);
    omni_client_fill(c, m - 1, n - 9, 2, 18, 0x111827);
    omni_client_rfill(c, m - 6, n - 6, 12, 12, 6, 0x111827);
    omni_client_rfill(c, m - 3, n - 3, 3, 3, 1, 0xffffff);
}

static void draw_flag(struct omni_client_conn *c, int x, int y)
{
    omni_client_fill(c, x + 12, y + 7, 2, 14, 0x374151);
    omni_client_fill(c, x + 8, y + 20, 10, 2, 0x374151);
    omni_client_fill(c, x + 14, y + 7, 7, 6, 0xdc2626);
}

static void draw_cell(struct omni_client_conn *c, int gx, int gy)
{
    static const uint32_t numc[9] = { 0, 0x2563eb, 0x16a34a, 0xdc2626, 0x1e3a8a,
                                      0x9f1239, 0x0f766e, 0x111827, 0x6b7280 };
    const struct cell *k = &g_c[gy][gx];
    int x = BX + gx * CELL, y = BY + gy * CELL;
    int show_mine = g_state == LOST && k->mine;

    omni_client_fill(c, x, y, CELL, CELL, C_BG);
    if (k->open || show_mine) {
        omni_client_fill(c, x, y, CELL, CELL, (gx == g_boom_x && gy == g_boom_y) ? 0xfca5a5 : C_OPEN);
        omni_client_fill(c, x, y + CELL - 1, CELL, 1, C_LINE);
        omni_client_fill(c, x + CELL - 1, y, 1, CELL, C_LINE);
        if (show_mine && !k->flag) {
            draw_mine(c, x, y);
        } else if (k->adj) {
            char s[2] = { (char)('0' + k->adj), 0 };
            omni_client_text2(c, x + (CELL - 14) / 2, y + (CELL - 16) / 2, numc[k->adj], s);
        }
        if (show_mine && k->flag)
            draw_flag(c, x, y);
    } else {
        omni_client_rfill(c, x + 1, y + 1, CELL - 2, CELL - 2, 4, C_HIDDEN);
        omni_client_rfill(c, x + 1, y + 1, CELL - 2, CELL - 5, 4, C_HID_HI);
        if (k->flag) {
            draw_flag(c, x, y);
            if (g_state == LOST && !k->mine) {          /* a wrong flag */
                omni_client_fill(c, x + 6, y + 13, CELL - 12, 2, 0x111827);
            }
        }
    }
    if (gx == g_cx && gy == g_cy && g_state == PLAYING) {
        omni_client_fill(c, x, y, CELL, 2, C_CURSOR);
        omni_client_fill(c, x, y + CELL - 2, CELL, 2, C_CURSOR);
        omni_client_fill(c, x, y, 2, CELL, C_CURSOR);
        omni_client_fill(c, x + CELL - 2, y, 2, CELL, C_CURSOR);
    }
}

static void draw_header(struct omni_client_conn *c)
{
    char s[32];
    const char *face = g_state == WON ? "B)" : (g_state == LOST ? ":(" : ":)");
    uint32_t fc = g_state == WON ? 0x16a34a : (g_state == LOST ? 0xdc2626 : 0xf59e0b);
    omni_client_fill(c, 0, 0, WIN_W, BY - 4, C_BG);
    omni_client_rfill(c, BX, 12, 84, 36, 8, 0x1f2937);
    snprintf(s, sizeof(s), "%03d", MINES - g_flags < 0 ? 0 : MINES - g_flags);
    omni_client_text2(c, BX + 18, 22, 0xf87171, s);
    omni_client_rfill(c, WIN_W / 2 - 22, 8, 44, 44, 22, fc);
    omni_client_text2(c, WIN_W / 2 - 13, 22, 0xffffff, face);
    omni_client_rfill(c, WIN_W - BX - 84, 12, 84, 36, 8, 0x1f2937);
    snprintf(s, sizeof(s), "%03d", g_secs > 999 ? 999 : g_secs);
    omni_client_text2(c, WIN_W - BX - 84 + 18, 22, 0xf87171, s);
}

static void draw_all(struct omni_client_conn *c)
{
    int x, y;
    draw_header(c);
    for (y = 0; y < GH; y++)
        for (x = 0; x < GW; x++)
            draw_cell(c, x, y);
    if (g_state != PLAYING) {
        const char *msg = g_state == WON ? "You cleared the field!" : "Boom! Press N to try again";
        int w = (int)strlen(msg) * 8 + 32;
        omni_client_rfill(c, (WIN_W - w) / 2, BY + GH * CELL / 2 - 18, w, 36, 8,
                          g_state == WON ? 0x166534 : 0x7f1d1d);
        omni_client_textt(c, (WIN_W - w) / 2 + 16, BY + GH * CELL / 2 - 4, 0xffffff, msg);
    }
}

#ifndef OMNI_MINES_NO_MAIN
int main(void)
{
    struct omni_client_conn conn;
    int quit = 0;

    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    memset(&conn, 0, sizeof(conn));
    if (omni_client_open(&conn, "Minesweeper") < 0)
        return 127;
    if (omni_client_window(&conn, "Minesweeper", WIN_W, WIN_H) < 0) {
        omni_client_close(&conn);
        return 127;
    }
    omni_client_clear(&conn, C_BG);
    new_game();
    g_cx = GW / 2;
    g_cy = GH / 2;
    draw_all(&conn);

    while (!quit) {
        struct pollfd pf = { conn.fd, POLLIN, 0 };
        struct omni_client_event e;
        int ticking = g_placed && g_state == PLAYING;
        int r = poll(&pf, 1, ticking ? 1000 : -1), changed = 0, before = g_state;
        if (r < 0)
            continue;
        if (ticking) {
            int s = (int)(time(NULL) - g_t0);
            if (s != g_secs) {
                g_secs = s;
                draw_header(&conn);
            }
        }
        while (r > 0 && !quit && (r = omni_client_poll(&conn, &e)) > 0) {
            if (e.type == 3) {
                quit = 1;
            } else if (e.type == 2 && e.pressed) {
                int gx = (e.x - BX) / CELL, gy = (e.y - BY) / CELL;
                if (e.y < BY - 4 && e.x >= WIN_W / 2 - 22 && e.x < WIN_W / 2 + 22) {
                    new_game();
                    changed = 1;
                } else if (e.x >= BX && e.y >= BY && gx < GW && gy < GH) {
                    g_cx = gx;
                    g_cy = gy;
                    if (e.key == 3)           /* right button: flag */
                        flag(gx, gy);
                    else
                        reveal(gx, gy);
                    changed = 1;
                }
            } else if (e.type == 1 && e.pressed) {
                changed = 1;
                switch (e.key) {
                case K_ESC:   quit = 1; break;
                case K_N:     new_game(); break;
                case K_UP:    g_cy = (g_cy + GH - 1) % GH; break;
                case K_DOWN:  g_cy = (g_cy + 1) % GH; break;
                case K_LEFT:  g_cx = (g_cx + GW - 1) % GW; break;
                case K_RIGHT: g_cx = (g_cx + 1) % GW; break;
                case K_SPACE: case K_ENTER: reveal(g_cx, g_cy); break;
                case K_F:     flag(g_cx, g_cy); break;
                default:      changed = 0; break;
                }
            }
        }
        if (r < 0)
            quit = 1;
        if (changed && !quit) {
            if (g_state != before)
                g_secs = g_placed ? (int)(time(NULL) - g_t0) : 0;
            draw_all(&conn);
        }
    }
    omni_client_close(&conn);
    return 0;
}
#endif
