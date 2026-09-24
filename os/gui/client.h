/*
 * OmniOS — os/gui/client.h
 *
 * Client side of the OmniOS window protocol. Application binaries link this
 * and use it to create windows on the desktop shell (which owns the
 * framebuffer) and receive keyboard focus.
 */
#ifndef OMNI_OS_GUI_CLIENT_H
#define OMNI_OS_GUI_CLIENT_H

#include "../lib/omni.h"
#include "protocol.h"

struct omni_client_conn {
    int  fd;
    int  win;              /* window id assigned by the server, 0 = none */
    int  screen_w, screen_h;
};

/* connect + HELLO handshake; returns 0 on success */
int  omni_client_open(struct omni_client_conn *c, const char *appname);
void omni_client_close(struct omni_client_conn *c);

/* create a titled window; fills in w/h. Returns 0 on success.             */
int  omni_client_window(struct omni_client_conn *c, const char *title,
                        int w, int h);

/* drawing primitives (window coordinates, title bar excluded) */
int  omni_client_clear(struct omni_client_conn *c, uint32_t rgb);
int  omni_client_fill(struct omni_client_conn *c, int x, int y, int w, int h,
                      uint32_t rgb);
int  omni_client_rect(struct omni_client_conn *c, int x, int y, int w, int h,
                      uint32_t rgb);
int  omni_client_text(struct omni_client_conn *c, int x, int y,
                      const char *s);
/* text in explicit colours (0xRRGGBB); every glyph cell is painted opaque */
int  omni_client_textc(struct omni_client_conn *c, int x, int y,
                       uint32_t fg, uint32_t bg, const char *s);

/* retrieve one pending event from the server: 1 = got event, 0 = none,
 * -1 = disconnected. Event semantics mirror struct omni_input + win id.  */
struct omni_client_event {
    int  type;             /* 1 key, 2 button, 3 closed */
    int  key;
    int  pressed;
    char text;
    int  x, y;
};

int  omni_client_poll(struct omni_client_conn *c, struct omni_client_event *e);

#endif /* OMNI_OS_GUI_CLIENT_H */
