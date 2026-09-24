/*
 * OmniOS — os/gui/wm.h
 *
 * Window manager + display server. The shell process owns the framebuffer
 * and runs this server on a UNIX socket (/tmp/.omnios-wm). Each desktop
 * application is a separate client process that talks the line protocol in
 * os/gui/protocol.h to create/draw into/close windows and receive
 * keyboard/mouse events — exactly how a "real" desktop is put together.
 *
 * The server also draws its own internal windows (the welcome screen) via
 * omni_wm_add() with client index -1.
 */
#ifndef OMNI_OS_GUI_WM_H
#define OMNI_OS_GUI_WM_H

#include "../lib/omni.h"
#include "protocol.h"

#define OMNI_WM_TITLE_H 32         /* window title bar height           */
#define OMNI_TASKBAR_H  44         /* the shell's taskbar, bottom edge  */
#define OMNI_WM_MAX_WIN 16
#define OMNI_WM_MAX_CLI 16

/* window chrome colors */
#define OMNI_COLOR_DESKTOP  omni_rgb(0x1e, 0x4b, 0x7a)
#define OMNI_COLOR_TITLE    omni_rgb(0x2d, 0x7d, 0xd2)
#define OMNI_COLOR_TITLE_LO omni_rgb(0x1b, 0x59, 0x98)
#define OMNI_COLOR_BORDER   omni_rgb(0x10, 0x30, 0x52)
#define OMNI_COLOR_BTN_BG   omni_rgb(0xc4, 0x2b, 0x1c)
#define OMNI_COLOR_FACE     omni_rgb(0xf0, 0xf0, 0xf0)
#define OMNI_COLOR_TEXT     omni_rgb(0x10, 0x10, 0x10)
#define OMNI_COLOR_WHITE    omni_rgb(0xff, 0xff, 0xff)

struct omni_win {
    int    used;
    int    id;
    int    x, y, w, h;
    int    minw, minh;
    char   title[64];
    int    client;              /* index into omni_wm.clients, or -1     */
    int    minimized;           /* hidden until restored from the taskbar */
    struct raster surface;      /* backing store                         */
};

struct omni_client {
    int    fd;                  /* connected socket, -1 = empty          */
    int    hello;               /* HELLO received                        */
    struct omni_win *win;       /* the client's window (NULL until OPEN) */
    char   rx[OMNI_PROTO_MAX_LINE];  /* partial line receive buffer      */
    int    rxlen;
};

struct omni_wm {
    struct raster screen;       /* desktop surface (framebuffer)         */
    int    listen_fd;           /* UNIX socket listener                  */
    /* optional wallpaper hook: if set, omni_wm_paint() calls it to draw
     * the desktop background instead of the built-in gradient.           */
    void (*draw_background)(struct omni_wm *wm);
    /* optional frame-finish hook: called after every omni_wm_paint() so
     * the shell can draw its always-on-top chrome (taskbar/menu/cursor)
     * and then present/flip the buffer as one final step.                */
    void (*finish)(struct omni_wm *wm);
    /* optional: title-bar icon for a window — returns its colour (0 =
     * default) and may set *glyph (default: the title's first letter)  */
    uint32_t (*icon_for)(const char *title, char *glyph);
    struct omni_client clients[OMNI_WM_MAX_CLI];

    struct omni_win wins[OMNI_WM_MAX_WIN];
    struct omni_win *order[OMNI_WM_MAX_WIN];   /* front (0) to back      */
    int    nwin;
    struct omni_win *active;

    /* set by client drawing commands; the shell repaints once per loop
     * pass instead of once per message (omni_wm_paint() clears it)     */
    int    dirty;

    int    pointer_x, pointer_y;
    int    button_down, button_which;
    int    drag;                /* 0 none, 1 move, 2..9 resize zone      */
    int    drag_offx, drag_offy;
};

void omni_wm_init(struct omni_wm *wm, struct raster *screen);

/* start the UNIX-socket display server; returns 0 on success */
int  omni_wm_start(struct omni_wm *wm);
int  omni_wm_fd(struct omni_wm *wm);
void omni_wm_accept(struct omni_wm *wm);
/* read + dispatch one client's pending protocol lines; returns 0 ok, -1 dropped */
int  omni_wm_handle_client(struct omni_wm *wm, int ci);

/* internal windows (used by the shell itself) */
struct omni_win *omni_wm_add(struct omni_wm *wm, const char *title,
                             int x, int y, int w, int h);
void omni_wm_close(struct omni_wm *wm, struct omni_win *w);
/* raise + focus (also restores a minimized window) */
void omni_wm_raise(struct omni_wm *wm, struct omni_win *w);
void omni_wm_minimize(struct omni_wm *wm, struct omni_win *w);

struct omni_win *omni_wm_find(struct omni_wm *wm, int win_id);
struct omni_win *omni_wm_at(struct omni_wm *wm, int x, int y);

/* chrome hit zones: 0 client, 1 title, 2 close, 3..9 resize edges,
 * 10 minimize */
int  omni_wm_hit(struct omni_win *w, int x, int y);
int  omni_wm_button(struct omni_wm *wm, int x, int y, int btn, int pressed);
int  omni_wm_motion(struct omni_wm *wm, int x, int y);
int  omni_wm_key(struct omni_wm *wm, int key, int pressed, char ch);

/* redraw everything (clears wm->dirty). Input and protocol handling only
 * set wm->dirty; the shell paints once per event-loop pass. */
void omni_wm_paint(struct omni_wm *wm);

#endif /* OMNI_OS_GUI_WM_H */
