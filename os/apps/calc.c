/*
 * OmniOS — os/apps/calc.c
 *
 * Calculator with a clickable, modern button grid. Every button also has
 * a key: 0-9 . + - * (or x) / = Enter, Backspace (last digit), Delete (CE),
 * Esc or C (clear), N (+/-). Operations chain: "2 + 3 +" shows 5.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "app.h"

#define BG        0xf1f3f7
#define MARGIN    8
#define GAP       4
#define GRID_Y    104
#define COLS      4
#define ROWS      5
#define FLASH_MS  120
#define MAXDIGITS 16
#define BIG_CW    14                /* TEXT2 (th_text2x) advance per char */

struct key {
    const char *label;
    char code;
    int kind;                       /* 0 digit, 1 operator, 2 = */
};

static const struct key keys[COLS * ROWS] = {
    { "C", 'C', 1 },  { "CE", 'E', 1 }, { "<-", 'B', 1 }, { "/", '/', 1 },
    { "7", '7', 0 },  { "8", '8', 0 },  { "9", '9', 0 },  { "x", '*', 1 },
    { "4", '4', 0 },  { "5", '5', 0 },  { "6", '6', 0 },  { "-", '-', 1 },
    { "1", '1', 0 },  { "2", '2', 0 },  { "3", '3', 0 },  { "+", '+', 1 },
    { "+/-", 'N', 0 }, { "0", '0', 0 }, { ".", '.', 0 },  { "=", '=', 2 },
};

static const uint32_t face[3]    = { 0xffffff, 0xe6eaf0, 0x2563eb };
static const uint32_t pressed[3] = { 0xdde2ea, 0xd2d8e2, 0x1d4ed8 };

static double acc = 0;
static char   op = 0;
static char   disp[64] = "0";
static char   expr[96] = "";
static int    fresh = 1;
static int    bw, bh;               /* button size */

static const char *opsym(char c)
{
    return c == '*' ? "x" : c == '/' ? "/" : c == '+' ? "+" : "-";
}

static void draw_display(struct app_win *a)
{
    int W = a->t.w, len = (int)strlen(disp), x;

    omni_client_fill(&a->t.conn, 0, 0, W, GRID_Y - GAP, BG);
    if (expr[0])
        omni_client_textt(&a->t.conn, W - 16 - (int)strlen(expr) * 8, 24,
                          0x6b7280, expr);
    if (len * BIG_CW <= W - 32) {               /* big digits if they fit */
        x = W - 18 - len * BIG_CW;
        omni_client_text2(&a->t.conn, x, 58, 0x111827, disp);
    } else {
        x = W - 16 - len * 8;
        omni_client_textt(&a->t.conn, x < 8 ? 8 : x, 64, 0x111827, disp);
    }
}

static void draw_key(struct app_win *a, int i, int down)
{
    const struct key *k = &keys[i];
    int x = MARGIN + (i % COLS) * (bw + GAP);
    int y = GRID_Y + (i / COLS) * (bh + GAP);
    int tw = (int)strlen(k->label) * BIG_CW - 2;    /* last glyph's gap */

    omni_client_rfill(&a->t.conn, x, y, bw, bh, 6,
                      down ? pressed[k->kind] : face[k->kind]);
    omni_client_text2(&a->t.conn, x + (bw - tw) / 2, y + (bh - 16) / 2,
                      k->kind == 2 ? 0xffffff : 0x1f2937, k->label);
}

static double compute(double l, char o, double r, int *err)
{
    *err = 0;
    switch (o) {
    case '+': return l + r;
    case '-': return l - r;
    case '*': return l * r;
    case '/':
        if (r == 0) { *err = 1; return 0; }
        return l / r;
    default:  return r;
    }
}

static void show(double v)
{
    snprintf(disp, sizeof(disp), "%.12g", v);
    if (strcmp(disp, "-0") == 0)
        snprintf(disp, sizeof(disp), "0");
}

static void append(char c)
{
    size_t l = strlen(disp);
    if (l + 1 < sizeof(disp)) {
        disp[l] = c;
        disp[l + 1] = 0;
    }
}

