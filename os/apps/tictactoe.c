/*
 * OmniOS — os/apps/tictactoe.c
 *
 * Tic-Tac-Toe (App Store): you are X. Play the computer (Easy or Hard,
 * which never loses) or a friend (2 Players). Click a square or press 1-9
 * (laid out like the number pad), M changes the mode, N starts a new game,
 * Esc closes.
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

#define CELL    100
#define BX      20
#define BY      74
#define WIN_W   (BX * 2 + 3 * CELL)
#define WIN_H   (BY + 3 * CELL + 96 + OMNI_WM_TITLE_H)

#define C_BG    0xffffff
#define C_GRID  0xe5e7eb
#define C_INK   0x1b1f24
#define C_DIM   0x6b7280
#define C_X     0xdb2777
#define C_O     0x2563eb
#define C_WIN   0xfdf2f8
#define C_BTN   0xf3f4f6

enum { K_ESC = 1, K_M = 50, K_N = 49 };
enum { EASY, HARD, TWO, NMODES };

static const char *g_mode_names[NMODES] = { "vs Computer: Easy", "vs Computer: Hard", "2 Players" };
static char g_b[9];                      /* ' ', 'X', 'O'                 */
static char g_turn = 'X';
static int  g_mode = HARD, g_winline = -1, g_done, g_sx, g_so, g_sd;

static const int g_lines[8][3] = { {0,1,2}, {3,4,5}, {6,7,8}, {0,3,6},
                                   {1,4,7}, {2,5,8}, {0,4,8}, {2,4,6} };

static char winner(const char *b, int *line)
{
    int i;
    for (i = 0; i < 8; i++) {
        const int *l = g_lines[i];
        if (b[l[0]] != ' ' && b[l[0]] == b[l[1]] && b[l[1]] == b[l[2]]) {
            if (line)
                *line = i;
            return b[l[0]];
        }
    }
    for (i = 0; i < 9; i++)
        if (b[i] == ' ')
            return 0;
    return 'D';                          /* draw */
}

/* minimax score for 'O' to move (O maximises) */
static int minimax(char *b, char who, int depth)
{
    char w = winner(b, NULL);
    int i, best = who == 'O' ? -100 : 100;
    if (w == 'O') return 10 - depth;
    if (w == 'X') return depth - 10;
    if (w == 'D') return 0;
    for (i = 0; i < 9; i++) {
        int s;
        if (b[i] != ' ')
            continue;
        b[i] = who;
        s = minimax(b, who == 'O' ? 'X' : 'O', depth + 1);
        b[i] = ' ';
        if (who == 'O' ? s > best : s < best)
            best = s;
    }
    return best;
}

static int computer_move(void)
{
    int i, best = -1000, pick[9], n = 0, free_n = 0, frees[9];
    for (i = 0; i < 9; i++)
        if (g_b[i] == ' ')
            frees[free_n++] = i;
    if (!free_n)
        return -1;
    if (g_mode == EASY && rand() % 100 < 60)
        return frees[rand() % free_n];
    for (i = 0; i < 9; i++) {
        int s;
        if (g_b[i] != ' ')
            continue;
        g_b[i] = 'O';
        s = minimax(g_b, 'X', 1);
        g_b[i] = ' ';
        if (s > best) {
            best = s;
            n = 0;
        }
        if (s == best)
            pick[n++] = i;
    }
    return pick[rand() % n];
}

static void new_game(void)
{
    memset(g_b, ' ', sizeof(g_b));
    g_turn = 'X';
    g_winline = -1;
    g_done = 0;
}

static void finish_check(void)
{
    char w = winner(g_b, &g_winline);
    if (!w)
        return;
    g_done = 1;
    if (w == 'X') g_sx++;
    else if (w == 'O') g_so++;
    else { g_sd++; g_winline = -1; }
}

static void play(int i)
{
    if (g_done || i < 0 || i > 8 || g_b[i] != ' ')
        return;
    g_b[i] = g_turn;
    finish_check();
    if (g_done)
        return;
    g_turn = g_turn == 'X' ? 'O' : 'X';
    if (g_mode != TWO && g_turn == 'O') {
        int m = computer_move();
        if (m >= 0) {
            g_b[m] = 'O';
            finish_check();
        }
        g_turn = 'X';
    }
}

/* ------------------------------------------------------------------ */
/* drawing                                                            */
/* ------------------------------------------------------------------ */

static void draw_x(struct omni_client_conn *c, int x, int y)
{
    int k;
    for (k = 0; k <= 50; k += 2) {       /* two thick diagonals */
        omni_client_rfill(c, x + 25 + k - 5, y + 25 + k - 5, 10, 10, 5, C_X);
        omni_client_rfill(c, x + 75 - k - 5, y + 25 + k - 5, 10, 10, 5, C_X);
    }
}

