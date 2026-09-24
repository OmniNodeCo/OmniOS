/*
 * OmniOS — os/gui/wm.c
 *
 * Window manager + display server core.
 *
 * The server owns the framebuffer. Windows have a 32-bit backing store which
 * clients draw into via protocol commands; painting blits each visible
 * window (front to back, clipped to the desktop) onto the screen so
 * overlapping windows composite correctly, then draws the title chrome.
 *
 * The server also routes mouse/keyboard input to whichever client owns the
 * active window.
 */
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "wm.h"
#include "theme.h"

/* ------------------------------------------------------------------ */
/* low-level helpers                                                  */
/* ------------------------------------------------------------------ */

static uint32_t parse_color(const char *s)
{
    return (uint32_t)strtoul(s, NULL, 16) | 0xff000000u;
}

static int send_line(int fd, const char *line)
{
    return proto_send(fd, line);
}

static int send_word(int fd, const char *verb, long a, long b, long c)
{
    char line[128];
    long num[3] = { a, b, c };
    proto_build(line, sizeof(line), verb, 3, num);
    return send_line(fd, line);
}

/* ------------------------------------------------------------------ */
/* window management                                                  */
/* ------------------------------------------------------------------ */

void omni_wm_init(struct omni_wm *wm, struct raster *screen)
{
    int i;
    memset(wm, 0, sizeof(*wm));
    wm->screen = *screen;
    wm->listen_fd = -1;
    for (i = 0; i < OMNI_WM_MAX_CLI; i++)
        wm->clients[i].fd = -1;
}

/* ------------------------------------------------------------------ */
/* window chrome                                                      */
/* ------------------------------------------------------------------ */

#define WM_RADIUS 8                 /* rounded window corners            */
#define WM_BTN_W  46                /* caption button width              */

static uint32_t win_icon(struct omni_wm *wm, const struct omni_win *w,
                         char *glyph)
{
    uint32_t c;
    *glyph = w->title[0] ? w->title[0] : '?';
    c = wm->icon_for ? wm->icon_for(w->title, glyph) : 0;
    return c ? c : TH_ACCENT;
}

/* anti-aliased diagonal cross, half-size n, centred on (cx, cy) */
static void draw_cross(struct raster *r, int cx, int cy, int n, uint32_t c)
{
    int i;
    for (i = -n; i <= n; i++) {
        th_px(r, cx + i, cy + i, c, 255);
        th_px(r, cx + i, cy - i, c, 255);
        if (i < n) {                        /* soften the stair steps */
            th_px(r, cx + i + 1, cy + i, c, 70);
            th_px(r, cx + i, cy + i + 1, c, 70);
            th_px(r, cx + i + 1, cy - i, c, 70);
            th_px(r, cx + i, cy - i - 1, c, 70);
        }
    }
}

/* chrome zone under the pointer, if w is the topmost window there */
static int hover_zone(struct omni_wm *wm, struct omni_win *w)
{
    if (omni_wm_at(wm, wm->pointer_x, wm->pointer_y) != w)
        return 0;
    return omni_wm_hit(w, wm->pointer_x, wm->pointer_y);
}

/* corner square k (0 TL, 1 TR, 2 BL, 3 BR) origin */
static void corner_origin(const struct omni_win *w, int k, int *ox, int *oy)
{
    *ox = (k & 1) ? w->x + w->w - WM_RADIUS : w->x;
    *oy = (k & 2) ? w->y + w->h - WM_RADIUS : w->y;
}

