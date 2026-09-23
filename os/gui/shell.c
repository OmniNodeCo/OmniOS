/*
 * OmniOS — os/gui/shell.c
 *
 * The graphical desktop shell: gradient wallpaper with the OmniOS logo,
 * a Windows-style taskbar (Start button, window buttons, clock) and a
 * pop-up start menu. Owns the framebuffer, the window manager and the
 * input devices, and spawns bundled apps via their /usr/bin paths.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include "shell.h"
#include "wm.h"

/* ------------------------------------------------------------------ */
/* pointer state (the desktop is the global mouse owner)               */
/* ------------------------------------------------------------------ */

static int g_px, g_py;
static struct omni_wm g_wm;
static struct raster g_screen;
static int g_mx, g_my;

void omni_shell_warp(int dx, int dy)
{
    /* accumulate relative motion into the pointer */
    if (dx > 63)   dx -= 128;  /* account for underflow in PS/2 bytes */
    if (dx < -64)  dx += 128;
    if (dy > 63)   dy -= 128;
    if (dy < -64)  dy += 128;

    /* sensitivity: raw counts -> pixels */
    g_px += dx;
    g_py += dy;
    if (g_px < 0) g_px = 0;
    if (g_py < 0) g_py = 0;
}

/* ------------------------------------------------------------------ */
/* wallpaper + logo                                                    */
/* ------------------------------------------------------------------ */

static void draw_logo(struct raster *r, int cx, int cy, int radius)
{
    int dy;

    for (dy = -radius; dy <= radius; dy++) {
        int w = 0;
        int rr = radius * radius;
        while (w * w + dy * dy <= rr)
            w++;
        if (w > 0)
            raster_hline(r, cx - w + 1, cx + w - 1, cy + dy,
                         ((dy + radius) & 1)
                            ? omni_rgb(0x7a, 0xc7, 0xff)
                            : omni_rgb(0x1b, 0x59, 0x98));
    }
    /* glossy highlight */
    for (dy = -radius; dy < -radius / 2; dy++) {
        int w = 0;
        int rr = radius * radius;
        while (w * w + dy * dy <= rr)
            w++;
        if (w > radius / 2)
            w = radius / 2;
        if (w > 0)
            raster_hline(r, cx - w + 1, cx + w - 1, cy + dy,
                         omni_mix(omni_rgb(0x7a, 0xc7, 0xff),
                                  omni_rgb(0xff, 0xff, 0xff)));
    }
}

static void draw_wallpaper(struct raster *r)
{
    int w = r->w, h = r->h;
    int cx = w / 2, cy = h / 2 - 20, rad = h / 5;
    struct canvas c;

    raster_gradient_v(r, 0, 0, w, h,
                      OMNI_COLOR_DESKTOP, omni_rgb(0x0a, 0x1e, 0x38));

    if (rad > w / 4) rad = w / 4;
    if (rad < 24) rad = 24;
    draw_logo(r, cx, cy, rad);

    canvas_init(&c, r, omni_rgb(0xff, 0xff, 0xff), 0);
    canvas_text(&c, "OmniOS", cx - 24, cy + rad + 8);
    canvas_text(&c, "a lightweight OS built from scratch",
                cx - 120, cy + rad + 22);
}

/* ------------------------------------------------------------------ */
/* taskbar                                                             */
/* ------------------------------------------------------------------ */

#if 0  /* start button + window strip now handled in draw_taskbar below */
#endif

static void draw_taskbar(struct omni_wm *wm, struct raster *r)
{
    int h = 30;
    int y = r->h - h;
    int x;
    struct canvas c;

    raster_gradient_v(r, 0, y, r->w, h,
                      omni_rgb(0x2d, 0x7d, 0xd2), omni_rgb(0x14, 0x3c, 0x66));
    raster_hline(r, 0, r->w - 1, y, omni_rgb(0x4a, 0x9e, 0xe8));

    /* Start button */
    raster_fill(r, 4, y + 4, 96, h - 8, omni_rgb(0x1a, 0x5a, 0x9e));
    raster_rect(r, 4, y + 4, 96, h - 8, omni_rgb(0xff, 0xff, 0xff));
    canvas_init(&c, r, omni_rgb(0xff, 0xff, 0xff), 0);
    canvas_text(&c, "Start", 4 + 26, y + 9);

    /* window buttons */
    x = 112;
    {
        int i, nbtn = 0;
        for (i = 0; i < wm->nwin && nbtn < OMNI_WM_MAX_WIN; i++) {
            struct omni_win *w = wm->order[wm->nwin - 1 - i];
            int bw = 128;
            if (x + bw > r->w - 120)
                bw = r->w - 120 - x;
            if (bw < 40)
                break;
            raster_fill(r, x, y + 4, bw, h - 8,
                        w == wm->active
                            ? omni_rgb(0x3a, 0x8c, 0xe0)
                            : omni_rgb(0x24, 0x66, 0xa9));
            raster_rect(r, x, y + 4, bw, h - 8, omni_rgb(0x10, 0x30, 0x52));
            canvas_init(&c, r, omni_rgb(0xff, 0xff, 0xff), 0);
            canvas_set_clip(&c, x + 4, y + 9, bw - 8, 8);
            canvas_text(&c, w->title, x + 4, y + 9);
            nbtn++;
            x += bw + 6;
        }
    }

    /* clock */
    {
        char tm[32];
        time_t t = time(NULL);
        struct tm tmv;
        localtime_r(&t, &tmv);
        strftime(tm, sizeof(tm), "%H:%M", &tmv);
        {
            int tw = ((int)strlen(tm)) * 8;
            raster_fill(r, r->w - 64, y + 4, 56, h - 8, 0);
            canvas_init(&c, r, omni_rgb(0xff, 0xff, 0xff), 0);
            canvas_text(&c, tm, r->w - 60 + (56 - tw) / 2 - 2, y + 9);
        }
    }
}

