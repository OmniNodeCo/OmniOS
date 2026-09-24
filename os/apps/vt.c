/*
 * OmniOS — os/apps/vt.c
 *
 * Terminal emulator core (see vt.h). Written from scratch for OmniOS.
 */
#include <stdio.h>
#include <string.h>

#include "vt.h"

enum { S_GROUND, S_ESC, S_CSI, S_OSC, S_OSC_ESC, S_STR, S_STR_ESC,
       S_SKIP1 };

/* ------------------------------------------------------------------ */
/* grid helpers                                                       */
/* ------------------------------------------------------------------ */

static void blank(struct vt *t, struct vt_cell *c)
{
    c->ch = ' ';
    c->fg = VT_DEFAULT_FG;
    c->bg = t->bg;              /* erase with the current background */
    c->attr = 0;
}

static void clear_cells(struct vt *t, int row, int from, int to)
{
    int x;
    if (row < 0 || row >= t->rows)
        return;
    if (from < 0) from = 0;
    if (to > t->cols) to = t->cols;
    for (x = from; x < to; x++)
        blank(t, &t->cell[row][x]);
    t->dirty[row] = 1;
}

static void clear_rows(struct vt *t, int from, int to)
{
    int r;
    for (r = from; r <= to; r++)
        clear_cells(t, r, 0, t->cols);
}

/* scroll rows top..bot up by n (content moves up, blank lines at bot) */
static void scroll_up(struct vt *t, int top, int bot, int n)
{
    int r;
    if (top < 0 || bot >= t->rows || top > bot || n <= 0)
        return;
    if (n > bot - top + 1)
        n = bot - top + 1;
    for (r = top; r + n <= bot; r++)
        memcpy(t->cell[r], t->cell[r + n], sizeof(t->cell[r]));
    clear_rows(t, bot - n + 1, bot);
    for (r = top; r <= bot; r++)
        t->dirty[r] = 1;
}

/* scroll rows top..bot down by n (blank lines appear at top) */
static void scroll_down(struct vt *t, int top, int bot, int n)
{
    int r;
    if (top < 0 || bot >= t->rows || top > bot || n <= 0)
        return;
    if (n > bot - top + 1)
        n = bot - top + 1;
    for (r = bot; r - n >= top; r--)
        memcpy(t->cell[r], t->cell[r - n], sizeof(t->cell[r]));
    clear_rows(t, top, top + n - 1);
    for (r = top; r <= bot; r++)
        t->dirty[r] = 1;
}

static void linefeed(struct vt *t)
{
    if (t->cy == t->bot)
        scroll_up(t, t->top, t->bot, 1);
    else if (t->cy < t->rows - 1)
        t->cy++;
    t->wrapnext = 0;
}

static void reverse_index(struct vt *t)
{
    if (t->cy == t->top)
        scroll_down(t, t->top, t->bot, 1);
    else if (t->cy > 0)
        t->cy--;
    t->wrapnext = 0;
}

