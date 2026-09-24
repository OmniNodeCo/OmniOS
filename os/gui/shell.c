/*
 * OmniOS — os/gui/shell.c
 *
 * The graphical desktop shell and display server. It owns the framebuffer,
 * runs the window manager (os/gui/wm.c) on a UNIX socket at
 * /tmp/.omnios-wm, accepts application clients, routes keyboard/mouse
 * input to them, and draws the desktop: wallpaper, windows, a
 * Windows-style taskbar (Start button, window buttons, clock) and the
 * Start menu.
 *
 * Rendering is built to be fast on a plain framebuffer:
 *   - frames are composed in a RAM back buffer (g_screen): the wallpaper
 *     is rendered once and copied in, windows and chrome drawn on top;
 *   - present_scene() compares the new frame with a copy of what the
 *     framebuffer shows (g_front) and writes only the spans that changed,
 *     so video memory (slow to write, very slow to read) is touched as
 *     little as possible and nothing flickers;
 *   - the mouse cursor is a sprite drawn straight onto the device over
 *     g_front: moving the mouse restores the old rectangle and draws the
 *     new one, without recomposing the desktop;
 *   - the desktop is recomposed only when something visible changed (app
 *     drawing, window moves, focus/hover changes, the clock).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <poll.h>
#include <pwd.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include "catalog.h"
#include "shell.h"
#include "theme.h"
#include "settings.h"
#include "account.h"
#include "wm.h"

/* ------------------------------------------------------------------ */
/* layout                                                             */
/* ------------------------------------------------------------------ */

#define TASKBAR_H    OMNI_TASKBAR_H
#define START_W      48         /* Start button                          */
#define TB_BTN_W     180        /* taskbar window button, widest         */
#define TB_BTN_MIN   44         /* ... narrowest (icon only)             */
#define TB_GAP       4
#define TB_TRAY_W    112        /* clock                                 */
#define MENU_W       340
#define MENU_HEAD_H  76
#define MENU_ROW_H   36
#define MENU_ROW_MIN 26          /* rows shrink to this before apps drop */
#define MENU_FOOT_H  56
#define MENU_RAD     10
#define PILL_W       96         /* Restart / Shut down buttons           */
#define PILL_H       28

/* hover targets (hover_key) */
#define HV_START     1
#define HV_TASK      10         /* + taskbar button index                */
#define HV_PANEL     50         /* Start menu background                 */
#define HV_ROW       100        /* + Start menu row                      */
#define HV_RESTART   200
#define HV_SHUTDOWN  201
#define HV_CAPTION   1000       /* + window id * 16 + chrome zone        */

#define KEY_LEFTMETA  125       /* the Windows keys                      */
#define KEY_RIGHTMETA 126

/* ------------------------------------------------------------------ */
/* global desktop state                                               */
/* ------------------------------------------------------------------ */

static struct omni_wm   g_wm;
static struct raster    g_screen;   /* the frame being composed (RAM)     */
static struct raster    g_dev;      /* the framebuffer                     */
static uint32_t        *g_front;    /* what g_dev shows, minus the cursor  */
static uint32_t        *g_wall;     /* the wallpaper, rendered once        */
static uint32_t        *g_rowbuf;   /* one device row of scratch           */
static uint32_t        *g_barbg;    /* taskbar glass over bare wallpaper   */
static struct omni_settings g_set;   /* the user's settings (settings.c)    */
static int              g_set_wall = -1;     /* wallpaper style rendered    */
static time_t           g_set_mtime;         /* settings file last seen     */
static off_t            g_set_size = -1;
static char             g_user[40] = "root"; /* account shown in Start      */
static int g_full_present;          /* next present rewrites every pixel   */
static int g_px, g_py;              /* pointer (mouse) position            */
static int g_menu_open;             /* Start menu visible                  */
static int g_menu_sel = -1;         /* highlighted Start menu row          */
static int g_hover;                 /* hovered chrome element (HV_*)       */
static int g_input_ok_ptr;          /* a pointer source was found          */
static int g_input_ok_kbd;          /* a keyboard source was found         */
static time_t g_shell_t0;           /* desktop start (for the banner)      */
static char g_version[32];          /* from /etc/omnios-release            */

/* Start menu entries: the installed apps from the catalog (catalog.c).
 * Rebuilt each time the menu opens, so the App Store's changes show up
 * straight away. */
static struct omni_menu_item g_menu[OMNI_CATALOG_MAX];
static const struct omni_app_info *g_menu_app[OMNI_CATALOG_MAX];
static int g_menu_n;

static void menu_reload(void)
{
    unsigned char installed[OMNI_CATALOG_MAX];
    int i;

    omni_apps_load(installed);
    g_menu_n = 0;
    for (i = 0; i < omni_catalog_n && g_menu_n < OMNI_CATALOG_MAX; i++) {
        if (!installed[i] || access(omni_catalog[i].path, X_OK) != 0)
            continue;               /* not installed, or not in this image */
        g_menu[g_menu_n].label = omni_catalog[i].name;
        g_menu[g_menu_n].path = omni_catalog[i].path;
        g_menu_app[g_menu_n] = &omni_catalog[i];
        g_menu_n++;
    }
}

/* icon for a window title (wm hook; also used by the taskbar) */
static uint32_t shell_icon_for(const char *title, char *glyph)
{
    const struct omni_app_info *a = omni_app_for_title(title);
    if (!a) {
        if (title && strncmp(title, "Welcome", 7) == 0) {
            *glyph = 'O';                   /* the OmniOS logo */
            return 0x8b5cf6;
        }
        return 0;
    }
    *glyph = a->glyph;
    return a->color;
}

