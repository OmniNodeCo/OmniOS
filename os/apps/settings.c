/*
 * OmniOS — os/apps/settings.c
 *
 * Settings, in the style of Windows 11: a navigation pane on the left and
 * pages of cards on the right.
 *
 *   System           display, processor, memory, uptime
 *   Personalization  wallpaper and accent colour
 *   Apps             installed apps, link to the App Store
 *   Accounts         password, sign-in screen, lock screen
 *   Time & language  time zone, 24-hour clock
 *   OmniOS Update    check / download / restart, automatic updates, history
 *
 * Changes are saved to the settings file at once; the desktop applies them
 * within a second. Mouse: click anything. Keyboard: Up/Down change page,
 * Tab moves between controls, Left/Right adjust, Enter/Space activate,
 * Esc closes.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../gui/account.h"
#include "../gui/catalog.h"
#include "../gui/client.h"
#include "../gui/settings.h"
#include "../gui/updstat.h"
#include "../gui/wm.h"          /* OMNI_WM_TITLE_H */

#define WIN_W    800
#define WIN_H    566
#define NAV_W    232
#define PAD      24
#define CARD_X   (NAV_W + PAD)
#define BTN_H    30

#define C_NAV    0xf0f2f5
#define C_BG     0xf7f8fa
#define C_CARD   0xffffff
#define C_BORDER 0xe2e5ea
#define C_INK    0x1b1f24
#define C_DIM    0x5f6b7a
#define C_SEL    0xe2e6ed
#define C_BTN    0xfbfbfc
#define C_BTN_BD 0xcfd4db
#define C_OK     0x15803d
#define C_ERR    0xc42b1c

enum { K_ESC = 1, K_BACKSPACE = 14, K_TAB = 15, K_ENTER = 28, K_SPACE = 57,
       K_KPENTER = 96, K_UP = 103, K_LEFT = 105, K_RIGHT = 106, K_DOWN = 108 };

enum { PG_SYSTEM, PG_PERSONAL, PG_APPS, PG_ACCOUNTS, PG_TIME, PG_UPDATE, PG_COUNT };
static const struct { const char *name; char icon; } g_pages[PG_COUNT] = {
    { "System", 'm' }, { "Personalization", 'p' }, { "Apps", 'a' },
    { "Accounts", 'u' }, { "Time & language", 't' }, { "OmniOS Update", 'U' },
};

/* clickable zones, rebuilt on every redraw */
enum { Z_NAV, Z_WALL, Z_ACCENT, Z_TOGGLE, Z_ZPREV, Z_ZNEXT, Z_BUTTON, Z_FIELD };
enum { T_CLOCK24, T_AUTOUPD, T_SIGNIN };
enum { B_STORE, B_PW_OPEN, B_PW_SAVE, B_PW_CANCEL, B_UPD_CHECK, B_UPD_INSTALL,
       B_UPD_RESTART };

struct zone { int x, y, w, h, kind, arg; };

struct app {
    struct omni_client_conn c;
    int w, h;                       /* content size                      */
    int page;
    struct omni_settings set;
    struct zone z[48];
    int nz, focus;                  /* focus: zone index, -1 = none       */
    /* Accounts: password form */
    int pw_open, pw_field, pw_ok;
    char pw[3][64];                 /* current, new, confirm              */
    char pw_msg[96];
    /* OmniOS Update */
    struct omni_update_status us;
};

static struct app g_a;

static uint32_t accent(const struct app *a)
{
    return omni_accents[a->set.accent % omni_accents_n].rgb;
}

static void add_zone(struct app *a, int x, int y, int w, int h, int kind, int arg)
{
    if (a->nz < (int)(sizeof(a->z) / sizeof(a->z[0]))) {
        struct zone *z = &a->z[a->nz++];
        z->x = x; z->y = y; z->w = w; z->h = h; z->kind = kind; z->arg = arg;
    }
}

/* ------------------------------------------------------------------ */
/* widgets                                                            */
/* ------------------------------------------------------------------ */

static int card_w(const struct app *a) { return a->w - NAV_W - 2 * PAD; }

static void card(struct app *a, int y, int h)
{
    omni_client_rfill(&a->c, CARD_X, y, card_w(a), h, 8, C_BORDER);
    omni_client_rfill(&a->c, CARD_X + 1, y + 1, card_w(a) - 2, h - 2, 7, C_CARD);
}

/* a one-line card: title + description on the left */
static void row_card(struct app *a, int y, int h, const char *title, const char *desc)
{
    card(a, y, h);
    omni_client_textt(&a->c, CARD_X + 18, y + (desc ? h / 2 - 11 : h / 2 - 4), C_INK, title);
    if (desc)
        omni_client_textt(&a->c, CARD_X + 18, y + h / 2 + 5, C_DIM, desc);
}