static void put(struct vt *t, unsigned char ch)
{
    struct vt_cell *c;

    if (t->wrapnext) {
        t->wrapnext = 0;
        if (t->autowrap) {
            t->cx = 0;
            linefeed(t);
        }
    }
    c = &t->cell[t->cy][t->cx];
    c->ch = ch;
    c->fg = t->fg;
    c->bg = t->bg;
    c->attr = t->attr;
    t->dirty[t->cy] = 1;
    if (t->cx >= t->cols - 1)
        t->wrapnext = 1;        /* deferred wrap, like a real VT100 */
    else
        t->cx++;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* move the cursor, honouring origin mode for the row */
static void go(struct vt *t, int x, int y)
{
    int lo = t->origin ? t->top : 0;
    int hi = t->origin ? t->bot : t->rows - 1;
    t->cx = clampi(x, 0, t->cols - 1);
    t->cy = clampi(y + (t->origin ? t->top : 0), lo, hi);
    t->wrapnext = 0;
}

static void reply(struct vt *t, const char *s)
{
    size_t n = strlen(s);
    if ((size_t)t->nreply + n <= sizeof(t->reply)) {
        memcpy(t->reply + t->nreply, s, n);
        t->nreply += (int)n;
    }
}

static void save_cursor(struct vt *t)
{
    t->sv_cx = t->cx;
    t->sv_cy = t->cy;
    t->sv_fg = t->fg;
    t->sv_bg = t->bg;
    t->sv_attr = t->attr;
}

static void restore_cursor(struct vt *t)
{
    t->cx = clampi(t->sv_cx, 0, t->cols - 1);
    t->cy = clampi(t->sv_cy, 0, t->rows - 1);
    t->fg = t->sv_fg;
    t->bg = t->sv_bg;
    t->attr = t->sv_attr;
    t->wrapnext = 0;
}

static void swap_screens(struct vt *t)
{
    int r;
    for (r = 0; r < t->rows; r++) {
        struct vt_cell tmp[VT_MAX_COLS];
        memcpy(tmp, t->cell[r], sizeof(tmp));
        memcpy(t->cell[r], t->alt[r], sizeof(tmp));
        memcpy(t->alt[r], tmp, sizeof(tmp));
        t->dirty[r] = 1;
    }
    t->in_alt = !t->in_alt;
}

static void reset_state(struct vt *t)
{
    t->cx = t->cy = 0;
    t->wrapnext = 0;
    t->top = 0;
    t->bot = t->rows - 1;
    t->fg = VT_DEFAULT_FG;
    t->bg = VT_DEFAULT_BG;
    t->attr = 0;
    t->cursor_on = 1;
    t->app_cursor = 0;
    t->autowrap = 1;
    t->origin = 0;
    t->state = S_GROUND;
    t->utf8_left = 0;
    save_cursor(t);
}

void vt_init(struct vt *t, int cols, int rows)
{
    int r;
    memset(t, 0, sizeof(*t));
    t->cols = clampi(cols, 2, VT_MAX_COLS);
    t->rows = clampi(rows, 2, VT_MAX_ROWS);
    reset_state(t);
    clear_rows(t, 0, t->rows - 1);
    for (r = 0; r < t->rows; r++)
        memcpy(t->alt[r], t->cell[r], sizeof(t->alt[r]));
}

/* ------------------------------------------------------------------ */
/* CSI                                                                */
/* ------------------------------------------------------------------ */

static int P(const struct vt *t, int i, int def)
{
    return (i < t->npar && t->par[i] > 0) ? t->par[i] : def;
}

static void sgr(struct vt *t)
{
    int i;
    if (t->npar == 0) {
        t->fg = VT_DEFAULT_FG; t->bg = VT_DEFAULT_BG; t->attr = 0;
        return;
    }
    for (i = 0; i < t->npar; i++) {
        int p = t->par[i];
        if (p == 0) { t->fg = VT_DEFAULT_FG; t->bg = VT_DEFAULT_BG; t->attr = 0; }
        else if (p == 1)  t->attr |= VT_BOLD;
        else if (p == 4)  t->attr |= VT_UNDERLINE;
        else if (p == 7)  t->attr |= VT_REVERSE;
        else if (p == 22) t->attr &= (unsigned char)~VT_BOLD;
        else if (p == 24) t->attr &= (unsigned char)~VT_UNDERLINE;
        else if (p == 27) t->attr &= (unsigned char)~VT_REVERSE;
        else if (p >= 30 && p <= 37)   t->fg = (unsigned char)(p - 30);
        else if (p == 39)              t->fg = VT_DEFAULT_FG;
        else if (p >= 40 && p <= 47)   t->bg = (unsigned char)(p - 40);
        else if (p == 49)              t->bg = VT_DEFAULT_BG;
        else if (p >= 90 && p <= 97)   t->fg = (unsigned char)(p - 90 + 8);
        else if (p >= 100 && p <= 107) t->bg = (unsigned char)(p - 100 + 8);
        else if (p == 38 || p == 48) {
            /* 38;5;n (256 colours) or 38;2;r;g;b: map the first 16
             * indexed colours, skip the rest of the parameters */
            int is_fg = p == 38;
            if (i + 2 < t->npar && t->par[i + 1] == 5) {
                int n = t->par[i + 2];
                if (n < 16) {
                    if (is_fg) t->fg = (unsigned char)n;
                    else       t->bg = (unsigned char)n;
                }
                i += 2;
            } else if (i + 1 < t->npar && t->par[i + 1] == 2) {
                i += 4;
            }
        }
    }
}

static void set_mode(struct vt *t, int on)
{
    int i;
    for (i = 0; i < t->npar; i++) {
        int p = t->par[i];
        if (t->priv != '?')
            continue;           /* ANSI modes (IRM, LNM) not supported */
        switch (p) {
        case 1:  t->app_cursor = on; break;
        case 6:  t->origin = on; go(t, 0, 0); break;
        case 7:  t->autowrap = on; break;
        case 25: t->cursor_on = on; t->dirty[t->cy] = 1; break;
        case 47: case 1047: case 1049:
            if (on && !t->in_alt) {
                if (p == 1049) save_cursor(t);
                swap_screens(t);
                clear_rows(t, 0, t->rows - 1);
            } else if (!on && t->in_alt) {
                swap_screens(t);
                if (p == 1049) restore_cursor(t);
            }
            break;
        default: break;
        }
    }
}

static void csi(struct vt *t, char f)
{
    int n = P(t, 0, 1);
    int x, r;

    switch (f) {
    case 'A':
        t->cy = (t->cy >= t->top) ? clampi(t->cy - n, t->top, t->rows - 1)
                                  : clampi(t->cy - n, 0, t->rows - 1);
        t->wrapnext = 0;
        break;
    case 'B': case 'e':
        t->cy = (t->cy <= t->bot) ? clampi(t->cy + n, 0, t->bot)
                                  : clampi(t->cy + n, 0, t->rows - 1);
        t->wrapnext = 0;
        break;
    case 'C': case 'a':
        t->cx = clampi(t->cx + n, 0, t->cols - 1); t->wrapnext = 0; break;
    case 'D':
        t->cx = clampi(t->cx - n, 0, t->cols - 1); t->wrapnext = 0; break;
    case 'E':
        t->cx = 0; t->cy = clampi(t->cy + n, 0, t->rows - 1); t->wrapnext = 0; break;
    case 'F':
        t->cx = 0; t->cy = clampi(t->cy - n, 0, t->rows - 1); t->wrapnext = 0; break;
    case 'G': case '`':
        t->cx = clampi(n - 1, 0, t->cols - 1); t->wrapnext = 0; break;
    case 'd':
        go(t, t->cx, n - 1); break;
    case 'H': case 'f':
        go(t, P(t, 1, 1) - 1, P(t, 0, 1) - 1); break;
    case 'J':
        switch (t->npar ? t->par[0] : 0) {
        case 0:
            clear_cells(t, t->cy, t->cx, t->cols);
            clear_rows(t, t->cy + 1, t->rows - 1);
            break;
        case 1:
            clear_rows(t, 0, t->cy - 1);
            clear_cells(t, t->cy, 0, t->cx + 1);
            break;
        default:
            clear_rows(t, 0, t->rows - 1);
            break;
        }
        break;
    case 'K':
        switch (t->npar ? t->par[0] : 0) {
        case 0:  clear_cells(t, t->cy, t->cx, t->cols); break;
        case 1:  clear_cells(t, t->cy, 0, t->cx + 1); break;
        default: clear_cells(t, t->cy, 0, t->cols); break;
        }
        break;
    case 'L':
        if (t->cy >= t->top && t->cy <= t->bot) {
            scroll_down(t, t->cy, t->bot, n);
            t->cx = 0;
        }
        break;
    case 'M':
        if (t->cy >= t->top && t->cy <= t->bot) {
            scroll_up(t, t->cy, t->bot, n);
            t->cx = 0;
        }
        break;
    case '@':
        n = clampi(n, 1, t->cols - t->cx);
        for (x = t->cols - 1; x >= t->cx + n; x--)
            t->cell[t->cy][x] = t->cell[t->cy][x - n];
        clear_cells(t, t->cy, t->cx, t->cx + n);
        break;
    case 'P':
        n = clampi(n, 1, t->cols - t->cx);
        for (x = t->cx; x + n < t->cols; x++)
            t->cell[t->cy][x] = t->cell[t->cy][x + n];
        clear_cells(t, t->cy, t->cols - n, t->cols);
        break;
    case 'X':
        clear_cells(t, t->cy, t->cx, t->cx + n);
        break;
    case 'S':
        if (!t->priv) scroll_up(t, t->top, t->bot, n);
        break;
    case 'T':
        if (!t->priv && t->npar <= 1) scroll_down(t, t->top, t->bot, n);
        break;
    case 'm':
        if (!t->priv) sgr(t);
        break;
    case 'h': set_mode(t, 1); break;
    case 'l': set_mode(t, 0); break;
    case 'r':
        if (!t->priv) {
            int top = P(t, 0, 1) - 1, bot = P(t, 1, t->rows) - 1;
            if (bot > t->rows - 1) bot = t->rows - 1;
            if (top < bot) {
                t->top = top;
                t->bot = bot;
                go(t, 0, 0);
            }
        }
        break;
    case 'n':
        if (!t->priv && t->npar >= 1 && t->par[0] == 5) {
            reply(t, "\033[0n");
        } else if (t->npar >= 1 && t->par[0] == 6) {
            char b[32];
            r = t->cy - (t->origin ? t->top : 0);
            snprintf(b, sizeof(b), "\033[%d;%dR", r + 1, t->cx + 1);
            reply(t, b);
        }
        break;
    case 'c':
        if (t->priv == '>')
            reply(t, "\033[>0;0;0c");
        else if (!t->priv && P(t, 0, 0) == 0)
            reply(t, "\033[?1;2c");     /* "VT100 with AVO" */
        break;
    case 's': if (!t->priv) save_cursor(t); break;
    case 'u': if (!t->priv) restore_cursor(t); break;
    default: break;             /* t (window ops), q (cursor style), ... */
    }
}

/* ------------------------------------------------------------------ */
/* byte-level parser                                                  */
/* ------------------------------------------------------------------ */

static void esc(struct vt *t, unsigned char c)
{
    t->state = S_GROUND;
    switch (c) {
    case '[':
        t->state = S_CSI;
        t->npar = 0;
        t->priv = 0;
        memset(t->par, 0, sizeof(t->par));
        break;
    case ']': t->state = S_OSC; break;
    case 'P': case 'X': case '^': case '_': t->state = S_STR; break;
    case '(': case ')': case '*': case '+': case '#': case '%':
        t->state = S_SKIP1;     /* charset designation etc.: one byte */
        break;
    case '7': save_cursor(t); break;
    case '8': restore_cursor(t); break;
    case 'D': linefeed(t); break;
    case 'E': t->cx = 0; linefeed(t); break;
    case 'M': reverse_index(t); break;
    case 'c': {
        int cols = t->cols, rows = t->rows;
        vt_init(t, cols, rows);
        break;
    }
    default: break;             /* '=' '>' keypad modes, 'H' tab set ... */
    }
}

static void csi_byte(struct vt *t, unsigned char c)
{
    if (c >= '0' && c <= '9') {
        if (t->npar == 0)
            t->npar = 1;
        if (t->par[t->npar - 1] < 10000)
            t->par[t->npar - 1] = t->par[t->npar - 1] * 10 + (c - '0');
    } else if (c == ';' || c == ':') {
        if (t->npar == 0)
            t->npar = 1;
        if (t->npar < (int)(sizeof(t->par) / sizeof(t->par[0])))
            t->par[t->npar++] = 0;
    } else if (c == '?' || c == '>' || c == '=' || c == '<') {
        t->priv = c;
    } else if (c >= 0x20 && c <= 0x2f) {
        /* intermediate byte (e.g. "CSI 2 SP q"): ignored */
    } else if (c >= 0x40 && c <= 0x7e) {
        t->state = S_GROUND;
        csi(t, (char)c);
    } else {
        t->state = S_GROUND;    /* malformed */
    }
}

static void control(struct vt *t, unsigned char c)
{
    switch (c) {
    case 0x07: t->bells++; break;
    case 0x08:
        if (t->cx > 0) t->cx--;
        t->wrapnext = 0;
        break;
    case 0x09:
        t->cx = clampi((t->cx / 8 + 1) * 8, 0, t->cols - 1);
        t->wrapnext = 0;
        break;
    case 0x0a: case 0x0b: case 0x0c: linefeed(t); break;
    case 0x0d: t->cx = 0; t->wrapnext = 0; break;
    case 0x1b: t->state = S_ESC; break;
    case 0x18: case 0x1a: t->state = S_GROUND; break;
    default: break;             /* SO/SI, NUL, ... */
    }
}

void vt_feed(struct vt *t, const char *buf, size_t n)
{
    size_t i;
    int old_cx = t->cx, old_cy = t->cy;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];

        /* string states swallow everything up to BEL or ESC \ */
        if (t->state == S_OSC || t->state == S_STR) {
            if (c == 0x07) t->state = S_GROUND;
            else if (c == 0x1b) t->state = (t->state == S_OSC) ? S_OSC_ESC : S_STR_ESC;
            continue;
        }
        if (t->state == S_OSC_ESC || t->state == S_STR_ESC) {
            t->state = (c == '\\') ? S_GROUND
                     : (t->state == S_OSC_ESC ? S_OSC : S_STR);
            continue;
        }
        if (t->state == S_SKIP1) {
            t->state = S_GROUND;
            continue;
        }

        if (c < 0x20 || c == 0x7f) {        /* C0 controls act anywhere */
            if (c != 0x7f)
                control(t, c);
            continue;
        }
        if (t->state == S_ESC) { esc(t, c); continue; }
        if (t->state == S_CSI) { csi_byte(t, c); continue; }

        if (c >= 0x80) {                    /* UTF-8: one cell per char */
            if (c >= 0xc0 && c <= 0xf7) {
                t->utf8_left = (c >= 0xf0) ? 3 : (c >= 0xe0) ? 2 : 1;
            } else if (c <= 0xbf && t->utf8_left > 0) {
                if (--t->utf8_left == 0)
                    put(t, '?');
            }
            continue;
        }
        t->utf8_left = 0;
        put(t, c);
    }

    /* the cursor is drawn inverted: old and new rows need a repaint */
    if (t->cx != old_cx || t->cy != old_cy) {
        if (old_cy >= 0 && old_cy < t->rows) t->dirty[old_cy] = 1;
        t->dirty[t->cy] = 1;
    }
}

