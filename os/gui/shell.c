/*
 * OmniOS — os/gui/shell.c
 *
 * The graphical desktop shell: gradient wallpaper with the OmniOS logo,
 * a Windows-style taskbar (Start button, window buttons, clock) and a
 * pop-up start menu.
 *
 * It is the display server: it owns the framebuffer, runs the window
 * manager (os/gui/wm.c) on a UNIX socket at /tmp/.omnios-wm, accepts
 * application clients, routes keyboard/mouse input to them, and paints
 * wallpaper + windows + taskbar each frame.
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

#define OMNI_WM_CURSOR_W 12        /* cursor sprite width, px           */
#define OMNI_WM_CURSOR_H 12        /* cursor sprite height, px          */

/* ------------------------------------------------------------------ */
/* global desktop state                                               */
/* ------------------------------------------------------------------ */

static struct omni_wm   g_wm;
static struct raster    g_screen;
static int g_px, g_py;              /* pointer (mouse) position           */
static int g_mx, g_my;              /* start-menu top-left origin         */
static int g_menu_open;             /* start menu visible                 */

/* scratch client-rect for the mouse-cursor sprite (rows padded by 1 px)  */
static uint32_t g_cursor_scratch[OMNI_WM_CURSOR_W * OMNI_WM_CURSOR_H
                                 + OMNI_WM_CURSOR_W + 1];

static struct canvas g_cursor_cv;   /* bound to the cursor scratch         */

static const struct omni_menu_item g_menu[] = {
    { "Terminal",     "/usr/bin/omnios-term" },
    { "File Manager", "/usr/bin/omnios-files" },
    { "Calculator",   "/usr/bin/omnios-calc" },
    { "Text Editor",  "/usr/bin/omnios-edit" },
    { "System Info",  "/usr/bin/omnios-sysinfo" },
    { "About OmniOS", "/usr/bin/omnios-about" },
};
static const int g_menu_n = (int)(sizeof(g_menu) / sizeof(g_menu[0]));

static const int TASKBAR_H = 30;

/* ------------------------------------------------------------------ */
/* mouse cursor                                                       */
/* ------------------------------------------------------------------ */

/* Build the arrow sprite into g_cursor_scratch (white fill, dark
 * outline, classic north-west arrow), once per session. */
static void cursor_sprite_build(void)
{
    const uint32_t fill = 0xffffffffu;   /* white   */
    const uint32_t outl = 0xff000000u;   /* outline */
    int y, x;

    for (y = 0; y < OMNI_WM_CURSOR_H; y++) {
        for (x = 0; x <= y && x < OMNI_WM_CURSOR_W; x++)
            raster_px(&g_cursor_cv.r,
                      x, y, (x == 0 || x == y || y == 0) ? outl : fill);
    }
    for (y = 4; y <= 6; y++)
        for (x = 4; x <= 11; x++)
            raster_px(&g_cursor_cv.r,
                      x, y, (y == 4 || y == 6 || x == 11) ? outl : fill);
}

/* Paint the cursor sprite onto the framebuffer at (g_px, g_py).
 * Sprite pixels are either transparent (0) or opaque (white fill /
 * black outline); opaque colors are written verbatim.  Because only
 * this 12x12 patch is touched, it double-paints cleanly on top of
 * whatever the last wallpaper/window paint left underneath. */
static void cursor_draw(void)
{
    int sy, sx;

    for (sy = 0; sy < OMNI_WM_CURSOR_H; sy++) {
        int dsty = g_py + sy;
        if (dsty < 0 || dsty >= g_screen.h)
            continue;
        for (sx = 0; sx < OMNI_WM_CURSOR_W; sx++) {
            int dstx = g_px + sx;
            uint32_t sc;
            if (dstx < 0 || dstx >= g_screen.w)
                continue;
            sc = g_cursor_cv.r.bits[(size_t)sy * (size_t)g_cursor_cv.r.stride
                                    + (size_t)sx];
            if (sc != 0)
                raster_px(&g_screen, dstx, dsty, sc);
        }
    }
}

/* ------------------------------------------------------------------ */
/* frame finish (chrome on top)                                       */
/* ------------------------------------------------------------------ */