static void right_text(struct app *a, int y, uint32_t fg, const char *s)
{
    omni_client_textt(&a->c, CARD_X + card_w(a) - 18 - (int)strlen(s) * 8, y, fg, s);
}

/* right-aligned value that must not run into `left_chars` of card text */
static void right_fit(struct app *a, int y, uint32_t fg, const char *s, int left_chars)
{
    char buf[128];
    int room = (card_w(a) - 36 - left_chars * 8 - 24) / 8, n = (int)strlen(s);
    if (room < 4)
        room = 4;
    if (n > room)
        snprintf(buf, sizeof(buf), "%.*s..", room - 2, s);
    else
        snprintf(buf, sizeof(buf), "%s", s);
    right_text(a, y, fg, buf);
}

static void toggle(struct app *a, int y, int on, int arg)      /* right-aligned */
{
    int x = CARD_X + card_w(a) - 18 - 44;
    struct omni_client_conn *c = &a->c;
    omni_client_textt(c, x - 36, y + 7, C_INK, on ? "On" : "Off");
    if (on) {
        omni_client_rfill(c, x, y, 44, 22, 11, accent(a));
        omni_client_rfill(c, x + 26, y + 4, 14, 14, 7, 0xffffff);
    } else {
        omni_client_rfill(c, x, y, 44, 22, 11, 0x7b8594);
        omni_client_rfill(c, x + 1, y + 1, 42, 20, 10, C_CARD);
        omni_client_rfill(c, x + 5, y + 5, 12, 12, 6, 0x5f6b7a);
    }
    add_zone(a, x, y, 44, 22, Z_TOGGLE, arg);
}

static void button(struct app *a, int x, int y, int w, const char *label,
                   int primary, int arg)
{
    struct omni_client_conn *c = &a->c;
    int tx = x + (w - (int)strlen(label) * 8) / 2;
    if (primary) {
        omni_client_rfill(c, x, y, w, BTN_H, 6, accent(a));
        omni_client_textt(c, tx, y + 11, 0xffffff, label);
    } else {
        omni_client_rfill(c, x, y, w, BTN_H, 6, C_BTN_BD);
        omni_client_rfill(c, x + 1, y + 1, w - 2, BTN_H - 2, 5, C_BTN);
        omni_client_textt(c, tx, y + 11, C_INK, label);
    }
    add_zone(a, x, y, w, BTN_H, Z_BUTTON, arg);
}

static void focus_ring(struct app *a)
{
    const struct zone *z;
    struct omni_client_conn *c = &a->c;
    if (a->focus < 0 || a->focus >= a->nz)
        return;
    z = &a->z[a->focus];
    omni_client_fill(c, z->x - 3, z->y - 3, z->w + 6, 2, C_INK);
    omni_client_fill(c, z->x - 3, z->y + z->h + 1, z->w + 6, 2, C_INK);
    omni_client_fill(c, z->x - 3, z->y - 3, 2, z->h + 6, C_INK);
    omni_client_fill(c, z->x + z->w + 1, z->y - 3, 2, z->h + 6, C_INK);
}

/* ------------------------------------------------------------------ */
/* system facts                                                       */
/* ------------------------------------------------------------------ */

static void read_line_of(const char *path, const char *key, char *out, size_t n)
{
    char line[256];
    size_t k = strlen(key);
    FILE *f = fopen(path, "r");
    out[0] = '\0';
    if (!f)
        return;
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, key, k) == 0) {
            char *v = strchr(line, ':');
            if (v) {
                v++;
                while (*v == ' ' || *v == '\t')
                    v++;
                v[strcspn(v, "\n")] = '\0';
                snprintf(out, n, "%s", v);
            }
            break;
        }
    fclose(f);
}

static long meminfo_kb(const char *key)
{
    char v[64];
    read_line_of("/proc/meminfo", key, v, sizeof(v));
    return atol(v);
}

/* ------------------------------------------------------------------ */
/* pages                                                              */
/* ------------------------------------------------------------------ */

