/*
 * OmniOS — os/apps/edit.c
 *
 * Text Editor: a cursor-based plain-text editor. Arrows / Home / End /
 * Page Up / Page Down move the caret, typing inserts at it, Backspace
 * deletes the character before it and Delete the character under it.
 * Long lines wrap; the view scrolls to keep the caret visible.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../gui/client.h"
#include "../gui/wm.h"          /* OMNI_WM_TITLE_H */

#define MAXTEXT  16000
#define WIN_W    560
#define WIN_H    400
#define LINE_H   10
#define TEXT_X   8
#define TEXT_Y   24
#define STATUS_H 16

#define C_PAPER  0xffffff
#define C_INK    0x101010
#define C_BAR    0xe8ecf2
#define C_DIM    0x505a66
#define C_CARET  0x000000

/* Linux key codes used here */
enum { K_ESC = 1, K_BKSP = 14, K_TAB = 15, K_ENTER = 28, K_KPENTER = 96,
       K_HOME = 102, K_UP = 103, K_PGUP = 104, K_LEFT = 105, K_RIGHT = 106,
       K_END = 107, K_DOWN = 108, K_PGDN = 109, K_DELETE = 111 };

struct editor {
    char text[MAXTEXT];
    int  len, cur;              /* text length, caret index 0..len  */
    int  top;                   /* first visible visual line        */
    int  cols, rows;            /* text area size in cells          */
    int  want_col;              /* column kept across Up/Down       */
};

static struct editor g_ed;

/* ------------------------------------------------------------------ */
/* editing (pure logic)                                               */
/* ------------------------------------------------------------------ */

static int line_start(const struct editor *e, int i)
{
    while (i > 0 && e->text[i - 1] != '\n')
        i--;
    return i;
}

static int line_end(const struct editor *e, int i)
{
    while (i < e->len && e->text[i] != '\n')
        i++;
    return i;
}

static void ed_insert(struct editor *e, char c)
{
    if (e->len >= MAXTEXT - 1)
        return;
    memmove(e->text + e->cur + 1, e->text + e->cur, (size_t)(e->len - e->cur));
    e->text[e->cur++] = c;
    e->len++;
}

static void ed_delete_at(struct editor *e, int i)   /* remove text[i] */
{
    if (i < 0 || i >= e->len)
        return;
    memmove(e->text + i, e->text + i + 1, (size_t)(e->len - i - 1));
    e->len--;
}

/* move to the same column of the previous (dir < 0) / next logical line */
static void ed_vertical(struct editor *e, int dir)
{
    int ls = line_start(e, e->cur);
    int target;
    if (e->want_col < 0)
        e->want_col = e->cur - ls;
    if (dir < 0) {
        if (ls == 0)
            return;
        target = line_start(e, ls - 1);
    } else {
        int le = line_end(e, e->cur);
        if (le >= e->len)
            return;
        target = le + 1;
    }
    e->cur = target;
    while (e->cur < e->len && e->text[e->cur] != '\n' &&
           e->cur - target < e->want_col)
        e->cur++;
}

/* Apply one key press; returns 1 if the view changed, -1 to quit. */
static int ed_key(struct editor *e, int key, char ch)
{
    int keep_col = 0, i;

    switch (key) {
    case K_ESC:   return -1;
    case K_LEFT:  if (e->cur > 0) e->cur--; break;
    case K_RIGHT: if (e->cur < e->len) e->cur++; break;
    case K_HOME:  e->cur = line_start(e, e->cur); break;
    case K_END:   e->cur = line_end(e, e->cur); break;
    case K_UP:    ed_vertical(e, -1); keep_col = 1; break;
    case K_DOWN:  ed_vertical(e, +1); keep_col = 1; break;
    case K_PGUP:
        for (i = 0; i < e->rows - 1; i++) ed_vertical(e, -1);
        keep_col = 1;
        break;
    case K_PGDN:
        for (i = 0; i < e->rows - 1; i++) ed_vertical(e, +1);
        keep_col = 1;
        break;
    case K_BKSP:
        if (e->cur > 0) {
            ed_delete_at(e, e->cur - 1);
            e->cur--;
        }
        break;
    case K_DELETE:
        ed_delete_at(e, e->cur);        /* the character under the caret */
        break;
    case K_ENTER: case K_KPENTER:
        ed_insert(e, '\n');
        break;
    case K_TAB:
        for (i = 0; i < 4; i++) ed_insert(e, ' ');
        break;
    default:
        if ((unsigned char)ch >= 32 && (unsigned char)ch < 127)
            ed_insert(e, ch);
        else
            return 0;
        break;
    }
    if (!keep_col)
        e->want_col = -1;
    return 1;
}