static void read_version(void)
{
    FILE *f = fopen("/etc/omnios-release", "r");
    char line[128], name[32], ver[32];

    g_version[0] = '\0';
    if (!f)
        return;
    /* "OmniOS 2026.2.1 (from-source lightweight OS)" */
    if (fgets(line, sizeof(line), f) &&
        sscanf(line, "%31s %31s", name, ver) == 2)
        snprintf(g_version, sizeof(g_version), "%s", ver);
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* mouse cursor: a sprite on the device, never part of the scene      */
/* ------------------------------------------------------------------ */

#define CUR_W 16
#define CUR_H 24

static uint32_t g_cur_rgb[CUR_H][CUR_W];
static uint8_t  g_cur_a[CUR_H][CUR_W];
static int g_cur_x, g_cur_y, g_cur_on;  /* where the sprite is drawn     */

/* the classic arrow: X outline, . fill; plus a soft shadow */
static void cursor_build(void)
{
    static const char *const arrow[] = {
        "X",
        "XX",
        "X.X",
        "X..X",
        "X...X",
        "X....X",
        "X.....X",
        "X......X",
        "X.......X",
        "X........X",
        "X.........X",
        "X..........X",
        "X......XXXXX",
        "X...X..X",
        "X..XX..X",
        "X.X  X..X",
        "XX   X..X",
        "X     X..X",
        "      X..X",
        "       XX",
    };
    const int rows = (int)(sizeof(arrow) / sizeof(arrow[0]));
    unsigned char m[CUR_H][CUR_W];
    int x, y, dx, dy;

    memset(m, 0, sizeof(m));
    for (y = 0; y < rows; y++)
        for (x = 0; arrow[y][x]; x++)
            if (arrow[y][x] != ' ')
                m[y][x] = 1;

    /* shadow: the shape offset by (1, 2), box-blurred */
    for (y = 0; y < CUR_H; y++)
        for (x = 0; x < CUR_W; x++) {
            int s = 0;
            for (dy = -1; dy <= 1; dy++)
                for (dx = -1; dx <= 1; dx++) {
                    int sy = y - 2 + dy, sx = x - 1 + dx;
                    if (sy >= 0 && sx >= 0 && sy < CUR_H && sx < CUR_W)
                        s += m[sy][sx];
                }
            g_cur_rgb[y][x] = 0x000000;
            g_cur_a[y][x] = (uint8_t)(s * 9);
        }
    for (y = 0; y < rows; y++)
        for (x = 0; arrow[y][x]; x++) {
            if (arrow[y][x] == ' ')
                continue;
            g_cur_rgb[y][x] = arrow[y][x] == 'X' ? 0x101010 : 0xffffff;
            g_cur_a[y][x] = 255;
        }
}

/* Rewrite a device rectangle from g_front, with the cursor on top. */
static void device_rect(int x, int y, int w, int h)
{
    int yy;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_screen.w) w = g_screen.w - x;
    if (y + h > g_screen.h) h = g_screen.h - y;
    if (w <= 0 || h <= 0)
        return;

    for (yy = y; yy < y + h; yy++) {
        const uint32_t *src = g_front + (size_t)yy * (size_t)g_screen.w + (size_t)x;
        int sy = yy - g_cur_y;

        if (g_cur_on && sy >= 0 && sy < CUR_H &&
            g_cur_x < x + w && g_cur_x + CUR_W > x) {
            int i;
            memcpy(g_rowbuf, src, (size_t)w * sizeof(uint32_t));
            for (i = 0; i < CUR_W; i++) {
                int xx = g_cur_x + i;
                unsigned a = g_cur_a[sy][i];
                if (a && xx >= x && xx < x + w)
                    g_rowbuf[xx - x] = th_blend(g_rowbuf[xx - x],
                                                g_cur_rgb[sy][i], a);
            }
            raster_put_row(&g_dev, x, yy, g_rowbuf, w);
        } else {
            raster_put_row(&g_dev, x, yy, src, w);
        }
    }
}

/* Move the cursor sprite: one device update covering old + new spot. */
static void cursor_move(int nx, int ny)
{
    int ox = g_cur_x, oy = g_cur_y, was = g_cur_on;

    if (was && nx == ox && ny == oy)
        return;
    g_cur_x = nx;
    g_cur_y = ny;
    g_cur_on = 1;

    if (was && ox < nx + CUR_W && nx < ox + CUR_W &&
        oy < ny + CUR_H && ny < oy + CUR_H) {
        int x0 = ox < nx ? ox : nx, y0 = oy < ny ? oy : ny;
        int x1 = (ox > nx ? ox : nx) + CUR_W, y1 = (oy > ny ? oy : ny) + CUR_H;
        device_rect(x0, y0, x1 - x0, y1 - y0);
    } else {
        if (was)
            device_rect(ox, oy, CUR_W, CUR_H);
        device_rect(nx, ny, CUR_W, CUR_H);
    }
}

/* ------------------------------------------------------------------ */
/* presenting a composed frame                                        */
/* ------------------------------------------------------------------ */

/* Copy the frame to the device, writing only the spans (in 16-pixel
 * blocks) that differ from what the device already shows. */
