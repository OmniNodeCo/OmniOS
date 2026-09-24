/*
 * OmniOS — os/lib/canvas.c
 *
 * Text rendering over the raster layer using the embedded 8x8 public-domain
 * font (os/vendor/font8x8/font8x8_basic.h). Handles UTF-8 input by
 * translating to the closest glyph in the basic-Latin table.
 */
#include <string.h>

#include "omni.h"
#include "../vendor/font8x8/font8x8_basic.h"

uint32_t omni_utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p;

    if (c == 0)
        return 0;
    if (c < 0x80) {
        (*s)++;
        return c;
    }
    if ((c & 0xe0) == 0xc0 && p[1]) {
        uint32_t r = ((uint32_t)(c & 0x1f) << 6) | (p[1] & 0x3f);
        *s += 2;
        return r;
    }
    if ((c & 0xf0) == 0xe0 && p[1] && p[2]) {
        uint32_t r = ((uint32_t)(c & 0x0f) << 12) |
                     ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
        *s += 3;
        return r;
    }
    /* invalid/unsupported: advance one byte */
    (*s)++;
    return 0xfffd;
}

void canvas_init(struct canvas *c, struct raster *r, uint32_t fg, uint32_t bg)
{
    c->r = *r;
    c->fg = fg;
    c->bg = bg;
    c->cx = 0;
    c->cy = 0;
    c->cellw = 8;
    c->cellh = 8;
    c->tab = 4;
    c->linewrap = 1;
    c->clip_x = 0;
    c->clip_y = 0;
    c->clip_w = r->w;
    c->clip_h = r->h;
}

void canvas_set_clip(struct canvas *c, int x, int y, int w, int h)
{
    c->clip_x = x;
    c->clip_y = y;
    c->clip_w = w;
    c->clip_h = h;
}

void canvas_gotoxy(struct canvas *c, int col, int row)
{
    c->cx = col * c->cellw;
    c->cy = row * c->cellh;
}

void canvas_clear(struct canvas *c, uint32_t bg)
{
    raster_fill(&c->r, c->clip_x, c->clip_y, c->clip_w, c->clip_h, bg);
    c->cx = c->clip_x;
    c->cy = c->clip_y;
}

/* The 8 row bytes of a glyph (LSB = leftmost pixel); '?' for anything
 * outside printable ASCII. For drawing code that renders glyphs itself
 * (transparent or scaled text in os/gui/theme.c). */
const unsigned char *omni_glyph8(uint32_t ch)
{
    if (ch < 32 || ch > 127)
        ch = '?';
    return (const unsigned char *)font8x8_basic[ch];
}

void canvas_draw_char(struct canvas *c, uint32_t ch, int x, int y,
                      uint32_t fg, uint32_t bg)
{
    int row;
    int px;

    if (ch < 32 || ch > 127)
        ch = '?';

    for (row = 0; row < 8; row++) {
        unsigned glyph = (unsigned)font8x8_basic[ch][row];
        for (px = 0; px < 8; px++) {
            /* font8x8 encodes each row least-significant-bit first
             * (the LSB is the leftmost pixel), per the upstream README:
             * "the least significant bit of each byte corresponds to the
             * first pixel in a row" — so test bit `px`, not `7 - px`. */
            uint32_t color = (glyph & (1u << px)) ? fg : bg;
            raster_px(&c->r, x + px, y + row, color);
        }
    }
}

static void canvas_newline(struct canvas *c)
{
    c->cx = c->clip_x;
    c->cy += c->cellh;
    if (c->cy + c->cellh > c->clip_y + c->clip_h && c->linewrap) {
        raster_scroll(&c->r, c->cellh, c->bg);
        c->cy = c->clip_y + c->clip_h - c->cellh;
    }
}

void canvas_puts_raw(struct canvas *c, uint32_t ch)
{
    if (ch == '\n') {
        canvas_newline(c);
        return;
    }
    if (ch == '\r') {
        c->cx = c->clip_x;
        return;
    }
    if (ch == '\t') {
        int cells = c->tab - ((c->cx - c->clip_x) / c->cellw) % c->tab;
        c->cx += cells * c->cellw;
        return;
    }
    if (c->cx + c->cellw > c->clip_x + c->clip_w) {
        if (c->linewrap)
            canvas_newline(c);
        else
            return;
    }
    canvas_draw_char(c, ch, c->cx, c->cy, c->fg, c->bg);
    c->cx += c->cellw;
}

int canvas_text(struct canvas *c, const char *s, int x, int y)
{
    const char *p = s;
    c->cx = x;
    c->cy = y;
    while (*p) {
        uint32_t ch = omni_utf8_next(&p);
        canvas_puts_raw(c, ch);
    }
    return c->cx - x;
}

int canvas_text_len(struct canvas *c, const char *s, int x, int y, int maxw)
{
    int lastw = c->linewrap;
    int w = 0;
    const char *p = s;
    c->linewrap = 0;
    c->cx = x;
    c->cy = y;
    while (*p) {
        uint32_t ch = omni_utf8_next(&p);
        if (c->cx - x + c->cellw > maxw)
            break;
        canvas_puts_raw(c, ch);
    }
    w = c->cx - x;
    c->linewrap = lastw;
    return w;
}

void canvas_fillcell(struct canvas *c, int col, int row, int n)
{
    raster_fill(&c->r,
                c->clip_x + col * c->cellw,
                c->clip_y + row * c->cellh,
                n * c->cellw, c->cellh, c->bg);
}