/* ------------------------------------------------------------------ */
/* keyboard                                                           */
/* ------------------------------------------------------------------ */

int vt_key(const struct vt *t, int key, char ch, char *out, int outsz)
{
    const char *s = NULL;
    char one[2] = { 0, 0 };
    int app = t->app_cursor;
    int n;

    switch (key) {
    case 28: case 96: s = "\r"; break;            /* Enter, keypad Enter */
    case 14:  s = "\177"; break;                  /* Backspace = DEL     */
    case 15:  s = "\t"; break;
    case 1:   s = "\033"; break;                  /* Esc                 */
    case 103: s = app ? "\033OA" : "\033[A"; break;
    case 108: s = app ? "\033OB" : "\033[B"; break;
    case 106: s = app ? "\033OC" : "\033[C"; break;
    case 105: s = app ? "\033OD" : "\033[D"; break;
    case 102: s = app ? "\033OH" : "\033[H"; break;   /* Home            */
    case 107: s = app ? "\033OF" : "\033[F"; break;   /* End             */
    case 110: s = "\033[2~"; break;               /* Insert              */
    case 111: s = "\033[3~"; break;               /* Delete              */
    case 104: s = "\033[5~"; break;               /* Page Up             */
    case 109: s = "\033[6~"; break;               /* Page Down           */
    case 59:  s = "\033OP"; break;                /* F1..F4              */
    case 60:  s = "\033OQ"; break;
    case 61:  s = "\033OR"; break;
    case 62:  s = "\033OS"; break;
    case 63:  s = "\033[15~"; break;              /* F5..F12             */
    case 64:  s = "\033[17~"; break;
    case 65:  s = "\033[18~"; break;
    case 66:  s = "\033[19~"; break;
    case 67:  s = "\033[20~"; break;
    case 68:  s = "\033[21~"; break;
    case 87:  s = "\033[23~"; break;
    case 88:  s = "\033[24~"; break;
    default:
        if (ch) {
            one[0] = ch;
            s = one;
        }
        break;
    }
    if (!s)
        return 0;
    n = (int)strlen(s);
    if (n > outsz)
        return 0;
    memcpy(out, s, (size_t)n);
    return n;
}

void vt_row_text(const struct vt *t, int r, char *out, int outsz)
{
    int x, n = 0;
    if (outsz <= 0)
        return;
    if (r >= 0 && r < t->rows)
        for (x = 0; x < t->cols && n < outsz - 1; x++)
            out[n++] = (char)t->cell[r][x].ch;
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
}