static void present_scene(void)
{
    const int W = g_screen.w, B = 16;
    int y;

    for (y = 0; y < g_screen.h; y++) {
        uint32_t *s = g_screen.bits + (size_t)y * (size_t)W;
        uint32_t *f = g_front + (size_t)y * (size_t)W;
        int x = 0;

        if (g_full_present) {
            memcpy(f, s, (size_t)W * sizeof(uint32_t));
            device_rect(0, y, W, 1);
            continue;
        }
        if (memcmp(s, f, (size_t)W * sizeof(uint32_t)) == 0)
            continue;
        while (x < W) {
            int x0, n = W - x < B ? W - x : B;
            if (memcmp(s + x, f + x, (size_t)n * sizeof(uint32_t)) == 0) {
                x += n;
                continue;
            }
            x0 = x;
            for (;;) {                  /* extend over changed blocks */
                x += n;
                if (x >= W)
                    break;
                n = W - x < B ? W - x : B;
                if (memcmp(s + x, f + x, (size_t)n * sizeof(uint32_t)) == 0)
                    break;
            }
            memcpy(f + x0, s + x0, (size_t)(x - x0) * sizeof(uint32_t));
            device_rect(x0, y, x - x0, 1);
        }
    }
    g_full_present = 0;
}

/* ------------------------------------------------------------------ */
/* pointer motion                                                     */
/* ------------------------------------------------------------------ */

/* clamp a delta so the cursor can never cross the desktop        */
#define OMNI_WARP_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

/* Relative motion.  Deltas arrive already sign-correct: evdev reports
 * plain signed REL_X/REL_Y values and the /dev/input/mice decoder
 * sign-extends its bytes, so no PS/2 wrap-around fix-up belongs here
 * (the old "9-bit underflow" correction reversed every fast movement:
 * a +70 delta became -58). */
static void omni_shell_warp(int dx, int dy)
{
    /* safety clamp: beyond one byte per event cannot come from sane
     * devices, and it stops a bogus device from teleporting the cursor. */
    dx = OMNI_WARP_CLAMP(dx, -255, 255);
    dy = OMNI_WARP_CLAMP(dy, -255, 255);

    if (dx != 0)
        g_px = OMNI_WARP_CLAMP(g_px + dx, 0, g_screen.w - 1);
    if (dy != 0)
        g_py = OMNI_WARP_CLAMP(g_py + dy, 0, g_screen.h - 1);
}

/* Absolute position from a tablet-style pointer (VMware vmmouse, USB or
 * virtio tablets), normalised to 0..65535 across the whole screen. */
static void omni_shell_moveto(int ax, int ay)
{
    ax = OMNI_WARP_CLAMP(ax, 0, 65535);
    ay = OMNI_WARP_CLAMP(ay, 0, 65535);
    g_px = (int)((long)ax * (g_screen.w - 1) / 65535);
    g_py = (int)((long)ay * (g_screen.h - 1) / 65535);
}

/* ------------------------------------------------------------------ */
/* taskbar                                                            */
/* ------------------------------------------------------------------ */

struct tb_btn {
    struct omni_win *w;
    int x, bw;
};

/* one button per window, in stable (creation slot) order */
static int taskbar_layout(struct tb_btn *b)
{
    int i, n = 0, cnt = 0, x = START_W + 8, bw;
    int end = g_screen.w - TB_TRAY_W;

    for (i = 0; i < OMNI_WM_MAX_WIN; i++)
        cnt += g_wm.wins[i].used ? 1 : 0;
    if (!cnt)
        return 0;
    bw = (end - x - (cnt - 1) * TB_GAP) / cnt;
    if (bw > TB_BTN_W) bw = TB_BTN_W;
    if (bw < TB_BTN_MIN) bw = TB_BTN_MIN;
    for (i = 0; i < OMNI_WM_MAX_WIN; i++) {
        if (!g_wm.wins[i].used)
            continue;
        if (x + bw > end)
            break;
        b[n].w = &g_wm.wins[i];
        b[n].x = x;
        b[n].bw = bw;
        n++;
        x += bw + TB_GAP;
    }
    return n;
}

static void taskbar_glass(struct raster *r, int y)
{
    th_frost(r, 0, y, r->w, TASKBAR_H, 0, 12);
    th_fill_a(r, 0, y, r->w, TASKBAR_H, TH_TASKBAR, 200);
    th_fill_a(r, 0, y, r->w, 1, 0xffffff, 26);
}

/* does any window (or its shadow) reach under the taskbar? */
static int taskbar_covered(void)
{
    int i, top = g_screen.h - TASKBAR_H;
    for (i = 0; i < g_wm.nwin; i++) {
        const struct omni_win *w = g_wm.order[i];
        if (!w->minimized && w->y + w->h + 32 > top)
            return 1;
    }
    return 0;
}

