/*
 * OmniOS — os/apps/store.c
 *
 * App Store: browse the OmniOS app catalog, get (install) and remove apps.
 * Installed apps appear in the Start menu straight away (the desktop
 * re-reads the catalog state every time the menu opens). System apps are
 * always installed.
 *
 * Mouse: click a row to select it, click its button to get/remove.
 * Keys:  Up/Down/PgUp/PgDn/Home/End select, Enter or Space get/remove,
 *        Delete removes, Esc closes.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../gui/catalog.h"
#include "../gui/client.h"
#include "../gui/wm.h"          /* OMNI_WM_TITLE_H */

#define WIN_W     600
#define WIN_H     520
#define HEADER_H  64
#define LIST_Y    (HEADER_H + 4)
#define ROW_H     44
#define FOOTER_H  28
#define SB_W      6               /* scrollbar (when the list scrolls)  */
#define SB_X      (WIN_W - 10)
#define BTN_W     92
#define BTN_H     24
#define BTN_X     (WIN_W - 16 - BTN_W)

#define C_HEADER  0x2563eb
#define C_HEADER_2 0x184098         /* gradient end: C_HEADER at 65 % */
#define C_HEAD_2  0xdbe6ff
#define C_WHITE   0xffffff
#define C_SELECT  0xe8f0fe
#define C_RULE    0xeceef2
#define C_INK     0x1f2937
#define C_META    0x6b7280
#define C_SUMMARY 0x4b5563
#define C_GET     0x2563eb
#define C_GET_LO  0xdbe6ff
#define C_REMOVE  0xf3f4f6
#define C_BORDER  0xd1d5db
#define C_FOOTER  0xf8fafc
#define C_FOOT_TX 0x4b5563
#define C_GONE    0x9ca3af

enum { K_ESC = 1, K_ENTER = 28, K_SPACE = 57, K_KPENTER = 96, K_HOME = 102,
       K_UP = 103, K_PGUP = 104, K_END = 107, K_DOWN = 108, K_PGDN = 109,
       K_DELETE = 111 };

struct store {
    unsigned char installed[OMNI_CATALOG_MAX];
    int  sel, top, rows, h;     /* selection, first visible row, rows shown */
    char status[128];
};

static struct store g_st;

/* ------------------------------------------------------------------ */
/* logic (no drawing)                                                 */
/* ------------------------------------------------------------------ */

static int app_present(int i)
{
    return access(omni_catalog[i].path, X_OK) == 0;
}

static void size_text(int i, char *out, size_t n)
{
    struct stat sb;
    if (stat(omni_catalog[i].path, &sb) != 0)
        snprintf(out, n, "not in this image");
    else
        snprintf(out, n, "%ld KB", (long)((sb.st_size + 1023) / 1024));
}

static void store_select(struct store *s, int sel)
{
    if (sel < 0) sel = 0;
    if (sel > omni_catalog_n - 1) sel = omni_catalog_n - 1;
    s->sel = sel;
    if (s->sel < s->top)
        s->top = s->sel;
    if (s->sel >= s->top + s->rows)
        s->top = s->sel - s->rows + 1;
}

/* install (want = 1) or remove (want = 0) app i; sets the status line.
 * Returns 1 if the installed set changed. */
static int store_set(struct store *s, int i, int want)
{
    const struct omni_app_info *a = &omni_catalog[i];

    if (a->system) {
        snprintf(s->status, sizeof(s->status),
                 "%s is part of OmniOS and is always installed.", a->name);
        return 0;
    }
    if (want && !app_present(i)) {
        snprintf(s->status, sizeof(s->status),
                 "%s is not included in this OmniOS image.", a->name);
        return 0;
    }
    if (s->installed[i] == want) {
        snprintf(s->status, sizeof(s->status), "%s is %s.", a->name,
                 want ? "already installed" : "not installed");
        return 0;
    }
    s->installed[i] = (unsigned char)want;
    if (omni_apps_save(s->installed) != 0) {
        s->installed[i] = (unsigned char)!want;
        snprintf(s->status, sizeof(s->status), "Could not save: %s",
                 strerror(errno));
        return 0;
    }
    if (want)
        snprintf(s->status, sizeof(s->status),
                 "%s installed. Open it from the Start menu.", a->name);
    else
        snprintf(s->status, sizeof(s->status),
                 "%s removed from the Start menu.", a->name);
    return 1;
}

static int store_count(const struct store *s, int installed)
{
    int i, n = 0;
    for (i = 0; i < omni_catalog_n; i++)
        if (installed ? s->installed[i] : (!s->installed[i] && app_present(i)))
            n++;
    return n;
}