static void paint_window(struct omni_wm *wm, struct omni_win *w, int active)
{
    struct raster *s = &wm->screen;
    const int R = WM_RADIUS, T = OMNI_WM_TITLE_H;
    uint32_t under[4][WM_RADIUS * WM_RADIUS];
    uint32_t ink = active ? TH_TITLE_TEXT : TH_TITLE_TEXT_2;
    unsigned edge = active ? 64 : 44;           /* outline darkness      */
    int bx_close = w->x + w->w - WM_BTN_W, bx_min = bx_close - WM_BTN_W;
    int hz = hover_zone(wm, w);
    int k, i, j, dy, src_y, src_h;
    char glyph;
    uint32_t icon = win_icon(wm, w, &glyph);

    /* 1. soft drop shadow, deeper for the active window */
    if (active)
        th_shadow(s, w->x, w->y, w->w, w->h, R, 22, 7, 110);
    else
        th_shadow(s, w->x, w->y, w->w, w->h, R, 12, 3, 60);

    /* 2. remember what the rounded corners will let show through */
    for (k = 0; k < 4; k++) {
        int ox, oy;
        corner_origin(w, k, &ox, &oy);
        for (j = 0; j < R; j++)
            for (i = 0; i < R; i++) {
                int xx = ox + i, yy = oy + j;
                under[k][j * R + i] =
                    (xx >= 0 && yy >= 0 && xx < s->w && yy < s->h)
                        ? s->bits[(size_t)yy * (size_t)s->stride + (size_t)xx] : 0;
            }
    }

    /* 3. title bar: app icon, title, minimize + close caption buttons */
    th_fill_a(s, w->x, w->y, w->w, T, active ? TH_TITLE_ACTIVE : TH_TITLE_IDLE, 255);
    th_icon(s, w->x + 10, w->y + (T - 16) / 2, 16, icon, glyph);
    th_text_clip(s, w->title, w->x + 34, w->y + (T - 8) / 2, ink,
                 bx_min - (w->x + 34) - 6);
    if (hz == 10)
        th_fill_a(s, bx_min, w->y, WM_BTN_W, T, TH_CAPTION_HOVER, 255);
    if (hz == 2)
        th_fill_a(s, bx_close, w->y, WM_BTN_W, T, TH_CLOSE_HOVER, 255);
    th_fill_a(s, bx_min + WM_BTN_W / 2 - 5, w->y + T / 2, 11, 1, ink, 255);
    draw_cross(s, bx_close + WM_BTN_W / 2, w->y + T / 2, 5,
               hz == 2 ? 0xffffff : ink);
    th_fill_a(s, w->x, w->y + T - 1, w->w, 1, 0x000000, 18);   /* separator */

    /* 4. client area from the backing store (windows resized larger than
     * their surface get a neutral fill instead of showing through) */
    dy = w->y + T;
    src_y = 0;
    src_h = w->h - T;
    if (dy < 0) {
        src_y = -dy;
        src_h -= src_y;
        dy = 0;
    }
    if (src_h > 0) {
        raster_blit_clip(s, &w->surface, w->x, dy, 0, src_y, w->w, src_h);
        if (w->w > w->surface.w)
            th_fill_a(s, w->x + w->surface.w, dy, w->w - w->surface.w, src_h,
                      0xf3f4f6, 255);
        if (src_y + src_h > w->surface.h) {
            int top = w->surface.h - src_y;
            if (top < 0) top = 0;
            th_fill_a(s, w->x, dy + top, w->w, src_h - top, 0xf3f4f6, 255);
        }
    }

    /* 5. hairline outline (straight parts; the arcs are done below) */
    th_fill_a(s, w->x + R, w->y, w->w - 2 * R, 1, 0x000000, edge);
    th_fill_a(s, w->x + R, w->y + w->h - 1, w->w - 2 * R, 1, 0x000000, edge);
    th_fill_a(s, w->x, w->y + R, 1, w->h - 2 * R, 0x000000, edge);
    th_fill_a(s, w->x + w->w - 1, w->y + R, 1, w->h - 2 * R, 0x000000, edge);

    /* 6. round the corners: outline arc, then blend with what was below */
    for (k = 0; k < 4; k++) {
        int ox, oy;
        corner_origin(w, k, &ox, &oy);
        for (j = 0; j < R; j++)
            for (i = 0; i < R; i++) {
                int xx = ox + i, yy = oy + j;
                int cx = (k & 1) ? R - 1 - i : i;   /* mirror to top-left */
                int cy = (k & 2) ? R - 1 - j : j;
                unsigned cov, ring;
                uint32_t *p, pix;
                float fx, fy, d;

                if (xx < 0 || yy < 0 || xx >= s->w || yy >= s->h)
                    continue;
                cov = th_corner_cov(cx, cy, R);
                p = s->bits + (size_t)yy * (size_t)s->stride + (size_t)xx;
                fx = (float)R - ((float)cx + 0.5f);
                fy = (float)R - ((float)cy + 0.5f);
                d = sqrtf(fx * fx + fy * fy) - ((float)R - 0.5f);
                d = d < 0 ? -d : d;
                ring = d < 1.0f ? (unsigned)((1.0f - d) * (float)edge) : 0;
                pix = th_blend(*p, 0x000000, ring);
                *p = th_blend(under[k][j * R + i], pix, cov);
            }
    }
}