static void draw_taskbar(void)
{
    struct raster *r = &g_screen;
    struct tb_btn b[OMNI_WM_MAX_WIN];
    int y = r->h - TASKBAR_H, i, n;

    /* frosted, tinted glass with a faint top highlight: pre-rendered
     * over the wallpaper, live only while a window reaches beneath */
    if (g_barbg && !taskbar_covered())
        memcpy(r->bits + (size_t)y * (size_t)r->stride, g_barbg,
               (size_t)r->w * TASKBAR_H * sizeof(uint32_t));
    else
        taskbar_glass(r, y);

    /* Start button: the OmniOS logo */
    if (g_menu_open || g_hover == HV_START)
        th_round_rect(r, 4, y + 4, START_W - 8, TASKBAR_H - 8, 6, 0xffffff,
                      g_menu_open ? 40 : 24);
    th_logo(r, START_W / 2, y + TASKBAR_H / 2, 11);

    /* window buttons: icon + title; accent bar under the active one */
    n = taskbar_layout(b);
    for (i = 0; i < n; i++) {
        struct omni_win *w = b[i].w;
        int active = (w == g_wm.active && !w->minimized);
        unsigned a = active ? 44 : (w->minimized ? 0 : 16);
        char glyph = w->title[0] ? w->title[0] : '?';
        uint32_t ic = shell_icon_for(w->title, &glyph);

        if (g_hover == HV_TASK + i)
            a += 20;
        if (a)
            th_round_rect(r, b[i].x, y + 4, b[i].bw, TASKBAR_H - 8, 6,
                          0xffffff, a);
        th_icon(r, b[i].x + 12, y + 12, 20, ic ? ic : TH_ACCENT, glyph);
        if (b[i].bw > 64)
            th_text_clip(r, w->title, b[i].x + 40, y + 18,
                         w->minimized ? TH_TEXT_DIM : TH_TEXT_LIGHT,
                         b[i].bw - 48);
        if (active)
            th_round_rect(r, b[i].x + b[i].bw / 2 - 10, y + TASKBAR_H - 7,
                          20, 3, 1, TH_ACCENT, 255);
        else
            th_round_rect(r, b[i].x + b[i].bw / 2 - 4, y + TASKBAR_H - 7,
                          8, 3, 1, 0xffffff, 110);
    }

    /* clock: time over date, right-aligned */
    {
        char tbuf[40], dbuf[40];
        time_t t = time(NULL);
        struct tm tmv;
        int hr, tw, dw;

        localtime_r(&t, &tmv);
        hr = tmv.tm_hour % 12;
        if (g_set.clock24)
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        else
            snprintf(tbuf, sizeof(tbuf), "%d:%02d %s", hr ? hr : 12, tmv.tm_min,
                     tmv.tm_hour < 12 ? "AM" : "PM");
        snprintf(dbuf, sizeof(dbuf), "%d/%d/%d", tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_year + 1900);
        tw = (int)strlen(tbuf) * 8;
        dw = (int)strlen(dbuf) * 8;
        th_text(r, tbuf, r->w - 16 - tw, y + 10, TH_TEXT_LIGHT);
        th_text(r, dbuf, r->w - 16 - dw, y + 26, TH_TEXT_DIM);
    }
}

/* ------------------------------------------------------------------ */
/* Start menu                                                         */
/* ------------------------------------------------------------------ */

struct menu_geom {
    int x, y, w, h;
    int rows_y, nrows;          /* app rows (as many as fit)             */
    int row_h;                  /* row pitch: shrinks so every app fits  */
    int foot_y;
    int restart_x, shutdown_x, pill_y;
};

static struct menu_geom menu_geom(void)
{
    struct menu_geom g;
    int avail = g_screen.h - TASKBAR_H - 16;
    int fixed = MENU_HEAD_H + 16 + MENU_FOOT_H;

    g.nrows = g_menu_n;
    g.row_h = MENU_ROW_H;
    if (g.nrows > 0 && fixed + g.nrows * g.row_h > avail) {
        g.row_h = (avail - fixed) / g.nrows;       /* small screen: tighter rows */
        if (g.row_h < MENU_ROW_MIN)
            g.row_h = MENU_ROW_MIN;
        if (fixed + g.nrows * g.row_h > avail)
            g.nrows = (avail - fixed) / g.row_h;
    }
    if (g.nrows < 0)
        g.nrows = 0;
    g.w = MENU_W;
    g.h = fixed + g.nrows * g.row_h;
    g.x = 8;
    g.y = g_screen.h - TASKBAR_H - 8 - g.h;
    if (g.y < 0)
        g.y = 0;
    g.rows_y = g.y + MENU_HEAD_H + 8;
    g.foot_y = g.y + g.h - MENU_FOOT_H;
    g.shutdown_x = g.x + g.w - 16 - PILL_W;
    g.restart_x = g.shutdown_x - 8 - PILL_W;
    g.pill_y = g.foot_y + (MENU_FOOT_H - PILL_H) / 2;
    return g;
}

/* what the point is over: HV_ROW + i, HV_RESTART, HV_SHUTDOWN,
 * HV_PANEL (elsewhere on the menu), or 0 (outside) */
static int menu_hit(int x, int y)
{
    struct menu_geom g = menu_geom();
    int i;

    if (x < g.x || y < g.y || x >= g.x + g.w || y >= g.y + g.h)
        return 0;
    for (i = 0; i < g.nrows; i++) {
        int ry = g.rows_y + i * g.row_h;
        if (y >= ry && y < ry + g.row_h && x >= g.x + 8 && x < g.x + g.w - 8)
            return HV_ROW + i;
    }
    if (y >= g.pill_y && y < g.pill_y + PILL_H) {
        if (x >= g.restart_x && x < g.restart_x + PILL_W)
            return HV_RESTART;
        if (x >= g.shutdown_x && x < g.shutdown_x + PILL_W)
            return HV_SHUTDOWN;
    }
    return HV_PANEL;
}

static void draw_pill(struct raster *r, int x, int y, const char *label, int hot)
{
    int tw = (int)strlen(label) * 8;
    th_round_rect(r, x, y, PILL_W, PILL_H, PILL_H / 2, 0xffffff, hot ? 46 : 16);
    th_text(r, label, x + (PILL_W - tw) / 2, y + (PILL_H - 8) / 2, TH_TEXT_LIGHT);
}

