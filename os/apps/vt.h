/*
 * OmniOS — os/apps/vt.h
 *
 * A small VT100/xterm-subset terminal emulator core for the Terminal app:
 * a cell grid plus an escape-sequence parser. Pure logic, no I/O — the
 * app feeds it the shell's output and draws the dirty rows, so it can be
 * unit-tested on any host.
 *
 * Supported: printable ASCII (other UTF-8 shows as '?'), BS HT LF VT FF CR,
 * ESC 7/8 D E M c, CSI A B C D E F G H J K L M P S T X @ a d e f m n r s u
 * c `, DEC private modes 1 (cursor keys) 6 (origin) 7 (autowrap)
 * 25 (cursor) 47/1047/1049 (alternate screen), SGR 16 colours + bold/
 * underline/reverse (38/48 extended colours are skipped over), OSC/DCS
 * strings are swallowed. DSR 5/6 and DA get answered through t->reply.
 */
#ifndef OMNI_OS_APPS_VT_H
#define OMNI_OS_APPS_VT_H

#include <stddef.h>

#define VT_MAX_COLS 160
#define VT_MAX_ROWS 64

#define VT_BOLD      0x01
#define VT_UNDERLINE 0x02
#define VT_REVERSE   0x04

#define VT_DEFAULT_FG 7
#define VT_DEFAULT_BG 0

struct vt_cell {
    unsigned char ch;           /* 32..126                              */
    unsigned char fg, bg;       /* palette index 0..15                  */
    unsigned char attr;         /* VT_BOLD | VT_UNDERLINE | VT_REVERSE  */
};

struct vt {
    int cols, rows;
    struct vt_cell cell[VT_MAX_ROWS][VT_MAX_COLS];
    struct vt_cell alt[VT_MAX_ROWS][VT_MAX_COLS];  /* inactive screen   */
    unsigned char dirty[VT_MAX_ROWS];              /* row needs redraw  */
    int in_alt;                 /* alternate screen active              */

    int cx, cy;                 /* cursor, 0-based                      */
    int wrapnext;               /* printed in last column: wrap first   */
    int top, bot;               /* scroll region, inclusive             */
    unsigned char fg, bg, attr; /* current rendition                    */
    int cursor_on;              /* DECTCEM                              */
    int app_cursor;             /* DECCKM: cursor keys send ESC O x     */
    int autowrap;               /* DECAWM                               */
    int origin;                 /* DECOM                                */

    int sv_cx, sv_cy;           /* ESC 7 / CSI s                        */
    unsigned char sv_fg, sv_bg, sv_attr;

    int state;                  /* parser state                         */
    int par[16], npar, priv;    /* CSI parameters, private marker       */
    int utf8_left;              /* UTF-8 continuation bytes pending     */

    char reply[64];             /* answers for the program (DSR, DA):   */
    int  nreply;                /* the app writes them to the pty       */
    unsigned long bells;
};

void vt_init(struct vt *t, int cols, int rows);
void vt_feed(struct vt *t, const char *buf, size_t n);

/* Bytes a key press sends to the program (keycode = Linux KEY_*, ch = the
 * character from the keymap or 0). Returns the length written to out. */
int  vt_key(const struct vt *t, int key, char ch, char *out, int outsz);

/* Row r as text (trailing spaces trimmed), for tests and diagnostics. */
void vt_row_text(const struct vt *t, int r, char *out, int outsz);

#endif /* OMNI_OS_APPS_VT_H */
