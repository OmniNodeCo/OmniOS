/*
 * OmniOS — os/apps/snake.c
 *
 * Snake (App Store game). Arrows or WASD steer, P or Space pauses, Enter
 * starts a new game after a crash, Esc quits. Only the cells that change
 * are redrawn each tick.
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

#define GW      24              /* grid size in cells */
#define GH      16
#define CELL    14
#define BOARD_X 8
#define BOARD_Y 32
#define WIN_W   (BOARD_X * 2 + GW * CELL)
#define WIN_H   (BOARD_Y + GH * CELL + 8 + OMNI_WM_TITLE_H)

#define C_PANEL 0xf0f0f0
#define C_TEXT  0x202020
#define C_BOARD 0x14281a
#define C_BODY  0x4fbf4f
#define C_HEAD  0x9bf09b
#define C_FOOD  0xe8453c

enum { K_ESC = 1, K_W = 17, K_P = 25, K_ENTER = 28, K_A = 30, K_S = 31,
       K_D = 32, K_SPACE = 57, K_KPENTER = 96, K_UP = 103, K_LEFT = 105,
       K_RIGHT = 106, K_DOWN = 108 };

struct pt { signed char x, y; };

struct game {
    struct pt body[GW * GH];    /* ring buffer: head at body[head]    */
    int head, len;
    int dx, dy;
    struct pt turns[2];         /* queued direction changes            */
    int nturns;
    struct pt food;
    int score, best, started, over, paused, grow;
};

/* step results for the renderer */
enum { STEP_IDLE, STEP_MOVED, STEP_ATE, STEP_DIED };

static struct game g_g;

/* ------------------------------------------------------------------ */
/* rules (no drawing)                                                 */
/* ------------------------------------------------------------------ */

static struct pt body_at(const struct game *g, int i)   /* 0 = head */
{
    return g->body[(g->head - i + GW * GH) % (GW * GH)];
}

static int occupied(const struct game *g, int x, int y, int skip_tail)
{
    int i, n = g->len - (skip_tail ? 1 : 0);
    for (i = 0; i < n; i++) {
        struct pt p = body_at(g, i);
        if (p.x == x && p.y == y)
            return 1;
    }
    return 0;
}

static void place_food(struct game *g)
{
    int free_cells = GW * GH - g->len, k, x, y;
    if (free_cells <= 0) {
        g->food.x = g->food.y = -1;
        return;
    }
    k = rand() % free_cells;
    for (y = 0; y < GH; y++)
        for (x = 0; x < GW; x++)
            if (!occupied(g, x, y, 0) && k-- == 0) {
                g->food.x = (signed char)x;
                g->food.y = (signed char)y;
                return;
            }
}

static void game_reset(struct game *g)
{
    int best = g->best, i;
    memset(g, 0, sizeof(*g));
    g->best = best;
    g->len = 4;
    for (i = 0; i < g->len; i++) {          /* horizontal, heading right */
        g->body[i].x = (signed char)(GW / 2 - g->len + 1 + i);
        g->body[i].y = GH / 2;
    }
    g->head = g->len - 1;
    g->dx = 1;
    place_food(g);
}

/* queue a turn; reversing onto yourself is ignored */
static void game_turn(struct game *g, int dx, int dy)
{
    int cdx = g->nturns ? g->turns[g->nturns - 1].x : g->dx;
    int cdy = g->nturns ? g->turns[g->nturns - 1].y : g->dy;
    if ((dx == -cdx && dy == -cdy) || (dx == cdx && dy == cdy))
        return;
    if (g->nturns < 2) {
        g->turns[g->nturns].x = (signed char)dx;
        g->turns[g->nturns].y = (signed char)dy;
        g->nturns++;
    }
    g->started = 1;
}

static int game_step(struct game *g)
{
    struct pt h = body_at(g, 0), n;
    int ate;

    if (!g->started || g->over || g->paused)
        return STEP_IDLE;
    if (g->nturns) {
        g->dx = g->turns[0].x;
        g->dy = g->turns[0].y;
        g->turns[0] = g->turns[1];
        g->nturns--;
    }
    n.x = (signed char)(h.x + g->dx);
    n.y = (signed char)(h.y + g->dy);
    ate = (n.x == g->food.x && n.y == g->food.y);
    if (n.x < 0 || n.y < 0 || n.x >= GW || n.y >= GH ||
        occupied(g, n.x, n.y, !ate)) {    /* the tail moves away unless eating */
        g->over = 1;
        if (g->score > g->best)
            g->best = g->score;
        return STEP_DIED;
    }
    g->head = (g->head + 1) % (GW * GH);
    g->body[g->head] = n;
    if (ate) {
        g->len++;
        g->score++;
        place_food(g);
        return STEP_ATE;
    }
    return STEP_MOVED;
}

static int game_tick_ms(const struct game *g)
{
    int ms = 140 - 3 * g->score;
    return ms < 70 ? 70 : ms;
}

/* ------------------------------------------------------------------ */
/* drawing                                                            */
/* ------------------------------------------------------------------ */

