/*
 * OmniOS — os/gui/shell.c
 *
 * The graphical desktop shell and display server. It owns the framebuffer,
 * runs the window manager (os/gui/wm.c) on a UNIX socket at
 * /tmp/.omnios-wm, accepts application clients, routes keyboard/mouse
 * input to them, and draws a Windows 11-style desktop: wallpaper and
 * desktop icons, windows, the centred taskbar (Start, Search, pinned and
 * open apps, the tray: OmniOS Update, network, clock), the Start menu
 * (search, pinned apps, Recommended, account and power), Quick Settings,
 * the calendar, popup menus, update notifications, and the lock and
 * sign-in screens.
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
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include "catalog.h"
#include "shell.h"
#include "theme.h"
#include "settings.h"
#include "account.h"
#include "updstat.h"
#include "netinfo.h"
#include "wm.h"

/* ------------------------------------------------------------------ */
/* layout                                                             */
/* ------------------------------------------------------------------ */

#define TASKBAR_H     OMNI_TASKBAR_H
#define TB_ITEM_W     44        /* taskbar icon button                    */
#define TB_ITEM_MIN   32        /* ... squeezed when many windows are open */
#define TB_GAP        4
#define TB_TRAY_W     112       /* clock (opens the calendar)             */
#define TB_NET_W      40        /* network (opens Quick Settings)         */
#define TB_UPD_W      36        /* OmniOS Update, while there is news     */
#define TB_PINS       4
#define TB_MAX        (2 + TB_PINS + OMNI_WM_MAX_WIN)
#define FLY_RAD       10        /* flyout corner radius                   */
#define FLY_GAP       12        /* flyouts float this far above the bar   */

/* Start menu, Windows 11 style: search, pinned apps, account + power */
#define SM_COLS       6
#define SM_TILE_W     88
#define SM_TILE_H     84
#define SM_PAD        24
#define SM_SEARCH_H   36
#define SM_FOOT_H     64
#define SM_ROW_H      40        /* one search result                      */
#define SM_REC_H      52        /* one "Recommended" entry                */
#define SM_REC_N      4         /* ... two rows of two                    */

/* Quick Settings and the calendar */
#define QS_W          360
#define QS_TILE_W     100
#define QS_TILE_H     48
#define QS_TILES      6
#define CAL_W         328
#define CAL_CELL_W    44
#define CAL_CELL_H    36

/* desktop icons */
#define DESK_W        84
#define DESK_H        88
#define DESK_ICON     44
#define DCLICK_MS     500       /* double-click interval                  */

/* popup menus (power, account, right-click) */
#define POP_ROW       36
#define POP_MAX       7

/* hover targets (hover_key) */
#define HV_TB         10        /* + taskbar item                         */
#define HV_PANEL      50        /* on a flyout, nothing in particular     */
#define HV_ITEM       100       /* + Start tile / search result           */
#define HV_USER       202       /* the account in the Start footer        */
#define HV_TRAY_UPD   203       /* OmniOS Update icon                     */
#define HV_TRAY_NET   204       /* network icon: Quick Settings           */
#define HV_TRAY_CLOCK 205       /* clock: calendar                        */
#define HV_SM_POWER   206       /* Start's power button                   */
#define HV_SM_SEARCH  207       /* Start's search box                     */
#define HV_REC        230       /* + Start "Recommended" entry            */
#define HV_TOAST      210       /* the update notification ...            */
#define HV_TOAST_GO   211       /* ... Restart now / Download             */
#define HV_TOAST_LATER 212      /* ... Later                              */
#define HV_TOAST_X    213       /* ... close                              */
#define HV_QS_TILE    300       /* + Quick Settings tile                  */
#define HV_QS_LOCK    310
#define HV_QS_GEAR    311
#define HV_QS_POWER   312
#define HV_CAL_PREV   320
#define HV_CAL_NEXT   321
#define HV_POP        339       /* the popup menu ...                     */
#define HV_POP_ITEM   340       /* ... + item                             */
#define HV_DESK       360       /* + desktop icon                         */
#define HV_SIGN_POWER 380       /* sign-in screen: power                  */
#define HV_CAPTION    1000      /* + window id * 16 + chrome zone         */

#define KEY_LEFTMETA  125       /* the Windows keys                       */
#define KEY_RIGHTMETA 126
#define KEY_L         38        /* Windows + L: lock                      */
#define KEY_A         30        /*         + A: Quick Settings            */
#define KEY_N         49        /*         + N: calendar                  */
#define KEY_I         23        /*         + I: Settings                  */
#define KEY_E         18        /*         + E: File Manager              */
#define KEY_D         32        /*         + D: show the desktop          */
#define KEY_S         31        /*         + S: search                    */
#define KEY_X         45        /*         + X: quick links menu          */

#define N_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

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
/* lock / sign-in screen */
static int              g_locked;            /* the lock screen is up       */
static int              g_lock_stage;        /* 0 = clock, 1 = sign in      */
static char             g_pw[64];            /* password being typed        */
static int              g_pw_bad;            /* last attempt was wrong      */
static uint32_t        *g_lockbg, *g_signbg; /* dimmed / frosted wallpaper  */
static int              g_lockbg_wall = -1, g_signbg_wall = -1;
static int              g_meta_held, g_meta_used;    /* Windows key chords  */
static int g_full_present;          /* next present rewrites every pixel   */
static int g_px, g_py;              /* pointer (mouse) position            */
static int g_menu_open;             /* Start menu visible                  */
static int g_menu_sel = -1;         /* highlighted Start tile / result     */
static int g_hover;                 /* hovered chrome element (HV_*)       */
static int g_input_ok_ptr;          /* a pointer source was found          */
static int g_input_ok_kbd;          /* a keyboard source was found         */
static time_t g_shell_t0;           /* desktop start (for the banner)      */
static char g_version[32];          /* from /etc/omnios-release            */
static struct omni_netinfo g_net;   /* network status, every 2 s           */
static int  g_qs_open;              /* Quick Settings visible              */
static int  g_cal_open;             /* calendar visible ...                */
static int  g_cal_month;            /* ... this many months from now       */
static char g_query[40];            /* typed into Start's search box       */
static int  g_sm_scroll;            /* first pinned row shown              */
static int  g_desk_sel = -1;        /* selected desktop icon               */
static int  g_desk_last = -1;       /* last clicked one, for double-clicks */
static long long g_desk_last_ms;
static unsigned g_showdesk;         /* Windows + D minimized these windows */

/* taskbar pins, left to right (all system apps: always installed) */
static const char *const g_pin_ids[TB_PINS] = { "files", "terminal", "store", "settings" };
static unsigned char g_pin_ok[TB_PINS];      /* in this image           */

/* Start menu: the installed apps from the catalog (catalog.c), rebuilt
 * each time the menu opens so the App Store's changes show straight
 * away; and what the search box finds. */
static const struct omni_app_info *g_menu_app[OMNI_CATALOG_MAX];
static int g_menu_n;
static unsigned char g_installed[OMNI_CATALOG_MAX];

enum { RES_APP, RES_SETTING, RES_STORE };
struct sm_result {
    int kind;
    const char *label, *sub;
    const char *path, *arg;     /* what opens it                         */
    char icon;
    uint32_t color;
};
#define SM_RES_MAX (OMNI_CATALOG_MAX + 16)
static struct sm_result g_res[SM_RES_MAX];
static int g_res_n;

/* Settings pages the search box finds, and the words that find them */
static const struct {
    const char *label, *keys, *page;
    char icon;
} g_setlinks[] = {
    { "Check for updates",  "update upgrade version pause automatic new", "update", 'U' },
    { "Network & internet", "network internet ethernet ip address dns online", "network", 'w' },
    { "Background",         "wallpaper background desktop picture", "personalization", 'p' },
    { "Colors",             "accent color colour theme", "personalization", 'p' },
    { "Taskbar",            "taskbar alignment left center centre desktop icons", "personalization", 'p' },
    { "Password",           "password account sign-in login user", "accounts", 'u' },
    { "Lock screen",        "lock screen sign-in login", "accounts", 'k' },
    { "Date & time",        "date time clock zone 24-hour", "time", 't' },
    { "Display",            "display screen resolution system memory processor about", "system", 'm' },
    { "Installed apps",     "apps installed programs uninstall remove", "apps", 'a' },
};

static void menu_reload(void)
{
    int i;

    omni_apps_load(g_installed);
    g_menu_n = 0;
    for (i = 0; i < omni_catalog_n && g_menu_n < OMNI_CATALOG_MAX; i++) {
        if (!g_installed[i] || access(omni_catalog[i].path, X_OK) != 0)
            continue;               /* not installed, or not in this image */
        g_menu_app[g_menu_n++] = &omni_catalog[i];
    }
}

/* case-insensitive "needle occurs in hay" */
static int contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, n) == 0)
            return 1;
    return n == 0;
}

static void add_result(int kind, const char *label, const char *sub,
                       const char *path, const char *arg, char icon, uint32_t color)
{
    struct sm_result *r;
    if (g_res_n >= SM_RES_MAX)
        return;
    r = &g_res[g_res_n++];
    r->kind = kind;
    r->label = label;
    r->sub = sub;
    r->path = path;
    r->arg = arg;
    r->icon = icon;
    r->color = color;
}

/* the pinned grid (nothing typed) or the search results: installed apps,
 * then Settings pages, then apps the App Store has */