struct omni_win *omni_wm_add(struct omni_wm *wm, const char *title,
                             int x, int y, int w, int h)
{
    struct omni_win *win = NULL;
    int i;

    for (i = 0; i < OMNI_WM_MAX_WIN; i++) {
        if (!wm->wins[i].used) {
            win = &wm->wins[i];
            break;
        }
    }
    if (!win)
        return NULL;

    memset(win, 0, sizeof(*win));
    win->used = 1;
    win->id = i + 1;
    win->client = -1;
    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
    win->minw = 200;
    win->minh = OMNI_WM_TITLE_H + 40;
    snprintf(win->title, sizeof(win->title), "%s", title ? title : "");

    win->surface.bits = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    win->surface.w = w;
    win->surface.h = h;
    win->surface.stride = w;
    if (!win->surface.bits) {
        win->used = 0;
        return NULL;
    }

    /* insert at front of z-order */
    for (i = wm->nwin; i > 0; i--)
        wm->order[i] = wm->order[i - 1];
    wm->order[0] = win;
    wm->nwin++;
    wm->active = win;
    return win;
}

static struct omni_win *topmost_visible(struct omni_wm *wm)
{
    int i;
    for (i = 0; i < wm->nwin; i++)
        if (!wm->order[i]->minimized)
            return wm->order[i];
    return NULL;
}

void omni_wm_close(struct omni_wm *wm, struct omni_win *w)
{
    int i, j;

    for (i = 0; i < wm->nwin; i++) {
        if (wm->order[i] == w) {
            for (j = i; j < wm->nwin - 1; j++)
                wm->order[j] = wm->order[j + 1];
            wm->nwin--;
            break;
        }
    }
    free(w->surface.bits);
    memset(w, 0, sizeof(*w));

    if (wm->active == w)
        wm->active = topmost_visible(wm);
    wm->dirty = 1;
}

void omni_wm_minimize(struct omni_wm *wm, struct omni_win *w)
{
    w->minimized = 1;
    if (wm->active == w)
        wm->active = topmost_visible(wm);
    wm->dirty = 1;
}

void omni_wm_raise(struct omni_wm *wm, struct omni_win *w)
{
    int i;
    w->minimized = 0;
    wm->active = w;
    wm->dirty = 1;
    for (i = 0; i < wm->nwin; i++) {
        if (wm->order[i] == w) {
            memmove(&wm->order[1], &wm->order[0],
                    (size_t)i * sizeof(wm->order[0]));
            wm->order[0] = w;
            break;
        }
    }
    wm->active = w;
}

struct omni_win *omni_wm_find(struct omni_wm *wm, int win_id)
{
    int i;
    for (i = 0; i < OMNI_WM_MAX_WIN; i++)
        if (wm->wins[i].used && wm->wins[i].id == win_id)
            return &wm->wins[i];
    return NULL;
}

struct omni_win *omni_wm_at(struct omni_wm *wm, int x, int y)
{
    int i;
    for (i = 0; i < wm->nwin; i++) {
        struct omni_win *w = wm->order[i];
        if (w->minimized)
            continue;
        if (x >= w->x && x < w->x + w->w &&
            y >= w->y && y < w->y + w->h)
            return w;
    }
    return NULL;
}

int omni_wm_hit(struct omni_win *w, int x, int y)
{
    const int E = 5, T = OMNI_WM_TITLE_H;
    int rx = x - w->x, ry = y - w->y;

    if (rx < 0 || ry < 0 || rx >= w->w || ry >= w->h)
        return 0;

    /* caption buttons take precedence over the top-right corner */
    if (ry < T && rx >= w->w - WM_BTN_W)        return 2;
    if (ry < T && rx >= w->w - 2 * WM_BTN_W)    return 10;

    if (rx < E && ry < E)                       return 3;
    if (rx < E && ry >= w->h - E)               return 7;
    if (rx >= w->w - E && ry >= w->h - E)       return 5;
    if (rx < E)                                 return 4;
    if (ry < T)                                 return 1;
    if (rx >= w->w - E)                         return 6;
    if (ry >= w->h - E)                         return 8;

    return 0;
}

