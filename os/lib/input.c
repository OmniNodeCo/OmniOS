/*
 * OmniOS — os/lib/input.c
 *
 * Input: keyboard translation table, raw-event decoding from
 * /dev/input/mice (mouse relative motion + buttons) and /dev/tty
 * (TTYKBD raw scancodes), plus tty helpers.
 *
 * The GUI reads the pointer and keyboard from evdev
 * (/dev/input/event*) whenever the kernel exposes them, and falls
 * back to /dev/input/mice + raw /dev/tty only when evdev has no such
 * device (see os/lib/devinput.c).  The table below is indexed by
 * Linux input event codes (include/uapi/linux/input.h — the codes
 * evdev delivers); the AT set-1 fallback path translates its
 * scancodes onto these before lookup.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

#include "omni.h"

/* ------------------------------------------------------------------ */
/* Keyboard table: Linux input event codes (KEY_*, uapi/linux/input.h). */
/* ------------------------------------------------------------------ */

static const struct omni_key kbd_scan[128] = {
    [1]     = {0, 0},       /* Esc */
    [2]     = {'1', '!'},   [3] = {'2', '@'},   [4] = {'3', '#'},
    [5]     = {'4', '$'},   [6] = {'5', '%'},   [7] = {'6', '^'},
    [8]     = {'7', '&'},   [9] = {'8', '*'},   [10] = {'9', '('},
    [11]    = {'0', ')'},   [12] = {'-', '_'},  [13] = {'=', '+'},
    [14]    = {0x08, 0x08}, /* Backspace */
    [15]    = {'\t', '\t'}, /* Tab */
    [16]    = {'q', 'Q'},   [17] = {'w', 'W'},  [18] = {'e', 'E'},
    [19]    = {'r', 'R'},   [20] = {'t', 'T'},  [21] = {'y', 'Y'},
    [22]    = {'u', 'U'},   [23] = {'i', 'I'},  [24] = {'o', 'O'},
    [25]    = {'p', 'P'},   [26] = {'[', '{'},  [27] = {']', '}'},
    [28]    = {'\n', '\n'}, /* Enter */
    [30]    = {'a', 'A'},   [31] = {'s', 'S'},  [32] = {'d', 'D'},
    [33]    = {'f', 'F'},   [34] = {'g', 'G'},  [35] = {'h', 'H'},
    [36]    = {'j', 'J'},   [37] = {'k', 'K'},  [38] = {'l', 'L'},
    [39]    = {';', ':'},   [40] = {'\'', '"'}, [41] = {'`', '~'},
    [43]    = {'\\', '|'},  [44] = {'z', 'Z'},  [45] = {'x', 'X'},
    [46]    = {'c', 'C'},   [47] = {'v', 'V'},  [48] = {'b', 'B'},
    [49]    = {'n', 'N'},   [50] = {'m', 'M'},  [51] = {',', '<'},
    [52]    = {'.', '>'},   [53] = {'/', '?'},  [55] = {'*', '*'},
    [57]    = {' ', ' '},   /* Space */
    [71]    = {'7', '7'},   [72] = {'8', '8'},  [73] = {'9', '9'},
    [74]    = {'-', '-'},   [75] = {'4', '4'},  [76] = {'5', '5'},
    [77]    = {'6', '6'},   [78] = {'+', '+'},  [79] = {'1', '1'},
    [80]    = {'2', '2'},   [81] = {'3', '3'},  [82] = {'0', '0'},
    [83]    = {'.', '.'},
    [86]    = {'<', '>'},   /* 102nd */
    [96]    = {'\n', '\n'}, /* KP enter */
};

const struct omni_key *omni_kbd_table(void)
{
    return kbd_scan;
}

char omni_key_char(unsigned code, int shift)
{
    const struct omni_key *k;
    if (code >= 128)
        return 0;
    k = &kbd_scan[code];
    return shift ? k->hi : k->lo;
}

/* ------------------------------------------------------------------ */
/* tty helpers                                                        */
/* ------------------------------------------------------------------ */

int omni_tty_open(const char *path, int *in, int *out)
{
    int fd = open(path, O_RDWR);
    if (fd < 0)
        return -1;
    if (in)  *in = fd;
    if (out) *out = fd;
    return 0;
}

int omni_tty_read_raw(int fd)
{
    unsigned char ch;
    ssize_t n = read(fd, &ch, 1);
    return n == 1 ? (int)ch : -1;
}

void omni_tty_write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0)
            break;
        buf += n;
        len -= (size_t)n;
    }
}

/* ------------------------------------------------------------------ */
/* Event queueing (simple ring shared by the input backends)          */
/* ------------------------------------------------------------------ */

#define OMNI_INPUT_RING 128

struct omni_input_queue {
    struct omni_input e[OMNI_INPUT_RING];
    int head, tail;
};

static struct omni_input_queue g_q;

void omni_input_init(struct omni_input *devs, int count)
{
    (void)devs;
    (void)count;
    g_q.head = g_q.tail = 0;
}

static void q_push(const struct omni_input *e)
{
    int next = (g_q.head + 1) % OMNI_INPUT_RING;
    if (next == g_q.tail)
        return; /* full: drop oldest */
    g_q.e[g_q.head] = *e;
    g_q.head = next;
}

int omni_input_next(struct omni_input *e)
{
    if (g_q.head == g_q.tail)
        return -1;
    *e = g_q.e[g_q.tail];
    g_q.tail = (g_q.tail + 1) % OMNI_INPUT_RING;
    return 0;
}

/* Push decoded events into the shared queue (used by the backends). */
void omni_input_push_key(int key, int pressed, int shift, char ch)
{
    struct omni_input e;
    memset(&e, 0, sizeof(e));
    (void)shift;
    e.type = 1;
    e.key = key;
    e.pressed = pressed;
    e.text = ch;
    q_push(&e);
}

void omni_input_push_mouse(int dx, int dy)
{
    struct omni_input e;
    memset(&e, 0, sizeof(e));
    e.type = 2;
    e.dx = dx;
    e.dy = dy;
    q_push(&e);
}

void omni_input_push_abs(int x, int y)
{
    struct omni_input e;
    memset(&e, 0, sizeof(e));
    e.type = 4;
    e.dx = x;
    e.dy = y;
    q_push(&e);
}

void omni_input_push_button(int btn, int pressed)
{
    struct omni_input e;
    memset(&e, 0, sizeof(e));
    e.type = 3;
    e.key = btn;
    e.pressed = pressed;
    q_push(&e);
}
