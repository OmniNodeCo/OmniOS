/*
 * OmniOS — os/lib/devinput.c
 *
 * Device input backends:
 *   /dev/input/event*    evdev protocol (preferred: keyboard + mouse)
 *   /dev/input/mice      PS/2 "ImPS/2" auxiliary 3-byte protocol
 *   /dev/tty             raw-mode scancodes (last resort keyboard)
 *
 * Events are decoded into struct omni_input and pushed onto the shared
 * queue from input.c. The desktop polls these fds with poll(2).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <linux/input.h>

#include "omni.h"

/* ------------------------------------------------------------------ */
/* evdev                                                              */
/* ------------------------------------------------------------------ */

/* Per-open shift state tracked across the descriptor's lifetime. */
static int ev_shift = 0;

static void ev_key(int code, int value)
{
    char ch = 0;

    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) {
        ev_shift = value != 0;
        return;
    }

    if (value != 0)               /* press or repeat -> printable     */
        ch = omni_key_char((unsigned)code, ev_shift);

    omni_input_push_key(code, value, ev_shift, ch);
}

static int read_ev(struct omni_devs *d, int fd)
{
    struct input_event ie;
    ssize_t n;

    (void)d;
    for (;;) {
        n = read(fd, &ie, sizeof(ie));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            return -1;
        }
        if (n < (ssize_t)sizeof(ie))
            return 0;

        switch (ie.type) {
        case EV_KEY:
            if (ie.value >= 0 && ie.value <= 2)
                ev_key(ie.code, ie.value);
            break;
        case EV_REL:
            if (ie.code == REL_X)
                omni_input_push_mouse(ie.value, 0);
            else if (ie.code == REL_Y)
                omni_input_push_mouse(0, ie.value);
            else if (ie.code == REL_WHEEL)
                omni_input_push_button(ie.value > 0 ? 4 : 5, 0);
            break;
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* PS/2 mice 3-byte protocol                                          */
/* ------------------------------------------------------------------ */

static int read_mice(struct omni_devs *d, int fd)
{
    unsigned char p[3];
    ssize_t n;

    for (;;) {
        n = read(fd, p, 3);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            return -1;
        }
        if (n < 3)
            return 0;

        if (p[0] & 0x08) /* always set in ImPS/2 */
            omni_input_push_mouse((int)((int8_t)p[1]), (int)((int8_t)(-p[2])));

        if (p[0] & 0x01)
            omni_input_push_button(1, 1);
        if (p[0] & 0x02)
            omni_input_push_button(3, 1);
        if (p[0] & 0x04)
            omni_input_push_button(2, 1);
        (void)d;
    }
}

/* ------------------------------------------------------------------ */
/* tty raw scancodes (fallback)                                       */
/* ------------------------------------------------------------------ */

static int read_tty(struct omni_devs *d, int fd)
{
    unsigned char raw;
    ssize_t n;
    int shift = 0;

    for (;;) {
        n = read(fd, &raw, 1);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            return -1;
        }
        if (n < 1)
            return 0;

        if (raw == 0xe0) {      /* extended prefix: skip next */
            char ext;
            read(fd, &ext, 1);
            continue;
        }
        if (raw & 0x80) {       /* release */
            char ch = omni_key_char(raw & 0x7f, shift);
            omni_input_push_key(raw & 0x7f, 0, shift, ch);
            continue;
        }
        if (raw == 0x2a || raw == 0x36) { /* shift make */
            shift = 1;
            continue;
        }
        {
            char ch = omni_key_char(raw, shift);
            omni_input_push_key(raw, 1, shift, ch);
        }
        (void)d;
    }
}

/* ------------------------------------------------------------------ */
/* device set                                                         */
/* ------------------------------------------------------------------ */

void omni_devs_open(struct omni_devs *d)
{
    int i;
    char path[64];

    memset(d, 0, sizeof(*d));
    d->mice_fd = -1;
    d->tty_fd = -1;
    for (i = 0; i < 8; i++)
        d->ev_fd[i] = -1;

    /* evdev keyboard/mouse */
    for (i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            d->ev_fd[d->ev_n++] = fd;
        }
    }

    d->mice_fd = open("/dev/input/mice", O_RDONLY | O_NONBLOCK);
    if (d->mice_fd < 0)
        d->mice_fd = -1;

    d->tty_fd = open("/dev/tty", O_RDONLY | O_NONBLOCK);
    if (d->tty_fd >= 0) {
        struct termios t;
        if (tcgetattr(d->tty_fd, &t) == 0) {
            cfmakeraw(&t);
            tcsetattr(d->tty_fd, TCSANOW, &t);
        }
    }
}

void omni_devs_close(struct omni_devs *d)
{
    int i;
    for (i = 0; i < d->ev_n; i++)
        if (d->ev_fd[i] >= 0)
            close(d->ev_fd[i]);
    if (d->mice_fd >= 0)
        close(d->mice_fd);
    if (d->tty_fd >= 0)
        close(d->tty_fd);
    memset(d, 0, sizeof(*d));
}

int omni_devs_nfds(struct omni_devs *d)
{
    return d->ev_n + (d->mice_fd >= 0 ? 1 : 0) + (d->tty_fd >= 0 ? 1 : 0);
}

void omni_devs_fill(struct omni_devs *d, struct pollfd *pfds)
{
    int i, n = 0;
    for (i = 0; i < d->ev_n; i++) {
        pfds[n].fd = d->ev_fd[i];
        pfds[n].events = POLLIN;
        pfds[n].revents = 0;
        n++;
    }
    if (d->mice_fd >= 0) {
        pfds[n].fd = d->mice_fd;
        pfds[n].events = POLLIN;
        pfds[n].revents = 0;
        n++;
    }
    if (d->tty_fd >= 0) {
        pfds[n].fd = d->tty_fd;
        pfds[n].events = POLLIN;
        pfds[n].revents = 0;
        n++;
    }
}

int omni_devs_drain(struct omni_devs *d, int idx)
{
    int fd;

    if (idx < d->ev_n) {
        fd = d->ev_fd[idx];
        return read_ev(d, fd);
    }
    idx -= d->ev_n;
    if (idx == 0 && d->mice_fd >= 0)
        return read_mice(d, d->mice_fd);
    idx -= (d->mice_fd >= 0 ? 1 : 0);
    if (idx == 0 && d->tty_fd >= 0)
        return read_tty(d, d->tty_fd);
    return -1;
}

void omni_console_puts(const char *s)
{
    int fd = open("/dev/console", O_WRONLY);
    if (fd < 0)
        return;
    omni_tty_write_all(fd, s, strlen(s));
    close(fd);
}