static void draw_menu(void)
{
    struct raster *r = &g_screen;
    struct menu_geom g = menu_geom();
    char sub[48];
    int i;

    /* panel: soft shadow, then frosted glass (blurred backdrop, dark
     * tint) with a light rim */
    th_shadow(r, g.x, g.y, g.w, g.h, MENU_RAD, 28, 10, 150);
    th_frost(r, g.x, g.y, g.w, g.h, MENU_RAD, 14);
    th_round_rect(r, g.x, g.y, g.w, g.h, MENU_RAD, 0xffffff, 40);
    th_round_rect(r, g.x + 1, g.y + 1, g.w - 2, g.h - 2, MENU_RAD - 1,
                  TH_PANEL, 212);

    /* header: logo, name, version */
    th_logo(r, g.x + 40, g.y + MENU_HEAD_H / 2, 18);
    th_text2x(r, "OmniOS", g.x + 72, g.y + 22, TH_TEXT_LIGHT);
    if (g_version[0])
        snprintf(sub, sizeof(sub), "Version %s", g_version);
    else
        snprintf(sub, sizeof(sub), "Built from scratch");
    th_text(r, sub, g.x + 72, g.y + 46, TH_TEXT_DIM);
    th_fill_a(r, g.x + 16, g.y + MENU_HEAD_H, g.w - 32, 1, 0xffffff, 22);

    /* apps: icon, name, category */
    for (i = 0; i < g.nrows; i++) {
        const struct omni_app_info *a = g_menu_app[i];
        int ry = g.rows_y + i * g.row_h, mid = ry + g.row_h / 2;

        if (i == g_menu_sel)
            th_round_rect(r, g.x + 8, ry + 1, g.w - 16, g.row_h - 2, 6,
                          0xffffff, 30);
        th_icon(r, g.x + 18, mid - 12, 24, a ? a->color : TH_ACCENT,
                a ? a->glyph : g_menu[i].label[0]);
        th_text(r, g_menu[i].label, g.x + 54, mid - 4, TH_TEXT_LIGHT);
        if (a)
            th_text(r, a->category,
                    g.x + g.w - 20 - (int)strlen(a->category) * 8, mid - 4,
                    TH_TEXT_DIM);
    }

    /* footer: the user, Restart, Shut down */
    th_fill_a(r, g.x + 16, g.foot_y, g.w - 32, 1, 0xffffff, 22);
    th_icon(r, g.x + 16, g.foot_y + 15, 26, TH_ACCENT, 'u');
    th_text(r, g_user, g.x + 50, g.foot_y + 24, TH_TEXT_LIGHT);
    draw_pill(r, g.restart_x, g.pill_y, "Restart", g_hover == HV_RESTART);
    draw_pill(r, g.shutdown_x, g.pill_y, "Shut down", g_hover == HV_SHUTDOWN);
}

static void open_menu(void)
{
    menu_reload();
    g_menu_open = 1;
    g_menu_sel = -1;
}

static void close_menu(void)
{
    g_menu_open = 0;
    g_menu_sel = -1;
}

/* ------------------------------------------------------------------ */
/* first-run banner when an input device is missing                   */
/* ------------------------------------------------------------------ */

static int banner_on(void)
{
    return (!g_input_ok_ptr || !g_input_ok_kbd) && time(NULL) - g_shell_t0 > 8;
}

static void draw_banner(void)
{
    char msg[80];
    int tw, bw, bx, by = 14;

    if (!banner_on())
        return;
    snprintf(msg, sizeof(msg), "No %s found - see omnios-serial.log",
             !g_input_ok_ptr ? "mouse" : "keyboard");
    tw = (int)strlen(msg) * 8;
    bw = tw + 40;
    bx = (g_screen.w - bw) / 2;
    th_shadow(&g_screen, bx, by, bw, 32, 16, 12, 4, 90);
    th_round_rect(&g_screen, bx, by, bw, 32, 16, 0xfbbf24, 250);
    th_text(&g_screen, msg, bx + 20, by + 12, 0x3b2505);
}

/* ------------------------------------------------------------------ */
/* frame composition (omni_wm_paint hooks)                            */
/* ------------------------------------------------------------------ */

/* before any window: the cached wallpaper */
static void shell_draw_background(struct omni_wm *wm)
{
    memcpy(wm->screen.bits, g_wall,
           (size_t)wm->screen.w * (size_t)wm->screen.h * sizeof(uint32_t));
}

/* after the windows: always-on-top chrome, then present the frame */
static void shell_finish(struct omni_wm *wm)
{
    (void)wm;
    draw_taskbar();
    if (g_menu_open)
        draw_menu();
    draw_banner();
    present_scene();
}

/* ------------------------------------------------------------------ */
/* actions                                                            */
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

/* ask init (PID 1, ominit) to power off or reboot: it syncs first */
static void power(int restart)
{
    sync();
    kill(1, restart ? SIGQUIT : SIGTERM);
}

/* ------------------------------------------------------------------ */
/* input -> shell / server routing                                    */
/* ------------------------------------------------------------------ */