/* ------------------------------------------------------------------ */
/* drawing                                                            */
/* ------------------------------------------------------------------ */

static void draw_button(struct omni_client_conn *c, struct store *s, int i,
                        int y, uint32_t rowbg)
{
    int by = y + (ROW_H - BTN_H) / 2;
    if (omni_catalog[i].system) {
        omni_client_textc(c, BTN_X + 14, by + 8, C_META, rowbg, "Installed");
    } else if (!app_present(i)) {
        omni_client_textc(c, BTN_X + 6, by + 8, C_GONE, rowbg, "Unavailable");
    } else if (s->installed[i]) {
        omni_client_rfill(c, BTN_X, by, BTN_W, BTN_H, 6, C_BORDER);
        omni_client_rfill(c, BTN_X + 1, by + 1, BTN_W - 2, BTN_H - 2, 5, C_REMOVE);
        omni_client_textt(c, BTN_X + (BTN_W - 6 * 8) / 2, by + 8, C_INK, "Remove");
    } else {
        omni_client_rfill(c, BTN_X, by, BTN_W, BTN_H, 6, C_GET);
        omni_client_textt(c, BTN_X + (BTN_W - 3 * 8) / 2, by + 8, C_WHITE, "Get");
    }
}

static void draw_row(struct omni_client_conn *c, struct store *s, int i, int y)
{
    const struct omni_app_info *a = &omni_catalog[i];
    uint32_t bg = (i == s->sel) ? C_SELECT : C_WHITE;
    char meta[64], size[32];

    omni_client_fill(c, 0, y, WIN_W, ROW_H, C_WHITE);
    if (i == s->sel)                               /* inset highlight */
        omni_client_rfill(c, 6, y + 2, WIN_W - 12, ROW_H - 4, 8, C_SELECT);
    else
        omni_client_fill(c, 60, y + ROW_H - 1, WIN_W - 76, 1, C_RULE);
    omni_client_icon(c, 16, y + 6, 32, a->color, a->glyph);   /* icon */
    omni_client_textt(c, 60, y + 10, C_INK, a->name);
    size_text(i, size, sizeof(size));
    snprintf(meta, sizeof(meta), "%s - %s", a->category, size);
    omni_client_textt(c, 60 + ((int)strlen(a->name) + 2) * 8, y + 10,
                      C_META, meta);
    omni_client_textt(c, 60, y + 26, C_SUMMARY, a->summary);
    draw_button(c, s, i, y, bg);
}

static void store_draw(struct omni_client_conn *c, struct store *s)
{
    char sub[96];
    int r, list_h = s->rows * ROW_H;

    omni_client_grad(c, 0, 0, WIN_W, HEADER_H, C_HEADER, C_HEADER_2, 0);
    omni_client_text2(c, 18, 16, C_WHITE, "App Store");
    snprintf(sub, sizeof(sub), "%d apps installed, %d more to get",
             store_count(s, 1), store_count(s, 0));
    omni_client_textt(c, 18, 42, C_HEAD_2, sub);

    omni_client_fill(c, 0, HEADER_H, WIN_W, s->h - HEADER_H, C_WHITE);
    for (r = 0; r < s->rows && s->top + r < omni_catalog_n; r++)
        draw_row(c, s, s->top + r, LIST_Y + r * ROW_H);
    if (omni_catalog_n > s->rows) {             /* scrollbar */
        int th = list_h * s->rows / omni_catalog_n;
        int ty = LIST_Y + list_h * s->top / omni_catalog_n;
        if (th < 24)
            th = 24;
        if (ty + th > LIST_Y + list_h)
            ty = LIST_Y + list_h - th;
        omni_client_rfill(c, SB_X, LIST_Y, SB_W, list_h, SB_W / 2, C_RULE);
        omni_client_rfill(c, SB_X, ty, SB_W, th, SB_W / 2, C_BORDER);
    }

    omni_client_fill(c, 0, s->h - FOOTER_H, WIN_W, FOOTER_H, C_FOOTER);
    omni_client_fill(c, 0, s->h - FOOTER_H, WIN_W, 1, C_RULE);
    omni_client_textt(c, 16, s->h - FOOTER_H + 10, C_FOOT_TX,
                      s->status[0] ? s->status
                      : "Up/Down select - Enter get/remove - Del remove - Esc close");
}

/* short progress bar inside the button while "installing" */
static void animate_install(struct omni_client_conn *c, struct store *s)
{
    int by = LIST_Y + (s->sel - s->top) * ROW_H + (ROW_H - BTN_H) / 2;
    int step;
    for (step = 1; step <= 10; step++) {
        omni_client_rfill(c, BTN_X, by, BTN_W, BTN_H, 6, C_GET_LO);
        omni_client_rfill(c, BTN_X, by, BTN_W * step / 10, BTN_H, 6, C_GET);
        usleep(35000);
    }
}