static void cell(struct omni_client_conn *c, int x, int y, uint32_t col)
{
    int inset = (col == C_BOARD) ? 0 : 1;
    if (x < 0 || y < 0)
        return;
    if (inset)
        omni_client_fill(c, BOARD_X + x * CELL, BOARD_Y + y * CELL, CELL, CELL, C_BOARD);
    omni_client_fill(c, BOARD_X + x * CELL + inset, BOARD_Y + y * CELL + inset,
                     CELL - 2 * inset, CELL - 2 * inset, col);
}

static void draw_panel(struct omni_client_conn *c, const struct game *g)
{
    char s[96];
    snprintf(s, sizeof(s), "Score %d   Best %d", g->score, g->best);
    omni_client_fill(c, 0, 0, WIN_W, BOARD_Y, C_PANEL);
    omni_client_textc(c, BOARD_X, 12, C_TEXT, C_PANEL, s);
    omni_client_textc(c, WIN_W - 8 - 13 * 8, 12, 0x6a737d, C_PANEL,
                      g->paused ? "Paused (P)   " : "P pause Esc  ");
}

static void banner(struct omni_client_conn *c, const char *l1, const char *l2)
{
    int w = 28 * 8, h = 40;
    int x = BOARD_X + (GW * CELL - w) / 2, y = BOARD_Y + (GH * CELL - h) / 2;
    omni_client_fill(c, x, y, w, h, 0x0b1a10);
    omni_client_rect(c, x, y, w, h, C_BODY);
    omni_client_textc(c, x + (w - (int)strlen(l1) * 8) / 2, y + 9, 0xffffff, 0x0b1a10, l1);
    omni_client_textc(c, x + (w - (int)strlen(l2) * 8) / 2, y + 23, 0x9fb8a0, 0x0b1a10, l2);
}

static void draw_all(struct omni_client_conn *c, const struct game *g)
{
    int i;
    draw_panel(c, g);
    omni_client_fill(c, BOARD_X, BOARD_Y, GW * CELL, GH * CELL, C_BOARD);
    for (i = g->len - 1; i >= 0; i--) {
        struct pt p = body_at(g, i);
        cell(c, p.x, p.y, i == 0 ? C_HEAD : C_BODY);
    }
    cell(c, g->food.x, g->food.y, C_FOOD);
    if (!g->started)
        banner(c, "Snake", "Press an arrow key");
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

#ifndef OMNI_SNAKE_NO_MAIN
int main(void)
{
    struct omni_client_conn conn;
    long next_tick;
    int running = 1;

    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    memset(&conn, 0, sizeof(conn));
    if (omni_client_open(&conn, "Snake") < 0)
        return 127;
    if (omni_client_window(&conn, "Snake", WIN_W, WIN_H) < 0) {
        omni_client_close(&conn);
        return 127;
    }
    omni_client_clear(&conn, C_PANEL);
    game_reset(&g_g);
    draw_all(&conn, &g_g);
    next_tick = now_ms() + game_tick_ms(&g_g);

    while (running) {
        struct pollfd pf = { conn.fd, POLLIN, 0 };
        struct omni_client_event e;
        long wait = next_tick - now_ms();
        int r;

        if (poll(&pf, 1, wait < 0 ? 0 : (int)wait) > 0) {
            while ((r = omni_client_poll(&conn, &e)) > 0) {
                int was_started = g_g.started;
                if (e.type == 3) { running = 0; break; }
                if (e.type != 1 || !e.pressed)
                    continue;
                switch (e.key) {
                case K_ESC: running = 0; break;
                case K_UP:    case K_W: game_turn(&g_g, 0, -1); break;
                case K_DOWN:  case K_S: game_turn(&g_g, 0, 1);  break;
                case K_LEFT:  case K_A: game_turn(&g_g, -1, 0); break;
                case K_RIGHT: case K_D: game_turn(&g_g, 1, 0);  break;
                case K_P: case K_SPACE:
                    if (g_g.started && !g_g.over) {
                        g_g.paused = !g_g.paused;
                        draw_panel(&conn, &g_g);
                    }
                    break;
                case K_ENTER: case K_KPENTER:
                    if (g_g.over) {
                        game_reset(&g_g);
                        draw_all(&conn, &g_g);
                    }
                    break;
                default: break;
                }
                if (!was_started && g_g.started)
                    draw_all(&conn, &g_g);          /* clear the banner */
            }
            if (r < 0)
                running = 0;
        }

        if (now_ms() >= next_tick) {
            struct pt old_tail = body_at(&g_g, g_g.len - 1);
            struct pt old_head = body_at(&g_g, 0);
            int res = game_step(&g_g);
            if (res == STEP_MOVED || res == STEP_ATE) {
                if (res == STEP_MOVED)
                    cell(&conn, old_tail.x, old_tail.y, C_BOARD);
                cell(&conn, old_head.x, old_head.y, C_BODY);
                cell(&conn, body_at(&g_g, 0).x, body_at(&g_g, 0).y, C_HEAD);
                if (res == STEP_ATE) {
                    cell(&conn, g_g.food.x, g_g.food.y, C_FOOD);
                    draw_panel(&conn, &g_g);
                }
            } else if (res == STEP_DIED) {
                char l1[48];
                snprintf(l1, sizeof(l1), "Game over - score %d", g_g.score);
                draw_panel(&conn, &g_g);
                banner(&conn, l1, "Enter: play again");
            }
            next_tick = now_ms() + game_tick_ms(&g_g);
        }
    }
    omni_client_close(&conn);
    return 0;
}
#endif