static void page_system(struct app *a)
{
    struct omni_client_conn *c = &a->c;
    char host[64] = "omnios", s[128], cpu[96];
    long total = meminfo_kb("MemTotal"), avail = meminfo_kb("MemAvailable");
    double up = 0;
    FILE *f;
    int y = 62;

    gethostname(host, sizeof(host) - 1);
    card(a, y, 92);
    omni_client_icon(c, CARD_X + 18, y + 18, 56, 0x38bdf8, 'm');
    omni_client_text2(c, CARD_X + 92, y + 24, C_INK, host);
    snprintf(s, sizeof(s), "OmniOS %s", omni_update_running_version()[0] ?
             omni_update_running_version() : "(development build)");
    omni_client_textt(c, CARD_X + 92, y + 52, C_DIM, s);
    y += 104;

    snprintf(s, sizeof(s), "%d x %d", c->screen_w, c->screen_h);
    row_card(a, y, 52, "Display", "Screen resolution");
    right_text(a, y + 22, C_INK, s);
    y += 60;

    read_line_of("/proc/cpuinfo", "model name", cpu, sizeof(cpu));
    snprintf(s, sizeof(s), "%ld x %.60s", sysconf(_SC_NPROCESSORS_ONLN),
             cpu[0] ? cpu : "x86-64");
    row_card(a, y, 52, "Processor", "Cores and model");
    right_fit(a, y + 22, C_INK, s, 15);
    y += 60;

    snprintf(s, sizeof(s), "%ld MB used of %ld MB", (total - avail) / 1024, total / 1024);
    row_card(a, y, 52, "Memory", "Installed RAM");
    right_text(a, y + 22, C_INK, s);
    y += 60;

    if ((f = fopen("/proc/uptime", "r"))) {
        if (fscanf(f, "%lf", &up) != 1)
            up = 0;
        fclose(f);
    }
    snprintf(s, sizeof(s), "%ld h %02ld min", (long)up / 3600, ((long)up / 60) % 60);
    row_card(a, y, 52, "Uptime", "Time since OmniOS started");
    right_text(a, y + 22, C_INK, s);
    y += 60;

    row_card(a, y, 52, "Storage", "OmniOS runs from memory: changes last until you restart");
}

static void wall_thumb(struct app *a, int x, int y, int w, int h, int i)
{
    const struct omni_wallinfo *wi = &omni_walls[i];
    struct omni_client_conn *c = &a->c;
    int k;
    omni_client_grad(c, x, y, w, h, wi->dark, wi->mid, 0);
    for (k = 0; k < 9; k++) {                   /* a ribbon of light */
        int px = x + 2 + k * (w - 8) / 8;
        int py = y + h * 3 / 5 + ((k % 4 == 1) ? -3 : (k % 4 == 3 ? 3 : 0));
        omni_client_rfill(c, px, py, (w - 8) / 8 + 3, 3, 1, wi->ribbon);
    }
}

static void page_personal(struct app *a)
{
    struct omni_client_conn *c = &a->c;
    int y = 62, i, x0 = CARD_X + 18, cw = card_w(a);
    int tw = (cw - 36 - 5 * 8) / 6, th = tw * 5 / 8;
    uint32_t ac = accent(a);
    char s[64];

    /* preview: a small desktop with a window and the taskbar */
    card(a, y, 170);
    wall_thumb(a, x0, y + 18, 224, 134, a->set.wallpaper);
    omni_client_rfill(c, x0 + 58, y + 44, 110, 64, 5, 0xffffff);
    omni_client_fill(c, x0 + 58 + 5, y + 44 + 14, 100, 1, C_BORDER);
    omni_client_rfill(c, x0 + 66, y + 66, 60, 5, 2, ac);
    omni_client_rfill(c, x0 + 66, y + 78, 84, 4, 2, C_BORDER);
    omni_client_fill(c, x0, y + 18 + 134 - 14, 224, 14, 0x0b1020);
    omni_client_rfill(c, x0 + 100, y + 18 + 134 - 4, 24, 2, 1, ac);
    omni_client_textt(c, x0 + 248, y + 40, C_DIM, "Background");
    omni_client_text2(c, x0 + 248, y + 54, C_INK, omni_walls[a->set.wallpaper % omni_walls_n].name);
    omni_client_textt(c, x0 + 248, y + 96, C_DIM, "Accent color");
    omni_client_text2(c, x0 + 248, y + 110, C_INK, omni_accents[a->set.accent % omni_accents_n].name);
    y += 182;

    card(a, y, 60 + th + 18);
    omni_client_textt(c, x0, y + 16, C_INK, "Background");
    omni_client_textt(c, x0 + 88, y + 16, C_DIM, "Choose a picture for your desktop");
    for (i = 0; i < omni_walls_n; i++) {
        int tx = x0 + i * (tw + 8), ty = y + 38;
        if (i == a->set.wallpaper)
            omni_client_rfill(c, tx - 3, ty - 3, tw + 6, th + 6, 6, ac);
        wall_thumb(a, tx, ty, tw, th, i);
        snprintf(s, sizeof(s), "%s", omni_walls[i].name);
        omni_client_textt(c, tx + (tw - (int)strlen(s) * 8) / 2, ty + th + 8,
                          i == a->set.wallpaper ? C_INK : C_DIM, s);
        add_zone(a, tx, ty, tw, th + 18, Z_WALL, i);
    }
    y += 60 + th + 30;

    card(a, y, 92);
    omni_client_textt(c, x0, y + 16, C_INK, "Accent color");
    omni_client_textt(c, x0 + 112, y + 16, C_DIM, "Used for highlights on the taskbar and Start");
    for (i = 0; i < omni_accents_n; i++) {
        int sx = x0 + 4 + i * 46, sy = y + 44;
        if (i == a->set.accent) {
            omni_client_rfill(c, sx - 5, sy - 5, 40, 40, 20, C_INK);
            omni_client_rfill(c, sx - 3, sy - 3, 36, 36, 18, C_CARD);
        }
        omni_client_rfill(c, sx, sy, 30, 30, 15, omni_accents[i].rgb);
        add_zone(a, sx - 4, sy - 4, 38, 38, Z_ACCENT, i);
    }
}