/* forward declarations (definitions live further down) */
static void draw_taskbar(struct omni_wm *wm, struct raster *r);
static void draw_menu(struct raster *r, int x, int y, int w, int n);

/* Called by omni_wm_paint() after the wallpaper and windows are in place:
 * draw the always-on-top taskbar/start-menu and the mouse cursor. */
static void shell_finish(struct omni_wm *wm)
{
    draw_taskbar(wm, &wm->screen);
    if (g_menu_open)
        draw_menu(&wm->screen, g_mx, g_my, 200, g_menu_n);
    cursor_draw();
}

/* ------------------------------------------------------------------ */
/* pointer motion                                                     */
/* ------------------------------------------------------------------ */

/* clamp a delta so the cursor can never cross the desktop        */
#define OMNI_WARP_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

static void omni_shell_warp(int dx, int dy)
{
    if (dx > 63)   dx -= 128;   /* PS/2 9-bit underflow */
    if (dx < -64)  dx += 128;
    if (dy > 63)   dy -= 128;
    if (dy < -64)  dy += 128;

    /* safety clamp: beyond one byte per event cannot come from sane
     * devices, and it stops a bogus device from teleporting the cursor. */
    dx = OMNI_WARP_CLAMP(dx, -255, 255);
    dy = OMNI_WARP_CLAMP(dy, -255, 255);

    {
        int nx = g_px, ny = g_py;

        if (dx != 0)
            nx = OMNI_WARP_CLAMP(g_px + dx, 0, g_screen.w - 1);
        if (dy != 0)
            ny = OMNI_WARP_CLAMP(g_py + dy, 0, g_screen.h - 1);

        if (nx != g_px) g_px = nx;
        if (ny != g_py) g_py = ny;
    }
}

/* ------------------------------------------------------------------ */
/* wallpaper + logo                                                   */
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

/* desktop background; invoked by omni_wm_paint() before any window is
 * blitted, so windows always composite on top of the wallpaper. */
static void shell_draw_background(struct omni_wm *wm)
{
    struct raster *r = &wm->screen;
    int cx = r->w / 2, cy = r->h / 2 - 20, rad = r->h / 5;
    struct canvas c;

    raster_gradient_v(r, 0, 0, r->w, r->h,
                      OMNI_COLOR_DESKTOP, omni_rgb(0x0a, 0x1e, 0x38));

    if (rad > r->w / 4) rad = r->w / 4;
    if (rad < 24) rad = 24;
    draw_logo(r, cx, cy, rad);

    canvas_init(&c, r, omni_rgb(0xff, 0xff, 0xff), 0);
    canvas_text(&c, "OmniOS", cx - 24, cy + rad + 8);
    canvas_text(&c, "a lightweight OS built from scratch",
                cx - 120, cy + rad + 22);
}

/* ------------------------------------------------------------------ */
/* taskbar (always on top of windows)                                 */
/* ------------------------------------------------------------------ */

static void draw_taskbar(struct omni_wm *wm, struct raster *r)
{
    int y = r->h - TASKBAR_H;
    int x;
    struct canvas c;

    raster_gradient_v(r, 0, y, r->w, TASKBAR_H,
                      omni_rgb(0x2d, 0x7d, 0xd2), omni_rgb(0x14, 0x3c, 0x66));
    raster_hline(r, 0, r->w - 1, y, omni_rgb(0x4a, 0x9e, 0xe8));

    /* Start button */
    raster_fill(r, 4, y + 4, 96, TASKBAR_H - 8, omni_rgb(0x1a, 0x5a, 0x9e));
    raster_rect(r, 4, y + 4, 96, TASKBAR_H - 8, omni_rgb(0xff, 0xff, 0xff));
    canvas_init(&c, r, omni_rgb(0xff, 0xff, 0xff), 0);
    canvas_text(&c, "Start", 4 + 26, y + 9);

    /* window buttons, left -> right in stacking order */
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
            raster_fill(r, x, y + 4, bw, TASKBAR_H - 8,
                        w == wm->active
                            ? omni_rgb(0x3a, 0x8c, 0xe0)
                            : omni_rgb(0x24, 0x66, 0xa9));
            raster_rect(r, x, y + 4, bw, TASKBAR_H - 8, omni_rgb(0x10, 0x30, 0x52));
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
            raster_fill(r, r->w - 64, y + 4, 56, TASKBAR_H - 8, 0);
            canvas_init(&c, r, omni_rgb(0xff, 0xff, 0xff), 0);
            canvas_text(&c, tm, r->w - 60 + (56 - tw) / 2 - 2, y + 9);
        }
    }
}