int omni_wm_button(struct omni_wm *wm, int x, int y, int btn, int pressed)
{
    if (pressed) {
        struct omni_win *w = omni_wm_at(wm, x, y);
        int zone;

        if (!w) {
            if (wm->active)
                wm->dirty = 1;
            wm->active = NULL;
            return 0;
        }
        omni_wm_raise(wm, w);
        wm->button_down = 1;
        wm->button_which = btn;
        zone = omni_wm_hit(w, x, y);

        if (zone == 2) {
            /* deliver window-close to owning client then destroy */
            if (w->client >= 0 && w->client < OMNI_WM_MAX_CLI &&
                wm->clients[w->client].fd >= 0)
                send_word(wm->clients[w->client].fd, "CLOSE", w->id, 0, 0);
            omni_wm_close(wm, w);
            return 0;
        }
        if (zone == 10) {
            wm->button_down = 0;
            omni_wm_minimize(wm, w);
            return 0;
        }
        if (zone == 1 || zone >= 3) {
            wm->drag = zone;
            wm->drag_offx = x - w->x;
            wm->drag_offy = y - w->y;
            return 0;
        }
        /* click inside client area: report to the owning client */
        if (w->client >= 0 && w->client < OMNI_WM_MAX_CLI) {
            char line[128];
            long num[5] = { w->id, x - w->x, y - w->y - OMNI_WM_TITLE_H,
                            btn, 1 };
            proto_build(line, sizeof(line), "BTN", 5, num);
            send_line(wm->clients[w->client].fd, line);
        }
        return 0;
    }

    wm->button_down = 0;
    wm->drag = 0;
    return 0;
}

int omni_wm_motion(struct omni_wm *wm, int x, int y)
{
    struct omni_win *w;

    wm->pointer_x = x;
    wm->pointer_y = y;

    if (!wm->button_down || !wm->drag)
        return 0;

    w = wm->active;
    if (!w)
        return 0;

    switch (wm->drag) {
    case 1:
        w->x = x - wm->drag_offx;
        w->y = y - wm->drag_offy;
        if (w->x < 0) w->x = 0;
        if (w->y < 0) w->y = 0;
        if (w->x + w->w > wm->screen.w) w->x = wm->screen.w - w->w;
        if (w->y + w->h > wm->screen.h) w->y = wm->screen.h - w->h;
        break;
    case 3: {
        int br_x = w->x + w->w, br_y = w->y + w->h;
        w->x = x < br_x - w->minw ? x : br_x - w->minw;
        w->y = y < br_y - w->minh ? y : br_y - w->minh;
        w->w = br_x - w->x;
        w->h = br_y - w->y;
        break;
    }
    case 9: {
        int bl_y = w->y + w->h;
        w->w = x - w->x;
        w->y = y < bl_y - w->minh ? y : bl_y - w->minh;
        w->h = bl_y - w->y;
        if (w->w < w->minw) w->w = w->minw;
        break;
    }
    case 7: {
        int tr_x = w->x + w->w;
        w->h = y - w->y;
        w->x = x < tr_x - w->minw ? x : tr_x - w->minw;
        w->w = tr_x - w->x;
        if (w->h < w->minh) w->h = w->minh;
        break;
    }
    case 4: {
        int r = w->x + w->w;
        w->x = x < r - w->minw ? x : r - w->minw;
        w->w = r - w->x;
        break;
    }
    case 6:
        w->w = x - w->x;
        if (w->w < w->minw) w->w = w->minw;
        break;
    case 8:
        w->h = y - w->y;
        if (w->h < w->minh) w->h = w->minh;
        break;
    case 5:
        w->w = x - w->x;
        w->h = y - w->y;
        if (w->w < w->minw) w->w = w->minw;
        if (w->h < w->minh) w->h = w->minh;
        break;
    default:
        break;
    }
    wm->dirty = 1;
    return 1;
}

int omni_wm_key(struct omni_wm *wm, int key, int pressed, char ch)
{
    struct omni_win *w = wm->active;
    if (!w || w->client < 0 || w->client >= OMNI_WM_MAX_CLI)
        return 0;
    {
        char line[128];
        long num[4] = { w->id, key, pressed, (unsigned char)ch };
        proto_build(line, sizeof(line), "KEY", 4, num);
        return send_line(wm->clients[w->client].fd, line) == 0;
    }
}