static void page_apps(struct app *a)
{
    struct omni_client_conn *c = &a->c;
    unsigned char inst[OMNI_CATALOG_MAX];
    int y = 62, i, n = 0, shown = 0, rows, cw = card_w(a);
    char s[64];

    omni_apps_load(inst);
    for (i = 0; i < omni_catalog_n; i++)
        n += inst[i] ? 1 : 0;
    rows = (a->h - 62 - 104 - 64) / 34;
    if (rows > n)
        rows = n;
    card(a, y, 50 + rows * 34 + (n > rows ? 20 : 0));
    omni_client_textt(c, CARD_X + 18, y + 18, C_INK, "Installed apps");
    snprintf(s, sizeof(s), "%d apps", n);
    right_text(a, y + 18, C_DIM, s);
    omni_client_fill(c, CARD_X + 18, y + 40, cw - 36, 1, C_BORDER);
    for (i = 0; i < omni_catalog_n && shown < rows; i++) {
        const struct omni_app_info *ap = &omni_catalog[i];
        int ry = y + 46 + shown * 34;
        if (!inst[i])
            continue;
        omni_client_icon(c, CARD_X + 18, ry + 5, 24, ap->color, ap->glyph);
        omni_client_textt(c, CARD_X + 54, ry + 13, C_INK, ap->name);
        right_text(a, ry + 13, C_DIM, ap->system ? "System app" : ap->category);
        shown++;
    }
    if (n > rows) {
        snprintf(s, sizeof(s), "and %d more", n - rows);
        omni_client_textt(c, CARD_X + 54, y + 46 + rows * 34 + 4, C_DIM, s);
    }
    y += 50 + rows * 34 + (n > rows ? 20 : 0) + 12;
    row_card(a, y, 64, "Get more apps", "Games, tools and utilities from the App Store");
    button(a, CARD_X + cw - 18 - 150, y + 17, 150, "Open App Store", 1, B_STORE);
}

static void field(struct app *a, int y, const char *label, int idx)
{
    struct omni_client_conn *c = &a->c;
    int x = CARD_X + 200, w = 220, n = (int)strlen(a->pw[idx]), i;
    char stars[64];
    omni_client_textt(c, CARD_X + 18, y + 11, C_INK, label);
    omni_client_rfill(c, x, y, w, 30, 5, a->pw_field == idx ? accent(a) : C_BTN_BD);
    omni_client_rfill(c, x + 1, y + 1, w - 2, 28, 4, C_CARD);
    for (i = 0; i < n && i < 24; i++)
        stars[i] = '*';
    stars[i] = '\0';
    omni_client_textt(c, x + 10, y + 11, C_INK, stars);
    if (a->pw_field == idx)
        omni_client_fill(c, x + 10 + i * 8 + 1, y + 8, 2, 14, C_INK);   /* caret */
    add_zone(a, x, y, w, 30, Z_FIELD, idx);
}

static void page_accounts(struct app *a)
{
    struct omni_client_conn *c = &a->c;
    int y = 62, cw = card_w(a), has = omni_account_has_password();
    char s[128];

    card(a, y, 100);
    omni_client_icon(c, CARD_X + 18, y + 18, 64, accent(a), 'u');
    omni_client_text2(c, CARD_X + 100, y + 30, C_INK, omni_account_display());
    snprintf(s, sizeof(s), "%s  -  Administrator  -  Local account", omni_account_login());
    omni_client_textt(c, CARD_X + 100, y + 58, C_DIM, s);
    y += 112;

    if (!a->pw_open) {
        row_card(a, y, 64, "Password", a->pw_msg[0] ? " " :
                 (has ? "Your account is protected with a password"
                      : "No password: anyone can sign in"));
        if (a->pw_msg[0])
            omni_client_textt(c, CARD_X + 18, y + 37, a->pw_ok ? C_OK : C_ERR, a->pw_msg);
        button(a, CARD_X + cw - 18 - 110, y + 17, 110, has ? "Change" : "Add", 0, B_PW_OPEN);
        y += 76;
    } else {
        int fy = y + 48, h = 48 + (has ? 3 : 2) * 40 + 88;
        card(a, y, h);
        omni_client_textt(c, CARD_X + 18, y + 18, C_INK, has ? "Change your password" : "Add a password");
        omni_client_textt(c, CARD_X + 250, y + 18, C_DIM, "Leave both new fields empty to remove it");
        if (has) {
            field(a, fy, "Current password", 0);
            fy += 40;
        }
        field(a, fy, "New password", 1);
        fy += 40;
        field(a, fy, "Confirm password", 2);
        fy += 48;
        button(a, CARD_X + 200, fy, 104, "Save", 1, B_PW_SAVE);
        button(a, CARD_X + 316, fy, 104, "Cancel", 0, B_PW_CANCEL);
        if (a->pw_msg[0])
            omni_client_textt(c, CARD_X + 200, fy + 42, a->pw_ok ? C_OK : C_ERR, a->pw_msg);
        y += h + 12;
    }

    row_card(a, y, 64, "Sign-in screen", "Show the sign-in screen when OmniOS starts");
    toggle(a, y + 21, a->set.signin, T_SIGNIN);
    y += 76;
    row_card(a, y, 64, "Lock your screen", "Press Windows + L at any time");
}