static void draw_o(struct omni_client_conn *c, int x, int y, uint32_t bg)
{
    omni_client_rfill(c, x + 24, y + 24, 52, 52, 26, C_O);
    omni_client_rfill(c, x + 33, y + 33, 34, 34, 17, bg);
}

static void draw(struct omni_client_conn *c)
{
    char s[96];
    const char *status;
    int i, win[9] = { 0 };

    if (g_winline >= 0)
        for (i = 0; i < 3; i++)
            win[g_lines[g_winline][i]] = 1;
    omni_client_fill(c, 0, 0, WIN_W, WIN_H - OMNI_WM_TITLE_H, C_BG);
    if (g_done) {
        char w = winner(g_b, NULL);
        status = w == 'D' ? "It's a draw." : (w == 'X' ? (g_mode == TWO ? "X wins!" : "You win!")
                                                        : (g_mode == TWO ? "O wins!" : "The computer wins."));
    } else {
        status = g_mode == TWO ? (g_turn == 'X' ? "X to play" : "O to play") : "Your turn (you are X)";
    }
    omni_client_text2(c, BX, 20, C_INK, status);
    omni_client_textt(c, BX, 46, C_DIM, g_mode_names[g_mode]);
    for (i = 0; i < 9; i++) {
        int x = BX + (i % 3) * CELL, y = BY + (i / 3) * CELL;
        uint32_t bg = win[i] ? C_WIN : C_BG;
        omni_client_fill(c, x, y, CELL, CELL, bg);
        if (g_b[i] == 'X') draw_x(c, x, y);
        if (g_b[i] == 'O') draw_o(c, x, y, bg);
    }
    omni_client_rfill(c, BX + CELL - 2, BY + 6, 4, 3 * CELL - 12, 2, C_GRID);
    omni_client_rfill(c, BX + 2 * CELL - 2, BY + 6, 4, 3 * CELL - 12, 2, C_GRID);
    omni_client_rfill(c, BX + 6, BY + CELL - 2, 3 * CELL - 12, 4, 2, C_GRID);
    omni_client_rfill(c, BX + 6, BY + 2 * CELL - 2, 3 * CELL - 12, 4, 2, C_GRID);

    snprintf(s, sizeof(s), g_mode == TWO ? "X %d   -   Draws %d   -   O %d"
                                         : "You %d   -   Draws %d   -   Computer %d", g_sx, g_sd, g_so);
    omni_client_textt(c, BX + (3 * CELL - (int)strlen(s) * 8) / 2, BY + 3 * CELL + 16, C_INK, s);
    omni_client_rfill(c, BX, BY + 3 * CELL + 40, 142, 32, 6, C_BTN);
    omni_client_textt(c, BX + (142 - 11 * 8) / 2, BY + 3 * CELL + 52, C_INK, "Change mode");
    omni_client_rfill(c, BX + 3 * CELL - 142, BY + 3 * CELL + 40, 142, 32, 6, C_X);
    omni_client_textt(c, BX + 3 * CELL - 142 + (142 - 8 * 8) / 2, BY + 3 * CELL + 52, 0xffffff, "New game");
}

#ifndef OMNI_TTT_NO_MAIN
int main(void)
{
    struct omni_client_conn conn;
    int quit = 0;

    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    memset(&conn, 0, sizeof(conn));
    if (omni_client_open(&conn, "Tic-Tac-Toe") < 0)
        return 127;
    if (omni_client_window(&conn, "Tic-Tac-Toe", WIN_W, WIN_H) < 0) {
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
            if (e.type == 3) {
                quit = 1;
            } else if (e.type == 2 && e.pressed) {
                int by = BY + 3 * CELL + 40;
                if (e.x >= BX && e.x < BX + 3 * CELL && e.y >= BY && e.y < BY + 3 * CELL) {
                    play((e.y - BY) / CELL * 3 + (e.x - BX) / CELL);
                } else if (e.y >= by && e.y < by + 32) {
                    if (e.x < BX + 142) {
                        g_mode = (g_mode + 1) % NMODES;
                        g_sx = g_so = g_sd = 0;
                    }
                    new_game();
                }
                changed = 1;
            } else if (e.type == 1 && e.pressed) {
                static const int pad[9] = { 6, 7, 8, 3, 4, 5, 0, 1, 2 };  /* 1..9 */
                changed = 1;
                if (e.key >= 2 && e.key <= 10)
                    play(pad[e.key - 2]);
                else if (e.key == K_N)
                    new_game();
                else if (e.key == K_M) {
                    g_mode = (g_mode + 1) % NMODES;
                    g_sx = g_so = g_sd = 0;
                    new_game();
                } else if (e.key == K_ESC)
                    quit = 1;
                else
                    changed = 0;
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
