/*
 * OmniOS — os/apps/terminal.c
 *
 * A minimal interactive terminal: echoes typed characters and handles
 * Enter/Backspace. (The desktop's full terminal is /usr/bin/nxterm from
 * Nano-X; this light client doubles as a keyboard-driven fallback.)
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "app.h"

static void draw_prompt(struct app_win *a)
{
    omni_client_text(&a->t.conn, 12, 12 + a->t.cur_row * 8, "$ ");
}

int main(void)
{
    struct app_win a;

    app_init(&a, "OmniOS Terminal", 520, 320);
    if (a.t.conn.fd < 0)
        return 127;

    omni_client_fill(&a.t.conn, 0, 0, a.t.w, a.t.h, 0x101418);
    omni_client_text(&a.t.conn, 12, 12, "OmniOS Terminal v1.0 — type and press Enter");
    a.t.cur_row = 2;
    draw_prompt(&a);

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
            break;               /* window closed by the user */

        if (e.type == 1 && e.pressed) {
            if (e.key == 28) {                /* Enter */
                a.t.cur_row++;
                a.t.cur_col = 0;
                if (a.t.cur_row >= a.t.rows) {
                    omni_client_clear(&a.t.conn, 0x101418);
                    a.t.cur_row = 0;
                }
                draw_prompt(&a);
            } else if (e.key == 14) {         /* Backspace */
                if (a.t.cur_col > 0) {
                    a.t.cur_col--;
                    omni_client_text(&a.t.conn,
                                     12 + a.t.cur_col * 8,
                                     12 + a.t.cur_row * 8, " ");
                }
            } else if (e.text >= 32 && e.text < 127) {
                char cbuf[2] = { e.text, 0 };
                omni_client_text(&a.t.conn,
                                 12 + a.t.cur_col * 8,
                                 12 + a.t.cur_row * 8, cbuf);
                a.t.cur_col++;
            }
        }
    }

    app_close(&a);
    return 0;
}