void omni_wm_paint(struct omni_wm *wm)
{
    int i;

    wm->dirty = 0;

    /* desktop background: shell-provided wallpaper, else built-in gradient */
    if (wm->draw_background)
        wm->draw_background(wm);
    else
        raster_gradient_v(&wm->screen, 0, 0, wm->screen.w, wm->screen.h,
                          OMNI_COLOR_DESKTOP, omni_rgb(0x0a, 0x1e, 0x38));

    for (i = wm->nwin - 1; i >= 0; i--) {
        struct omni_win *w = wm->order[i];
        if (!w->minimized)
            paint_window(wm, w, w == wm->active);
    }

    /* last step of the frame: the shell draws its chrome (taskbar, Start
     * menu, banner) into the scene and presents the changed blocks to the
     * device. */
    if (wm->finish)
        wm->finish(wm);
}

/* ------------------------------------------------------------------ */
/* display-server socket handling                                     */
/* ------------------------------------------------------------------ */

int omni_wm_start(struct omni_wm *wm)
{
    struct sockaddr_un addr;
    int fd;

    unlink(OMNI_WM_SOCKET);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", OMNI_WM_SOCKET);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    wm->listen_fd = fd;
    return 0;
}

int omni_wm_fd(struct omni_wm *wm)
{
    return wm->listen_fd;
}

static int client_slot(struct omni_wm *wm)
{
    int i;
    for (i = 0; i < OMNI_WM_MAX_CLI; i++)
        if (wm->clients[i].fd < 0)
            return i;
    return -1;
}

void omni_wm_accept(struct omni_wm *wm)
{
    int fd, ci;

    fd = accept(wm->listen_fd, NULL, NULL);
    if (fd < 0)
        return;
    fcntl(fd, F_SETFL, O_NONBLOCK);

    ci = client_slot(wm);
    if (ci < 0) {
        send_line(fd, "ERR server full\n");
        close(fd);
        return;
    }
    wm->clients[ci].fd = fd;
    wm->clients[ci].hello = 0;
    wm->clients[ci].win = NULL;
}

static void drop_client(struct omni_wm *wm, int ci)
{
    struct omni_client *cl = &wm->clients[ci];
    if (cl->win)
        omni_wm_close(wm, cl->win);
    if (cl->fd >= 0)
        close(cl->fd);
    cl->fd = -1;
    cl->win = NULL;
    cl->hello = 0;
    cl->rxlen = 0;
    cl->rx[0] = '\0';
}

