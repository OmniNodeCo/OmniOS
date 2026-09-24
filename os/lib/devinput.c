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
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/input.h>

#include "omni.h"

/* ------------------------------------------------------------------ */
/* evdev                                                              */
/* ------------------------------------------------------------------ */

/* Modifier state, shared by both keyboard paths (only one is active). */
static int kb_shift, kb_ctrl, kb_caps;

/* Track a modifier key; returns 1 if `code` was one (consumed). */
static int kb_modifier(int code, int value)
{
    switch (code) {
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT:
        kb_shift = value != 0;
        return 1;
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
        kb_ctrl = value != 0;
        return 1;
    case KEY_CAPSLOCK:
        if (value == 1)
            kb_caps = !kb_caps;
        return 1;
    default:
        return 0;
    }
}

/* The character a key press produces under the current modifiers:
 * Caps Lock flips letter case, Ctrl+letter gives the control character
 * (Ctrl+C = 0x03), exactly what a terminal expects. Keys without a
 * character (arrows, Delete, F-keys, ...) give 0: apps use the keycode. */
static char kb_char(int code)
{
    char ch = omni_key_char((unsigned)code, kb_shift);

    if (kb_caps) {
        if (ch >= 'a' && ch <= 'z')      ch = (char)(ch - 'a' + 'A');
        else if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    }
    if (kb_ctrl) {
        if (ch >= 'a' && ch <= 'z')              ch = (char)(ch - 'a' + 1);
        else if (ch >= 'A' && ch <= 'Z')         ch = (char)(ch - 'A' + 1);
        else if (ch == '[')                      ch = 0x1b;
        else if (ch == '\\')                     ch = 0x1c;
        else if (ch == ']')                      ch = 0x1d;
        else if ((unsigned char)ch >= 32)        ch = 0;
    }
    return ch;
}

static void ev_key(int code, int value)
{
    char ch = 0;

    if (kb_modifier(code, value))
        return;
    if (value != 0)               /* press or repeat -> character      */
        ch = kb_char(code);
    omni_input_push_key(code, value, kb_shift, ch);
}

/* Absolute axis value -> 0..65535 using the device's own range. */
static int abs_norm(const struct omni_devs *d, int idx, int axis)
{
    long long lo = d->ev_abs_min[idx][axis];
    long long hi = d->ev_abs_max[idx][axis];
    long long v  = d->ev_abs_cur[idx][axis];

    if (hi <= lo)
        return 0;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (int)((v - lo) * 65535 / (hi - lo));
}

/* One-time "events are arriving" note per device: the quickest way to
 * tell a hypervisor that routes the mouse elsewhere from a desktop bug. */
static void note_first_event(struct omni_devs *d, int idx, const char *kind)
{
    char line[96];

    if (d->ev_seen[idx])
        return;
    d->ev_seen[idx] = 1;
    snprintf(line, sizeof(line),
             "desktop: input: first %s event from event%d\n",
             kind, d->ev_num[idx]);
    omni_console_puts(line);
}