static void page_time(struct app *a)
{
    struct omni_client_conn *c = &a->c;
    time_t t = time(NULL);
    struct tm tmv;
    char s[96];
    int y = 62, cw = card_w(a), bx;

    localtime_r(&t, &tmv);
    card(a, y, 96);
    if (a->set.clock24)
        strftime(s, sizeof(s), "%H:%M:%S", &tmv);
    else {
        int hr = tmv.tm_hour % 12;
        snprintf(s, sizeof(s), "%d:%02d:%02d %s", hr ? hr : 12, tmv.tm_min, tmv.tm_sec,
                 tmv.tm_hour < 12 ? "AM" : "PM");
    }
    omni_client_text2(c, CARD_X + 18, y + 20, C_INK, s);
    strftime(s, sizeof(s), "%A, %B %d, %Y", &tmv);
    omni_client_textt(c, CARD_X + 18, y + 50, C_INK, s);
    omni_client_textt(c, CARD_X + 18, y + 68, C_DIM, omni_zones[a->set.zone % omni_zones_n].name);
    y += 108;

    row_card(a, y, 64, "Time zone", NULL);
    bx = CARD_X + cw - 18 - 34 - 272 - 34 - 8;
    button(a, bx, y + 17, 34, "<", 0, 0);
    a->z[a->nz - 1].kind = Z_ZPREV;
    omni_client_rfill(c, bx + 38, y + 17, 272, BTN_H, 5, C_BTN_BD);
    omni_client_rfill(c, bx + 39, y + 18, 270, BTN_H - 2, 4, C_CARD);
    snprintf(s, sizeof(s), "%.32s", omni_zones[a->set.zone % omni_zones_n].name);
    omni_client_textt(c, bx + 38 + (272 - (int)strlen(s) * 8) / 2, y + 28, C_INK, s);
    button(a, bx + 38 + 272 + 4, y + 17, 34, ">", 0, 0);
    a->z[a->nz - 1].kind = Z_ZNEXT;
    y += 76;

    row_card(a, y, 64, "24-hour clock", "Show the time as 17:30 instead of 5:30 PM");
    toggle(a, y + 21, a->set.clock24, T_CLOCK24);
    y += 76;
    row_card(a, y, 52, "Language", NULL);
    right_text(a, y + 22, C_INK, "English (United States)");
    y += 60;
    row_card(a, y, 52, "Keyboard layout", NULL);
    right_text(a, y + 22, C_INK, "US");
}

static void when_text(long t, char *out, size_t n)
{
    time_t now = time(NULL), tt = (time_t)t;
    struct tm a, b;
    int hr;
    localtime_r(&now, &a);
    localtime_r(&tt, &b);
    hr = b.tm_hour % 12;
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday)
        snprintf(out, n, "Today, %d:%02d %s", hr ? hr : 12, b.tm_min, b.tm_hour < 12 ? "AM" : "PM");
    else
        strftime(out, n, "%b %d, %Y", &b);
}