/* the chrome element under the pointer (HV_*), 0 if none */
static int hover_key(void)
{
    struct omni_win *w;

    if (g_menu_open) {
        int k = menu_hit(g_px, g_py);
        if (k)
            return k;
    }
    if (g_py >= g_screen.h - TASKBAR_H) {
        struct tb_btn b[OMNI_WM_MAX_WIN];
        int i, n;
        if (g_px < START_W)
            return HV_START;
        n = taskbar_layout(b);
        for (i = 0; i < n; i++)
            if (g_px >= b[i].x && g_px < b[i].x + b[i].bw)
                return HV_TASK + i;
        return 0;
    }
    w = omni_wm_at(&g_wm, g_px, g_py);
    if (w) {
        int z = omni_wm_hit(w, g_px, g_py);
        if (z == 2 || z == 10)
            return HV_CAPTION + w->id * 16 + z;
    }
    return 0;
}

/* returns 1 if the hovered element changed (the frame needs redrawing) */
static int update_hover(void)
{
    int k = hover_key();
    if (k == g_hover)
        return 0;
    g_hover = k;
    if (g_menu_open && k >= HV_ROW && k < HV_ROW + g_menu_n)
        g_menu_sel = k - HV_ROW;
    return 1;
}

/* a pointer press: Start menu, taskbar; returns 1 if the shell took it */
static int shell_button(int x, int y)
{
    if (g_menu_open) {
        int k = menu_hit(x, y);
        if (k >= HV_ROW && k < HV_ROW + g_menu_n) {
            launch(&g_menu[k - HV_ROW]);
            close_menu();
        } else if (k == HV_RESTART || k == HV_SHUTDOWN) {
            close_menu();
            power(k == HV_RESTART);
        } else if (k != HV_PANEL) {
            close_menu();       /* a click elsewhere (or on Start) closes */
        }
        return 1;
    }

    if (y >= g_screen.h - TASKBAR_H) {
        struct tb_btn b[OMNI_WM_MAX_WIN];
        int i, n;

        if (x < START_W) {
            open_menu();
            return 1;
        }
        /* window buttons: minimize the active window, else bring it up */
        n = taskbar_layout(b);
        for (i = 0; i < n; i++) {
            if (x >= b[i].x && x < b[i].x + b[i].bw) {
                struct omni_win *w = b[i].w;
                if (w == g_wm.active && !w->minimized)
                    omni_wm_minimize(&g_wm, w);
                else
                    omni_wm_raise(&g_wm, w);
                break;
            }
        }
        return 1;               /* the taskbar is on top of everything */
    }
    return 0;
}

/* a key: the Windows key toggles Start; the open menu has the keyboard.
 * Returns 1 if the shell took the key. */
