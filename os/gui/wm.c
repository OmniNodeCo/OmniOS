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
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "wm.h"

/* ------------------------------------------------------------------ */
/* low-level helpers                                                  */
/* ------------------------------------------------------------------ */

static void paint_frame(struct omni_wm *wm, struct omni_win *w);

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

void omni_draw_title(struct raster *r, int x, int y, int w,
                     const char *title, int active)
{
    uint32_t top    = active ? OMNI_COLOR_TITLE : OMNI_COLOR_TITLE_LO;
    uint32_t bottom = active ? OMNI_COLOR_TITLE_LO : omni_rgb(0x35, 0x35, 0x3a);

    raster_gradient_v(r, x, y, w, OMNI_WM_TITLE_H, top, bottom);
    raster_rect(r, x, y, w, OMNI_WM_TITLE_H, OMNI_COLOR_BORDER);

    if (title && title[0]) {
        struct canvas c;
        canvas_init(&c, r, OMNI_COLOR_WHITE, top);
        canvas_set_clip(&c, x + 6, y + 1, w - 6 - 24, OMNI_WM_TITLE_H - 2);
        c.cellh = 8;
        canvas_text(&c, title, x + 6, y + (OMNI_WM_TITLE_H - 8) / 2);
    }

    /* close button (X) */
    {
        int bx = x + w - 17, by = y + 3;
        raster_fill(r, bx, by, 13, 13, OMNI_COLOR_BTN_BG);
        raster_rect(r, bx, by, 13, 13, OMNI_COLOR_BORDER);
        raster_hline(r, bx + 3, bx + 9, by + 3, OMNI_COLOR_WHITE);
        raster_hline(r, bx + 3, bx + 9, by + 9, OMNI_COLOR_WHITE);
        raster_vline(r, bx + 3, by + 3, by + 9, OMNI_COLOR_WHITE);
        raster_vline(r, bx + 9, by + 3, by + 9, OMNI_COLOR_WHITE);
    }
}

static void paint_frame(struct omni_wm *wm, struct omni_win *w)
{
    omni_draw_title(&wm->screen, w->x, w->y, w->w, w->title,
                    w == wm->active);
    raster_rect(&wm->screen, w->x, w->y + OMNI_WM_TITLE_H,
                w->w, w->h - OMNI_WM_TITLE_H, OMNI_COLOR_BORDER);
}

void omni_win_paint_frame(struct omni_wm *wm, struct omni_win *w)
{
    paint_frame(wm, w);
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
    win->minw = 120;
    win->minh = OMNI_WM_TITLE_H + 24;
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
        wm->active = wm->nwin ? wm->order[0] : NULL;
}

void omni_wm_raise(struct omni_wm *wm, struct omni_win *w)
{
    int i;
    if (wm->order[0] == w)
        return;
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
        if (x >= w->x && x < w->x + w->w &&
            y >= w->y && y < w->y + w->h)
            return w;
    }
    return NULL;
}