static void page_update(struct app *a)
{
    struct omni_client_conn *c = &a->c;
    const struct omni_update_status *us = &a->us;
    const char *title, *btn = NULL;
    char sub[160], s[160], line[128];
    int y = 62, cw = card_w(a), act = B_UPD_CHECK, n = 0;
    FILE *f;

    sub[0] = '\0';
    switch (us->state) {
    case UPD_CHECKING:
        title = "Checking for updates...";
        snprintf(sub, sizeof(sub), "Looking for a newer OmniOS on GitHub");
        break;
    case UPD_UPTODATE:
        title = "You're up to date";
        when_text(us->checked, s, sizeof(s));
        snprintf(sub, sizeof(sub), "Last checked: %s", s);
        btn = "Check for updates";
        break;
    case UPD_AVAILABLE:
        title = "Update available";
        snprintf(sub, sizeof(sub), "OmniOS %s is ready to download", us->latest);
        btn = "Download & install";
        act = B_UPD_INSTALL;
        break;
    case UPD_DOWNLOADING:
        title = "Downloading update";
        snprintf(sub, sizeof(sub), "OmniOS %s  -  %d%%", us->latest, us->progress);
        break;
    case UPD_READY:
        title = "Restart required";
        snprintf(sub, sizeof(sub), "OmniOS %s is installed. Restart to start using it.", us->latest);
        btn = "Restart now";
        act = B_UPD_RESTART;
        break;
    case UPD_ERROR:
        title = "Couldn't check for updates";
        snprintf(sub, sizeof(sub), "%.150s", us->message[0] ? us->message : "Something went wrong.");
        btn = "Try again";
        break;
    default:
        title = "OmniOS Update";
        snprintf(sub, sizeof(sub), "OmniOS looks for updates when it's connected to the internet");
        btn = "Check for updates";
        break;
    }
    card(a, y, 110);
    omni_client_icon(c, CARD_X + 18, y + 26, 56, 0x3b82f6, 'U');
    omni_client_text2(c, CARD_X + 92, y + 30, C_INK, title);
    omni_client_textt(c, CARD_X + 92, y + 58, C_DIM, sub);
    if (us->state == UPD_DOWNLOADING) {
        int bw = cw - 92 - 36, p = us->progress < 0 ? 0 : (us->progress > 100 ? 100 : us->progress);
        omni_client_rfill(c, CARD_X + 92, y + 78, bw, 6, 3, C_SEL);
        if (p)
            omni_client_rfill(c, CARD_X + 92, y + 78, bw * p / 100 < 6 ? 6 : bw * p / 100, 6, 3, accent(a));
    }
    if (btn)
        button(a, CARD_X + cw - 18 - 170, y + 22, 170, btn, 1, act);
    y += 122;

    row_card(a, y, 64, "Get updates automatically",
             "Download and install new versions as soon as they're available");
    toggle(a, y + 21, a->set.autoupdate, T_AUTOUPD);
    y += 76;

    row_card(a, y, 52, "Current version", NULL);
    snprintf(s, sizeof(s), "OmniOS %s", us->current[0] ? us->current : "(development build)");
    right_text(a, y + 22, C_INK, s);
    y += 64;

    card(a, y, 132);
    omni_client_textt(c, CARD_X + 18, y + 16, C_INK, "Update history");
    snprintf(s, sizeof(s), "%s/history", omni_update_dir());
    if ((f = fopen(s, "r"))) {
        char last[5][128];
        int k, total = 0;
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = '\0';
            snprintf(last[total % 5], sizeof(last[0]), "%s", line);
            total++;
        }
        fclose(f);
        n = total < 5 ? total : 5;
        for (k = 0; k < n; k++)                 /* newest first */
            omni_client_textt(c, CARD_X + 18, y + 38 + k * 17, C_DIM,
                              last[(total - 1 - k) % 5]);
    }
    if (n == 0)
        omni_client_textt(c, CARD_X + 18, y + 38, C_DIM, "No updates yet.");
    y += 144;
    omni_client_textt(c, CARD_X + 4, y, C_DIM,
                      "Updates come from github.com/OmniNodeCo/OmniOS and are checked");
    omni_client_textt(c, CARD_X + 4, y + 14, C_DIM,
                      "against the SHA-256 checksum published with each release.");
}

/* ------------------------------------------------------------------ */
/* drawing                                                            */
/* ------------------------------------------------------------------ */

static void draw_nav(struct app *a)
{
    struct omni_client_conn *c = &a->c;
    int i;
    omni_client_fill(c, 0, 0, NAV_W, a->h, C_NAV);
    omni_client_icon(c, 16, 18, 48, accent(a), 'u');
    omni_client_textt(c, 74, 28, C_INK, omni_account_display());
    omni_client_textt(c, 74, 44, C_DIM, "Local account");
    for (i = 0; i < PG_COUNT; i++) {
        int y = 90 + i * 40;
        if (i == a->page) {
            omni_client_rfill(c, 8, y, NAV_W - 16, 34, 6, C_SEL);
            omni_client_rfill(c, 8, y + 9, 3, 16, 2, accent(a));
        }
        omni_client_icon(c, 22, y + 7, 20, 0, g_pages[i].icon);
        omni_client_textt(c, 54, y + 13, C_INK, g_pages[i].name);
        add_zone(a, 8, y, NAV_W - 16, 34, Z_NAV, i);
    }
}

static void draw(struct app *a)
{
    a->nz = 0;
    draw_nav(a);
    omni_client_fill(&a->c, NAV_W, 0, a->w - NAV_W, a->h, C_BG);
    omni_client_text2(&a->c, CARD_X, 24, C_INK, g_pages[a->page].name);
    switch (a->page) {
    case PG_SYSTEM:   page_system(a); break;
    case PG_PERSONAL: page_personal(a); break;
    case PG_APPS:     page_apps(a); break;
    case PG_ACCOUNTS: page_accounts(a); break;
    case PG_TIME:     page_time(a); break;
    case PG_UPDATE:   page_update(a); break;
    default: break;
    }
    if (a->focus >= a->nz)
        a->focus = -1;
    focus_ring(a);
}