static void toggle(struct omni_client_conn *c, struct store *s)
{
    int i = s->sel;
    int want = !s->installed[i];
    if (want && !omni_catalog[i].system && app_present(i))
        animate_install(c, s);
    store_set(s, i, want);
}

/* scroll the list by d rows, keeping the selection in view */
static void store_scroll(struct store *s, int d)
{
    int max_top = omni_catalog_n - s->rows;
    s->top += d;
    if (s->top > max_top)
        s->top = max_top;
    if (s->top < 0)
        s->top = 0;
    if (s->sel < s->top)
        s->sel = s->top;
    if (s->sel >= s->top + s->rows)
        s->sel = s->top + s->rows - 1;
}

/* row index under content y, or -1 */
static int row_at(const struct store *s, int y)
{
    int r;
    if (y < LIST_Y)
        return -1;
    r = (y - LIST_Y) / ROW_H;
    if (r >= s->rows || s->top + r >= omni_catalog_n)
        return -1;
    return s->top + r;
}

#ifndef OMNI_STORE_NO_MAIN
int main(void)
{
    struct omni_client_conn conn;
    int h = WIN_H;

    memset(&conn, 0, sizeof(conn));
    if (omni_client_open(&conn, "App Store") < 0)
        return 127;
    if (conn.screen_h > 0 && h > conn.screen_h - 30 - 48)
        h = conn.screen_h - 30 - 48;       /* keep clear of the taskbar */
    if (omni_client_window(&conn, "App Store", WIN_W, h) < 0) {
        omni_client_close(&conn);
        return 127;
    }
    memset(&g_st, 0, sizeof(g_st));
    g_st.h = h - OMNI_WM_TITLE_H;
    g_st.rows = (g_st.h - LIST_Y - FOOTER_H) / ROW_H;
    if (g_st.rows < 1)
        g_st.rows = 1;
    omni_apps_load(g_st.installed);
    store_draw(&conn, &g_st);

    for (;;) {
        struct omni_client_event e;
        struct pollfd pf = { conn.fd, POLLIN, 0 };
        int r, changed = 0, quit = 0;

        if (poll(&pf, 1, -1) < 0)
            continue;
        while ((r = omni_client_poll(&conn, &e)) > 0) {
            if (e.type == 3) { quit = 1; break; }
            if (e.type == 4 && (e.key == 4 || e.key == 5)) { /* wheel   */
                store_scroll(&g_st, e.key == 4 ? -2 : 2);
                changed = 1;
            } else if (e.type == 2 && e.pressed && e.x >= SB_X - 4 &&
                       omni_catalog_n > g_st.rows && e.y >= LIST_Y &&
                       e.y < LIST_Y + g_st.rows * ROW_H) {  /* scrollbar: page */
                int ty = LIST_Y + g_st.rows * ROW_H * g_st.top / omni_catalog_n;
                store_scroll(&g_st, e.y < ty ? -g_st.rows : g_st.rows);
                changed = 1;
            } else if (e.type == 2 && e.pressed) {           /* click   */
                int i = row_at(&g_st, e.y);
                if (i >= 0) {
                    int by = LIST_Y + (i - g_st.top) * ROW_H + (ROW_H - BTN_H) / 2;
                    store_select(&g_st, i);
                    if (e.x >= BTN_X && e.x < BTN_X + BTN_W &&
                        e.y >= by && e.y < by + BTN_H)
                        toggle(&conn, &g_st);
                    changed = 1;
                }
            } else if (e.type == 1 && e.pressed) {           /* key     */
                changed = 1;
                switch (e.key) {
                case K_ESC:    quit = 1; break;
                case K_UP:     store_select(&g_st, g_st.sel - 1); break;
                case K_DOWN:   store_select(&g_st, g_st.sel + 1); break;
                case K_PGUP:   store_select(&g_st, g_st.sel - g_st.rows); break;
                case K_PGDN:   store_select(&g_st, g_st.sel + g_st.rows); break;
                case K_HOME:   store_select(&g_st, 0); break;
                case K_END:    store_select(&g_st, omni_catalog_n - 1); break;
                case K_ENTER: case K_KPENTER: case K_SPACE:
                    toggle(&conn, &g_st);
                    break;
                case K_DELETE: store_set(&g_st, g_st.sel, 0); break;
                default:       changed = 0; break;
                }
                if (quit)
                    break;
            }
        }
        if (r < 0 || quit)
            break;
        if (changed)
            store_draw(&conn, &g_st);
    }
    omni_client_close(&conn);
    return 0;
}
#endif
