/*
 * OmniOS — os/gui/protocol.h
 *
 * Line-based window protocol between desktop applications and the desktop
 * shell (which is the sole owner of the framebuffer).
 *
 * Transport: SOCK_STREAM UNIX socket at /tmp/.omnios-wm. Every message is a
 * single line terminated by '\n'. Fields are space-separated; a "text"
 * field (TITLE / TEXT / TEXTC / ERR / colours) extends to end-of-line and
 * is taken verbatim after the verb's fixed count of numeric fields, so
 * "TEXT 1 10 10 42" draws "42".
 *
 *   client -> server:
 *     HELLO <proto> <appname>\n      introduce (proto: 1)
 *     OPEN <w> <h> <title...>\n     create a window, returns its id
 *     CLOSE <win>\n
 *     TITLE <win> <title...>\n
 *     RAISE <win>\n
 *     CLEAR <win> <rrggbb>\n
 *     FILL  <win> <x> <y> <w> <h> <rrggbb>\n
 *     RECT  <win> <x> <y> <w> <h> <rrggbb>\n
 *     TEXT  <win> <x> <y> <text...>\n
 *     TEXTC <win> <x> <y> <fg> <bg> <text...>\n   fg/bg: 0xRRGGBB as decimal
 *     QUIT\n
 *
 *   server -> client:
 *     HELLO <screen_w> <screen_h>\n           session established
 *     OK    <win>\n                           window created
 *     ERR   <reason...>\n
 *     CLOSE <win>\n                           window closed (app should exit)
 *     KEY   <win> <keycode> <pressed> <ch>\n
 *     BTN   <win> <x> <y> <btn> <pressed>\n
 *     BYE\n
 */
#ifndef OMNI_OS_GUI_PROTOCOL_H
#define OMNI_OS_GUI_PROTOCOL_H

#define OMNI_WM_SOCKET "/tmp/.omnios-wm"
#define OMNI_PROTO_VERSION 1

#define OMNI_PROTO_MAX_LINE 4096

/* A decoded protocol command exchanged in either direction. */
struct proto_msg {
    char verb[16];       /* HELLO/OPEN/CLOSE/...   */
    int  n;              /* numeric field count    */
    long num[16];        /* integer fields         */
    char text[OMNI_PROTO_MAX_LINE];   /* trailing text field (TITLE/TEXT/ERR) */
};

/* Parse one protocol line into m. Returns 0 on success, -1 on error. */
int  proto_parse(const char *line, struct proto_msg *m);

/* Build a protocol line from a verb + fixed numerica fields. Returns len.  */
int  proto_build(char *out, size_t outsz, const char *verb, int numc,
                 const long *num);

/* Build a protocol line ending in an arbitrary text field.               */
int  proto_build_text(char *out, size_t outsz, const char *verb,
                      int numc, const long *num, const char *text);

/* Send/recv one line on a connected socket.                              */
int  proto_send(int fd, const char *line);
int  proto_recv(int fd, char *buf, size_t bufsz);   /* -1 eof/err, 0 got  */

#endif /* OMNI_OS_GUI_PROTOCOL_H */