int omni_wm_hit(struct omni_win *w, int x, int y)
{
    const int E = 6;
    int rx = x - w->x, ry = y - w->y;

    if (rx < 0 || ry < 0 || rx >= w->w || ry >= w->h)
        return 0;

    if (rx >= w->w - 19 && rx < w->w - 4 && ry >= 2 && ry < 17)
        return 2;

    if (ry < OMNI_WM_TITLE_H)
        return 1;

    if (rx < E && ry < E)                       return 3;
    if (rx >= w->w - E && ry < E)               return 9;
    if (rx < E && ry >= w->h - E)               return 7;
    if (rx >= w->w - E && ry >= w->h - E)       return 5;
    if (rx < E)                                 return 4;
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
            omni_wm_paint(wm);
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
    omni_wm_paint(wm);
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

    raster_gradient_v(&wm->screen, 0, 0, wm->screen.w, wm->screen.h,
                      OMNI_COLOR_DESKTOP, omni_rgb(0x0a, 0x1e, 0x38));

    for (i = wm->nwin - 1; i >= 0; i--) {
        struct omni_win *w = wm->order[i];
        int dy = w->y + OMNI_WM_TITLE_H;
        int src_y = 0;
        int src_h = w->h - OMNI_WM_TITLE_H;

        if (dy < 0) {
            src_y = -dy;
            src_h -= src_y;
            dy = 0;
        }
        if (src_h > 0)
            raster_blit_clip(&wm->screen, &w->surface,
                             w->x, dy, 0, src_y, w->w, src_h);
        paint_frame(wm, w);
    }
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
        int x = 40 + (ci * 24) % 200;
        int y = 40 + (ci * 24) % 160;
        struct omni_win *win;

        if (m.n < 2 || w <= 0 || h <= 0) {
            send_line(cl->fd, "ERR OPEN needs width height title\n");
            return 0;
        }
        if (cl->win) {
            send_line(cl->fd, "ERR client already has a window\n");
            return 0;
        }
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
        omni_wm_paint(wm);
        return 0;
    }

    if (strcmp(m.verb, "CLOSE") == 0 || strcmp(m.verb, "QUIT") == 0) {
        drop_client(wm, ci);
        omni_wm_paint(wm);
        return (strcmp(m.verb, "QUIT") == 0) ? 1 : 0;
    }

    if (!cl->win) {
        send_line(cl->fd, "ERR no window (OPEN first)\n");
        return 0;
    }

    if (strcmp(m.verb, "TITLE") == 0) {
        snprintf(cl->win->title, sizeof(cl->win->title), "%s", m.text);
        omni_wm_paint(wm);
    } else if (strcmp(m.verb, "RAISE") == 0) {
        omni_wm_raise(wm, cl->win);
        omni_wm_paint(wm);
    } else if (strcmp(m.verb, "CLEAR") == 0) {
        struct raster *s = &cl->win->surface;
        uint32_t c = m.text[0] ? parse_color(m.text) : OMNI_COLOR_FACE;
        raster_fill(s, 0, 0, s->w, s->h, c);
        omni_wm_paint(wm);
    } else if (strcmp(m.verb, "FILL") == 0) {
        struct raster *s = &cl->win->surface;
        uint32_t c;
        if (m.n < 5) { send_line(cl->fd, "ERR FILL x y w h color\n"); return 0; }
        c = parse_color(m.text);
        raster_fill(s, (int)m.num[1], (int)m.num[2],
                    (int)m.num[3], (int)m.num[4], c);
        omni_wm_paint(wm);
    } else if (strcmp(m.verb, "RECT") == 0) {
        struct raster *s = &cl->win->surface;
        uint32_t c;
        if (m.n < 5) { send_line(cl->fd, "ERR RECT x y w h color\n"); return 0; }
        c = parse_color(m.text);
        raster_rect(s, (int)m.num[1], (int)m.num[2],
                    (int)m.num[3], (int)m.num[4], c);
        omni_wm_paint(wm);
    } else if (strcmp(m.verb, "TEXT") == 0) {
        struct raster *s = &cl->win->surface;
        struct canvas cv;
        canvas_init(&cv, s, OMNI_COLOR_TEXT, OMNI_COLOR_FACE);
        canvas_text(&cv, m.text, (int)m.num[1], (int)m.num[2]);
        omni_wm_paint(wm);
    } else {
        char buf[OMNI_PROTO_MAX_LINE];
        snprintf(buf, sizeof(buf), "ERR unknown verb %s\n", m.verb);
        send_line(cl->fd, buf);
    }
    return 0;
}

/* Dispatch one buffered protocol line from the client. Called repeatedly
 * for as long as more complete lines are already buffered. */
int omni_wm_handle_client(struct omni_wm *wm, int ci)
{
    struct omni_client *cl = &wm->clients[ci];
    char buf[OMNI_PROTO_MAX_LINE];

    if (cl->fd < 0)
        return -1;

    for (;;) {
        char peek;
        ssize_t rr;

        memset(buf, 0, sizeof(buf));
        if (proto_recv(cl->fd, buf, sizeof(buf)) < 0) {
            drop_client(wm, ci);
            omni_wm_paint(wm);
            return -1;
        }
        if (buf[0] == '\0')
            break;
        if (handle_line(wm, ci, buf) == 1) {
            drop_client(wm, ci);
            omni_wm_paint(wm);
            return -1;
        }
        /* continue only if another full line is already buffered */
        rr = recv(cl->fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
        if (rr <= 0)
            break;
    }
    return 0;
}
