/*
 * OmniOS — os/lib/devinput.c
 *
 * Device input backends:
 *   /dev/input/event*    evdev protocol (preferred: keyboard + mouse)
 *   /dev/input/mice      PS/2 "ImPS/2" auxiliary 3-byte protocol (fallback)
 *   /dev/tty             raw-mode AT scancodes (last resort keyboard)
 *
 * Events are decoded into struct omni_input and pushed onto the shared
 * queue from input.c. The desktop polls these fds with poll(2).
 *
 * Single-source rule: the mousedev driver and the in-kernel VT are
 * *additional* consumers of the same input devices that evdev exposes,
 * so at most ONE of {evdev, mice} may be read for the pointer and at
 * most ONE of {evdev, tty} for the keyboard — otherwise every motion or
 * key press would be applied twice.
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
#include <sys/ioctl.h>
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
            /* Mouse buttons ride the EV_KEY event type on evdev; keep
             * them out of the keyboard stream.  1=left 2=middle 3=right. */
            switch (ie.code) {
            case BTN_LEFT:   omni_input_push_button(1, ie.value); continue;
            case BTN_MIDDLE: omni_input_push_button(2, ie.value); continue;
            case BTN_RIGHT:  omni_input_push_button(3, ie.value); continue;
            default:
                break;
            }
            if (ie.value >= 0 && ie.value <= 2)
                ev_key(ie.code, ie.value);
            break;
        case EV_REL:
            if (ie.code == REL_X)
                omni_input_push_mouse(ie.value, 0);
            else if (ie.code == REL_Y)
                omni_input_push_mouse(0, ie.value);
            else if (ie.code == REL_WHEEL)
                omni_input_push_button(ie.value > 0 ? 4 : 5, 1);
            else if (ie.code == REL_HWHEEL)
                omni_input_push_button(ie.value > 0 ? 6 : 7, 1);
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

    (void)d;
    for (;;) {
        n = read(fd, p, 3);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            return -1;
        }
        if (n < 3)
            return 0;

        if (p[0] & 0x08) /* sync bit always set in ImPS/2 */
            omni_input_push_mouse((int)(int8_t)p[1], (int)(int8_t)(-p[2]));

        /* button transitions come on their own packet, so report the
         * resulting state (press AND release) every time so the desktop
         * sees a full click.  BTN_LEFT 1, BTN_MIDDLE 2 (bit 0x04),
         * BTN_RIGHT 3 (bit 0x02). */
        omni_input_push_button(1, (p[0] & 0x01) ? 1 : 0);
        omni_input_push_button(2, (p[0] & 0x04) ? 1 : 0);
        omni_input_push_button(3, (p[0] & 0x02) ? 1 : 0);
    }
}

/* ------------------------------------------------------------------ */
/* tty raw scancodes (fallback)                                       */
/* ------------------------------------------------------------------ */

/* The /dev/tty fallback delivers raw AT set-1 scancodes, while evdev
 * delivers Linux input event codes (linux/input.h).  The main block of
 * the keyboard happens to use identical numbers in both schemes, so
 * only the stragglers need translating before the shared lookup table. */
static int at_to_linux_code(unsigned sc)
{
    switch (sc) {
    case 0x3d: return KEY_SPACE;      /* 61  space                       */
    case 0x69: return KEY_KP1;        /* 105 numpad 1                    */
    case 0x72: return KEY_KP2;        /* 114 numpad 2                    */
    case 0x7b: return KEY_KP3;        /* 123 numpad 3                    */
    case 0x70: return KEY_KP4;        /* 112 numpad 4                    */
    case 0x71: return KEY_KP5;        /* 113 numpad 5                    */
    case 0x7a: return KEY_KP6;        /* 122 numpad 6                    */
    case 0x75: return KEY_KP7;        /* 117 numpad 7                    */
    case 0x76: return KEY_KP8;        /* 118 numpad 8                    */
    case 0x77: return KEY_KP9;        /* 119 numpad 9                    */
    case 0x6b: return KEY_KP0;        /* 107 numpad 0                    */
    case 0x6e: return KEY_KPDOT;      /* 110 numpad .                    */
    case 0x54: return KEY_KPPLUS;     /* 84  numpad +                    */
    case 0x52: return KEY_KPMINUS;    /* 82  numpad -                    */
    default:   return (int)sc;
    }
}