static int handle_line(struct omni_wm *wm, int ci, const char *line)
{
    struct proto_msg m;
    struct omni_client *cl = &wm->clients[ci];
    char out[OMNI_PROTO_MAX_LINE];

    if (proto_parse(line, &m) < 0)
        return -1;

    if (!cl->hello) {
        if (strcmp(m.verb, "HELLO") != 0)
            return -1;
        cl->hello = 1;
        {
            long num[2] = { wm->screen.w, wm->screen.h };
            proto_build(out, sizeof(out), "HELLO", 2, num);
            send_line(cl->fd, out);
        }
        return 0;
    }

    if (strcmp(m.verb, "OPEN") == 0) {
        long w = m.num[0], h = m.num[1];
        int step = ci % 6;
        int x = (wm->screen.w - (int)w) / 2 - 70 + step * 28;
        int y = (wm->screen.h - OMNI_TASKBAR_H - (int)h) / 2 - 50 + step * 26;
        struct omni_win *win;

        if (m.n < 2 || w <= 0 || h <= 0) {
            send_line(cl->fd, "ERR OPEN needs width height title\n");
            return 0;
        }
        if (cl->win) {
            send_line(cl->fd, "ERR client already has a window\n");
            return 0;
        }
        if (x + w > wm->screen.w - 8) x = wm->screen.w - 8 - (int)w;
        if (y + h > wm->screen.h - OMNI_TASKBAR_H - 4)
            y = wm->screen.h - OMNI_TASKBAR_H - 4 - (int)h;
        if (x < 8) x = 8;
        if (y < 8) y = 8;
        win = omni_wm_add(wm, m.text[0] ? m.text : "OmniOS window",
                          x, y, (int)w, (int)h);
        if (!win) {
            send_line(cl->fd, "ERR window limit reached\n");
            return 0;
        }
        win->client = ci;
        cl->win = win;
        {
            long num[1] = { win->id };
            proto_build(out, sizeof(out), "OK", 1, num);
            send_line(cl->fd, out);
        }
        wm->dirty = 1;
        return 0;
    }

    if (strcmp(m.verb, "CLOSE") == 0 || strcmp(m.verb, "QUIT") == 0) {
        drop_client(wm, ci);
        wm->dirty = 1;
        return (strcmp(m.verb, "QUIT") == 0) ? 1 : 0;
    }

    if (!cl->win) {
        send_line(cl->fd, "ERR no window (OPEN first)\n");
        return 0;
    }

    if (strcmp(m.verb, "TITLE") == 0) {
        snprintf(cl->win->title, sizeof(cl->win->title), "%.*s",
                 (int)sizeof(cl->win->title) - 1, m.text);
        wm->dirty = 1;
    } else if (strcmp(m.verb, "RAISE") == 0) {
        omni_wm_raise(wm, cl->win);
        wm->dirty = 1;
    } else if (strcmp(m.verb, "CLEAR") == 0) {
        struct raster *s = &cl->win->surface;
        uint32_t c = m.text[0] ? parse_color(m.text) : OMNI_COLOR_FACE;
        raster_fill(s, 0, 0, s->w, s->h, c);
        wm->dirty = 1;
    } else if (strcmp(m.verb, "FILL") == 0) {
        struct raster *s = &cl->win->surface;
        uint32_t c;
        if (m.n < 5) { send_line(cl->fd, "ERR FILL x y w h color\n"); return 0; }
        c = parse_color(m.text);
        raster_fill(s, (int)m.num[1], (int)m.num[2],
                    (int)m.num[3], (int)m.num[4], c);
        wm->dirty = 1;
    } else if (strcmp(m.verb, "RFILL") == 0) {
        /* RFILL win x y w h radius color: anti-aliased rounded rect */
        struct raster *s = &cl->win->surface;
        int rw, rh, rad;
        if (m.n < 6) { send_line(cl->fd, "ERR RFILL x y w h radius color\n"); return 0; }
        rw = (int)m.num[3]; rh = (int)m.num[4]; rad = (int)m.num[5];
        if (rad > rw / 2) rad = rw / 2;
        if (rad > rh / 2) rad = rh / 2;
        if (rad < 0) rad = 0;
        th_round_rect(s, (int)m.num[1], (int)m.num[2], rw, rh, rad,
                      parse_color(m.text) & 0xffffff, 255);
        wm->dirty = 1;
    } else if (strcmp(m.verb, "GRAD") == 0) {
        /* GRAD win x y w h c0 c1 vertical: linear gradient (decimal
         * colours) -- one message instead of dozens of FILL bands */
        struct raster *s = &cl->win->surface;
        int gx, gy, gw, gh, vert, i, j, i0, i1, j0, j1, span;
        uint32_t c0, c1;
        if (m.n < 8) { send_line(cl->fd, "ERR GRAD x y w h c0 c1 vertical\n"); return 0; }
        gx = (int)m.num[1]; gy = (int)m.num[2]; gw = (int)m.num[3]; gh = (int)m.num[4];
        c0 = (uint32_t)m.num[5] & 0xffffff;
        c1 = (uint32_t)m.num[6] & 0xffffff;
        vert = m.num[7] != 0;
        span = (vert ? gh : gw) - 1;
        i0 = gx < 0 ? -gx : 0;  i1 = gw < s->w - gx ? gw : s->w - gx;   /* clip */
        j0 = gy < 0 ? -gy : 0;  j1 = gh < s->h - gy ? gh : s->h - gy;
        for (j = j0; j < j1; j++) {
            uint32_t *row = s->bits + (size_t)(gy + j) * (size_t)s->stride + gx;
            for (i = i0; i < i1; i++) {
                int k = vert ? j : i;
                unsigned t = span > 0 ? (unsigned)((long)k * 255 / span) : 0;
                row[i] = 0xff000000u | (th_blend(c0, c1, t) & 0xffffff);
            }
        }
        wm->dirty = 1;
    } else if (strcmp(m.verb, "ICON") == 0) {
        /* ICON win x y size rgb glyph: the gradient app icon of the
         * taskbar and Start menu (rgb decimal, glyph an ASCII code) */
        int size;
        char glyph;
        if (m.n < 6) { send_line(cl->fd, "ERR ICON x y size rgb glyph\n"); return 0; }
        size = (int)m.num[3];
        glyph = (char)(m.num[5] & 0x7f);
        if (size < 8) size = 8;
        if (size > 128) size = 128;
        th_icon(&cl->win->surface, (int)m.num[1], (int)m.num[2], size,
                (uint32_t)m.num[4] & 0xffffff, glyph >= 32 ? glyph : '?');
        wm->dirty = 1;
    } else if (strcmp(m.verb, "TEXT2") == 0 || strcmp(m.verb, "TEXTT") == 0) {
        /* TEXT2 win x y fg text: 16 px smoothed text; TEXTT: 8 px.
         * Both transparent (drawn over what is there). */
        struct raster *s = &cl->win->surface;
        uint32_t fg;
        if (m.n < 4) { send_line(cl->fd, "ERR TEXT2/TEXTT x y fg text\n"); return 0; }
        fg = (uint32_t)m.num[3] & 0xffffff;
        if (m.verb[4] == '2')
            th_text2x(s, m.text, (int)m.num[1], (int)m.num[2], fg);
        else
            th_text(s, m.text, (int)m.num[1], (int)m.num[2], fg);
        wm->dirty = 1;
    } else if (strcmp(m.verb, "RECT") == 0) {
        struct raster *s = &cl->win->surface;
        uint32_t c;
        if (m.n < 5) { send_line(cl->fd, "ERR RECT x y w h color\n"); return 0; }
        c = parse_color(m.text);
        raster_rect(s, (int)m.num[1], (int)m.num[2],
                    (int)m.num[3], (int)m.num[4], c);
        wm->dirty = 1;
    } else if (strcmp(m.verb, "TEXT") == 0) {
        struct raster *s = &cl->win->surface;
        struct canvas cv;
        canvas_init(&cv, s, OMNI_COLOR_TEXT, OMNI_COLOR_FACE);
        canvas_text(&cv, m.text, (int)m.num[1], (int)m.num[2]);
        wm->dirty = 1;
    } else if (strcmp(m.verb, "TEXTC") == 0) {
        /* TEXTC <win> <x> <y> <fg> <bg> <text>: explicit colours, clipped
         * at the window edge (no wrap: a wrap at the bottom would scroll
         * the client's whole surface). */
        struct raster *s = &cl->win->surface;
        struct canvas cv;
        if (m.n < 5) { send_line(cl->fd, "ERR TEXTC x y fg bg text\n"); return 0; }
        canvas_init(&cv, s, (uint32_t)m.num[3] | 0xff000000u,
                    (uint32_t)m.num[4] | 0xff000000u);
        cv.linewrap = 0;
        canvas_text(&cv, m.text, (int)m.num[1], (int)m.num[2]);
        wm->dirty = 1;
    } else {
        char buf[OMNI_PROTO_MAX_LINE];
        snprintf(buf, sizeof(buf), "ERR unknown verb %s\n", m.verb);
        send_line(cl->fd, buf);
    }
    return 0;
}