static void sm_refresh(void)
{
    int i;

    g_res_n = 0;
    for (i = 0; i < g_menu_n; i++) {
        const struct omni_app_info *a = g_menu_app[i];
        if (!g_query[0] || contains(a->name, g_query) || contains(a->category, g_query))
            add_result(RES_APP, a->name, "App", a->path, NULL, a->glyph, a->color);
    }
    if (!g_query[0])
        return;
    for (i = 0; i < N_OF(g_setlinks); i++)
        if (contains(g_setlinks[i].label, g_query) || contains(g_setlinks[i].keys, g_query))
            add_result(RES_SETTING, g_setlinks[i].label, "Settings",
                       "/usr/bin/omnios-settings", g_setlinks[i].page, g_setlinks[i].icon, 0);
    for (i = 0; i < omni_catalog_n; i++) {
        const struct omni_app_info *a = &omni_catalog[i];
        if (!g_installed[i] && (contains(a->name, g_query) || contains(a->category, g_query)))
            add_result(RES_STORE, a->name, "App Store", "/usr/bin/omnios-store", NULL,
                       a->glyph, a->color);
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

/* "OmniOS 2026.2.2 (from-source lightweight OS)" in /etc/omnios-release */
static void read_version(void)
{
    snprintf(g_version, sizeof(g_version), "%s", omni_update_running_version());
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
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* start an app (arg may be NULL) */
static void run_app(const char *path, const char *arg)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (arg)
            execl(path, path, arg, (char *)NULL);
        else
            execl(path, path, (char *)NULL);
        _exit(127);
    }
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

/* Settings, on a page ("update", "network", ...; NULL = the first) */
static void open_settings(const char *page)
{
    run_app("/usr/bin/omnios-settings", page);
}

/* ask init (PID 1, ominit) to power off or reboot: it syncs first */
static void power(int restart)
{
    sync();
    kill(1, restart ? SIGQUIT : SIGTERM);
}

static int in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

/* text centred on cx, cut to max_w with ".." */
static void text_mid(struct raster *r, const char *s, int cx, int y, uint32_t rgb,
                     int max_w)
{
    int w = (int)strlen(s) * 8;
    if (w > max_w)
        w = max_w / 8 * 8;
    th_text_clip(r, s, cx - w / 2, y, rgb, max_w);
}

/* a small X, for "Close" */
static void draw_x(struct raster *r, int cx, int cy, int s, uint32_t rgb)
{
    int k;
    for (k = -s; k <= s; k++) {
        th_px(r, cx + k, cy + k, rgb, 230);
        th_px(r, cx + k, cy - k, rgb, 230);
        th_px(r, cx + k + 1, cy + k, rgb, 90);
        th_px(r, cx + k + 1, cy - k, rgb, 90);
    }
}

/* flyout: frosted, tinted glass with a soft shadow and a light rim */
static void fly_panel(struct raster *r, int x, int y, int w, int h)
{
    th_shadow(r, x, y, w, h, FLY_RAD, 28, 10, 150);
    th_frost(r, x, y, w, h, FLY_RAD, 14);
    th_round_rect(r, x, y, w, h, FLY_RAD, 0xffffff, 40);
    th_round_rect(r, x + 1, y + 1, w - 2, h - 2, FLY_RAD - 1, TH_PANEL, 214);
}

/* a flyout's darker footer band, rows y..bottom-1, inside the rim and
 * following the rounded bottom corners */
static void fly_footer(struct raster *r, int x, int y, int w, int bottom)
{
    int rad = FLY_RAD - 1, j, i, h;

    x += 1;
    w -= 2;
    h = bottom - 1 - y;
    for (j = 0; j < h; j++) {
        int yy = y + j, up = h - 1 - j;          /* rows above the bottom */
        if (up >= rad) {
            th_fill_a(r, x, yy, w, 1, 0x000000, 52);
            continue;
        }
        th_fill_a(r, x + rad, yy, w - 2 * rad, 1, 0x000000, 52);
        for (i = 0; i < rad; i++) {
            unsigned cov = th_corner_cov(i, up, rad);
            if (cov) {
                th_px(r, x + i, yy, 0x000000, 52 * cov / 255);
                th_px(r, x + w - 1 - i, yy, 0x000000, 52 * cov / 255);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* popup menus: power, account, right-click                           */
/* ------------------------------------------------------------------ */

enum { ACT_LOCK = 1, ACT_RESTART, ACT_SHUTDOWN, ACT_ACCOUNT, ACT_PERSONALIZE,
       ACT_DISPLAY, ACT_SETTINGS, ACT_TERMINAL, ACT_FILES, ACT_TASKMGR,
       ACT_DESKICONS, ACT_LAUNCH, ACT_OPEN, ACT_CLOSE, ACT_CLOSE_ALL, ACT_DESKTOP };
enum { POP_POWER_START = 1, POP_ACCOUNT, POP_POWER_QS, POP_POWER_SIGNIN,
       POP_DESKTOP, POP_TASKBAR, POP_QUICKLINK };
enum { POP_ABOVE_LEFT, POP_ABOVE_RIGHT, POP_ABOVE_CENTER, POP_AT };

struct pop_item {
    const char *label;
    char icon;                          /* pictogram, 'x' = a cross      */
    int act;                            /* ACT_*                         */
    int badge;                          /* amber dot: an update waits    */
};

static struct {
    int open, owner;                    /* POP_*                         */
    int x, y, w, h, n, sel;
    struct pop_item it[POP_MAX];
    const struct omni_app_info *app;    /* ACT_LAUNCH, ACT_CLOSE_ALL     */
    const char *path;                   /* ACT_OPEN                      */
    int win_id;                         /* ACT_CLOSE                     */
} g_pop;

static void shell_lock(void);
static int  shell_apply_settings(int force);
static void show_desktop(void);

static void pop_begin(int owner)
{
    memset(&g_pop, 0, sizeof(g_pop));
    g_pop.owner = owner;
    g_pop.sel = -1;
}

static void pop_add(const char *label, char icon, int act, int badge)
{
    if (g_pop.n < POP_MAX) {
        struct pop_item *p = &g_pop.it[g_pop.n++];
        p->label = label;
        p->icon = icon;
        p->act = act;
        p->badge = badge;
    }
}

/* open it by an anchor: above a button (x = its left, right edge or
 * centre, y = its top) or at the pointer */
static void pop_place(int ax, int ay, int mode)
{
    int i, lw = 0;

    for (i = 0; i < g_pop.n; i++)
        if ((int)strlen(g_pop.it[i].label) > lw)
            lw = (int)strlen(g_pop.it[i].label);
    g_pop.w = lw * 8 + 76;
    if (g_pop.w < 176)
        g_pop.w = 176;
    g_pop.h = g_pop.n * POP_ROW + 8;
    switch (mode) {
    case POP_ABOVE_RIGHT:  g_pop.x = ax - g_pop.w;     g_pop.y = ay - g_pop.h - 6; break;
    case POP_ABOVE_CENTER: g_pop.x = ax - g_pop.w / 2; g_pop.y = ay - g_pop.h - 6; break;
    case POP_ABOVE_LEFT:   g_pop.x = ax;               g_pop.y = ay - g_pop.h - 6; break;
    default:
        g_pop.x = ax;
        g_pop.y = ay;
        if (g_pop.y + g_pop.h > g_screen.h - TASKBAR_H - 4)
            g_pop.y = ay - g_pop.h;
        break;
    }
    if (g_pop.x + g_pop.w > g_screen.w - 4)
        g_pop.x = g_screen.w - 4 - g_pop.w;
    if (g_pop.x < 4)
        g_pop.x = 4;
    if (g_pop.y < 4)
        g_pop.y = 4;
    g_pop.open = g_pop.n > 0;
}

/* HV_POP_ITEM + i, HV_POP (elsewhere on it) or 0 */
static int pop_hit(int x, int y)
{
    int i;
    if (!g_pop.open || !in_rect(x, y, g_pop.x, g_pop.y, g_pop.w, g_pop.h))
        return 0;
    i = (y - g_pop.y - 4) / POP_ROW;
    if (y >= g_pop.y + 4 && i >= 0 && i < g_pop.n)
        return HV_POP_ITEM + i;
    return HV_POP;
}

static void draw_pop(void)
{
    struct raster *r = &g_screen;
    int i;

    if (!g_pop.open)
        return;
    th_shadow(r, g_pop.x, g_pop.y, g_pop.w, g_pop.h, 8, 18, 6, 140);
    th_frost(r, g_pop.x, g_pop.y, g_pop.w, g_pop.h, 8, 12);
    th_round_rect(r, g_pop.x, g_pop.y, g_pop.w, g_pop.h, 8, 0xffffff, 44);
    th_round_rect(r, g_pop.x + 1, g_pop.y + 1, g_pop.w - 2, g_pop.h - 2, 7, 0x1b2030, 236);
    for (i = 0; i < g_pop.n; i++) {
        const struct pop_item *p = &g_pop.it[i];
        int iy = g_pop.y + 4 + i * POP_ROW;
        if (g_pop.sel == i)
            th_round_rect(r, g_pop.x + 4, iy + 1, g_pop.w - 8, POP_ROW - 2, 5, 0xffffff, 30);
        if (p->icon == 'x')
            draw_x(r, g_pop.x + 20, iy + POP_ROW / 2, 5, TH_TEXT_LIGHT);
        else if (p->icon)
            th_icon(r, g_pop.x + 12, iy + (POP_ROW - 16) / 2, 16, TH_ACCENT, p->icon);
        th_text(r, p->label, g_pop.x + 40, iy + (POP_ROW - 8) / 2, TH_TEXT_LIGHT);
        if (p->badge)
            th_round_rect(r, g_pop.x + g_pop.w - 22, iy + POP_ROW / 2 - 4, 8, 8, 4,
                          0xf59e0b, 255);
    }
}

/* ------------------------------------------------------------------ */
/* OmniOS Update: tray icon, notification, restart hints              */
/* ------------------------------------------------------------------ */

#define TOAST_W      372
#define TOAST_H      124
#define TOAST_BTN_W  112
#define TOAST_BTN_H  28
#define TOAST_SECS   12            /* the notification hides after this   */
#define UPD_BADGE    0xf59e0b      /* amber dot: an update waits          */

static struct omni_update_status g_upd;     /* omnios-update's last status */
static struct timespec g_upd_mtime;
static off_t  g_upd_size = -1;
static char   g_toast_seen[48];             /* "<state>:<version>" shown   */
static int    g_toast_on, g_toast_pending, g_toast_kind;
static time_t g_toast_t;

/* an update is available or downloaded (and isn't the one running: after
 * restarting into it, the status says "ready" until the updater notices) */
static int upd_tray_on(void)
{
    return (g_upd.state == UPD_READY || g_upd.state == UPD_AVAILABLE) &&
           g_upd.latest[0] && strcmp(g_upd.latest, g_upd.current) != 0;
}

static int upd_ready(void)
{
    return upd_tray_on() && g_upd.state == UPD_READY;
}

/* one line for Quick Settings */
static void upd_summary(char *s, size_t n)
{
    const char *ver = g_version[0] ? g_version : g_upd.current;
    if (upd_ready()) {
        snprintf(s, n, "Restart to install OmniOS %s", g_upd.latest);
    } else if (omni_updates_paused(&g_set)) {
        time_t until = (time_t)g_set.pause_until;
        struct tm tmv;
        char mon[16];
        localtime_r(&until, &tmv);
        strftime(mon, sizeof(mon), "%b", &tmv);
        snprintf(s, n, "Updates paused until %s %d", mon, tmv.tm_mday);
    } else if (g_upd.state == UPD_AVAILABLE && upd_tray_on()) {
        snprintf(s, n, "OmniOS %s is available", g_upd.latest);
    } else if (g_upd.state == UPD_DOWNLOADING) {
        snprintf(s, n, "Downloading OmniOS %s: %d%%", g_upd.latest, g_upd.progress);
    } else if (g_upd.state == UPD_CHECKING) {
        snprintf(s, n, "Checking for updates...");
    } else if (g_upd.state == UPD_UPTODATE) {
        snprintf(s, n, "OmniOS %s: you're up to date", ver);
    } else {
        snprintf(s, n, "OmniOS %s", ver);
    }
}

static void toast_geom(int *x, int *y, int *bx, int *lx, int *by)
{
    *x = g_screen.w - TOAST_W - 12;
    *y = g_screen.h - TASKBAR_H - TOAST_H - 12;
    *lx = *x + TOAST_W - 16 - TOAST_BTN_W;       /* Later                */
    *bx = *lx - 8 - TOAST_BTN_W;                  /* Restart now/Download */
    *by = *y + TOAST_H - 14 - TOAST_BTN_H;
}

/* HV_TOAST_* under the point, 0 if not on the notification */
static int toast_hit(int px, int py)
{
    int x, y, bx, lx, by;
    if (!g_toast_on || g_locked)
        return 0;
    toast_geom(&x, &y, &bx, &lx, &by);
    if (px < x || py < y || px >= x + TOAST_W || py >= y + TOAST_H)
        return 0;
    if (py >= by && py < by + TOAST_BTN_H) {
        if (px >= bx && px < bx + TOAST_BTN_W)
            return HV_TOAST_GO;
        if (px >= lx && px < lx + TOAST_BTN_W)
            return HV_TOAST_LATER;
    }
    if (px >= x + TOAST_W - 38 && py < y + 34)
        return HV_TOAST_X;
    return HV_TOAST;
}

static void draw_toast(void)
{
    struct raster *r = &g_screen;
    int x, y, bx, lx, by, ready = g_toast_kind == UPD_READY;
    const char *title = ready ? "Restart to finish updating" : "An update is available";
    const char *go = ready ? "Restart now" : "Download";
    char body[96];

    if (!g_toast_on || g_locked)
        return;
    toast_geom(&x, &y, &bx, &lx, &by);
    th_shadow(r, x, y, TOAST_W, TOAST_H, 10, 24, 8, 140);
    th_frost(r, x, y, TOAST_W, TOAST_H, 10, 14);
    th_round_rect(r, x, y, TOAST_W, TOAST_H, 10, 0xffffff, 40);
    th_round_rect(r, x + 1, y + 1, TOAST_W - 2, TOAST_H - 2, 9, TH_PANEL, 224);

    th_icon(r, x + 16, y + 13, 18, TH_ACCENT, 'U');
    th_text(r, "OmniOS Update", x + 42, y + 18, TH_TEXT_DIM);
    if (g_hover == HV_TOAST_X)                              /* close: X */
        th_round_rect(r, x + TOAST_W - 36, y + 9, 26, 24, 5, 0xffffff, 32);
    draw_x(r, x + TOAST_W - 23, y + 21, 4, TH_TEXT_LIGHT);

    th_text_bold(r, title, x + 16, y + 44, TH_TEXT_LIGHT);
    snprintf(body, sizeof(body), ready ? "OmniOS %s is ready to install."
                                       : "OmniOS %s can be downloaded now.",
             g_upd.latest);
    th_text(r, body, x + 16, y + 62, TH_TEXT_DIM);

    th_round_rect(r, bx, by, TOAST_BTN_W, TOAST_BTN_H, 6,
                  g_hover == HV_TOAST_GO ? th_shade(TH_ACCENT, 24) : TH_ACCENT, 255);
    th_text(r, go, bx + (TOAST_BTN_W - (int)strlen(go) * 8) / 2, by + 10, 0xffffff);
    th_round_rect(r, lx, by, TOAST_BTN_W, TOAST_BTN_H, 6, 0xffffff,
                  g_hover == HV_TOAST_LATER ? 46 : 20);
    th_text(r, "Later", lx + (TOAST_BTN_W - 40) / 2, by + 10, TH_TEXT_LIGHT);
}

/* once a second: has omnios-update written a new status? 1 = redraw */
static int shell_poll_update(void)
{
    char p[512], seen[48], old_latest[32];
    struct stat st;
    int have, old_on = upd_tray_on(), old_state = g_upd.state;

    snprintf(p, sizeof(p), "%s/status", omni_update_dir());
    have = stat(p, &st) == 0;
    if (have ? (st.st_mtim.tv_sec == g_upd_mtime.tv_sec &&
                st.st_mtim.tv_nsec == g_upd_mtime.tv_nsec && st.st_size == g_upd_size)
             : g_upd_size == -1)
        return 0;
    if (have)
        g_upd_mtime = st.st_mtim;
    g_upd_size = have ? st.st_size : -1;
    snprintf(old_latest, sizeof(old_latest), "%s", g_upd.latest);
    omni_update_read(&g_upd);
    if (upd_tray_on()) {
        snprintf(seen, sizeof(seen), "%d:%s", g_upd.state, g_upd.latest);
        if (strcmp(seen, g_toast_seen) != 0) {      /* news: tell once */
            snprintf(g_toast_seen, sizeof(g_toast_seen), "%s", seen);
            g_toast_kind = g_upd.state;
            g_toast_pending = 1;
        }
    } else {
        g_toast_on = g_toast_pending = 0;   /* installed, failed, withdrawn */
    }
    return old_on != upd_tray_on() || old_state != g_upd.state ||
           strcmp(old_latest, g_upd.latest) != 0 || g_upd.state == UPD_DOWNLOADING;
}

/* ------------------------------------------------------------------ */
/* taskbar: Start, Search, pinned apps and open windows (centred like   */
/* Windows 11, or on the left); the tray: update, network, clock        */
/* ------------------------------------------------------------------ */

enum { TBI_START, TBI_SEARCH, TBI_APP, TBI_WIN };

struct tb_item {
    int kind;                           /* TBI_*                          */
    const struct omni_app_info *app;    /* TBI_APP                        */
    struct omni_win *w;                 /* a window of it (TBI_WIN: its)  */
    int nwin;                           /* windows open                   */
    int active;                         /* holds the focused window       */
    int x;
};

static int g_tb_w = TB_ITEM_W;          /* item width in the last layout  */

static int tray_clock_x(void) { return g_screen.w - TB_TRAY_W; }
static int tray_net_x(void)   { return tray_clock_x() - TB_NET_W; }
static int tray_upd_x(void)   { return tray_net_x() - TB_UPD_W; }
static int tray_left(void)    { return upd_tray_on() ? tray_upd_x() : tray_net_x(); }

/* the items, in order: Start, Search, the pins, then other open apps;
 * an app's windows share one icon */
static int tb_layout(struct tb_item *it)
{
    int n = 0, i, k, total, x0, room;

    memset(it, 0, sizeof(*it) * TB_MAX);
    it[n++].kind = TBI_START;
    it[n++].kind = TBI_SEARCH;
    for (k = 0; k < TB_PINS; k++) {
        int idx = omni_app_find(g_pin_ids[k]);
        if (idx < 0 || !g_pin_ok[k])
            continue;
        it[n].kind = TBI_APP;
        it[n++].app = &omni_catalog[idx];
    }
    for (i = 0; i < OMNI_WM_MAX_WIN; i++) {
        struct omni_win *w = &g_wm.wins[i];
        const struct omni_app_info *a;
        int j;
        if (!w->used)
            continue;
        a = omni_app_for_title(w->title);
        for (j = 0; a && j < n; j++)
            if (it[j].kind == TBI_APP && it[j].app == a)
                break;
        if (!a || j == n) {
            if (n >= TB_MAX)
                continue;
            j = n++;
            it[j].kind = a ? TBI_APP : TBI_WIN;
            it[j].app = a;
        }
        it[j].nwin++;
        if (!it[j].w)
            it[j].w = w;
        if (w == g_wm.active && !w->minimized) {
            it[j].active = 1;
            it[j].w = w;
        }
    }

    room = tray_left() - 16;
    g_tb_w = TB_ITEM_W;
    while (g_tb_w > TB_ITEM_MIN && n * g_tb_w + (n - 1) * TB_GAP > room)
        g_tb_w--;
    while (n > 2 && n * g_tb_w + (n - 1) * TB_GAP > room)
        n--;                                    /* no room: the last go */
    total = n * g_tb_w + (n - 1) * TB_GAP;
    x0 = g_set.taskbar_left ? 8 : (g_screen.w - total) / 2;
    if (x0 + total > tray_left() - 8)
        x0 = tray_left() - 8 - total;
    if (x0 < 8)
        x0 = 8;
    for (i = 0; i < n; i++)
        it[i].x = x0 + i * (g_tb_w + TB_GAP);
    return n;
}

/* the frontmost window of app a */
static struct omni_win *app_front(const struct omni_app_info *a)
{
    int i;
    for (i = 0; i < g_wm.nwin; i++)
        if (omni_app_for_title(g_wm.order[i]->title) == a)
            return g_wm.order[i];
    return NULL;
}

/* app a's next window after cur (in the order they opened) */
static struct omni_win *app_next(const struct omni_app_info *a, const struct omni_win *cur)
{
    int k, start = cur ? (int)(cur - g_wm.wins) : 0;
    for (k = 1; k < OMNI_WM_MAX_WIN; k++) {
        struct omni_win *w = &g_wm.wins[(start + k) % OMNI_WM_MAX_WIN];
        if (w->used && w != cur && omni_app_for_title(w->title) == a)
            return w;
    }
    return NULL;
}

/* HV_TB + i or a tray HV_*, for a point on the taskbar */
static int tb_hit(int x)
{
    struct tb_item it[TB_MAX];
    int i, n;

    if (upd_tray_on() && x >= tray_upd_x() && x < tray_upd_x() + TB_UPD_W)
        return HV_TRAY_UPD;
    if (x >= tray_net_x() && x < tray_net_x() + TB_NET_W)
        return HV_TRAY_NET;
    if (x >= tray_clock_x())
        return HV_TRAY_CLOCK;
    n = tb_layout(it);
    for (i = 0; i < n; i++)
        if (x >= it[i].x && x < it[i].x + g_tb_w)
            return HV_TB + i;
    return 0;
}

static const char *tb_label(const struct tb_item *t)
{
    switch (t->kind) {
    case TBI_START:  return "Start";
    case TBI_SEARCH: return "Search";
    case TBI_APP:    return (t->nwin == 1 && t->w) ? t->w->title : t->app->name;
    default:         return t->w ? t->w->title : "";
    }
}

/* left click on an app or window icon */
static void tb_click(const struct tb_item *t)
{
    if (t->kind == TBI_APP) {
        if (!t->nwin) {
            run_app(t->app->path, NULL);                /* a pin: open it */
        } else if (t->active) {                         /* focused: next  */
            struct omni_win *nx = t->nwin > 1 ? app_next(t->app, g_wm.active) : NULL;
            if (nx)
                omni_wm_raise(&g_wm, nx);
            else
                omni_wm_minimize(&g_wm, g_wm.active);
        } else {
            struct omni_win *w = app_front(t->app);
            if (w)
                omni_wm_raise(&g_wm, w);
        }
    } else if (t->kind == TBI_WIN && t->w) {
        if (t->w == g_wm.active && !t->w->minimized)
            omni_wm_minimize(&g_wm, t->w);
        else
            omni_wm_raise(&g_wm, t->w);
    }
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

/* a status dot on the network icon: amber = no internet yet, red = none */
static void net_badge(struct raster *r, int x, int y, uint32_t under)
{
    uint32_t c;
    if (g_net.state == NET_ONLINE)
        return;
    c = (g_net.state == NET_LOCAL || g_net.state == NET_CONNECTING) ? UPD_BADGE : 0xef4444;
    th_round_rect(r, x - 1, y - 1, 11, 11, 5, under, 255);
    th_round_rect(r, x, y, 9, 9, 4, c, 255);
    if (c != UPD_BADGE)
        draw_x(r, x + 4, y + 4, 2, 0xffffff);
}

static void draw_taskbar(void)
{
    struct raster *r = &g_screen;
    struct tb_item it[TB_MAX];
    int y = r->h - TASKBAR_H, i, n, cy = y + TASKBAR_H / 2 - 1;
    int flyout = g_menu_open || g_qs_open || g_cal_open || g_pop.open;

    /* frosted, tinted glass with a faint top highlight: pre-rendered
     * over the wallpaper, live only while a window reaches beneath */
    if (g_barbg && !taskbar_covered())
        memcpy(r->bits + (size_t)y * (size_t)r->stride, g_barbg,
               (size_t)r->w * TASKBAR_H * sizeof(uint32_t));
    else
        taskbar_glass(r, y);

    n = tb_layout(it);
    for (i = 0; i < n; i++) {
        const struct tb_item *t = &it[i];
        int x = t->x, cx = x + g_tb_w / 2;
        unsigned a = 0;

        if (t->active || (t->kind == TBI_START && g_menu_open))
            a = 34;
        if (g_hover == HV_TB + i)
            a += 22;
        if (a)
            th_round_rect(r, x, y + 3, g_tb_w, TASKBAR_H - 6, 5, 0xffffff, a);
        if (t->kind == TBI_START) {
            th_logo(r, cx, cy, 10);
        } else if (t->kind == TBI_SEARCH) {
            th_icon(r, cx - 10, cy - 10, 20, 0, 's');
        } else if (t->kind == TBI_APP) {
            th_icon(r, cx - 12, cy - 12, 24, t->app->color, t->app->glyph);
        } else {
            char glyph = t->w->title[0] ? t->w->title[0] : '?';
            uint32_t col = shell_icon_for(t->w->title, &glyph);
            th_icon(r, cx - 12, cy - 12, 24, col ? col : TH_ACCENT, glyph);
        }
        if (t->nwin) {                  /* open: a dash; focused: longer */
            if (t->active)
                th_round_rect(r, cx - 8, y + TASKBAR_H - 6, 16, 3, 1, TH_ACCENT, 255);
            else
                th_round_rect(r, cx - 3, y + TASKBAR_H - 6, 6, 3, 1, 0xffffff, 150);
        }
    }

    /* OmniOS Update: an update is available (or ready: amber dot) */
    if (upd_tray_on()) {
        int ux = tray_upd_x(), iy = y + (TASKBAR_H - 20) / 2;
        if (g_hover == HV_TRAY_UPD)
            th_round_rect(r, ux + 1, y + 6, TB_UPD_W - 2, TASKBAR_H - 12, 6, 0xffffff, 26);
        th_icon(r, ux + 8, iy, 20, TH_ACCENT, 'U');
        if (upd_ready()) {
            th_round_rect(r, ux + 21, iy - 3, 10, 10, 5, TH_TASKBAR, 255);
            th_round_rect(r, ux + 22, iy - 2, 8, 8, 4, UPD_BADGE, 255);
        }
    }

    /* network: opens Quick Settings */
    {
        int nx = tray_net_x(), iy = y + (TASKBAR_H - 20) / 2;
        if (g_hover == HV_TRAY_NET || g_qs_open)
            th_round_rect(r, nx + 2, y + 6, TB_NET_W - 4, TASKBAR_H - 12, 6, 0xffffff,
                          g_qs_open ? 40 : 26);
        th_icon(r, nx + 10, iy, 20, 0, 'e');
        net_badge(r, nx + 23, iy + 11, TH_TASKBAR);
    }

    /* clock: time over date, right-aligned; opens the calendar */
    {
        char tbuf[40], dbuf[40];
        time_t t = time(NULL);
        struct tm tmv;
        int hr, tw, dw;

        if (g_hover == HV_TRAY_CLOCK || g_cal_open)
            th_round_rect(r, tray_clock_x() + 2, y + 6, TB_TRAY_W - 8, TASKBAR_H - 12, 6,
                          0xffffff, g_cal_open ? 40 : 26);
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

    /* the hovered icon's name, above it */
    if (!flyout && g_hover >= HV_TB && g_hover < HV_TB + n) {
        const struct tb_item *t = &it[g_hover - HV_TB];
        const char *s = tb_label(t);
        int tw = (int)strlen(s) * 8, w, tx;
        if (tw > 320)
            tw = 320;
        w = tw + 20;
        tx = t->x + g_tb_w / 2 - w / 2;
        if (tx + w > r->w - 6)
            tx = r->w - 6 - w;
        if (tx < 6)
            tx = 6;
        th_shadow(r, tx, y - 38, w, 28, 6, 10, 3, 110);
        th_round_rect(r, tx, y - 38, w, 28, 6, 0xffffff, 40);
        th_round_rect(r, tx + 1, y - 37, w - 2, 26, 5, 0x1c2130, 248);
        th_text_clip(r, s, tx + 10, y - 28, TH_TEXT_LIGHT, tw);
    }
}

/* ------------------------------------------------------------------ */
/* Start menu: search box, pinned apps, account + power               */
/* ------------------------------------------------------------------ */

struct sm_geom {
    int x, y, w, h;
    int sx, sy, sw;             /* search box                             */
    int head_y, body_y, body_h; /* heading, then the grid or results      */
    int cols, rows, total_rows; /* pinned grid: shown / all rows          */
    int list_n;                 /* search results that fit                */
    int rec_y;                  /* "Recommended" heading, 0 = no room     */
    int foot_y;
    int user_x, user_w;         /* account button                         */
    int power_x;                /* power button                           */
};

static struct sm_geom sm_geom(void)
{
    struct sm_geom g;
    int avail = g_screen.h - TASKBAR_H - FLY_GAP - 8;
    int fixed = SM_PAD + SM_SEARCH_H + 20 + 26 + 12 + SM_FOOT_H;
    int rec = 30 + 2 * SM_REC_H + 8;            /* "Recommended" block */

    g.cols = SM_COLS;
    while (g.cols > 3 && g.cols * SM_TILE_W + 2 * SM_PAD > g_screen.w - 16)
        g.cols--;
    g.w = g.cols * SM_TILE_W + 2 * SM_PAD;
    g.total_rows = (g_menu_n + g.cols - 1) / g.cols;
    g.rows = g.total_rows < 2 ? 2 : g.total_rows;
    if (fixed + g.rows * SM_TILE_H + rec > avail)
        rec = 0;                                /* small screen: apps first */
    while (g.rows > 1 && fixed + g.rows * SM_TILE_H + rec > avail)
        g.rows--;
    g.body_h = g.rows * SM_TILE_H;
    g.h = fixed + g.body_h + rec;
    g.x = g_set.taskbar_left ? FLY_GAP : (g_screen.w - g.w) / 2;
    g.y = g_screen.h - TASKBAR_H - FLY_GAP - g.h;
    if (g.y < 4)
        g.y = 4;
    g.sx = g.x + SM_PAD;
    g.sy = g.y + SM_PAD;
    g.sw = g.w - 2 * SM_PAD;
    g.head_y = g.sy + SM_SEARCH_H + 20;
    g.body_y = g.head_y + 26;
    g.rec_y = rec ? g.body_y + g.body_h + 8 : 0;
    g.list_n = (g.body_h + rec) / SM_ROW_H;     /* results use it all */
    g.foot_y = g.y + g.h - SM_FOOT_H;
    g.user_x = g.x + 40;
    g.user_w = g.w / 2 - 40;
    g.power_x = g.x + g.w - 40 - 36;
    return g;
}

/* "Recommended": what's new with OmniOS and this PC, a click away */
struct sm_rec {
    const char *title;
    char sub[64];
    char icon;
    const char *path, *arg;
};

static int sm_recs(struct sm_rec *rc)
{
    const char *settings = "/usr/bin/omnios-settings";
    int n = 0, i, more = 0;

    for (i = 0; i < omni_catalog_n; i++)
        more += !g_installed[i];
    memset(rc, 0, sizeof(*rc) * SM_REC_N);
    rc[n].icon = 'U';
    rc[n].path = settings;
    rc[n].arg = "update";
    if (upd_ready()) {
        rc[n].title = "Restart to update";
        snprintf(rc[n].sub, sizeof(rc[n].sub), "OmniOS %s is ready", g_upd.latest);
    } else if (upd_tray_on()) {
        rc[n].title = "Update available";
        snprintf(rc[n].sub, sizeof(rc[n].sub), "OmniOS %s", g_upd.latest);
    } else {
        rc[n].title = "OmniOS Update";
        snprintf(rc[n].sub, sizeof(rc[n].sub), "%s",
                 omni_updates_paused(&g_set) ? "Updates are paused"
                 : g_upd.state == UPD_UPTODATE ? "You're up to date" : "Check for updates");
    }
    n++;
    rc[n].title = "Network & internet";
    snprintf(rc[n].sub, sizeof(rc[n].sub), "%s",
             g_net.ip[0] ? g_net.ip : omni_net_state_text(g_net.state));
    rc[n].icon = 'w';
    rc[n].path = settings;
    rc[n++].arg = "network";
    rc[n].title = more ? "Get more apps" : "App Store";
    if (more)
        snprintf(rc[n].sub, sizeof(rc[n].sub), "%d more in the App Store", more);
    else
        snprintf(rc[n].sub, sizeof(rc[n].sub), "Every app is installed");
    rc[n].icon = 'A';
    rc[n++].path = "/usr/bin/omnios-store";
    rc[n].title = "Personalize";
    snprintf(rc[n].sub, sizeof(rc[n].sub), "Wallpaper: %s",
             omni_walls[g_set.wallpaper % omni_walls_n].name);
    rc[n].icon = 'p';
    rc[n].path = settings;
    rc[n++].arg = "personalization";
    return n;
}

static void sm_rec_xy(const struct sm_geom *g, int i, int *x, int *y)
{
    *x = g->x + SM_PAD + (i % 2) * ((g->w - 2 * SM_PAD) / 2);
    *y = g->rec_y + 30 + (i / 2) * SM_REC_H;
}

/* HV_ITEM + i, HV_REC + i, HV_SM_SEARCH, HV_USER, HV_SM_POWER, HV_PANEL, 0 */
static int sm_hit(int x, int y)
{
    struct sm_geom g = sm_geom();
    int list_h = g_query[0] ? g.list_n * SM_ROW_H : g.body_h;

    if (!in_rect(x, y, g.x, g.y, g.w, g.h))
        return 0;
    if (in_rect(x, y, g.sx, g.sy, g.sw, SM_SEARCH_H))
        return HV_SM_SEARCH;
    if (!g_query[0] && g.rec_y) {
        int i, rx, ry;
        for (i = 0; i < SM_REC_N; i++) {
            sm_rec_xy(&g, i, &rx, &ry);
            if (in_rect(x, y, rx - 8, ry, (g.w - 2 * SM_PAD) / 2 - 4, SM_REC_H - 4))
                return HV_REC + i;
        }
    }
    if (y >= g.body_y && y < g.body_y + list_h) {
        if (g_query[0]) {
            int i = (y - g.body_y) / SM_ROW_H;
            if (i < g_res_n && i < g.list_n &&
                x >= g.x + SM_PAD - 8 && x < g.x + g.w - SM_PAD + 8)
                return HV_ITEM + i;
        } else if (x >= g.x + SM_PAD && x < g.x + SM_PAD + g.cols * SM_TILE_W) {
            int col = (x - g.x - SM_PAD) / SM_TILE_W, row = (y - g.body_y) / SM_TILE_H;
            int i = (row + g_sm_scroll) * g.cols + col;
            if (i < g_res_n)
                return HV_ITEM + i;
        }
    }
    if (y >= g.foot_y + 12 && y < g.foot_y + SM_FOOT_H - 12) {
        if (x >= g.user_x - 12 && x < g.user_x - 12 + g.user_w)
            return HV_USER;
        if (x >= g.power_x && x < g.power_x + 36)
            return HV_SM_POWER;
    }
    return HV_PANEL;
}

/* a Start tile's name: one line, or two split at a space */
static void tile_label(struct raster *r, const char *s, int cx, int y)
{
    int n = (int)strlen(s), cut;
    char first[48];

    if (n <= 10 || !strchr(s, ' ')) {
        text_mid(r, s, cx, y, TH_TEXT_LIGHT, SM_TILE_W);
        return;
    }
    for (cut = n < 11 ? n : 11; cut > 0 && s[cut] != ' '; cut--)
        ;
    if (cut <= 0)
        cut = (int)(strchr(s, ' ') - s);
    snprintf(first, sizeof(first), "%.*s", cut, s);
    text_mid(r, first, cx, y, TH_TEXT_LIGHT, SM_TILE_W);
    text_mid(r, s + cut + 1, cx, y + 12, TH_TEXT_LIGHT, SM_TILE_W);
}

static void draw_menu(void)
{
    struct raster *r = &g_screen;
    struct sm_geom g = sm_geom();
    char s[96];
    int i;

    fly_panel(r, g.x, g.y, g.w, g.h);

    /* search box: type to find apps and settings */
    th_round_rect(r, g.sx, g.sy, g.sw, SM_SEARCH_H, SM_SEARCH_H / 2, 0xffffff,
                  g_hover == HV_SM_SEARCH ? 50 : 36);
    th_round_rect(r, g.sx + 1, g.sy + 1, g.sw - 2, SM_SEARCH_H - 2, SM_SEARCH_H / 2 - 1,
                  0x0b0f1a, 196);
    th_icon(r, g.sx + 14, g.sy + 10, 16, 0, 's');
    if (g_query[0]) {
        int tw = th_text_clip(r, g_query, g.sx + 42, g.sy + 14, TH_TEXT_LIGHT, g.sw - 64);
        th_fill_a(r, g.sx + 43 + tw, g.sy + 10, 2, 16, TH_ACCENT, 255);
    } else {
        th_fill_a(r, g.sx + 42, g.sy + 10, 2, 16, TH_ACCENT, 255);
        th_text(r, "Search for apps, settings and more", g.sx + 50, g.sy + 14, TH_TEXT_DIM);
    }

    /* heading */
    th_text_bold(r, g_query[0] ? "Results" : "Pinned", g.x + SM_PAD + 10, g.head_y,
                 TH_TEXT_LIGHT);
    if (!g_query[0]) {
        if (upd_ready())
            snprintf(s, sizeof(s), "Update ready: restart to install");
        else
            snprintf(s, sizeof(s), "%d apps", g_menu_n);
        th_text(r, s, g.x + g.w - SM_PAD - 10 - (int)strlen(s) * 8, g.head_y,
                upd_ready() ? UPD_BADGE : TH_TEXT_DIM);
    }

    if (!g_query[0]) {                          /* pinned: icon grid */
        int first = g_sm_scroll * g.cols, last = (g_sm_scroll + g.rows) * g.cols;
        for (i = first; i < g_res_n && i < last; i++) {
            int k = i - first;
            int tx = g.x + SM_PAD + (k % g.cols) * SM_TILE_W;
            int ty = g.body_y + (k / g.cols) * SM_TILE_H;
            if (i == g_menu_sel)
                th_round_rect(r, tx + 3, ty + 2, SM_TILE_W - 6, SM_TILE_H - 4, 6, 0xffffff, 28);
            th_icon(r, tx + (SM_TILE_W - 36) / 2, ty + 10, 36, g_res[i].color, g_res[i].icon);
            tile_label(r, g_res[i].label, tx + SM_TILE_W / 2, ty + 54);
        }
        if (g.total_rows > g.rows) {            /* more rows: a scroll bar */
            int bh = g.body_h * g.rows / g.total_rows;
            int by = g.body_y + g.body_h * g_sm_scroll / g.total_rows;
            th_round_rect(r, g.x + g.w - 12, by, 4, bh, 2, 0xffffff, 90);
        }
        if (g.rec_y) {                          /* Recommended */
            struct sm_rec rc[SM_REC_N];
            int nrec = sm_recs(rc), rx, ry;
            th_text_bold(r, "Recommended", g.x + SM_PAD + 10, g.rec_y, TH_TEXT_LIGHT);
            for (i = 0; i < nrec; i++) {
                sm_rec_xy(&g, i, &rx, &ry);
                if (g_hover == HV_REC + i)
                    th_round_rect(r, rx - 8, ry, (g.w - 2 * SM_PAD) / 2 - 4, SM_REC_H - 4, 6,
                                  0xffffff, 26);
                th_icon(r, rx + 2, ry + 8, 32, TH_ACCENT, rc[i].icon);
                th_text_clip(r, rc[i].title, rx + 46, ry + 12, TH_TEXT_LIGHT,
                             (g.w - 2 * SM_PAD) / 2 - 64);
                th_text_clip(r, rc[i].sub, rx + 46, ry + 27,
                             (i == 0 && upd_tray_on()) ? UPD_BADGE : TH_TEXT_DIM,
                             (g.w - 2 * SM_PAD) / 2 - 64);
            }
        }
    } else if (g_res_n == 0) {
        snprintf(s, sizeof(s), "No results for \"%s\"", g_query);
        text_mid(r, s, g.x + g.w / 2, g.body_y + 40, TH_TEXT_DIM, g.w - 2 * SM_PAD);
    } else {                                    /* search results */
        for (i = 0; i < g_res_n && i < g.list_n; i++) {
            const struct sm_result *res = &g_res[i];
            int ry = g.body_y + i * SM_ROW_H;
            if (i == g_menu_sel)
                th_round_rect(r, g.x + SM_PAD - 8, ry + 1, g.w - 2 * SM_PAD + 16, SM_ROW_H - 2, 6,
                              0xffffff, 28);
            th_icon(r, g.x + SM_PAD + 2, ry + 8, 24, res->color, res->icon);
            th_text_clip(r, res->label, g.x + SM_PAD + 40, ry + 16, TH_TEXT_LIGHT,
                         g.w - 2 * SM_PAD - 40 - 160);
            th_text(r, res->sub, g.x + g.w - SM_PAD - (int)strlen(res->sub) * 8, ry + 16,
                    TH_TEXT_DIM);
        }
    }

    /* footer: the account (menu: settings, lock) and power */
    fly_footer(r, g.x, g.foot_y, g.w, g.y + g.h);
    if (g_hover == HV_USER || (g_pop.open && g_pop.owner == POP_ACCOUNT))
        th_round_rect(r, g.user_x - 12, g.foot_y + 12, g.user_w, 40, 6, 0xffffff, 26);
    th_icon(r, g.user_x, g.foot_y + 16, 32, TH_ACCENT, 'u');
    th_text_clip(r, g_user, g.user_x + 44, g.foot_y + 28, TH_TEXT_LIGHT, g.user_w - 64);
    if (g_hover == HV_SM_POWER || (g_pop.open && g_pop.owner == POP_POWER_START))
        th_round_rect(r, g.power_x, g.foot_y + 14, 36, 36, 6, 0xffffff, 26);
    th_icon(r, g.power_x + 9, g.foot_y + 23, 18, 0, 'P');
    if (upd_ready())                        /* restarting installs the update */
        th_round_rect(r, g.power_x + 26, g.foot_y + 18, 8, 8, 4, UPD_BADGE, 255);
}

static void close_flyouts(void)
{
    g_menu_open = 0;
    g_menu_sel = -1;
    g_query[0] = '\0';
    g_qs_open = 0;
    g_cal_open = 0;
    g_pop.open = 0;
}

static void open_menu(void)
{
    close_flyouts();
    menu_reload();
    sm_refresh();
    g_menu_open = 1;
    g_sm_scroll = 0;
}

static void close_menu(void)
{
    close_flyouts();
}

static void sm_open(int i)
{
    if (i < 0 || i >= g_res_n)
        return;
    run_app(g_res[i].path, g_res[i].arg);
    close_menu();
}

static void sm_wheel(int key)
{
    struct sm_geom g = sm_geom();
    int most = g.total_rows - g.rows;
    if (g_query[0])
        return;
    g_sm_scroll += key == 4 ? -1 : 1;
    if (g_sm_scroll > most)
        g_sm_scroll = most;
    if (g_sm_scroll < 0)
        g_sm_scroll = 0;
}

static void power_menu(int owner, int right_x, int top_y)
{
    pop_begin(owner);
    if (owner != POP_POWER_SIGNIN)
        pop_add("Lock", 'l', ACT_LOCK, 0);
    pop_add(upd_ready() ? "Update and restart" : "Restart", 'r', ACT_RESTART, upd_ready());
    pop_add("Shut down", 'P', ACT_SHUTDOWN, 0);
    pop_place(right_x, top_y, POP_ABOVE_RIGHT);
}

static void sm_click(int k, int btn)
{
    struct sm_geom g = sm_geom();

    if (k >= HV_ITEM && k < HV_ITEM + g_res_n) {
        if (btn == 1)
            sm_open(k - HV_ITEM);
    } else if (k >= HV_REC && k < HV_REC + SM_REC_N) {
        struct sm_rec rc[SM_REC_N];
        if (btn == 1 && k - HV_REC < sm_recs(rc)) {
            run_app(rc[k - HV_REC].path, rc[k - HV_REC].arg);
            close_menu();
        }
    } else if (k == HV_USER) {
        pop_begin(POP_ACCOUNT);
        pop_add("Change account settings", 'g', ACT_ACCOUNT, 0);
        pop_add("Lock", 'l', ACT_LOCK, 0);
        pop_place(g.user_x - 12, g.foot_y + 10, POP_ABOVE_LEFT);
    } else if (k == HV_SM_POWER) {
        power_menu(POP_POWER_START, g.power_x + 36, g.foot_y + 12);
    }
}

/* a key while Start is open (typing searches) */
static int sm_key(int code, int pressed, char text)
{
    struct sm_geom g = sm_geom();
    size_t n = strlen(g_query);
    int step;

    if (!pressed)
        return 1;
    switch (code) {
    case OMNI_KEY_ESC:
        if (g_query[0]) {
            g_query[0] = '\0';
            sm_refresh();
            g_menu_sel = -1;
        } else {
            close_menu();
        }
        return 1;
    case OMNI_KEY_BACKSPACE:
        if (n) {
            g_query[n - 1] = '\0';
            sm_refresh();
            g_menu_sel = (g_query[0] && g_res_n) ? 0 : -1;
        }
        return 1;
    case OMNI_KEY_ENTER:
    case OMNI_KEY_KPENTER:
        if (g_menu_sel >= 0)
            sm_open(g_menu_sel);
        else if (g_query[0])
            sm_open(0);
        return 1;
    case OMNI_KEY_UP: case OMNI_KEY_DOWN: case OMNI_KEY_LEFT: case OMNI_KEY_RIGHT:
        if (!g_res_n)
            return 1;
        if (g_query[0])
            step = (code == OMNI_KEY_UP || code == OMNI_KEY_LEFT) ? -1 : 1;
        else
            step = code == OMNI_KEY_UP ? -g.cols : code == OMNI_KEY_DOWN ? g.cols
                 : code == OMNI_KEY_LEFT ? -1 : 1;
        if (g_menu_sel < 0)
            g_menu_sel = 0;
        else if (g_menu_sel + step >= 0 && g_menu_sel + step < g_res_n)
            g_menu_sel += step;
        if (g_query[0] && g_menu_sel >= g.list_n)
            g_menu_sel = g.list_n - 1;
        if (!g_query[0]) {                      /* keep it in view */
            int row = g_menu_sel / g.cols;
            if (row < g_sm_scroll)
                g_sm_scroll = row;
            if (row >= g_sm_scroll + g.rows)
                g_sm_scroll = row - g.rows + 1;
        }
        return 1;
    default:
        if (text >= 32 && text < 127 && n + 1 < sizeof(g_query) && !(n == 0 && text == ' ')) {
            g_query[n] = text;
            g_query[n + 1] = '\0';
            sm_refresh();
            g_menu_sel = g_res_n ? 0 : -1;      /* the best match */
        }
        return 1;
    }
}

/* Windows + X, or right-click Start: the quick links */
static void quick_links(int x, int top_y)
{
    close_flyouts();
    menu_reload();
    pop_begin(POP_QUICKLINK);
    if (omni_app_find("taskmgr") >= 0 && g_installed[omni_app_find("taskmgr")])
        pop_add("Task Manager", 'T', ACT_TASKMGR, 0);
    pop_add("Settings", 'G', ACT_SETTINGS, 0);
    pop_add("File Manager", 'F', ACT_FILES, 0);
    pop_add("Terminal", '>', ACT_TERMINAL, 0);
    pop_add("Desktop", 'm', ACT_DESKTOP, 0);
    pop_add(upd_ready() ? "Update and restart" : "Restart", 'r', ACT_RESTART, upd_ready());
    pop_add("Shut down", 'P', ACT_SHUTDOWN, 0);
    pop_place(x, top_y, POP_ABOVE_LEFT);
}

/* right-click on a taskbar icon */
static void tb_menu(const struct tb_item *t, int start_x)
{
    int top = g_screen.h - TASKBAR_H;
    close_flyouts();
    if (t->kind == TBI_START) {
        quick_links(start_x, top);
        return;
    }
    if (t->kind == TBI_SEARCH)
        return;
    pop_begin(POP_TASKBAR);
    if (t->app) {
        g_pop.app = t->app;
        pop_add(t->app->name, t->app->glyph, ACT_LAUNCH, 0);
    }
    if (t->nwin == 1 && t->w) {
        g_pop.win_id = t->w->id;
        pop_add("Close window", 'x', ACT_CLOSE, 0);
    } else if (t->nwin > 1) {
        pop_add("Close all windows", 'x', ACT_CLOSE_ALL, 0);
    }
    pop_place(t->x + g_tb_w / 2, top, POP_ABOVE_CENTER);
}

/* ------------------------------------------------------------------ */
/* Quick Settings: network, update status, toggles, lock + power      */
/* ------------------------------------------------------------------ */

enum { QT_NET, QT_AUTOUPD, QT_CLOCK24, QT_DESKICONS, QT_CENTER, QT_WALL };

struct qs_geom { int x, y, w, h, tiles_y, info_y, foot_y, btn_x; };

static struct qs_geom qs_geom(void)
{
    struct qs_geom g;
    g.w = QS_W;
    g.h = 18 + 2 * 78 + 50 + 52;
    g.x = g_screen.w - FLY_GAP - g.w;
    g.y = g_screen.h - TASKBAR_H - FLY_GAP - g.h;
    g.tiles_y = g.y + 18;
    g.info_y = g.tiles_y + 2 * 78;
    g.foot_y = g.y + g.h - 52;
    g.btn_x = g.x + g.w - 16 - 36;              /* power; gear, lock left */
    return g;
}

static void qs_tile_xy(const struct qs_geom *g, int i, int *x, int *y)
{
    *x = g->x + 16 + (i % 3) * (QS_TILE_W + 14);
    *y = g->tiles_y + (i / 3) * 78;
}

static int qs_on(int i)
{
    switch (i) {
    case QT_NET:       return g_net.state == NET_ONLINE || g_net.state == NET_LOCAL;
    case QT_AUTOUPD:   return g_set.autoupdate && !omni_updates_paused(&g_set);
    case QT_CLOCK24:   return g_set.clock24;
    case QT_DESKICONS: return g_set.desktop_icons;
    case QT_CENTER:    return !g_set.taskbar_left;
    default:           return 0;
    }
}

static const char *qs_label(int i)
{
    static const char *const names[QS_TILES] = {
        "Ethernet", "Auto update", "24-hour clock", "Desktop icons", "Centered", "Wallpaper"
    };
    if (i == QT_NET && g_net.state == NET_NO_ADAPTER)
        return "No network";
    return names[i];
}

static int qs_hit(int x, int y)
{
    struct qs_geom g = qs_geom();
    int i, tx, ty;

    if (!in_rect(x, y, g.x, g.y, g.w, g.h))
        return 0;
    for (i = 0; i < QS_TILES; i++) {
        qs_tile_xy(&g, i, &tx, &ty);
        if (in_rect(x, y, tx, ty, QS_TILE_W, QS_TILE_H + 22))
            return HV_QS_TILE + i;
    }
    if (y >= g.foot_y + 8 && y < g.foot_y + 44) {
        if (x >= g.btn_x && x < g.btn_x + 36)
            return HV_QS_POWER;
        if (x >= g.btn_x - 44 && x < g.btn_x - 8)
            return HV_QS_GEAR;
        if (x >= g.btn_x - 88 && x < g.btn_x - 52)
            return HV_QS_LOCK;
    }
    return HV_PANEL;
}

static void draw_qs(void)
{
    struct raster *r = &g_screen;
    struct qs_geom g = qs_geom();
    static const char icons[QS_TILES] = { 'e', 'r', 'c', 'a', 'b', 'p' };
    char s[96];
    int i, tx, ty;

    fly_panel(r, g.x, g.y, g.w, g.h);
    for (i = 0; i < QS_TILES; i++) {
        int on = qs_on(i), hot = g_hover == HV_QS_TILE + i;
        qs_tile_xy(&g, i, &tx, &ty);
        if (on)
            th_round_rect(r, tx, ty, QS_TILE_W, QS_TILE_H, 6,
                          hot ? th_shade(TH_ACCENT, 22) : TH_ACCENT, 255);
        else
            th_round_rect(r, tx, ty, QS_TILE_W, QS_TILE_H, 6, 0xffffff, hot ? 44 : 24);
        th_icon(r, tx + (QS_TILE_W - 20) / 2, ty + 14, 20, 0, icons[i]);
        text_mid(r, qs_label(i), tx + QS_TILE_W / 2, ty + QS_TILE_H + 9, TH_TEXT_LIGHT,
                 QS_TILE_W + 8);
    }

    /* status: network, then OmniOS Update */
    if (g_net.state == NET_ONLINE || g_net.state == NET_LOCAL)
        snprintf(s, sizeof(s), "%s: %s", omni_net_state_text(g_net.state), g_net.ip);
    else
        snprintf(s, sizeof(s), "%s", omni_net_state_text(g_net.state));
    th_icon(r, g.x + 18, g.info_y + 2, 14, 0, 'e');
    th_text_clip(r, s, g.x + 40, g.info_y + 5, TH_TEXT_LIGHT, g.w - 58);
    upd_summary(s, sizeof(s));
    th_icon(r, g.x + 18, g.info_y + 22, 14, 0, 'r');
    th_text_clip(r, s, g.x + 40, g.info_y + 25, upd_ready() ? UPD_BADGE : TH_TEXT_DIM, g.w - 58);

    /* footer: version; lock, Settings, power */
    fly_footer(r, g.x, g.foot_y, g.w, g.y + g.h);
    snprintf(s, sizeof(s), "OmniOS %s", g_version[0] ? g_version : g_upd.current);
    th_text(r, s, g.x + 20, g.foot_y + 22, TH_TEXT_DIM);
    {
        static const struct { int hv, dx; char icon; } b[3] = {
            { HV_QS_LOCK, -88, 'l' }, { HV_QS_GEAR, -44, 'g' }, { HV_QS_POWER, 0, 'P' }
        };
        for (i = 0; i < 3; i++) {
            int bx = g.btn_x + b[i].dx;
            if (g_hover == b[i].hv || (b[i].hv == HV_QS_POWER && g_pop.open &&
                                        g_pop.owner == POP_POWER_QS))
                th_round_rect(r, bx, g.foot_y + 8, 36, 36, 6, 0xffffff, 26);
            th_icon(r, bx + 9, g.foot_y + 17, 18, 0, b[i].icon);
        }
        if (upd_ready())
            th_round_rect(r, g.btn_x + 26, g.foot_y + 12, 8, 8, 4, UPD_BADGE, 255);
    }
}

static void save_settings(void)
{
    omni_settings_save(&g_set);
    shell_apply_settings(1);
}

static void open_qs(void)
{
    close_flyouts();
    omni_net_read(&g_net);
    g_qs_open = 1;
    g_toast_on = 0;
}

static void qs_click(int k)
{
    struct qs_geom g = qs_geom();

    switch (k) {
    case HV_QS_TILE + QT_NET:
        close_flyouts();
        open_settings("network");
        break;
    case HV_QS_TILE + QT_AUTOUPD:
        g_set.autoupdate = !qs_on(QT_AUTOUPD);
        if (g_set.autoupdate)
            g_set.pause_until = 0;              /* on = not paused either */
        save_settings();
        break;
    case HV_QS_TILE + QT_CLOCK24:
        g_set.clock24 = !g_set.clock24;
        save_settings();
        break;
    case HV_QS_TILE + QT_DESKICONS:
        g_set.desktop_icons = !g_set.desktop_icons;
        save_settings();
        break;
    case HV_QS_TILE + QT_CENTER:
        g_set.taskbar_left = !g_set.taskbar_left;
        save_settings();
        break;
    case HV_QS_TILE + QT_WALL:
        g_set.wallpaper = (g_set.wallpaper + 1) % omni_walls_n;
        save_settings();
        break;
    case HV_QS_LOCK:
        close_flyouts();
        shell_lock();
        break;
    case HV_QS_GEAR:
        close_flyouts();
        open_settings(NULL);
        break;
    case HV_QS_POWER:
        power_menu(POP_POWER_QS, g.btn_x + 36, g.foot_y + 8);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* calendar (click the clock)                                         */
/* ------------------------------------------------------------------ */

struct cal_geom { int x, y, w, h, nav_y, grid_x, grid_y, prev_x, next_x; };

static struct cal_geom cal_geom(void)
{
    struct cal_geom g;
    g.w = CAL_W;
    g.h = 18 + 44 + 14 + 34 + 22 + 6 * CAL_CELL_H + 14;
    g.x = g_screen.w - FLY_GAP - g.w;
    g.y = g_screen.h - TASKBAR_H - FLY_GAP - g.h;
    g.nav_y = g.y + 18 + 44 + 14;
    g.grid_x = g.x + (g.w - 7 * CAL_CELL_W) / 2;
    g.grid_y = g.nav_y + 34 + 22;
    g.next_x = g.x + g.w - 16 - 32;
    g.prev_x = g.next_x - 36;
    return g;
}

static int cal_hit(int x, int y)
{
    struct cal_geom g = cal_geom();
    if (!in_rect(x, y, g.x, g.y, g.w, g.h))
        return 0;
    if (y >= g.nav_y && y < g.nav_y + 30) {
        if (x >= g.prev_x && x < g.prev_x + 32)
            return HV_CAL_PREV;
        if (x >= g.next_x && x < g.next_x + 32)
            return HV_CAL_NEXT;
    }
    return HV_PANEL;
}

static void draw_cal(void)
{
    struct raster *r = &g_screen;
    struct cal_geom g = cal_geom();
    static const char *const wd[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
    time_t t = time(NULL);
    struct tm now, m, e;
    char s[64], mon[24];
    int i, ndays, pdays;

    localtime_r(&t, &now);
    fly_panel(r, g.x, g.y, g.w, g.h);

    /* today, large */
    strftime(s, sizeof(s), "%A", &now);
    th_text(r, s, g.x + 22, g.y + 20, TH_TEXT_DIM);
    strftime(mon, sizeof(mon), "%B", &now);
    snprintf(s, sizeof(s), "%s %d, %d", mon, now.tm_mday, now.tm_year + 1900);
    th_text2x(r, s, g.x + 22, g.y + 36, TH_TEXT_LIGHT);
    th_fill_a(r, g.x + 16, g.y + 18 + 44 + 4, g.w - 32, 1, 0xffffff, 22);

    /* the month shown */
    m = now;
    m.tm_mday = 1;
    m.tm_mon += g_cal_month;
    m.tm_hour = 12;
    m.tm_isdst = -1;
    mktime(&m);
    e = m;
    e.tm_mon += 1;
    e.tm_mday = 0;                              /* last day of the month */
    mktime(&e);
    ndays = e.tm_mday;
    e = m;
    e.tm_mday = 0;                              /* last of the one before */
    mktime(&e);
    pdays = e.tm_mday;
    strftime(mon, sizeof(mon), "%B", &m);
    snprintf(s, sizeof(s), "%s %d", mon, m.tm_year + 1900);
    th_text_bold(r, s, g.grid_x + 8, g.nav_y + 11, TH_TEXT_LIGHT);
    th_round_rect(r, g.prev_x, g.nav_y, 32, 30, 6, 0xffffff, g_hover == HV_CAL_PREV ? 40 : 16);
    th_round_rect(r, g.next_x, g.nav_y, 32, 30, 6, 0xffffff, g_hover == HV_CAL_NEXT ? 40 : 16);
    th_text_bold(r, "<", g.prev_x + 12, g.nav_y + 11, TH_TEXT_LIGHT);
    th_text_bold(r, ">", g.next_x + 12, g.nav_y + 11, TH_TEXT_LIGHT);

    for (i = 0; i < 7; i++)
        text_mid(r, wd[i], g.grid_x + i * CAL_CELL_W + CAL_CELL_W / 2, g.nav_y + 40,
                 TH_TEXT_DIM, CAL_CELL_W);
    for (i = 0; i < 42; i++) {
        int day = i - m.tm_wday + 1, shown = day, in = 1;
        int cx = g.grid_x + (i % 7) * CAL_CELL_W + CAL_CELL_W / 2;
        int cy = g.grid_y + (i / 7) * CAL_CELL_H;
        if (day < 1) {
            shown = pdays + day;
            in = 0;
        } else if (day > ndays) {
            shown = day - ndays;
            in = 0;
        }
        snprintf(s, sizeof(s), "%d", shown);
        if (in && g_cal_month == 0 && day == now.tm_mday) {
            th_round_rect(r, cx - 15, cy + 3, 30, 30, 15, TH_ACCENT, 255);
            text_mid(r, s, cx, cy + 14, 0xffffff, CAL_CELL_W);
        } else {
            text_mid(r, s, cx, cy + 14, in ? TH_TEXT_LIGHT : 0x5d6677, CAL_CELL_W);
        }
    }
}

static void open_cal(void)
{
    close_flyouts();
    g_cal_open = 1;
    g_cal_month = 0;
    g_toast_on = 0;
}

static void cal_click(int k)
{
    if (k == HV_CAL_PREV && g_cal_month > -240)
        g_cal_month--;
    if (k == HV_CAL_NEXT && g_cal_month < 240)
        g_cal_month++;
}

/* ------------------------------------------------------------------ */
/* desktop icons                                                      */
/* ------------------------------------------------------------------ */

static const struct {
    const char *label, *path;
    char icon;
    uint32_t color;
} g_desk[] = {
    { "This PC",   "/usr/bin/omnios-files",    'm', 0x38bdf8 },
    { "Terminal",  "/usr/bin/omnios-term",     '>', 0x10b981 },
    { "App Store", "/usr/bin/omnios-store",    'A', 0xf59e0b },
    { "Settings",  "/usr/bin/omnios-settings", 'G', 0x64748b },
};

static int desk_n(void)
{
    return g_set.desktop_icons ? N_OF(g_desk) : 0;
}

/* top-left, in columns */
static void desk_cell(int i, int *x, int *y)
{
    int per = (g_screen.h - TASKBAR_H - 12) / DESK_H;
    if (per < 1)
        per = 1;
    *x = 10 + (i / per) * (DESK_W + 6);
    *y = 10 + (i % per) * DESK_H;
}

/* the icon under a point of bare desktop, -1 if none */
static int desk_hit(int x, int y)
{
    int i, cx, cy, n = desk_n();
    if (y >= g_screen.h - TASKBAR_H || omni_wm_at(&g_wm, x, y))
        return -1;
    for (i = 0; i < n; i++) {
        desk_cell(i, &cx, &cy);
        if (in_rect(x, y, cx, cy, DESK_W, DESK_H - 6))
            return i;
    }
    return -1;
}

static void draw_desktop_icons(struct raster *r)
{
    int i, cx, cy, n = desk_n();

    for (i = 0; i < n; i++) {
        desk_cell(i, &cx, &cy);
        if (i == g_desk_sel) {                  /* selected: translucent blue */
            th_round_rect(r, cx, cy, DESK_W, DESK_H - 6, 6, 0xffffff, 56);
            th_round_rect(r, cx + 1, cy + 1, DESK_W - 2, DESK_H - 8, 5, 0x4f86e6, 96);
        } else if (g_hover == HV_DESK + i) {
            th_round_rect(r, cx, cy, DESK_W, DESK_H - 6, 6, 0xffffff, 34);
        }
        th_icon(r, cx + (DESK_W - DESK_ICON) / 2, cy + 8, DESK_ICON, g_desk[i].color,
                g_desk[i].icon);
        text_mid(r, g_desk[i].label, cx + DESK_W / 2 + 1, cy + 62, 0x000000, DESK_W - 4);
        text_mid(r, g_desk[i].label, cx + DESK_W / 2, cy + 61, 0xffffff, DESK_W - 4);
    }
}

/* a press on bare desktop: select / open icons, or the desktop menu;
 * returns 1 if the shell used it */
static int desk_click(int x, int y, int btn)
{
    int i = desk_hit(x, y);
    long long t = now_ms();

    if (btn == 3) {
        close_flyouts();
        g_desk_sel = i;
        pop_begin(POP_DESKTOP);
        if (i >= 0) {
            g_pop.path = g_desk[i].path;
            pop_add("Open", g_desk[i].icon, ACT_OPEN, 0);
        } else {
            pop_add("Personalize", 'p', ACT_PERSONALIZE, 0);
            pop_add("Display settings", 'm', ACT_DISPLAY, 0);
            pop_add("Open in Terminal", '>', ACT_TERMINAL, 0);
        }
        pop_add(g_set.desktop_icons ? "Hide desktop icons" : "Show desktop icons", 'a',
                ACT_DESKICONS, 0);
        pop_place(x, y, POP_AT);
        return 1;
    }
    if (btn != 1)
        return 0;
    if (i < 0) {
        g_desk_sel = g_desk_last = -1;
        return 0;                               /* the wm unfocuses windows */
    }
    if (i == g_desk_last && t - g_desk_last_ms <= DCLICK_MS) {
        run_app(g_desk[i].path, NULL);          /* double-click: open */
        g_desk_last = -1;
    } else {
        g_desk_last = i;
        g_desk_last_ms = t;
    }
    g_desk_sel = i;
    g_wm.active = NULL;                         /* the desktop has the keys */
    g_wm.dirty = 1;
    return 1;
}

/* keys for the desktop (no window focused): Enter opens the selected
 * icon, the arrows move between them */
static int desk_key(int code, int pressed)
{
    int n = desk_n();
    if (g_wm.active || g_desk_sel < 0 || g_desk_sel >= n || !pressed)
        return 0;
    switch (code) {
    case OMNI_KEY_ENTER:
    case OMNI_KEY_KPENTER:
        if (pressed == 1)
            run_app(g_desk[g_desk_sel].path, NULL);
        return 1;
    case OMNI_KEY_UP:
        g_desk_sel = g_desk_sel > 0 ? g_desk_sel - 1 : 0;
        return 1;
    case OMNI_KEY_DOWN:
        g_desk_sel = g_desk_sel + 1 < n ? g_desk_sel + 1 : n - 1;
        return 1;
    case OMNI_KEY_ESC:
        g_desk_sel = -1;
        return 1;
    default:
        return 0;
    }
}

/* Windows + D: hide every window; again: bring them back */
static void show_desktop(void)
{
    int i, any = 0;

    for (i = 0; i < OMNI_WM_MAX_WIN; i++)
        if (g_wm.wins[i].used && !g_wm.wins[i].minimized)
            any = 1;
    if (any) {
        g_showdesk = 0;
        for (i = 0; i < OMNI_WM_MAX_WIN; i++)
            if (g_wm.wins[i].used && !g_wm.wins[i].minimized) {
                g_showdesk |= 1u << i;
                omni_wm_minimize(&g_wm, &g_wm.wins[i]);
            }
    } else {
        for (i = 0; i < OMNI_WM_MAX_WIN; i++)
            if (((g_showdesk >> i) & 1) && g_wm.wins[i].used && g_wm.wins[i].minimized)
                omni_wm_raise(&g_wm, &g_wm.wins[i]);
        g_showdesk = 0;
    }
}

static void pop_do(int i)
{
    struct pop_item p;
    int k;

    if (i < 0 || i >= g_pop.n)
        return;
    p = g_pop.it[i];
    g_pop.open = 0;
    switch (p.act) {
    case ACT_LOCK:        close_flyouts(); shell_lock(); break;
    case ACT_RESTART:     close_flyouts(); power(1); break;
    case ACT_SHUTDOWN:    close_flyouts(); power(0); break;
    case ACT_ACCOUNT:     close_flyouts(); open_settings("accounts"); break;
    case ACT_PERSONALIZE: open_settings("personalization"); break;
    case ACT_DISPLAY:     open_settings("system"); break;
    case ACT_SETTINGS:    close_flyouts(); open_settings(NULL); break;
    case ACT_TERMINAL:    close_flyouts(); run_app("/usr/bin/omnios-term", NULL); break;
    case ACT_FILES:       close_flyouts(); run_app("/usr/bin/omnios-files", NULL); break;
    case ACT_TASKMGR:     close_flyouts(); run_app("/usr/bin/omnios-taskmgr", NULL); break;
    case ACT_DESKTOP:     close_flyouts(); show_desktop(); break;
    case ACT_DESKICONS:
        g_set.desktop_icons = !g_set.desktop_icons;
        save_settings();
        break;
    case ACT_LAUNCH:
        if (g_pop.app)
            run_app(g_pop.app->path, NULL);
        break;
    case ACT_OPEN:
        if (g_pop.path)
            run_app(g_pop.path, NULL);
        break;
    case ACT_CLOSE: {
        struct omni_win *w = omni_wm_find(&g_wm, g_pop.win_id);
        if (w)
            omni_wm_request_close(&g_wm, w);
        break;
    }
    case ACT_CLOSE_ALL:
        for (k = 0; k < OMNI_WM_MAX_WIN; k++)
            if (g_wm.wins[k].used && omni_app_for_title(g_wm.wins[k].title) == g_pop.app)
                omni_wm_request_close(&g_wm, &g_wm.wins[k]);
        break;
    default:
        break;
    }
}

/* keys while a popup menu is open */
static int pop_key(int code, int pressed)
{
    if (!pressed)
        return 1;
    switch (code) {
    case OMNI_KEY_ESC:
        g_pop.open = 0;
        break;
    case OMNI_KEY_UP:
        g_pop.sel = g_pop.sel <= 0 ? g_pop.n - 1 : g_pop.sel - 1;
        break;
    case OMNI_KEY_DOWN:
        g_pop.sel = (g_pop.sel + 1) % g_pop.n;
        break;
    case OMNI_KEY_ENTER: case OMNI_KEY_KPENTER: case OMNI_KEY_SPACE:
        pop_do(g_pop.sel >= 0 ? g_pop.sel : 0);
        break;
    default:
        break;
    }
    return 1;
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
    if (!g_locked)
        draw_desktop_icons(&wm->screen);
}

/* after the windows: always-on-top chrome, then present the frame */
/* ------------------------------------------------------------------ */
/* lock + sign-in screen                                              */
/* ------------------------------------------------------------------ */

/* dimmed wallpaper (lock screen), cached per style */
static void lock_backgrounds(void)
{
    size_t n = (size_t)g_dev.w * (size_t)g_dev.h, i;
    if (g_lockbg_wall == g_set_wall && g_lockbg)
        return;
    if (!g_lockbg)
        g_lockbg = malloc(n * sizeof(uint32_t));
    if (!g_lockbg || !g_wall)
        return;
    for (i = 0; i < n; i++)
        g_lockbg[i] = th_blend(g_wall[i], 0x000000, 70);
    g_lockbg_wall = g_set_wall;
}

/* frosted wallpaper (sign in), cached per style. Made when the sign-in
 * screen is first shown rather than with the lock screen: blurring the
 * whole screen is the slowest step of drawing the lock screen, and the
 * lock screen is what the user waits for at startup. */
static void sign_background(void)
{
    size_t n = (size_t)g_dev.w * (size_t)g_dev.h, i;
    struct raster fr;
    if (g_signbg_wall == g_set_wall && g_signbg)
        return;
    if (!g_signbg)
        g_signbg = malloc(n * sizeof(uint32_t));
    if (!g_signbg || !g_wall)
        return;
    memcpy(g_signbg, g_wall, n * sizeof(uint32_t));
    raster_init(&fr, g_signbg, g_dev.w, g_dev.h, g_dev.w);
    th_frost(&fr, 0, 0, g_dev.w, g_dev.h, 0, 14);
    for (i = 0; i < n; i++)
        g_signbg[i] = th_blend(g_signbg[i], 0x000000, 100);
    g_signbg_wall = g_set_wall;
}

static void shell_lock(void)
{
    g_locked = 1;
    g_lock_stage = 0;
    g_pw_bad = 0;
    memset(g_pw, 0, sizeof(g_pw));
    close_flyouts();
    g_toast_on = 0;
    lock_backgrounds();
    g_wm.dirty = 1;
}

static void shell_unlock(void)
{
    g_locked = 0;
    memset(g_pw, 0, sizeof(g_pw));
    g_wm.dirty = 1;
}

static void text_center(struct raster *r, const char *s, int y, uint32_t rgb)
{
    th_text(r, s, (r->w - (int)strlen(s) * 8) / 2, y, rgb);
}

/* geometry of the sign-in controls */
static void signin_geom(int *cy, int *bx, int *by, int *bw, int *bh)
{
    *cy = g_screen.h * 26 / 100;
    *bw = omni_account_has_password() ? 300 : 160;
    *bh = 40;
    *bx = (g_screen.w - *bw) / 2;
    *by = *cy + 176;
}

/* the sign-in screen's power button */
static int sign_power_hit(int x, int y)
{
    return g_locked && g_lock_stage == 1 &&
           in_rect(x, y, g_screen.w - 60, g_screen.h - 58, 40, 40);
}

static void draw_lock(void)
{
    struct raster *r = &g_screen;
    time_t t = time(NULL);
    struct tm tmv;
    char buf[64];

    localtime_r(&t, &tmv);
    if (g_lock_stage == 0) {                     /* the clock */
        int ty = r->h * 15 / 100, tw;
        if (g_lockbg)
            memcpy(r->bits, g_lockbg, (size_t)r->w * (size_t)r->h * sizeof(uint32_t));
        if (g_set.clock24)
            snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        else
            snprintf(buf, sizeof(buf), "%d:%02d", tmv.tm_hour % 12 ? tmv.tm_hour % 12 : 12, tmv.tm_min);
        tw = th_text_big_width(buf, 8);
        th_text_big(r, buf, (r->w - tw) / 2 + 2, ty + 2, 8, 0x000000);   /* soft shadow */
        th_text_big(r, buf, (r->w - tw) / 2, ty, 8, 0xffffff);
        strftime(buf, sizeof(buf), "%A, %B %d", &tmv);
        tw = th_text2x_width(buf);
        th_text2x(r, buf, (r->w - tw) / 2, ty + 84, 0xf1f5f9);
        th_icon(r, r->w / 2 - 14, r->h - 118, 28, 0xf59e0b, 'k');
        text_center(r, "Press a key or click to sign in", r->h - 72, 0xe2e8f0);
        th_icon(r, r->w - 50, r->h - 48, 20, 0, 'e');           /* network */
        net_badge(r, r->w - 37, r->h - 37, 0x101010);
    } else {                                     /* sign in */
        int cy, bx, by, bw, bh, nw;
        const char *name = omni_account_display();
        sign_background();
        if (g_signbg)
            memcpy(r->bits, g_signbg, (size_t)r->w * (size_t)r->h * sizeof(uint32_t));
        signin_geom(&cy, &bx, &by, &bw, &bh);
        th_icon(r, r->w / 2 - 60, cy, 120, TH_ACCENT, 'u');
        nw = th_text2x_width(name);
        th_text2x(r, name, (r->w - nw) / 2, cy + 138, 0xffffff);
        if (omni_account_has_password()) {
            int i, n = (int)strlen(g_pw);
            th_round_rect(r, bx - 1, by - 1, bw + 2, bh + 2, 9, 0xffffff, 90);
            th_round_rect(r, bx, by, bw, bh, 8, 0xffffff, 238);
            if (n == 0)
                th_text(r, "Password", bx + 16, by + 16, 0x6b7280);
            for (i = 0; i < n && i < 17; i++)
                th_round_rect(r, bx + 16 + i * 14, by + 16, 8, 8, 4, 0x1f2937, 255);
            if (n < 17)
                th_fill_a(r, bx + 16 + n * 14, by + 11, 2, 18, 0x1f2937, 255);
            th_round_rect(r, bx + bw - 36, by + 4, 32, 32, 6, TH_ACCENT, 255);
            th_text_bold(r, ">", bx + bw - 24, by + 16, 0xffffff);
            if (g_pw_bad)
                text_center(r, "The password is incorrect. Try again.", by + bh + 20, 0xfecaca);
            else
                text_center(r, "Enter your password, then press Enter", by + bh + 20, 0xcbd5e1);
        } else {
            th_round_rect(r, bx, by, bw, bh, 8, TH_ACCENT, 255);
            th_text(r, "Sign in", bx + (bw - 7 * 8) / 2, by + 16, 0xffffff);
        }
        text_center(r, "Esc: back to the lock screen", r->h - 48, 0x94a3b8);

        /* like Windows: the account at the bottom left, network and
         * power at the bottom right */
        th_round_rect(r, 16, r->h - 76, 232, 56, 8, 0xffffff, 34);
        th_icon(r, 28, r->h - 68, 40, TH_ACCENT, 'u');
        th_text_clip(r, name, 80, r->h - 52, 0xffffff, 156);
        th_icon(r, r->w - 98, r->h - 48, 20, 0, 'e');
        net_badge(r, r->w - 85, r->h - 37, 0x202020);
        if (g_hover == HV_SIGN_POWER || (g_pop.open && g_pop.owner == POP_POWER_SIGNIN))
            th_round_rect(r, r->w - 60, r->h - 58, 40, 40, 6, 0xffffff, 36);
        th_icon(r, r->w - 50, r->h - 48, 20, 0, 'P');
    }
}

static void signin_submit(void)
{
    if (!omni_account_has_password() || omni_account_check(g_pw)) {
        shell_unlock();
        return;
    }
    g_pw_bad = 1;
    memset(g_pw, 0, sizeof(g_pw));
}

/* a key while locked; returns 1 if the screen changed */
static int lock_key(int code, int pressed, char text)
{
    size_t n = strlen(g_pw);
    if (g_pop.open)
        return pop_key(code, pressed);
    if (!pressed)
        return 0;
    if (g_lock_stage == 0) {
        g_lock_stage = 1;
        g_pw_bad = 0;
        return 1;
    }
    switch (code) {
    case OMNI_KEY_ESC:
        g_lock_stage = 0;
        memset(g_pw, 0, sizeof(g_pw));
        return 1;
    case OMNI_KEY_ENTER: case OMNI_KEY_KPENTER:
        signin_submit();
        return 1;
    case OMNI_KEY_BACKSPACE:
        if (n)
            g_pw[n - 1] = '\0';
        return 1;
    default:
        if (text >= 32 && text < 127 && n + 1 < sizeof(g_pw) && omni_account_has_password()) {
            g_pw[n] = text;
            g_pw[n + 1] = '\0';
            g_pw_bad = 0;
            return 1;
        }
        if (code == OMNI_KEY_SPACE && !omni_account_has_password())
            signin_submit();
        return 1;
    }
}

/* a click while locked */
static int lock_button(int x, int y)
{
    int cy, bx, by, bw, bh, k;
    if (g_pop.open) {                            /* the power menu */
        k = pop_hit(x, y);
        if (k >= HV_POP_ITEM)
            pop_do(k - HV_POP_ITEM);
        else if (!k)
            g_pop.open = 0;
        return 1;
    }
    if (g_lock_stage == 0) {
        g_lock_stage = 1;
        return 1;
    }
    if (sign_power_hit(x, y)) {
        power_menu(POP_POWER_SIGNIN, g_screen.w - 20, g_screen.h - 58);
        return 1;
    }
    signin_geom(&cy, &bx, &by, &bw, &bh);
    if (omni_account_has_password()) {
        if (x >= bx + bw - 36 && x < bx + bw && y >= by && y < by + bh)
            signin_submit();                     /* the arrow */
    } else if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
        signin_submit();
    }
    return 1;
}

static void shell_finish(struct omni_wm *wm)
{
    (void)wm;
    if (g_locked) {
        draw_lock();
        draw_pop();
        present_scene();
        return;
    }
    draw_taskbar();
    draw_toast();
    if (g_menu_open)
        draw_menu();
    if (g_qs_open)
        draw_qs();
    if (g_cal_open)
        draw_cal();
    draw_pop();
    draw_banner();
    present_scene();
}

/* ------------------------------------------------------------------ */
/* input -> shell / server routing                                    */
/* ------------------------------------------------------------------ */

/* the chrome element under the pointer (HV_*), 0 if none */
static int hover_key(void)
{
    struct omni_win *w;
    int k;

    if (g_locked) {
        if ((k = pop_hit(g_px, g_py)))
            return k;
        return sign_power_hit(g_px, g_py) ? HV_SIGN_POWER : 0;
    }
    if ((k = pop_hit(g_px, g_py)))
        return k;
    if (g_menu_open && (k = sm_hit(g_px, g_py)))
        return k;
    if (g_qs_open && (k = qs_hit(g_px, g_py)))
        return k;
    if (g_cal_open && (k = cal_hit(g_px, g_py)))
        return k;
    if ((k = toast_hit(g_px, g_py)))
        return k;
    if (g_py >= g_screen.h - TASKBAR_H)
        return tb_hit(g_px);
    w = omni_wm_at(&g_wm, g_px, g_py);
    if (w) {
        int z = omni_wm_hit(w, g_px, g_py);
        if (z == 2 || z == 10)
            return HV_CAPTION + w->id * 16 + z;
        return 0;
    }
    k = desk_hit(g_px, g_py);
    return k >= 0 ? HV_DESK + k : 0;
}

/* returns 1 if the hovered element changed (the frame needs redrawing) */
static int update_hover(void)
{
    int k = hover_key();
    if (k == g_hover)
        return 0;
    g_hover = k;
    if (g_menu_open && k >= HV_ITEM && k < HV_ITEM + g_res_n)
        g_menu_sel = k - HV_ITEM;
    if (g_pop.open && k >= HV_POP_ITEM && k < HV_POP_ITEM + g_pop.n)
        g_pop.sel = k - HV_POP_ITEM;
    return 1;
}

/* a press on the taskbar; was_* = that flyout was open before the click
 * (its own button then just closes it) */
static void tb_press(int x, int btn, int was_menu, int was_qs, int was_cal)
{
    struct tb_item it[TB_MAX];
    int n = tb_layout(it), k = tb_hit(x);

    if (k == HV_TRAY_UPD) {
        open_settings("update");
    } else if (k == HV_TRAY_NET) {
        if (!was_qs)
            open_qs();
    } else if (k == HV_TRAY_CLOCK) {
        if (!was_cal)
            open_cal();
    } else if (k >= HV_TB && k < HV_TB + n) {
        const struct tb_item *t = &it[k - HV_TB];
        if (btn == 3)
            tb_menu(t, it[0].x);
        else if (btn != 1)
            return;
        else if (t->kind == TBI_START || t->kind == TBI_SEARCH) {
            if (!was_menu)
                open_menu();
        } else {
            tb_click(t);
        }
    }
}

/* a pointer press: flyouts, popups, taskbar, desktop icons; returns 1
 * if the shell took it (else it goes to the window manager) */
static int shell_button(int x, int y, int btn)
{
    int k, on_bar = y >= g_screen.h - TASKBAR_H;
    int was_menu = g_menu_open, was_qs = g_qs_open, was_cal = g_cal_open;

    if (g_pop.open) {                           /* a popup menu first */
        k = pop_hit(x, y);
        if (k >= HV_POP_ITEM) {
            if (btn == 1)
                pop_do(k - HV_POP_ITEM);
            return 1;
        }
        if (k)
            return 1;
        g_pop.open = 0;                         /* a click elsewhere closes it */
        if (!on_bar)
            return 1;
    }
    if (g_menu_open) {
        if ((k = sm_hit(x, y))) {
            sm_click(k, btn);
            return 1;
        }
        close_menu();
        if (!on_bar)
            return 1;
    }
    if (g_qs_open) {
        if ((k = qs_hit(x, y))) {
            if (btn == 1)
                qs_click(k);
            return 1;
        }
        g_qs_open = 0;
        if (!on_bar)
            return 1;
    }
    if (g_cal_open) {
        if ((k = cal_hit(x, y))) {
            if (btn == 1)
                cal_click(k);
            return 1;
        }
        g_cal_open = 0;
        if (!on_bar)
            return 1;
    }

    switch (toast_hit(x, y)) {          /* the update notification */
    case HV_TOAST_GO:
        g_toast_on = 0;
        if (g_toast_kind == UPD_READY)
            power(1);                   /* init restarts into the update */
        else
            run_app("/usr/bin/omnios-update", "install");
        return 1;
    case HV_TOAST_LATER:
    case HV_TOAST_X:
        g_toast_on = 0;
        return 1;
    case HV_TOAST:
        g_toast_on = 0;
        open_settings("update");
        return 1;
    }

    if (on_bar) {
        tb_press(x, btn, was_menu, was_qs, was_cal);
        return 1;                       /* the taskbar is on top of everything */
    }
    if (!omni_wm_at(&g_wm, x, y))
        return desk_click(x, y, btn);
    if (g_desk_sel >= 0) {
        g_desk_sel = -1;
        g_wm.dirty = 1;
    }
    return 0;
}

/* a key: the Windows key and its shortcuts, then open menus and flyouts.
 * Returns 1 if the shell took the key. */
static int shell_key(int code, int pressed, char text)
{
    if (code == KEY_LEFTMETA || code == KEY_RIGHTMETA) {
        if (pressed == 1) {                     /* Start opens on release, */
            g_meta_held = 1;                    /* unless it was a chord   */
            g_meta_used = 0;
        } else if (pressed == 0) {
            if (g_meta_held && !g_meta_used) {
                if (g_menu_open)
                    close_menu();
                else
                    open_menu();
            }
            g_meta_held = 0;
        }
        return 1;
    }
    if (g_meta_held && pressed) {               /* Windows + key */
        g_meta_used = 1;
        if (pressed != 1)
            return 1;
        switch (code) {
        case KEY_L: close_flyouts(); shell_lock(); break;
        case KEY_A: if (g_qs_open) close_flyouts(); else open_qs(); break;
        case KEY_N: if (g_cal_open) close_flyouts(); else open_cal(); break;
        case KEY_I: close_flyouts(); open_settings(NULL); break;
        case KEY_E: close_flyouts(); run_app("/usr/bin/omnios-files", NULL); break;
        case KEY_D: close_flyouts(); show_desktop(); break;
        case KEY_S: if (!g_menu_open) open_menu(); break;
        case KEY_X: {
            struct tb_item it[TB_MAX];
            tb_layout(it);
            quick_links(it[0].x, g_screen.h - TASKBAR_H);
            break;
        }
        default: break;
        }
        return 1;
    }
    if (g_pop.open)
        return pop_key(code, pressed);
    if (g_menu_open)
        return sm_key(code, pressed, text);
    if ((g_qs_open || g_cal_open) && pressed && code == OMNI_KEY_ESC) {
        close_flyouts();
        return 1;
    }
    return desk_key(code, pressed);
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
        { 'S', 0x3b82f6, "Open Start with the Windows key, then type to search." },
        { 'T', 0x10b981, "Click taskbar icons to open apps and switch windows." },
        { 'M', 0x8b5cf6, "Drag title bars to move windows, edges to resize." },
        { 'Q', 0x0ea5e9, "Network and quick settings: click the network icon." },
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
    if (g_set.signin)
        shell_lock();                           /* sign in first */
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
    {
        int k;
        for (k = 0; k < TB_PINS; k++) {
            int i = omni_app_find(g_pin_ids[k]);
            g_pin_ok[k] = i >= 0 && access(omni_catalog[i].path, X_OK) == 0;
        }
    }
    menu_reload();
    omni_net_read(&g_net);

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
    time_t settings_checked = 0, net_checked = 0;
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
    {
        /* the kernel log's time stamp on this line is the boot time */
        char line[64];
        snprintf(line, sizeof(line), "desktop: ready, %dx%d\n", g_screen.w, g_screen.h);
        omni_console_puts(line);
    }

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
                if (!g_locked)
                    omni_wm_motion(&g_wm, g_px, g_py);  /* drags mark dirty */
                moved = 1;
            } else if (e.type == 3 && e.key >= 4 && e.key <= 7) {
                /* the wheel scrolls Start's apps, or the window under
                 * the pointer */
                if (!e.pressed || g_locked) {
                    /* nothing */
                } else if (g_menu_open) {
                    if (sm_hit(g_px, g_py)) {
                        sm_wheel(e.key);
                        need = 1;
                    }
                } else if (!g_qs_open && !g_cal_open && !g_pop.open &&
                           g_py < g_screen.h - TASKBAR_H && !toast_hit(g_px, g_py)) {
                    omni_wm_wheel(&g_wm, g_px, g_py, e.key);
                }
            } else if (e.type == 3) {                 /* button */
                if (g_locked) {
                    if (e.pressed && lock_button(g_px, g_py))
                        need = 1;
                } else if (e.pressed && shell_button(g_px, g_py, e.key))
                    need = 1;
                else
                    omni_wm_button(&g_wm, g_px, g_py, e.key, e.pressed);
            } else if (e.type == 1) {                 /* key */
                if (g_locked) {
                    if (lock_key(e.key, e.pressed, e.text))
                        need = 1;
                } else if (shell_key(e.key, e.pressed, e.text))
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
            if (shell_poll_update())            /* OmniOS Update news? */
                need = 1;
        }
        if (t - net_checked >= 2) {             /* network status */
            struct omni_netinfo n;
            net_checked = t;
            omni_net_read(&n);
            if (n.state != g_net.state || strcmp(n.ip, g_net.ip) != 0)
                need = 1;
            g_net = n;
        }
        if (g_toast_pending && !g_locked && !g_qs_open && !g_cal_open) {
            g_toast_pending = 0;
            g_toast_on = 1;
            g_toast_t = t;
            need = 1;
        }
        if (g_toast_on && t - g_toast_t >= TOAST_SECS &&
            !(g_hover >= HV_TOAST && g_hover <= HV_TOAST_X)) {
            g_toast_on = 0;
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