/* ------------------------------------------------------------------ */
/* actions                                                            */
/* ------------------------------------------------------------------ */

static void spawn(const char *path, const char *arg)
{
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", O_RDWR);
        signal(SIGCHLD, SIG_DFL);
        setsid();
        if (fd >= 0) {
            dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
            if (fd > 2) close(fd);
        }
        execl(path, path, arg, (char *)NULL);
        _exit(127);
    }
}

static const char *updater(void)
{
    const char *p = getenv("OMNI_UPDATER");
    return (p && *p) ? p : "/usr/bin/omnios-update";
}

static void save(struct app *a)
{
    omni_settings_save(&a->set);
}

static void set_page(struct app *a, int p)
{
    a->page = (p + PG_COUNT) % PG_COUNT;
    a->focus = -1;
    a->pw_msg[0] = '\0';
    if (a->page == PG_UPDATE)
        omni_update_read(&a->us);
}

static void pw_save(struct app *a)
{
    int has = omni_account_has_password();
    a->pw_ok = 0;
    if (has && !omni_account_check(a->pw[0])) {
        snprintf(a->pw_msg, sizeof(a->pw_msg), "The current password is incorrect.");
        a->pw_field = 0;
        return;
    }
    if (strcmp(a->pw[1], a->pw[2]) != 0) {
        snprintf(a->pw_msg, sizeof(a->pw_msg), "The new passwords don't match.");
        a->pw_field = 2;
        return;
    }
    if (omni_account_set_password(a->pw[1]) != 0) {
        snprintf(a->pw_msg, sizeof(a->pw_msg), "Couldn't save the password.");
        return;
    }
    a->pw_ok = 1;
    snprintf(a->pw_msg, sizeof(a->pw_msg), a->pw[1][0] ? "Password saved." : "Password removed.");
    memset(a->pw, 0, sizeof(a->pw));
    a->pw_open = 0;
}

static void activate(struct app *a, const struct zone *z, int dir)
{
    switch (z->kind) {
    case Z_NAV:
        set_page(a, z->arg);
        break;
    case Z_WALL:
        a->set.wallpaper = dir ? (a->set.wallpaper + dir + omni_walls_n) % omni_walls_n : z->arg;
        save(a);
        break;
    case Z_ACCENT:
        a->set.accent = dir ? (a->set.accent + dir + omni_accents_n) % omni_accents_n : z->arg;
        save(a);
        break;
    case Z_TOGGLE:
        if (z->arg == T_CLOCK24)  a->set.clock24 = !a->set.clock24;
        if (z->arg == T_AUTOUPD)  a->set.autoupdate = !a->set.autoupdate;
        if (z->arg == T_SIGNIN)   a->set.signin = !a->set.signin;
        save(a);
        break;
    case Z_ZPREV: case Z_ZNEXT:
        a->set.zone = (a->set.zone + (dir ? dir : (z->kind == Z_ZNEXT ? 1 : -1)) +
                       omni_zones_n) % omni_zones_n;
        save(a);
        omni_settings_apply_tz(&a->set);
        break;
    case Z_FIELD:
        a->pw_field = z->arg;
        break;
    case Z_BUTTON:
        switch (z->arg) {
        case B_STORE:      spawn("/usr/bin/omnios-store", NULL); break;
        case B_PW_OPEN:
            a->pw_open = 1;
            a->pw_msg[0] = '\0';
            memset(a->pw, 0, sizeof(a->pw));
            a->pw_field = omni_account_has_password() ? 0 : 1;
            a->focus = -1;
            break;
        case B_PW_SAVE:    pw_save(a); break;
        case B_PW_CANCEL:
            a->pw_open = 0;
            a->pw_msg[0] = '\0';
            memset(a->pw, 0, sizeof(a->pw));
            a->focus = -1;
            break;
        case B_UPD_CHECK:
            spawn(updater(), "check");
            a->us.state = UPD_CHECKING;
            break;
        case B_UPD_INSTALL:
            spawn(updater(), "install");
            a->us.state = UPD_DOWNLOADING;
            a->us.progress = 0;
            break;
        case B_UPD_RESTART:
            sync();
            kill(1, SIGQUIT);                   /* init restarts into the update */
            break;
        default: break;
        }
        break;
    default:
        break;
    }
}

static int zone_at(const struct app *a, int x, int y)
{
    int i;
    for (i = a->nz - 1; i >= 0; i--)
        if (x >= a->z[i].x && x < a->z[i].x + a->z[i].w &&
            y >= a->z[i].y && y < a->z[i].y + a->z[i].h)
            return i;
    return -1;
}

