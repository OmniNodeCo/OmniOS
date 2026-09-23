/*
 * OmniOS — os/gui/client.c
 *
 * Client side of the OmniOS window protocol.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>

#include "client.h"

static int connect_socket(void)
{
    struct sockaddr_un addr;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", OMNI_WM_SOCKET);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int omni_client_open(struct omni_client_conn *c, const char *appname)
{
    char line[OMNI_PROTO_MAX_LINE];
    char rbuf[OMNI_PROTO_MAX_LINE];
    long num[1] = { OMNI_PROTO_VERSION };
    struct proto_msg m;

    memset(c, 0, sizeof(*c));
    c->fd = -1;

    c->fd = connect_socket();
    if (c->fd < 0)
        return -1;

    /* HELLO <proto> <appname> */
    snprintf(line, sizeof(line), "HELLO %d %s\n", OMNI_PROTO_VERSION,
             appname ? appname : "app");
    if (proto_send(c->fd, line) < 0)
        goto fail;
    (void)num;

    /* read HELLO response, tolerate any initial OK/ERR ordering */
    for (;;) {
        if (proto_recv(c->fd, rbuf, sizeof(rbuf)) < 0)
            goto fail;
        if (rbuf[0] == '\0')
            continue;
        if (proto_parse(rbuf, &m) < 0)
            continue;
        if (strcmp(m.verb, "HELLO") == 0 && m.n >= 2) {
            c->screen_w = (int)m.num[0];
            c->screen_h = (int)m.num[1];
            return 0;
        }
        if (strcmp(m.verb, "ERR") == 0)
            goto fail;
    }

fail:
    close(c->fd);
    c->fd = -1;
    return -1;
}

void omni_client_close(struct omni_client_conn *c)
{
    if (c->fd >= 0) {
        proto_send(c->fd, "QUIT\n");
        close(c->fd);
    }
    c->fd = -1;
    c->win = 0;
}

int omni_client_window(struct omni_client_conn *c, const char *title,
                       int w, int h)
{
    char line[OMNI_PROTO_MAX_LINE];
    char rbuf[OMNI_PROTO_MAX_LINE];
    long num[2] = { w, h };
    struct proto_msg m;

    proto_build_text(line, sizeof(line), "OPEN", 2, num,
                     title ? title : "window");
    if (proto_send(c->fd, line) < 0)
        return -1;

    for (;;) {
        if (proto_recv(c->fd, rbuf, sizeof(rbuf)) < 0)
            return -1;
        if (rbuf[0] == '\0')
            continue;
        if (proto_parse(rbuf, &m) < 0)
            continue;
        if (strcmp(m.verb, "OK") == 0 && m.n >= 1) {
            c->win = (int)m.num[0];
            return 0;
        }
        if (strcmp(m.verb, "ERR") == 0)
            return -1;
    }
}

static int send_cmd(const struct omni_client_conn *c, const char *verb,
                    int numc, const long *num)
{
    char line[OMNI_PROTO_MAX_LINE];
    if (c->fd < 0 || c->win <= 0)
        return -1;
    proto_build(line, sizeof(line), verb, numc, num);
    return proto_send(c->fd, line);
}

int omni_client_clear(struct omni_client_conn *c, uint32_t rgb)
{
    char line[OMNI_PROTO_MAX_LINE];
    char hexc[16];
    long num[3] = { c->win, 0, 0 };
    if (c->fd < 0 || c->win <= 0)
        return -1;
    snprintf(hexc, sizeof(hexc), "%06x", rgb & 0xffffff);
    proto_build_text(line, sizeof(line), "CLEAR", 3, num, hexc);
    return proto_send(c->fd, line);
}

int omni_client_fill(struct omni_client_conn *c, int x, int y, int w, int h,
                     uint32_t rgb)
{
    char line[OMNI_PROTO_MAX_LINE];
    char hexc[16];
    long num[5] = { c->win, x, y, w, h };
    if (c->fd < 0 || c->win <= 0)
        return -1;
    snprintf(hexc, sizeof(hexc), "%06x", rgb & 0xffffff);
    proto_build_text(line, sizeof(line), "FILL", 5, num, hexc);
    return proto_send(c->fd, line);
}

int omni_client_rect(struct omni_client_conn *c, int x, int y, int w, int h,
                     uint32_t rgb)
{
    char line[OMNI_PROTO_MAX_LINE];
    char hexc[16];
    long num[5] = { c->win, x, y, w, h };
    if (c->fd < 0 || c->win <= 0)
        return -1;
    snprintf(hexc, sizeof(hexc), "%06x", rgb & 0xffffff);
    proto_build_text(line, sizeof(line), "RECT", 5, num, hexc);
    return proto_send(c->fd, line);
}

int omni_client_text(struct omni_client_conn *c, int x, int y, const char *s)
{
    char line[OMNI_PROTO_MAX_LINE];
    long num[3] = { c->win, x, y };
    if (c->fd < 0 || c->win <= 0)
        return -1;
    proto_build_text(line, sizeof(line), "TEXT", 3, num, s);
    return proto_send(c->fd, line);
}

int omni_client_poll(struct omni_client_conn *c, struct omni_client_event *e)
{
    char rbuf[OMNI_PROTO_MAX_LINE];
    struct proto_msg m;
    fd_set rfds;
    struct timeval tv = { 0, 0 };

    FD_ZERO(&rfds);
    FD_SET(c->fd, &rfds);
    if (select(c->fd + 1, &rfds, NULL, NULL, &tv) <= 0)
        return 0;

    if (proto_recv(c->fd, rbuf, sizeof(rbuf)) < 0)
        return -1;
    if (rbuf[0] == '\0')
        return 0;
    if (proto_parse(rbuf, &m) < 0)
        return 0;

    memset(e, 0, sizeof(*e));
    if (strcmp(m.verb, "KEY") == 0 && m.n >= 4) {
        e->type = 1;
        e->key = (int)m.num[1];
        e->pressed = (int)m.num[2];
        e->text = (char)(m.num[3] & 0xff);
        return 1;
    }
    if (strcmp(m.verb, "BTN") == 0 && m.n >= 5) {
        e->type = 2;
        e->x = (int)m.num[1];
        e->y = (int)m.num[2];
        e->key = (int)m.num[3];
        e->pressed = (int)m.num[4];
        return 1;
    }
    if (strcmp(m.verb, "CLOSE") == 0) {
        e->type = 3;
        return 1;
    }
    if (strcmp(m.verb, "BYE") == 0)
        return -1;

    /* unknown server message: keep looking */
    return 0;
}
