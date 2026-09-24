/*
 * OmniOS — os/apps/calc.c
 *
 * Four-function calculator with a Retro Windows-look button grid. Numbers
 * and operators are typed on the keyboard (or tapped via the mouse if the
 * server sends BTN events).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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
    int x = a->t.w - 20 - (int)strlen(disp) * 8;   /* right-aligned */
    if (x < 16)
        x = 16;
    omni_client_fill(&a->t.conn, 8, 8, a->t.w - 16, 40, 0xffffff);
    omni_client_rect(&a->t.conn, 8, 8, a->t.w - 16, 40, 0x101010);
    omni_client_textc(&a->t.conn, x, 24, 0x101010, 0xffffff, disp);
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
    } else if (c == 'C') {                 /* C: clear everything      */
        strcpy(disp, "0"); acc = 0; op = 0; fresh = 1;
    } else if (c == 'E') {                 /* CE (Delete): clear entry */
        strcpy(disp, "0"); fresh = 1;
    } else if (c == 'B') {                 /* Backspace: last digit    */
        size_t l = strlen(disp);
        if (fresh || l <= 1 || (l == 2 && disp[0] == '-')) {
            strcpy(disp, "0"); fresh = 1;
        } else {
            disp[l - 1] = 0;
        }
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
    omni_client_textc(&a.t.conn, 16, 60, 0x101010, 0xd8d8d8, "Keys: 0-9 . + - * / = Enter");
    omni_client_textc(&a.t.conn, 16, 74, 0x101010, 0xd8d8d8, "Backspace: last digit");
    omni_client_textc(&a.t.conn, 16, 88, 0x101010, 0xd8d8d8, "Delete: CE    Esc or C: C");

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
            else if (e.text == 'c' || e.text == 'C' || e.key == 1 /* Esc */)
                press(&a, 'C');
            else if (e.key == 111)                 /* Delete */
                press(&a, 'E');
            else if (e.key == 14)                  /* Backspace */
                press(&a, 'B');
            else if (e.key == 28 || e.key == 96)   /* Enter, keypad Enter */
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