static void press(char c)
{
    int err = 0;
    int numeric = disp[0] == '-' || (disp[0] >= '0' && disp[0] <= '9');

    if (!numeric && (c == 'N' || c == 'B'))  /* error text: start over */
        c = 'C';
    if (c >= '0' && c <= '9') {
        if (fresh) {
            disp[0] = c; disp[1] = 0;
            fresh = 0;
        } else if (strlen(disp) < MAXDIGITS) {
            if (strcmp(disp, "0") == 0)
                disp[0] = c;
            else
                append(c);
        }
    } else if (c == '.') {
        if (fresh) { snprintf(disp, sizeof(disp), "0."); fresh = 0; }
        else if (!strchr(disp, '.') && strlen(disp) < MAXDIGITS)
            append('.');
    } else if (c == 'C') {                  /* C: clear everything      */
        snprintf(disp, sizeof(disp), "0"); acc = 0; op = 0; fresh = 1;
        expr[0] = 0;
    } else if (c == 'E') {                  /* CE: clear entry          */
        snprintf(disp, sizeof(disp), "0"); fresh = 1;
    } else if (c == 'B') {                  /* Backspace: last digit    */
        size_t l = strlen(disp);
        if (fresh || l <= 1 || (l == 2 && disp[0] == '-')) {
            snprintf(disp, sizeof(disp), "0"); fresh = 1;
        } else {
            disp[l - 1] = 0;
        }
    } else if (c == 'N') {                  /* +/- */
        if (disp[0] == '-')
            memmove(disp, disp + 1, strlen(disp));
        else if (strcmp(disp, "0") != 0 && strlen(disp) < sizeof(disp) - 1) {
            memmove(disp + 1, disp, strlen(disp) + 1);
            disp[0] = '-';
        }
    } else if (c == '+' || c == '-' || c == '*' || c == '/') {
        if (op && !fresh) {                 /* chain: 2 + 3 + -> 5 + */
            acc = compute(acc, op, atof(disp), &err);
            if (err) goto divzero;
            show(acc);
        } else {
            acc = atof(disp);
        }
        op = c;
        fresh = 1;
        snprintf(expr, sizeof(expr), "%.12g %s", acc, opsym(op));
    } else if (c == '=') {
        double v = atof(disp), r;
        if (!op)
            return;
        r = compute(acc, op, v, &err);
        if (err) goto divzero;
        snprintf(expr, sizeof(expr), "%.12g %s %.12g =", acc, opsym(op), v);
        show(r);
        acc = r; op = 0; fresh = 1;
    }
    return;

divzero:
    snprintf(disp, sizeof(disp), "Cannot divide by zero");
    expr[0] = 0; acc = 0; op = 0; fresh = 1;
}

static int key_index(char code)
{
    int i;
    for (i = 0; i < COLS * ROWS; i++)
        if (keys[i].code == code)
            return i;
    return -1;
}

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static char code_for_key(const struct omni_client_event *e)
{
    static const char row1[] = "1234567890";
    if (e->text && strchr("0123456789.+-*/=", e->text))
        return e->text;
    if (e->text == 'x' || e->text == 'X')               /* as labelled */
        return '*';
    if (e->text == 'c' || e->text == 'C' || e->key == 1)   /* Esc */
        return 'C';
    if (e->text == 'n' || e->text == 'N')
        return 'N';
    if (e->key == 111) return 'E';                      /* Delete    */
    if (e->key == 14)  return 'B';                      /* Backspace */
    if (e->key == 28 || e->key == 96) return '=';       /* Enter     */
    if (e->key >= 2 && e->key <= 11)                    /* raw digit */
        return row1[e->key - 2];
    return 0;
}

int main(void)
{
    struct app_win a;
    int i, lit = -1;
    long lit_until = 0;

    app_init(&a, "Calculator", 320, 420);
    if (a.t.conn.fd < 0)
        return 127;
    bw = (a.t.w - 2 * MARGIN - (COLS - 1) * GAP) / COLS;
    bh = (a.t.ch - GRID_Y - MARGIN - (ROWS - 1) * GAP) / ROWS;

    omni_client_clear(&a.t.conn, BG);
    draw_display(&a);
    for (i = 0; i < COLS * ROWS; i++)
        draw_key(&a, i, 0);

    for (;;) {
        struct omni_client_event e;
        int timeout = lit >= 0 ? (int)(lit_until - now_ms()) : -1;
        int r, quit = 0;

        if (lit >= 0 && timeout <= 0) {         /* end of the press flash */
            draw_key(&a, lit, 0);
            lit = -1;
            continue;
        }
        r = app_wait(&a, timeout);
        if (r < 0)
            break;
        while ((r = omni_client_poll(&a.t.conn, &e)) > 0) {
            int hit = -1;
            char code = 0;

            if (e.type == 3) {
                quit = 1;
                break;
            }
            if (e.type == 1 && e.pressed) {
                code = code_for_key(&e);
                hit = code ? key_index(code) : -1;
            } else if (e.type == 2 && e.pressed && e.y >= GRID_Y &&
                       e.x >= MARGIN) {
                int col = (e.x - MARGIN) / (bw + GAP);
                int row = (e.y - GRID_Y) / (bh + GAP);
                int inx = (e.x - MARGIN) % (bw + GAP) < bw;
                int iny = (e.y - GRID_Y) % (bh + GAP) < bh;
                if (col < COLS && row < ROWS && inx && iny) {
                    hit = row * COLS + col;
                    code = keys[hit].code;
                }
            }
            if (!code)
                continue;
            press(code);
            draw_display(&a);
            if (hit >= 0) {
                if (lit >= 0 && lit != hit)
                    draw_key(&a, lit, 0);
                draw_key(&a, hit, 1);
                lit = hit;
                lit_until = now_ms() + FLASH_MS;
            }
        }
        if (r < 0 || quit)
            break;
    }

    app_close(&a);
    return 0;
}