/* ------------------------------------------------------------------ */
/* start menu                                                         */
/* ------------------------------------------------------------------ */

static void draw_menu(struct raster *r, int x, int y, int w, int n)
{
    struct canvas c;
    int row_h = 22;
    int h = (n + 1) * row_h + 8;
    int i;

    raster_fill(r, x, y, w, h, omni_rgb(0xf0, 0xf0, 0xf0));
    raster_rect(r, x, y, w, h, omni_rgb(0x40, 0x40, 0x40));

    canvas_init(&c, r, OMNI_COLOR_TEXT, omni_rgb(0xf0, 0xf0, 0xf0));
    canvas_text(&c, "OmniOS Menu", x + 8, y + 6);
    for (i = 0; i < n; i++) {
        int my = y + 6 + (i + 1) * row_h;
        if (my >= y + h - 4)
            break;
        canvas_text(&c, g_menu[i].label, x + 12, my + row_h / 2 - 4);
    }
}

static int menu_hit(int x, int y, int *idx)
{
    int row_h = 22;
    int mw = 200;
    int i;

    if (x < g_mx || y < g_my)
        return 0;
    for (i = 0; i < g_menu_n; i++) {
        int ry = g_my + 6 + (i + 1) * row_h;
        if (y >= ry && y < ry + row_h && x >= g_mx && x < g_mx + mw) {
            *idx = i;
            return 1;
        }
    }
    return 0;
}

static void open_menu(void)
{
    int row_h = 22;
    g_mx = 4;
    g_my = g_screen.h - TASKBAR_H - (g_menu_n + 1) * row_h - 8;
    if (g_my < 0)
        g_my = 0;
    g_menu_open = 1;
}

/* ------------------------------------------------------------------ */
/* app launching                                                      */
/* ------------------------------------------------------------------ */

static void launch(const struct omni_menu_item *item)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl(item->path, item->path, (char *)NULL);
        _exit(127);
    }
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

/* ------------------------------------------------------------------ */
/* input -> shell / server routing                                    */
/* ------------------------------------------------------------------ */

/* handle a pointer click: start menu, taskbar buttons, or window chrome */
static int shell_button(int x, int y)
{
    /* open start menu routes clicks to entries */
    if (g_menu_open) {
        int idx;
        if (menu_hit(x, y, &idx))
            launch(&g_menu[idx]);
        g_menu_open = 0;
        return 1;
    }

    /* Start button */
    if (y >= g_screen.h - TASKBAR_H && x >= 4 && x < 100) {
        open_menu();
        return 1;
    }

    /* taskbar window buttons: focus/restore, or minimize the active one */
    if (y >= g_screen.h - TASKBAR_H) {
        int i, nbtn = 0, bx = 112;
        for (i = 0; i < g_wm.nwin && nbtn < OMNI_WM_MAX_WIN; i++) {
            struct omni_win *w = g_wm.order[g_wm.nwin - 1 - i];
            int bw = 128;
            if (bx + bw > g_screen.w - 120) bw = g_screen.w - 120 - bx;
            if (bw < 40) break;
            if (x >= bx && x < bx + bw) {
                if (w == g_wm.active && w->y < g_screen.h) {
                    /* minimize: slide below the visible desktop */
                    w->y = g_screen.h + 4;
                    g_wm.active = NULL;
                } else {
                    if (w->y >= g_screen.h)
                        w->y = 60;   /* restore */
                    omni_wm_raise(&g_wm, w);
                }
                return 1;
            }
            nbtn++;
            bx += bw + 6;
        }
    }
    return 0;
}

