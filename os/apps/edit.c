/*
 * OmniOS — os/apps/edit.c
 *
 * Tiny line-oriented text editor client: type to append, Enter for newline,
 * Backspace to remove the last typed character.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "app.h"

#define MAXTEXT 4000

static char text[MAXTEXT];
static int  tlen = 0;

static void redraw(struct app_win *a)
{
    int i, row = 0, col = 0;
    omni_client_clear(&a->t.conn, 0xf0f0f0);
    omni_client_text(&a->t.conn, 12, 12,
                     "OmniOS Text Editor — type, Backspace deletes, Esc quits");
    for (i = 0; i < tlen; i++) {
        if (text[i] == '\n') {
            row++; col = 0;
            if (row >= a->t.rows - 2) break;
            continue;
        }
        {
            char cbuf[2] = { text[i], 0 };
            omni_client_text(&a->t.conn, 12 + (col % a->t.cols) * 8,
                             24 + row * 8, cbuf);
        }
        col++;
        if (col >= a->t.cols) {
            col = 0;
            row++;
            if (row >= a->t.rows - 2) break;
        }
    }
}

int main(void)
{
    struct app_win a;

    app_init(&a, "Text Editor", 560, 400);
    if (a.t.conn.fd < 0)
        return 127;

    redraw(&a);

    for (;;) {
        struct omni_client_event e;
        int r = omni_client_poll(&a.t.conn, &e);
        if (r < 0)
            break;
        if (r == 0) {
            usleep(20000);
            continue;
        }
        if (e.type == 3)
            break;
        if (e.type == 1 && e.pressed) {
            if (e.key == 1 /* Esc */)
                break;
            if (e.key == 14 /* Backspace */) {
                if (tlen > 0) {
                    tlen--;
                    redraw(&a);
                }
            } else if (e.key == 28 /* Enter */) {
                if (tlen < MAXTEXT - 1) {
                    text[tlen++] = '\n';
                    redraw(&a);
                }
            } else if (e.text >= 32 && e.text < 127
                       && tlen < MAXTEXT - 1) {
                text[tlen++] = e.text;
                redraw(&a);
            }
        }
    }

    app_close(&a);
    return 0;
}