/* ------------------------------------------------------------------ */
/* start menu                                                          */
/* ------------------------------------------------------------------ */

static void draw_menu(struct raster *r, int open, int x, int y, int w,
                      const struct omni_menu_item *items, int n)
{
    struct canvas c;
    int row_h = 22;
    int h = (n + 1) * row_h + 8;
    int i;

    if (!open)
        return;

    raster_fill(r, x, y, w, h, omni_rgb(0xf0, 0xf0, 0xf0));
    raster_rect(r, x, y, w, h, omni_rgb(0x40, 0x40, 0x40));

    canvas_init(&c, r, OMNI_COLOR_TEXT, omni_rgb(0xf0, 0xf0, 0xf0));
    canvas_text(&c, "OmniOS Menu", x + 8, y + 6);
    for (i = 0; i < n; i++) {
        int my = y + 6 + (i + 1) * row_h;
        if (my >= y + h - 4)
            break;
        canvas_text(&c, items[i].label, x + 12, my + row_h / 2 - 4);
    }
}

static int menu_hit(int x, int y, int mx, int my, int mw,
                    const struct omni_menu_item *items, int n, int *idx)
{
    int row_h = 22;
    int i;

    (void)items;
    (void)mw;
    if (x < mx || y < my)
        return 0;
    for (i = 0; i < n; i++) {
        int ry = my + 6 + (i + 1) * row_h;
        if (y >= ry && y < ry + row_h && x >= mx && x < mx + 200) {
            *idx = i;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* app launching                                                       */
/* ------------------------------------------------------------------ */

static void launch(const struct omni_menu_item *item)
{
    pid_t pid = fork();
    if (pid == 0) {
        /* child: detach and exec; the shell keeps running */
        execl(item->path, item->path, (char *)NULL);
        _exit(127);
    }
    /* reap asynchronously to avoid zombies */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

/* ------------------------------------------------------------------ */
/* external app windows                                                */
/* ------------------------------------------------------------------ */

static struct omni_win *new_app_window(const char *title, int x, int y,
                                       int w, int h)
{
    return omni_wm_add(&g_wm, title, x, y, w, h);
}

/* The shell manages its own welcome window plus whatever app windows the
 * user opens. External apps (omnios-*) are separate processes today; the
 * window manager API is also exported so they can talk over a future pipe.
 */

/* ------------------------------------------------------------------ */
/* main loop                                                           */
/* ------------------------------------------------------------------ */

void omni_shell_open_initial_windows(void)
{
    struct omni_win *w = new_app_window("Welcome to OmniOS", 60, 60, 480, 280);
    if (w) {
        struct raster *s = &w->surface;
        struct canvas c;
        raster_fill(s, 0, 0, s->w, s->h, OMNI_COLOR_FACE);
        canvas_init(&c, s, OMNI_COLOR_TEXT, OMNI_COLOR_FACE);
        canvas_text(&c, "Welcome to OmniOS!", 16, 16);
        canvas_text(&c, "This is a from-scratch desktop shell.", 16, 32);
        canvas_text(&c, "Use the Start menu to launch apps.", 16, 48);
        canvas_text(&c, "Drag the title bar to move a window.", 16, 72);
        canvas_text(&c, "Drag the edges to resize it.", 16, 88);
        canvas_text(&c, "Click X to close it.", 16, 104);
    }
}

void omni_shell_key(int code, char ch)
{
    (void)code;
    (void)ch;
}

static const struct omni_menu_item g_menu[] = {
    { "Terminal",     "/usr/bin/omnios-term" },
    { "File Manager", "/usr/bin/omnios-files" },
    { "Calculator",   "/usr/bin/omnios-calc" },
    { "Text Editor",  "/usr/bin/omnios-edit" },
    { "System Info",  "/usr/bin/omnios-sysinfo" },
    { "About OmniOS", "/usr/bin/omnios-about" },
};
static const int g_menu_n = (int)(sizeof(g_menu) / sizeof(g_menu[0]));

static int g_menu_open = 0;

static void set_menu_open(void)
{
    int row_h = 22;
    g_mx = 4;
    g_my = g_screen.h - 26                                      /* taskbar   */
           - (g_menu_n + 1) * row_h - 8;                        /* menu box  */
    g_menu_open = 1;
}

static int omni_button_event(int btn, int pressed, int x, int y)
{
    if (btn != 1 || !pressed)
        return 0;

    /* start menu open: route clicks inside it */
    if (g_menu_open) {
        int idx;
        if (menu_hit(x, y, g_mx, g_my, 200, g_menu, g_menu_n, &idx)) {
            launch(&g_menu[idx]);
        }
        g_menu_open = 0;
        return 1;
    }

    /* Start button (4, screen_h-26, 96, 22) */
    if (g_screen.h > 26 && x >= 4 && x < 100 && y >= g_screen.h - 26) {
        set_menu_open();
        return 1;
    }

    /* taskbar window buttons */
    {
        int i, nbtn = 0, bx = 112;
        for (i = 0; i < g_wm.nwin && nbtn < OMNI_WM_MAX_WIN; i++) {
            struct omni_win *w = g_wm.order[g_wm.nwin - 1 - i];
            int bw = 128;
            if (bx + bw > g_screen.w - 120) bw = g_screen.w - 120 - bx;
            if (bw < 40) break;
            if (x >= bx && x < bx + bw && y >= g_screen.h - 26) {
                if (w->y >= g_screen.h) {
                    /* restore a minimized window */
                    w->y = 60;
                }
                omni_wm_raise(&g_wm, w);
                return 1;
            }
            nbtn++;
            bx += bw + 6;
        }
    }

    return 0;
}

static int omni_button_fwd(int btn, int pressed, int x, int y)
{
    if (pressed && btn != 1)
        return 0;

    /* window-manager-level handling (move/resize/close/raise) */
    omni_wm_button(&g_wm, x, y, btn, pressed);
    return 0;
}

void omni_shell_run(void)
{
    struct osfb fb;
    struct omni_devs devs;
    struct pollfd pfds[16];
    int nfds;
    int clock_last = -1;

    if (osfb_wait("/dev/fb0", 50, 200) < 0)
        omni_console_puts("desktop: no framebuffer\n");
    if (osfb_open(&fb, "/dev/fb0") < 0) {
        omni_console_puts("desktop: cannot open /dev/fb0\n");
        return;
    }
    omni_console_puts("desktop: framebuffer ready\n");

    /* build the raster view of the mapped framebuffer */
    g_screen = fb_raster(&fb);

    omni_wm_init(&g_wm, &g_screen);
    omni_devs_open(&devs);
    nfds = omni_devs_nfds(&devs);
    omni_devs_fill(&devs, pfds);

    omni_shell_open_initial_windows();
    g_px = g_screen.w / 2;
    g_py = g_screen.h / 2;

    omni_wm_paint(&g_wm);
    draw_wallpaper(&g_screen);
    draw_taskbar(&g_wm, &g_screen);

    for (;;) {
        time_t t = time(NULL);
        struct tm tmv;
        int redraw = 0;
        struct omni_input e;

        poll(pfds, (nfds_t)nfds, 120);

        /* drain readable devices */
        {
            int idx;
            for (idx = 0; idx < nfds; idx++) {
                if (pfds[idx].revents & POLLIN)
                    omni_devs_drain(&devs, idx);
            }
        }

        while (omni_input_next(&e) == 0) {
            if (e.type == 2) {
                omni_shell_warp(e.dx, e.dy);
                if (g_px >= g_screen.w) g_px = g_screen.w - 1;
                if (g_py >= g_screen.h) g_py = g_screen.h - 1;
                if (omni_wm_motion(&g_wm, g_px, g_py) == 1)
                    redraw = 1;
            } else if (e.type == 3) {
                omni_button_fwd(e.key, e.pressed, g_px, g_py);
                if (omni_button_event(e.key, e.pressed, g_px, g_py))
                    redraw = 1;
            } else if (e.type == 1 && e.pressed) {
                omni_shell_key(e.key, e.text);
                redraw = 1;
            }
        }

        localtime_r(&t, &tmv);
        if (tmv.tm_min != clock_last) {
            clock_last = tmv.tm_min;
            redraw = 1;
        }

        if (redraw) {
            omni_wm_paint(&g_wm);
            draw_wallpaper(&g_screen);
            draw_taskbar(&g_wm, &g_screen);
            draw_menu(&g_screen, g_menu_open, g_mx, g_my, 200,
                      g_menu, g_menu_n);
        }
    }

    omni_devs_close(&devs);
    osfb_close(&fb);
}