/* ------------------------------------------------------------------ */
/* layout + drawing                                                   */
/* ------------------------------------------------------------------ */

/* Visual line (wrapped at cols) and column of the caret at text index i.
 * Same rule as ed_draw(): a character that does not fit wraps *before*
 * it is placed, so a line of exactly `cols` characters takes one row. */
static void ed_locate(const struct editor *e, int i, int *vrow, int *vcol)
{
    int p, row = 0, col = 0;
    for (p = 0; p < i; p++) {
        if (e->text[p] == '\n') {
            row++;
            col = 0;
        } else {
            if (col >= e->cols) {
                row++;
                col = 0;
            }
            col++;
        }
    }
    /* caret after a full row, before another character: that character
     * starts the next row, so the caret belongs there too */
    if (col >= e->cols && i < e->len && e->text[i] != '\n') {
        row++;
        col = 0;
    }
    *vrow = row;
    *vcol = col;
}

static void ed_draw(struct omni_client_conn *c, struct editor *e)
{
    int crow, ccol, row = 0, p = 0, y, lines = 1, i;
    char buf[256], status[96];
    int text_h = e->rows * LINE_H;

    ed_locate(e, e->cur, &crow, &ccol);
    if (crow < e->top)
        e->top = crow;
    if (crow >= e->top + e->rows)
        e->top = crow - e->rows + 1;

    omni_client_fill(c, 0, 0, WIN_W, TEXT_Y - 4, C_BAR);
    omni_client_textc(c, TEXT_X, 6, C_DIM, C_BAR,
                      "Arrows move - Backspace/Delete erase - Esc quits");
    omni_client_fill(c, 0, TEXT_Y - 4, WIN_W, text_h + 8, C_PAPER);

    /* walk visual lines; draw the ones in view */
    while (p <= e->len) {
        int n = 0;
        while (p < e->len && e->text[p] != '\n' && n < e->cols)
            buf[n++] = e->text[p++];
        buf[n] = '\0';
        if (row >= e->top && row < e->top + e->rows && n > 0) {
            y = TEXT_Y + (row - e->top) * LINE_H;
            omni_client_textc(c, TEXT_X, y, C_INK, C_PAPER, buf);
        }
        row++;
        if (p < e->len && e->text[p] == '\n')
            p++;
        else if (p >= e->len)
            break;
    }

    /* caret: a 2 px bar, Windows style */
    y = TEXT_Y + (crow - e->top) * LINE_H - 1;
    omni_client_fill(c, TEXT_X + ccol * 8 - 1, y, 2, LINE_H, C_CARET);

    for (i = 0; i < e->len; i++)
        if (e->text[i] == '\n')
            lines++;
    snprintf(status, sizeof(status), " Ln %d, Col %d    %d lines, %d chars",
             crow + 1, ccol + 1, lines, e->len);
    y = TEXT_Y + text_h + 4;
    omni_client_fill(c, 0, y, WIN_W, STATUS_H, C_BAR);
    omni_client_textc(c, TEXT_X, y + 4, C_DIM, C_BAR, status);
}

#ifndef OMNI_EDIT_NO_MAIN
int main(void)
{
    struct omni_client_conn conn;
    int content_h = WIN_H - OMNI_WM_TITLE_H;

    memset(&conn, 0, sizeof(conn));
    if (omni_client_open(&conn, "Text Editor") < 0)
        return 127;
    if (omni_client_window(&conn, "Text Editor", WIN_W, WIN_H) < 0) {
        omni_client_close(&conn);
        return 127;
    }
    memset(&g_ed, 0, sizeof(g_ed));
    g_ed.cols = (WIN_W - 2 * TEXT_X) / 8;
    g_ed.rows = (content_h - TEXT_Y - STATUS_H - 8) / LINE_H;
    g_ed.want_col = -1;
    omni_client_clear(&conn, C_PAPER);
    ed_draw(&conn, &g_ed);

    for (;;) {
        struct omni_client_event e;
        struct pollfd pf = { conn.fd, POLLIN, 0 };
        int r, changed = 0, quit = 0;

        if (poll(&pf, 1, -1) < 0)
            continue;
        while ((r = omni_client_poll(&conn, &e)) > 0) {
            if (e.type == 3) { quit = 1; break; }
            if (e.type == 1 && e.pressed) {
                int k = ed_key(&g_ed, e.key, e.text);
                if (k < 0) { quit = 1; break; }
                changed |= k;
            }
        }
        if (r < 0 || quit)
            break;
        if (changed)
            ed_draw(&conn, &g_ed);          /* once per burst of keys */
    }
    omni_client_close(&conn);
    return 0;
}
#endif