static int read_tty(struct omni_devs *d, int fd)
{
    unsigned char raw;
    ssize_t n;
    int shift = 0;

    (void)d;
    for (;;) {
        n = read(fd, &raw, 1);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            return -1;
        }
        if (n < 1)
            return 0;

        if (raw == 0xe0) {      /* extended prefix */
            unsigned char ext;
            if (read(fd, &ext, 1) != 1)
                continue;
            if (ext == 0x53)    /* extended backspace (e0 53) */
                omni_input_push_key(KEY_BACKSPACE, 1, shift, 0x08);
            continue;
        }
        if (raw == 0xaa || raw == 0xb6) { shift = 0; continue; }
        if (raw & 0x80) {       /* release */
            int code = at_to_linux_code(raw & 0x7f);
            omni_input_push_key(code, 0, shift, 0);
            continue;
        }
        if (raw == 0x2a || raw == 0x36) { /* shift make */
            shift = 1;
            continue;
        }
        {
            int code = at_to_linux_code(raw);
            char ch = omni_key_char(code, shift);
            omni_input_push_key(code, 1, shift, ch);
        }
    }
}

/* ------------------------------------------------------------------ */
/* device set                                                         */
/* ------------------------------------------------------------------ */

/* True if one of the already-opened evdev fds carries a capability
 * bit: type=EV_REL, bit 0 is REL_X (a pointer); type=EV_KEY, bit 30
 * is KEY_A — on every real keyboard, on no mouse (mouse buttons ride
 * EV_KEY too, so "has EV_KEY" alone is not a keyboard test). */
static int evdev_has_bit(struct omni_devs *d, int type, int bit)
{
    int i;
    unsigned long bits[1];

    for (i = 0; i < d->ev_n; i++) {
        memset(bits, 0, sizeof(bits));
        if (ioctl(d->ev_fd[i], EVIOCGBIT(type, sizeof(bits)), bits) > 0 &&
            (bits[0] & (1UL << bit)))
            return 1;
    }
    return 0;
}

/* One pass: (re)open /dev/input/event0..7, then decide which legacy
 * sources are safe to open:
 *   /dev/input/mice  only if no evdev device carries the pointer,
 *   /dev/tty         only if no evdev device carries the keyboard.
 * A legacy source opened on an earlier pass is closed again as soon
 * as its evdev equivalent appears (USB mice can enumerate after boot).
 * Public: the desktop's main loop keeps calling it every couple of
 * seconds until a pointer and a keyboard have both been found.  */
void omni_devs_rescan(struct omni_devs *d)
{
    int i;
    char path[64];

    for (i = 0; i < 8; i++) {
        if (d->ev_fd[i] >= 0)
            close(d->ev_fd[i]);
        d->ev_fd[i] = -1;
    }
    d->ev_n = 0;

    for (i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0)
            d->ev_fd[d->ev_n++] = fd;
    }

    d->ev_has_rel = evdev_has_bit(d, EV_REL, 0);
    d->ev_has_kbd = evdev_has_bit(d, EV_KEY, KEY_A);

    if (d->ev_has_rel) {
        if (d->mice_fd >= 0) {
            close(d->mice_fd);
            d->mice_fd = -1;
        }
    } else if (d->mice_fd < 0) {
        d->mice_fd = open("/dev/input/mice", O_RDONLY | O_NONBLOCK);
        if (d->mice_fd < 0)
            d->mice_fd = -1;
    }

    if (d->ev_has_kbd) {
        if (d->tty_fd >= 0) {
            close(d->tty_fd);
            d->tty_fd = -1;
        }
    } else if (d->tty_fd < 0) {
        d->tty_fd = open("/dev/tty", O_RDONLY | O_NONBLOCK);
        if (d->tty_fd >= 0) {
            struct termios t;
            if (tcgetattr(d->tty_fd, &t) == 0) {
                cfmakeraw(&t);
                tcsetattr(d->tty_fd, TCSANOW, &t);
            }
        } else {
            d->tty_fd = -1;
        }
    }
}

void omni_devs_open(struct omni_devs *d)
{
    int i, try;

    memset(d, 0, sizeof(*d));
    d->mice_fd = -1;
    d->tty_fd = -1;
    for (i = 0; i < 8; i++)
        d->ev_fd[i] = -1;

    for (try = 0; try < 20; try++) {
        omni_devs_rescan(d);
        if ((d->ev_has_rel || d->mice_fd >= 0) &&
            (d->ev_has_kbd || d->tty_fd >= 0))
            break;
        if (try < 19)
            usleep(250 * 1000);     /* wait out USB enumeration at boot */
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