/* Drain one client's socket into its per-client line buffer and dispatch
 * every complete line. Partial lines survive across calls, so fragmented
 * writes on the non-blocking socket are handled correctly.
 * Returns 0 (ok) or -1 (client dropped). */
int omni_wm_handle_client(struct omni_wm *wm, int ci)
{
    struct omni_client *cl = &wm->clients[ci];
    char tmp[1024];

    if (cl->fd < 0)
        return -1;

    for (;;) {
        ssize_t n = read(cl->fd, tmp, sizeof(tmp));
        int i;

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            drop_client(wm, ci);
            wm->dirty = 1;
            return -1;
        }
        if (n == 0) {          /* EOF */
            drop_client(wm, ci);
            wm->dirty = 1;
            return -1;
        }

        for (i = 0; i < n; i++) {
            if (tmp[i] == '\n') {
                cl->rx[cl->rxlen] = '\0';
                cl->rxlen = 0;
                if (handle_line(wm, ci, cl->rx) == 1) {
                    drop_client(wm, ci);
                    wm->dirty = 1;
                    return -1;
                }
            } else if (cl->rxlen < OMNI_PROTO_MAX_LINE - 1) {
                cl->rx[cl->rxlen++] = tmp[i];
            } else {
                /* overlong line: drop the misbehaving client */
                drop_client(wm, ci);
                wm->dirty = 1;
                return -1;
            }
        }
    }
    return 0;
}