/* returns 1 to quit */
static int on_key(struct app *a, int key, char ch)
{
    const struct zone *fz = (a->focus >= 0 && a->focus < a->nz) ? &a->z[a->focus] : NULL;

    if (a->pw_open && a->pw_field >= 0 && (!fz || fz->kind == Z_FIELD || fz->kind == Z_NAV ||
                                           a->focus < 0)) {
        char *f = a->pw[a->pw_field];
        size_t n = strlen(f);
        if (key == K_BACKSPACE) {
            if (n) f[n - 1] = '\0';
            return 0;
        }
        if (key == K_ENTER || key == K_KPENTER) {  /* next field, then save */
            if (a->pw_field < 2)
                a->pw_field++;
            else
                pw_save(a);
            return 0;
        }
        if (key == K_ESC) {
            a->pw_open = 0;
            memset(a->pw, 0, sizeof(a->pw));
            return 0;
        }
        if (ch >= 32 && ch < 127 && key != K_TAB) {
            if (n + 1 < sizeof(a->pw[0])) {
                f[n] = ch;
                f[n + 1] = '\0';
            }
            return 0;
        }
    }
    switch (key) {
    case K_ESC:
        return 1;
    case K_TAB:
        if (a->nz)
            a->focus = (a->focus + 1) % a->nz;
        if (a->focus >= 0 && a->z[a->focus].kind == Z_FIELD)
            a->pw_field = a->z[a->focus].arg;
        break;
    case K_UP: case K_DOWN:
        if (!fz || fz->kind == Z_NAV) {
            set_page(a, a->page + (key == K_UP ? -1 : 1));
            a->focus = a->page;                 /* nav zones come first */
        } else {
            a->focus = (a->focus + (key == K_UP ? a->nz - 1 : 1)) % a->nz;
        }
        break;
    case K_LEFT: case K_RIGHT:
        if (fz && (fz->kind == Z_WALL || fz->kind == Z_ACCENT || fz->kind == Z_ZPREV ||
                   fz->kind == Z_ZNEXT))
            activate(a, fz, key == K_LEFT ? -1 : 1);
        break;
    case K_ENTER: case K_KPENTER: case K_SPACE:
        if (fz)
            activate(a, fz, 0);
        break;
    default:
        break;
    }
    return 0;
}

#ifndef OMNI_SETTINGS_NO_MAIN
int main(int argc, char **argv)
{
    struct app *a = &g_a;
    int h = WIN_H, quit = 0;

    signal(SIGCHLD, SIG_IGN);                   /* spawned helpers reap themselves */
    memset(a, 0, sizeof(*a));
    a->focus = -1;
    a->pw_field = -1;
    omni_settings_load(&a->set);
    omni_settings_apply_tz(&a->set);
    if (argc > 1) {                             /* "omnios-settings update" etc. */
        int i;
        for (i = 0; i < PG_COUNT; i++)
            if (strncasecmp(argv[1], g_pages[i].name, strlen(argv[1])) == 0)
                a->page = i;
        if (strcmp(argv[1], "update") == 0)
            a->page = PG_UPDATE;
    }
    if (omni_client_open(&a->c, "Settings") < 0)
        return 127;
    if (a->c.screen_h > 0 && h > a->c.screen_h - OMNI_TASKBAR_H - 24)
        h = a->c.screen_h - OMNI_TASKBAR_H - 24;
    if (omni_client_window(&a->c, "Settings", WIN_W, h) < 0) {
        omni_client_close(&a->c);
        return 127;
    }
    a->w = WIN_W;
    a->h = h - OMNI_WM_TITLE_H;
    omni_update_read(&a->us);
    draw(a);

    while (!quit) {
        struct omni_client_event e;
        struct pollfd pf = { a->c.fd, POLLIN, 0 };
        int timeout = (a->page == PG_TIME || a->page == PG_UPDATE) ? 1000 : -1;
        int r, changed = 0;

        r = poll(&pf, 1, timeout);
        if (r < 0)
            continue;                           /* interrupted */
        if (r == 0) {                           /* tick: clock / update status */
            if (a->page == PG_UPDATE) {
                struct omni_update_status old = a->us;
                omni_update_read(&a->us);
                changed = memcmp(&old, &a->us, sizeof(old)) != 0;
            } else {
                changed = 1;
            }
        } else {
            while (!quit && (r = omni_client_poll(&a->c, &e)) > 0) {
                if (e.type == 3) {
                    quit = 1;
                } else if (e.type == 2 && e.pressed && e.key == 1) {
                    int zi = zone_at(a, e.x, e.y);
                    if (zi >= 0) {
                        struct zone z = a->z[zi];
                        a->focus = -1;
                        activate(a, &z, 0);
                        changed = 1;
                    }
                } else if (e.type == 1 && e.pressed) {
                    quit = on_key(a, e.key, e.text);
                    changed = 1;
                }
            }
            if (r < 0)
                quit = 1;                       /* the desktop went away */
        }
        if (changed && !quit)
            draw(a);
    }
    omni_client_close(&a->c);
    return 0;
}
#endif