void omni_shell_open_initial_windows(void)
{
    struct omni_win *w = omni_wm_add(&g_wm, "Welcome to OmniOS", 60, 60, 480, 280);
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

/* ------------------------------------------------------------------ */
/* main loop                                                          */
/* ------------------------------------------------------------------ */

void omni_shell_run(void)
{
    struct osfb fb;
    struct omni_devs devs;
    struct pollfd pfds[1 + 8 + 1 + 1];   /* wm listener + ev* + mice + tty */
    int nfds_dev, nfds;
    int clock_last = -1;
    int lfdidx = 0;

    if (osfb_wait("/dev/fb0", 50, 200) < 0)
        omni_console_puts("desktop: no framebuffer\n");
    if (osfb_open(&fb, "/dev/fb0") < 0) {
        omni_console_puts("desktop: cannot open /dev/fb0\n");
        return;
    }
    omni_console_puts("desktop: framebuffer ready\n");

    g_screen = fb_raster(&fb);

    /* The mouse-cursor sprite is a private scratch raster (native 32-bit)
     * so it can be painted over the framebuffer without touching device
     * pixel-format conversion. */
    raster_init(&g_cursor_cv.r, g_cursor_scratch,
                OMNI_WM_CURSOR_W, OMNI_WM_CURSOR_H, OMNI_WM_CURSOR_W);
    g_cursor_cv.fg = 0xffffffffu;
    g_cursor_cv.bg = 0;
    cursor_sprite_build();

    omni_wm_init(&g_wm, &g_screen);
    g_wm.draw_background = shell_draw_background;
    g_wm.finish = shell_finish;

    if (omni_wm_start(&g_wm) != 0)
        omni_console_puts("desktop: WARN display socket not started\n");

    omni_devs_open(&devs);
    nfds_dev = omni_devs_nfds(&devs);
    omni_devs_fill(&devs, &pfds[1]);

    /* index 0 = display socket listener */
    pfds[0].fd = omni_wm_fd(&g_wm);
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    nfds = 1 + nfds_dev;

    omni_shell_open_initial_windows();
    g_px = g_screen.w / 2;
    g_py = g_screen.h / 2;

    /* first paint (wallpaper + welcome window + chrome + cursor) */
    omni_wm_paint(&g_wm);

    for (;;) {
        time_t t = time(NULL);
        struct tm tmv;
        int redraw = 0;
        struct omni_input e;
        int ci;

        if (poll(pfds, (nfds_t)nfds, 120) < 0)
            continue;

        /* new client connections */
        if (pfds[lfdidx].revents & POLLIN)
            omni_wm_accept(&g_wm);

        /* dispatch client protocol (non-blocking, one pass each) */
        for (ci = 0; ci < OMNI_WM_MAX_CLI; ci++) {
            if (g_wm.clients[ci].fd >= 0) {
                struct pollfd pfd;
                pfd.fd = g_wm.clients[ci].fd;
                pfd.events = POLLIN;
                pfd.revents = 0;
                if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN))
                    omni_wm_handle_client(&g_wm, ci);
            }
        }

        /* input devices */
        {
            int idx;
            for (idx = 0; idx < nfds_dev; idx++) {
                if (pfds[1 + idx].revents & POLLIN)
                    omni_devs_drain(&devs, idx);
            }
        }

        while (omni_input_next(&e) == 0) {
            if (e.type == 2) {                       /* mouse motion */
                omni_shell_warp(e.dx, e.dy);
                omni_wm_motion(&g_wm, g_px, g_py);
                redraw = 1;
            } else if (e.type == 3) {                 /* button */
                if (e.pressed && shell_button(g_px, g_py))
                    redraw = 1;
                else
                    omni_wm_button(&g_wm, g_px, g_py, e.key, e.pressed);
                if (e.pressed)
                    redraw = 1;
            } else if (e.type == 1) {                 /* key */
                if (e.pressed && g_wm.active)
                    omni_wm_key(&g_wm, e.key, e.pressed, e.text);
            }
        }

        localtime_r(&t, &tmv);
        if (tmv.tm_min != clock_last) {
            clock_last = tmv.tm_min;
            redraw = 1;
        }

        if (redraw)
            omni_wm_paint(&g_wm);            /* wallpaper + windows +
                                                chrome + cursor (hook) */
    }

    omni_devs_close(&devs);
    osfb_close(&fb);
}