static int shell_key(int code, int pressed)
{
    int rows;

    if (code == KEY_LEFTMETA || code == KEY_RIGHTMETA) {
        if (pressed == 1) {
            if (g_menu_open)
                close_menu();
            else
                open_menu();
        }
        return 1;
    }
    if (!g_menu_open)
        return 0;
    rows = menu_geom().nrows;
    if (pressed && rows > 0) {
        switch (code) {
        case OMNI_KEY_ESC:
            close_menu();
            break;
        case OMNI_KEY_UP:
            g_menu_sel = g_menu_sel <= 0 ? rows - 1 : g_menu_sel - 1;
            break;
        case OMNI_KEY_DOWN:
            g_menu_sel = (g_menu_sel + 1) % rows;
            break;
        case OMNI_KEY_ENTER:
        case OMNI_KEY_KPENTER:
            if (g_menu_sel >= 0 && g_menu_sel < rows)
                launch(&g_menu[g_menu_sel]);
            close_menu();
            break;
        default:
            break;
        }
    } else if (pressed && code == OMNI_KEY_ESC) {
        close_menu();
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* welcome window                                                     */
/* ------------------------------------------------------------------ */

static void welcome_paint(struct raster *s)
{
    static const struct {
        char glyph;
        uint32_t color;
        const char *text;
    } tips[] = {
        { 'S', 0x3b82f6, "Open apps from the Start menu or the Windows key." },
        { 'M', 0x8b5cf6, "Drag title bars to move windows, edges to resize." },
        { '-', 0x0ea5e9, "Minimize or close windows from the title bar." },
        { 'T', 0x10b981, "Click taskbar buttons to switch between windows." },
        { 'A', 0xf59e0b, "Find more apps in the App Store." },
    };
    int ch = s->h - OMNI_WM_TITLE_H;        /* visible content height */
    int x, i;
    char foot[64];

    /* header: blue-to-violet band with the logo */
    for (x = 0; x < s->w; x++)
        th_fill_a(s, x, 0, 1, 92,
                  th_blend(0x2563eb, 0x7c3aed,
                           (unsigned)(x * 255 / (s->w > 1 ? s->w - 1 : 1))),
                  255);
    th_round_rect(s, 24, 20, 52, 52, 26, 0xffffff, 255);
    th_logo(s, 50, 46, 20);
    th_text2x(s, "Welcome to OmniOS", 92, 28, 0xffffff);
    th_text(s, "A lightweight desktop OS, built from scratch.", 92, 56, 0xdbeafe);

    /* body: tips */
    th_fill_a(s, 0, 92, s->w, ch - 92, 0xffffff, 255);
    for (i = 0; i < (int)(sizeof(tips) / sizeof(tips[0])); i++) {
        int y = 110 + i * 32;
        th_letter_icon(s, 28, y, 22, tips[i].color, tips[i].glyph);
        th_text(s, tips[i].text, 64, y + 7, 0x1f2937);
    }

    /* footer */
    th_fill_a(s, 0, ch - 40, s->w, 40, 0xf5f6f8, 255);
    th_fill_a(s, 0, ch - 40, s->w, 1, 0xe5e7eb, 255);
    snprintf(foot, sizeof(foot), "OmniOS %s", g_version[0] ? g_version : "");
    th_text(s, foot, 28, ch - 24, 0x6b7280);
    th_text(s, "Enjoy!", s->w - 28 - 6 * 8, ch - 24, 0x6b7280);
}

void omni_shell_open_initial_windows(void)
{
    const int ww = 520, wh = 318 + OMNI_WM_TITLE_H;
    int x = (g_screen.w - ww) / 2, y = (g_screen.h - TASKBAR_H - wh) / 2;
    struct omni_win *w;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    w = omni_wm_add(&g_wm, "Welcome to OmniOS", x, y, ww, wh);
    if (w)
        welcome_paint(&w->surface);
}

/* ------------------------------------------------------------------ */
/* setup + main loop                                                  */
/* ------------------------------------------------------------------ */

/* wallpaper in the chosen style + the taskbar glass pre-rendered over it */
static void shell_backdrop(void)
{
    th_wallpaper_style(g_wall, g_dev.w, g_dev.h, g_set.wallpaper);
    g_set_wall = g_set.wallpaper;
    if (g_barbg && g_dev.h > TASKBAR_H) {
        struct raster bar;
        memcpy(g_barbg, g_wall + (size_t)(g_dev.h - TASKBAR_H) * (size_t)g_dev.w,
               (size_t)g_dev.w * TASKBAR_H * sizeof(uint32_t));
        raster_init(&bar, g_barbg, g_dev.w, TASKBAR_H, g_dev.w);
        taskbar_glass(&bar, 0);
    }
}

/* (re)load the settings file when it changed (or `force`) and apply it:
 * accent, time zone (inherited by apps launched from now on), clock
 * format, wallpaper. Returns 1 if anything was applied. */
static int shell_apply_settings(int force)
{
    struct stat st;
    int have = stat(omni_settings_path(), &st) == 0;

    if (!force && have && st.st_mtime == g_set_mtime && st.st_size == g_set_size)
        return 0;
    if (!force && !have && g_set_size == -1)
        return 0;
    g_set_mtime = have ? st.st_mtime : 0;
    g_set_size = have ? st.st_size : -1;
    omni_settings_load(&g_set);
    th_accent = omni_accents[g_set.accent % omni_accents_n].rgb;
    omni_settings_apply_tz(&g_set);
    if (g_wall && g_set.wallpaper != g_set_wall)
        shell_backdrop();
    return 1;
}

/* the account the desktop runs as, for the Start menu (full name if set) */
static void read_user(void)
{
    snprintf(g_user, sizeof(g_user), "%s", omni_account_display());
}

/* Frame buffers, wallpaper, cursor and window manager for a display. */
static int shell_display_init(struct raster dev)
{
    size_t n = (size_t)dev.w * (size_t)dev.h;
    uint32_t *scene = malloc(n * sizeof(uint32_t));

    g_dev = dev;
    g_front = malloc(n * sizeof(uint32_t));
    g_wall = malloc(n * sizeof(uint32_t));
    g_rowbuf = malloc((size_t)dev.w * sizeof(uint32_t));
    if (!scene || !g_front || !g_wall || !g_rowbuf) {
        free(scene);
        free(g_front);
        free(g_wall);
        free(g_rowbuf);
        g_front = g_wall = g_rowbuf = NULL;
        return -1;
    }
    raster_init(&g_screen, scene, dev.w, dev.h, dev.w);
    g_barbg = malloc((size_t)dev.w * TASKBAR_H * sizeof(uint32_t));
    g_set_wall = -1;
    shell_apply_settings(1);                   /* renders the backdrop */
    cursor_build();
    read_version();
    read_user();

    omni_wm_init(&g_wm, &g_screen);
    g_wm.draw_background = shell_draw_background;
    g_wm.finish = shell_finish;
    g_wm.icon_for = shell_icon_for;
    g_full_present = 1;
    g_cur_on = 0;
    return 0;
}

void omni_shell_run(void)
{
    struct osfb fb;
    struct omni_devs devs;
    /* wm listener + ev* + mice + tty, then one slot per app connection */
    struct pollfd pfds[1 + 8 + 1 + 1 + OMNI_WM_MAX_CLI];
    int pfd_ci[OMNI_WM_MAX_CLI];         /* client -> pfds index, -1 none */
    int npoll;
    int nfds_dev, nfds;
    int clock_last = -1, banner_last = 0;
    time_t settings_checked = 0;
    time_t last_rescan = 0;

    if (osfb_wait("/dev/fb0", 50, 200) < 0)
        omni_console_puts("desktop: no framebuffer\n");
    if (osfb_open(&fb, "/dev/fb0") < 0) {
        omni_console_puts("desktop: cannot open /dev/fb0\n");
        return;
    }
    omni_console_puts("desktop: framebuffer ready\n");

    if (shell_display_init(fb_raster(&fb)) != 0) {
        omni_console_puts("desktop: out of memory for the frame buffers\n");
        osfb_close(&fb);
        return;
    }

    if (omni_wm_start(&g_wm) != 0)
        omni_console_puts("desktop: WARN display socket not started\n");

    omni_devs_open(&devs);
    nfds_dev = omni_devs_nfds(&devs);
    omni_devs_fill(&devs, &pfds[1]);

    /* report which input sources were found, with device names (kernel
     * log -> serial log: the fastest way to see why the mouse/keyboard
     * are or are not alive) */
    omni_devs_log(&devs);
    g_input_ok_ptr = (devs.ev_has_rel || devs.ev_has_abs ||
                      devs.mice_fd >= 0) ? 1 : 0;
    g_input_ok_kbd = (devs.ev_has_kbd || devs.tty_fd >= 0) ? 1 : 0;
    g_shell_t0 = time(NULL);

    /* index 0 = display socket listener */
    pfds[0].fd = omni_wm_fd(&g_wm);
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    nfds = 1 + nfds_dev;

    omni_shell_open_initial_windows();
    g_px = g_wm.pointer_x = g_screen.w / 2;
    g_py = g_wm.pointer_y = g_screen.h / 2;

    /* first frame (full present), then the cursor on top */
    omni_wm_paint(&g_wm);
    cursor_move(g_px, g_py);

    for (;;) {
        time_t t = time(NULL);
        struct tm tmv;
        struct omni_input e;
        int need = 0, moved = 0, ci;

        /* a pointer or keyboard may appear only after the boot-time
         * scan (late USB enumeration, hypervisor quirks): keep
         * rescanning every 2 s until both sources exist, then stop. */
        if ((devs.ev_has_rel == 0 && devs.ev_has_abs == 0 && devs.mice_fd < 0) ||
            (devs.ev_has_kbd == 0 && devs.tty_fd < 0)) {
            if (t - last_rescan >= 2) {
                last_rescan = t;
                omni_devs_rescan(&devs);
                omni_devs_fill(&devs, &pfds[1]);
                nfds_dev = omni_devs_nfds(&devs);
                nfds = 1 + nfds_dev;
                g_input_ok_ptr = (devs.ev_has_rel || devs.ev_has_abs ||
                                  devs.mice_fd >= 0) ? 1 : 0;
                g_input_ok_kbd = (devs.ev_has_kbd || devs.tty_fd >= 0) ? 1 : 0;
                if (g_input_ok_ptr && g_input_ok_kbd)
                    omni_devs_log(&devs);      /* late device: say so */
            }
        }

        /* app connections are polled too, so an app's drawing wakes the
         * loop at once instead of waiting for the next tick */
        npoll = nfds;
        for (ci = 0; ci < OMNI_WM_MAX_CLI; ci++) {
            pfd_ci[ci] = -1;
            if (g_wm.clients[ci].fd >= 0) {
                pfds[npoll].fd = g_wm.clients[ci].fd;
                pfds[npoll].events = POLLIN;
                pfds[npoll].revents = 0;
                pfd_ci[ci] = npoll++;
            }
        }

        /* input devices and app sockets wake the loop at once; the timeout
         * only paces the once-a-second checks (taskbar clock, input-device
         * rescan, input banner), so an idle desktop wakes once a second */
        if (poll(pfds, (nfds_t)npoll, 1000) < 0)
            continue;

        /* new client connections */
        if (pfds[0].revents & POLLIN)
            omni_wm_accept(&g_wm);

        /* dispatch client protocol (non-blocking; drains each socket) */
        for (ci = 0; ci < OMNI_WM_MAX_CLI; ci++) {
            int k = pfd_ci[ci];
            if (k >= 0 && g_wm.clients[ci].fd == pfds[k].fd &&
                (pfds[k].revents & (POLLIN | POLLHUP | POLLERR)))
                omni_wm_handle_client(&g_wm, ci);
        }

        /* reap apps whose windows were closed, so they do not linger as
         * zombies until the next launch */
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;

        /* input devices */
        {
            int idx;
            for (idx = 0; idx < nfds_dev; idx++) {
                if (pfds[1 + idx].revents & POLLIN)
                    omni_devs_drain(&devs, idx);
            }
        }

        while (omni_input_next(&e) == 0) {
            if (e.type == 2 || e.type == 4) {        /* pointer motion */
                if (e.type == 2)
                    omni_shell_warp(e.dx, e.dy);
                else
                    omni_shell_moveto(e.dx, e.dy);
                omni_wm_motion(&g_wm, g_px, g_py);  /* drags mark dirty */
                moved = 1;
            } else if (e.type == 3) {                 /* button */
                if (e.pressed && shell_button(g_px, g_py))
                    need = 1;
                else
                    omni_wm_button(&g_wm, g_px, g_py, e.key, e.pressed);
            } else if (e.type == 1) {                 /* key */
                if (shell_key(e.key, e.pressed))
                    need = 1;
                else if (e.pressed && g_wm.active)
                    omni_wm_key(&g_wm, e.key, e.pressed, e.text);
            }
        }

        if (update_hover())
            need = 1;
        if (t != settings_checked) {             /* Settings app saved? */
            settings_checked = t;
            if (shell_apply_settings(0))
                need = 1;
        }
        localtime_r(&t, &tmv);
        if (tmv.tm_min != clock_last) {
            clock_last = tmv.tm_min;
            need = 1;
        }
        if (banner_on() != banner_last) {
            banner_last = banner_on();
            need = 1;
        }

        /* recompose only when something visible changed; plain mouse
         * motion just moves the cursor sprite */
        if (need || g_wm.dirty)
            omni_wm_paint(&g_wm);
        if (moved)
            cursor_move(g_px, g_py);
    }

    omni_devs_close(&devs);
    osfb_close(&fb);
}