static int read_ev(struct omni_devs *d, int idx)
{
    struct input_event ie;
    ssize_t n;
    int fd = d->ev_fd[idx];

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
            note_first_event(d, idx, "key/button");
            /* Mouse buttons ride the EV_KEY event type on evdev; keep
             * them out of the keyboard stream.  1=left 2=middle 3=right.
             * BTN_TOUCH is the "click" of touchscreens and tablets. */
            switch (ie.code) {
            case BTN_LEFT:
            case BTN_TOUCH:  omni_input_push_button(1, ie.value); continue;
            case BTN_MIDDLE: omni_input_push_button(2, ie.value); continue;
            case BTN_RIGHT:  omni_input_push_button(3, ie.value); continue;
            default:
                break;
            }
            if (ie.value >= 0 && ie.value <= 2)
                ev_key(ie.code, ie.value);
            break;
        case EV_REL:
            note_first_event(d, idx, "relative pointer");
            if (ie.code == REL_X)
                omni_input_push_mouse(ie.value, 0);
            else if (ie.code == REL_Y)
                omni_input_push_mouse(0, ie.value);
            else if (ie.code == REL_WHEEL)
                omni_input_push_button(ie.value > 0 ? 4 : 5, 1);
            else if (ie.code == REL_HWHEEL)
                omni_input_push_button(ie.value > 0 ? 6 : 7, 1);
            break;
        case EV_ABS:
            /* Absolute pointers (VMware vmmouse, USB/virtio tablets)
             * report only the axes that changed; remember them and emit
             * one position per SYN_REPORT frame. */
            if (d->ev_abs[idx] && (ie.code == ABS_X || ie.code == ABS_Y)) {
                note_first_event(d, idx, "absolute pointer");
                d->ev_abs_cur[idx][ie.code == ABS_Y ? 1 : 0] = ie.value;
                d->ev_abs_dirty[idx] = 1;
            }
            break;
        case EV_SYN:
            if (ie.code == SYN_REPORT && d->ev_abs_dirty[idx]) {
                d->ev_abs_dirty[idx] = 0;
                omni_input_push_abs(abs_norm(d, idx, 0), abs_norm(d, idx, 1));
            }
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

/* The /dev/tty fallback is a terminal in raw termios mode. The keyboard
 * itself is NOT switched to K_RAW, so the kernel's keymap has already
 * turned keys into characters and VT escape sequences ("a", "\r", 0x7f,
 * "\033[3~" for Delete, "\033[A" for Up): decode those back into key
 * events. (The old code read these bytes as AT scancodes, so typed "a"
 * became a bogus key and Delete was never recognised.) */

/* keycode for a printable character, via the shared keymap */
static int tty_char_key(unsigned char c, int *shift)
{
    int code;
    for (code = 1; code < 128; code++) {
        if (omni_key_char((unsigned)code, 0) == (char)c) { *shift = 0; return code; }
        if (omni_key_char((unsigned)code, 1) == (char)c) { *shift = 1; return code; }
    }
    return 0;
}

static void tty_emit(int code, char ch)
{
    if (code <= 0)
        return;
    omni_input_push_key(code, 1, 0, ch);  /* a tty has no key releases: */
    omni_input_push_key(code, 0, 0, 0);   /* synthesize press + release  */
}

/* key for the escape sequence body after ESC ("[3~", "OA", "[A", ...) */
static int tty_seq_key(const char *q)
{
    static const struct { const char *seq; int code; } t[] = {
        { "[A", KEY_UP },     { "[B", KEY_DOWN },    { "[C", KEY_RIGHT },
        { "[D", KEY_LEFT },   { "OA", KEY_UP },      { "OB", KEY_DOWN },
        { "OC", KEY_RIGHT },  { "OD", KEY_LEFT },    { "[H", KEY_HOME },
        { "[F", KEY_END },    { "OH", KEY_HOME },    { "OF", KEY_END },
        { "[1~", KEY_HOME },  { "[7~", KEY_HOME },   { "[4~", KEY_END },
        { "[8~", KEY_END },   { "[2~", KEY_INSERT }, { "[3~", KEY_DELETE },
        { "[5~", KEY_PAGEUP },{ "[6~", KEY_PAGEDOWN },
        { "[[A", KEY_F1 },    { "[[B", KEY_F2 },     { "[[C", KEY_F3 },
        { "[[D", KEY_F4 },    { "[[E", KEY_F5 },     { "OP", KEY_F1 },
        { "OQ", KEY_F2 },     { "OR", KEY_F3 },      { "OS", KEY_F4 },
    };
    size_t i;
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (strcmp(q, t[i].seq) == 0)
            return t[i].code;
    return 0;
}

/* escape-sequence state survives across reads (bytes may be split) */
static char tty_seq[8];
static int  tty_seqlen = -1;          /* -1: not inside a sequence */

static int tty_seq_done(void)
{
    char last = tty_seq[tty_seqlen - 1];
    if (tty_seqlen == 1)
        return tty_seq[0] != '[' && tty_seq[0] != 'O';   /* ESC x: Alt+x */
    if (tty_seqlen == 2 && tty_seq[0] == '[' && tty_seq[1] == '[')
        return 0;                                      /* "[[A": F-key */
    return (last >= 'A' && last <= 'Z') || last == '~' ||
           tty_seqlen >= (int)sizeof(tty_seq) - 1;
}

static void tty_byte(unsigned char c)
{
    int shift = 0, code;

    if (tty_seqlen >= 0) {                       /* inside ESC ...  */
        tty_seq[tty_seqlen++] = (char)c;
        tty_seq[tty_seqlen] = '\0';
        if (tty_seq_done()) {
            tty_emit(tty_seq_key(tty_seq), 0);
            tty_seqlen = -1;
        }
        return;
    }
    switch (c) {
    case 0x1b: tty_seqlen = 0; tty_seq[0] = '\0'; return;
    case '\r': case '\n': tty_emit(KEY_ENTER, '\n'); return;
    case 0x7f: case 0x08: tty_emit(KEY_BACKSPACE, 0x08); return;
    case '\t': tty_emit(KEY_TAB, '\t'); return;
    default: break;
    }
    if (c >= 1 && c <= 26) {                     /* Ctrl+letter     */
        tty_emit(tty_char_key((unsigned char)('a' + c - 1), &shift), (char)c);
        return;
    }
    if (c >= 32 && c < 127) {
        code = tty_char_key(c, &shift);
        tty_emit(code, (char)c);
    }
}

static int read_tty(struct omni_devs *d, int fd)
{
    unsigned char buf[64];
    ssize_t n, i;

    (void)d;
    for (;;) {
        n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return -1;
        }
        if (n == 0)
            break;
        for (i = 0; i < n; i++)
            tty_byte(buf[i]);
    }
    /* a lone ESC with nothing after it in this burst is the Esc key */
    if (tty_seqlen == 0) {
        tty_emit(KEY_ESC, 0x1b);
        tty_seqlen = -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* device set                                                         */
/* ------------------------------------------------------------------ */

/* Safety net for an incomplete /dev: recreate a missing character-device
 * node from the "major:minor" that sysfs publishes for it. With a healthy
 * devtmpfs /dev every node exists already and this does nothing.
 * Returns 1 if the node was created, 0 if it existed, -1 otherwise. */
static int node_from_sysfs(const char *node, const char *sysdev)
{
    char buf[32];
    unsigned maj, min;
    struct stat st;
    ssize_t n;
    int fd;

    if (stat(node, &st) == 0)
        return 0;
    fd = open(sysdev, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;                          /* the device does not exist */
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    if (sscanf(buf, "%u:%u", &maj, &min) != 2)
        return -1;
    if (strncmp(node, "/dev/input/", 11) == 0)
        mkdir("/dev/input", 0755);
    if (mknod(node, S_IFCHR | 0600, makedev(maj, min)) != 0)
        return -1;
    return 1;
}

#define OMNI_LONG_BITS  (8 * (int)sizeof(unsigned long))
#define OMNI_NLONGS(n)  (((n) + OMNI_LONG_BITS - 1) / OMNI_LONG_BITS)

/* Does evdev device `fd` advertise capability `bit` of event `type`?
 * The buffer covers KEY_MAX, the largest bitmap (BTN_LEFT is bit 272,
 * so a single word is not enough for button tests). */
static int evdev_test_bit(int fd, int type, int bit)
{
    unsigned long bits[OMNI_NLONGS(KEY_MAX + 1)];

    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(type, sizeof(bits)), bits) < 0)
        return 0;
    return (int)((bits[bit / OMNI_LONG_BITS] >> (bit % OMNI_LONG_BITS)) & 1UL);
}

/* Classify opened device `i`:
 *   relative pointer  REL_X                       (PS/2, USB mice)
 *   absolute pointer  ABS_X + ABS_Y + BTN_LEFT/BTN_TOUCH
 *                     (VMware vmmouse, USB/virtio tablets; the button
 *                     test keeps joysticks and accelerometers out)
 *   keyboard          KEY_A — on every real keyboard, on no mouse
 *                     (mouse buttons ride EV_KEY too). */
static void evdev_classify(struct omni_devs *d, int i)
{
    int fd = d->ev_fd[i];
    int axis;

    d->ev_rel[i] = (unsigned char)evdev_test_bit(fd, EV_REL, REL_X);
    d->ev_kbd[i] = (unsigned char)evdev_test_bit(fd, EV_KEY, KEY_A);
    d->ev_abs[i] = (unsigned char)(evdev_test_bit(fd, EV_ABS, ABS_X) &&
                                   evdev_test_bit(fd, EV_ABS, ABS_Y) &&
                                   (evdev_test_bit(fd, EV_KEY, BTN_LEFT) ||
                                    evdev_test_bit(fd, EV_KEY, BTN_TOUCH)));
    d->ev_seen[i] = 0;
    d->ev_abs_dirty[i] = 0;

    for (axis = 0; axis < 2; axis++) {
        struct input_absinfo ai;

        d->ev_abs_min[i][axis] = 0;
        d->ev_abs_max[i][axis] = 0;
        d->ev_abs_cur[i][axis] = 0;
        if (!d->ev_abs[i])
            continue;
        memset(&ai, 0, sizeof(ai));
        if (ioctl(fd, EVIOCGABS(axis == 0 ? ABS_X : ABS_Y), &ai) < 0 ||
            ai.maximum <= ai.minimum) {
            d->ev_abs[i] = 0;          /* unusable range: ignore device */
            break;
        }
        d->ev_abs_min[i][axis] = ai.minimum;
        d->ev_abs_max[i][axis] = ai.maximum;
        d->ev_abs_cur[i][axis] = ai.value;
    }
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

    for (i = 0; i < OMNI_EV_MAX; i++) {
        if (d->ev_fd[i] >= 0)
            close(d->ev_fd[i]);
        d->ev_fd[i] = -1;
    }
    d->ev_n = 0;
    d->ev_has_rel = d->ev_has_abs = d->ev_has_kbd = 0;

    d->ev_created = 0;
    for (i = 0; i < OMNI_EV_MAX; i++) {
        char sys[64];
        int fd;

        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        snprintf(sys, sizeof(sys), "/sys/class/input/event%d/dev", i);
        if (node_from_sysfs(path, sys) > 0)
            d->ev_created++;
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        d->ev_fd[d->ev_n] = fd;
        d->ev_num[d->ev_n] = i;
        evdev_classify(d, d->ev_n);
        d->ev_has_rel |= d->ev_rel[d->ev_n];
        d->ev_has_abs |= d->ev_abs[d->ev_n];
        d->ev_has_kbd |= d->ev_kbd[d->ev_n];
        d->ev_n++;
    }

    /* mousedev translates relative AND absolute evdev pointers into
     * /dev/input/mice, so either kind rules the legacy source out */
    if (d->ev_has_rel || d->ev_has_abs) {
        if (d->mice_fd >= 0) {
            close(d->mice_fd);
            d->mice_fd = -1;
        }
    } else if (d->mice_fd < 0) {
        if (node_from_sysfs("/dev/input/mice", "/sys/class/input/mice/dev") > 0)
            d->ev_created++;
        d->mice_fd = open("/dev/input/mice", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (d->mice_fd < 0)
            d->mice_fd = -1;
    }

    if (d->ev_has_kbd) {
        if (d->tty_fd >= 0) {
            close(d->tty_fd);
            d->tty_fd = -1;
        }
    } else if (d->tty_fd < 0) {
        d->tty_fd = open("/dev/tty", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
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
    for (i = 0; i < OMNI_EV_MAX; i++)
        d->ev_fd[i] = -1;

    for (try = 0; try < 20; try++) {
        omni_devs_rescan(d);
        if ((d->ev_has_rel || d->ev_has_abs || d->mice_fd >= 0) &&
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
    if (idx < d->ev_n)
        return read_ev(d, idx);
    idx -= d->ev_n;
    if (idx == 0 && d->mice_fd >= 0)
        return read_mice(d, d->mice_fd);
    idx -= (d->mice_fd >= 0 ? 1 : 0);
    if (idx == 0 && d->tty_fd >= 0)
        return read_tty(d, d->tty_fd);
    return -1;
}

/* Diagnostics go to the kernel log (/dev/kmsg): printk copies them to
 * EVERY console, so they reach the serial log and dmesg.  Writing to
 * /dev/console instead only reaches the last console= on the command
 * line (tty1, the screen underneath the desktop), never the serial port.
 * Each write is one log record; a fresh open per call also gives each
 * message its own devkmsg rate-limit budget. */
void omni_console_puts(const char *s)
{
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT &&
        node_from_sysfs("/dev/kmsg", "/sys/class/mem/kmsg/dev") > 0)
        fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
    if (fd < 0)
        return;
    omni_tty_write_all(fd, s, strlen(s));
    close(fd);
}

void omni_devs_log(struct omni_devs *d)
{
    char line[160];
    int i;
    const char *ptr = d->ev_has_abs ? (d->ev_has_rel ? "evdev(abs+rel)" : "evdev(abs)")
                    : d->ev_has_rel ? "evdev(rel)"
                    : (d->mice_fd >= 0 ? "/dev/input/mice" : "NONE");
    const char *kbd = d->ev_has_kbd ? "evdev"
                    : (d->tty_fd >= 0 ? "/dev/tty" : "NONE");

    snprintf(line, sizeof(line),
             "desktop: input: evdev=%d pointer=%s keyboard=%s\n",
             d->ev_n, ptr, kbd);
    omni_console_puts(line);
    if (d->ev_created > 0) {
        snprintf(line, sizeof(line),
                 "desktop: input: WARN /dev was incomplete: created %d "
                 "input node(s) from sysfs\n", d->ev_created);
        omni_console_puts(line);
    }

    for (i = 0; i < d->ev_n; i++) {
        char name[64];

        memset(name, 0, sizeof(name));
        if (ioctl(d->ev_fd[i], EVIOCGNAME(sizeof(name) - 1), name) < 0)
            strcpy(name, "?");
        snprintf(line, sizeof(line),
                 "desktop: input:   event%d \"%s\"%s%s%s%s\n",
                 d->ev_num[i], name,
                 d->ev_rel[i] ? " rel-pointer" : "",
                 d->ev_abs[i] ? " abs-pointer" : "",
                 d->ev_kbd[i] ? " keyboard" : "",
                 (d->ev_rel[i] | d->ev_abs[i] | d->ev_kbd[i]) ? "" : " (unused)");
        omni_console_puts(line);
    }
}
