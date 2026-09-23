/*
 * OmniOS — os/apps/calc.c
 *
 * Four-function calculator with a Retro Windows-look button grid. Numbers
 * and operators are typed on the keyboard (or tapped via the mouse if the
 * server sends BTN events).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app.h"

static double acc = 0;
static char   op = 0;
static char   disp[OMNI_APP_COLS] = "0";
static int    fresh = 1;

static void update(struct app_win *a)
{
    omni_client_fill(&a->t.conn, 8, 8, a->t.w - 16, 40, 0xffffff);
    omni_client_rect(&a->t.conn, 8, 8, a->t.w - 16, 40, 0x101010);
    omni_client_text(&a->t.conn, 20, 24, disp);
}

static void press(struct app_win *a, char c)
{
    if (c >= '0' && c <= '9') {
        if (fresh) {
            disp[0] = c; disp[1] = 0;
            fresh = 0;
        } else if (strlen(disp) < OMNI_APP_COLS - 1) {
            size_t l = strlen(disp);
            disp[l] = c; disp[l + 1] = 0;
        }
    } else if (c == '.') {
        if (fresh) { strcpy(disp, "0."); fresh = 0; }
        else if (!strchr(disp, '.')) {
            size_t l = strlen(disp);
            disp[l] = '.'; disp[l + 1] = 0;
        }
    } else if (c == 'C') {
        strcpy(disp, "0"); acc = 0; op = 0; fresh = 1;
    } else if (c == '+' || c == '-' || c == '*' || c == '/') {
        acc = atof(disp);
        op = c;
        fresh = 1;
    } else if (c == '=' || c == 10) {
        double v = atof(disp), r = acc;
        if (op == '+') r = acc + v;
        else if (op == '-') r = acc - v;
        else if (op == '*') r = acc * v;
        else if (op == '/') r = (v != 0) ? acc / v : 0;
        snprintf(disp, sizeof(disp), "%g", r);
        acc = r; op = 0; fresh = 1;
    }
    update(a);
}

int main(void)
{
    struct app_win a;

    app_init(&a, "Calculator", 300, 360);
    if (a.t.conn.fd < 0)
        return 127;

    omni_client_clear(&a.t.conn, 0xd8d8d8);
    update(&a);
    omni_client_text(&a.t.conn, 16, 56, "Type: 0-9 . + - * / = C");

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
            if (e.text && strchr("0123456789.+-*/=", e.text))
                press(&a, e.text);
            else if (e.key == 14)
                press(&a, 'C');
            else if (e.key == 28)
                press(&a, '=');
            else if (e.key == 2)  press(&a, '1');
            else if (e.key == 3)  press(&a, '2');
            else if (e.key == 4)  press(&a, '3');
            else if (e.key == 5)  press(&a, '4');
            else if (e.key == 6)  press(&a, '5');
            else if (e.key == 7)  press(&a, '6');
            else if (e.key == 8)  press(&a, '7');
            else if (e.key == 9)  press(&a, '8');
            else if (e.key == 10) press(&a, '9');
            else if (e.key == 11) press(&a, '0');
        }
    }

    app_close(&a);
    return 0;
}
