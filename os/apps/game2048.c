/*
 * OmniOS — os/apps/game2048.c
 *
 * 2048 (App Store). Arrow keys or WASD slide the tiles; equal tiles merge.
 * Reach 2048 (and keep going). N or "New game" starts over, Esc closes.
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

#define N       4
#define TILE    80
#define GAP     10
#define BOARD   (N * TILE + (N + 1) * GAP)
#define BX      15
#define BY      96
#define WIN_W   (BX * 2 + BOARD)
#define WIN_H   (BY + BOARD + 44 + OMNI_WM_TITLE_H)

#define C_BG    0xfaf8ef
#define C_BOARD 0xbbada0
#define C_EMPTY 0xcdc1b4
#define C_DARK  0x776e65

enum { K_ESC = 1, K_W = 17, K_A = 30, K_S = 31, K_D = 32, K_N = 49, K_UP = 103,
       K_LEFT = 105, K_RIGHT = 106, K_DOWN = 108 };

static int g_b[N][N], g_score, g_best, g_won, g_keep, g_over;

static void spawn(void)
{
    int free_cells[N * N], n = 0, i, j, k;
    for (i = 0; i < N; i++)
        for (j = 0; j < N; j++)
            if (!g_b[i][j])
                free_cells[n++] = i * N + j;
    if (!n)
        return;
    k = free_cells[rand() % n];
    g_b[k / N][k % N] = (rand() % 10) ? 2 : 4;
}

static void new_game(void)
{
    memset(g_b, 0, sizeof(g_b));
    g_score = g_won = g_keep = g_over = 0;
    spawn();
    spawn();
}

/* slide one line toward index 0; returns 1 if anything moved */
static int slide(int *v[N])
{
    int out[N] = { 0 }, n = 0, i, moved = 0, last = 0;
    for (i = 0; i < N; i++) {
        int x = *v[i];
        if (!x)
            continue;
        if (last && out[n - 1] == x) {
            out[n - 1] = 2 * x;
            g_score += 2 * x;
            if (2 * x == 2048)
                g_won = 1;
            last = 0;
        } else {
            out[n++] = x;
            last = 1;
        }
    }
    for (i = 0; i < N; i++) {
        if (*v[i] != out[i])
            moved = 1;
        *v[i] = out[i];
    }
    return moved;
}

static int move(int dx, int dy)
{
    int line, i, moved = 0;
    for (line = 0; line < N; line++) {
        int *v[N];
        for (i = 0; i < N; i++) {
            int r, c;
            if (dx) { r = line; c = dx < 0 ? i : N - 1 - i; }
            else    { c = line; r = dy < 0 ? i : N - 1 - i; }
            v[i] = &g_b[r][c];
        }
        moved |= slide(v);
    }
    return moved;
}

static int can_move(void)
{
    int i, j;
    for (i = 0; i < N; i++)
        for (j = 0; j < N; j++) {
            if (!g_b[i][j])
                return 1;
            if (j + 1 < N && g_b[i][j] == g_b[i][j + 1])
                return 1;
            if (i + 1 < N && g_b[i][j] == g_b[i + 1][j])
                return 1;
        }
    return 0;
}

static uint32_t tile_color(int v)
{
    switch (v) {
    case 2:    return 0xeee4da;
    case 4:    return 0xede0c8;
    case 8:    return 0xf2b179;
    case 16:   return 0xf59563;
    case 32:   return 0xf67c5f;
    case 64:   return 0xf65e3b;
    case 128:  return 0xedcf72;
    case 256:  return 0xedcc61;
    case 512:  return 0xedc850;
    case 1024: return 0xedc53f;
    case 2048: return 0xedc22e;
    default:   return v ? 0x3c3a32 : C_EMPTY;
    }
}

static void score_box(struct omni_client_conn *c, int x, const char *label, int v)
{
    char s[16];
    snprintf(s, sizeof(s), "%d", v);
    omni_client_rfill(c, x, 14, 92, 48, 6, C_BOARD);
    omni_client_textt(c, x + (92 - (int)strlen(label) * 8) / 2, 20, 0xeee4da, label);
    omni_client_text2(c, x + (92 - (int)strlen(s) * 14) / 2, 38, 0xffffff, s);
}

static void draw(struct omni_client_conn *c)
{
    int i, j;
    omni_client_fill(c, 0, 0, WIN_W, WIN_H - OMNI_WM_TITLE_H, C_BG);
    omni_client_text2(c, BX, 22, C_DARK, "2048");
    omni_client_textt(c, BX, 48, C_DARK, "Join the tiles!");
    score_box(c, WIN_W - BX - 92 - 8 - 92, "SCORE", g_score);
    score_box(c, WIN_W - BX - 92, "BEST", g_best);
    omni_client_rfill(c, BX, BY, BOARD, BOARD, 8, C_BOARD);
    for (i = 0; i < N; i++)
        for (j = 0; j < N; j++) {
            int v = g_b[i][j], x = BX + GAP + j * (TILE + GAP), y = BY + GAP + i * (TILE + GAP);
            omni_client_rfill(c, x, y, TILE, TILE, 6, tile_color(v));
            if (v) {
                char s[12];
                int w;
                snprintf(s, sizeof(s), "%d", v);
                w = (int)strlen(s) * 14;
                omni_client_text2(c, x + (TILE - w) / 2, y + (TILE - 16) / 2,
                                  v <= 4 ? C_DARK : 0xf9f6f2, s);
            }
        }
    omni_client_rfill(c, BX, BY + BOARD + 8, 110, 28, 6, 0x8f7a66);
    omni_client_textt(c, BX + (110 - 8 * 8) / 2, BY + BOARD + 18, 0xf9f6f2, "New game");
    omni_client_textt(c, BX + 124, BY + BOARD + 18, C_DARK, "Arrow keys or WASD");
    if (g_over || (g_won && !g_keep)) {
        const char *l1 = g_over ? "Game over!" : "You made 2048!";
        const char *l2 = g_over ? "Press N for a new game" : "Keep going: any arrow";
        omni_client_rfill(c, BX + 40, BY + BOARD / 2 - 36, BOARD - 80, 72, 8,
                          g_over ? 0x776e65 : 0xedc22e);
        omni_client_text2(c, BX + (BOARD - (int)strlen(l1) * 14) / 2, BY + BOARD / 2 - 22, 0xffffff, l1);
        omni_client_textt(c, BX + (BOARD - (int)strlen(l2) * 8) / 2, BY + BOARD / 2 + 10, 0xffffff, l2);
    }
}

#ifndef OMNI_2048_NO_MAIN
int main(void)
{
    struct omni_client_conn conn;
    int quit = 0;

    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    memset(&conn, 0, sizeof(conn));
    if (omni_client_open(&conn, "2048") < 0)
        return 127;
    if (omni_client_window(&conn, "2048", WIN_W, WIN_H) < 0) {
        omni_client_close(&conn);
        return 127;
    }
    new_game();
    draw(&conn);
    while (!quit) {
        struct pollfd pf = { conn.fd, POLLIN, 0 };
        struct omni_client_event e;
        int r = poll(&pf, 1, -1), changed = 0;
        if (r < 0)
            continue;
        while (!quit && (r = omni_client_poll(&conn, &e)) > 0) {
            int dx = 0, dy = 0;
            if (e.type == 3) {
                quit = 1;
                continue;
            }
            if (e.type == 2 && e.pressed) {
                if (e.x >= BX && e.x < BX + 110 && e.y >= BY + BOARD + 8 && e.y < BY + BOARD + 36) {
                    new_game();
                    changed = 1;
                }
                continue;
            }
            if (e.type != 1 || !e.pressed)
                continue;
            switch (e.key) {
            case K_ESC:   quit = 1; break;
            case K_N:     new_game(); changed = 1; break;
            case K_UP:    case K_W: dy = -1; break;
            case K_DOWN:  case K_S: dy = 1; break;
            case K_LEFT:  case K_A: dx = -1; break;
            case K_RIGHT: case K_D: dx = 1; break;
            default: break;
            }
            if ((dx || dy) && !g_over) {
                if (g_won)
                    g_keep = 1;
                if (move(dx, dy)) {
                    spawn();
                    if (g_score > g_best)
                        g_best = g_score;
                    g_over = !can_move();
                }
                changed = 1;
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
