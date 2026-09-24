/*
 * OmniOS — os/gui/theme.h
 *
 * The desktop's look: palette plus the drawing toolkit behind it —
 * procedural wallpaper, alpha-blended and anti-aliased rounded
 * rectangles, soft drop shadows, transparent and 2x-smoothed text, app
 * icons and the OmniOS logo.
 *
 * Everything draws into native 32-bit rasters in RAM (the compositor's
 * back buffer or a window surface), so reading pixels back for blending
 * is cheap. Colours are 0xRRGGBB; alpha is 0..255.
 */
#ifndef OMNI_OS_GUI_THEME_H
#define OMNI_OS_GUI_THEME_H

#include "../lib/omni.h"

/* ---- palette ------------------------------------------------------- */
#define TH_ACCENT        0x3b82f6   /* blue                              */
#define TH_ACCENT_2      0x8b5cf6   /* violet                            */
#define TH_TITLE_ACTIVE  0xf7f8fb   /* window title bars                 */
#define TH_TITLE_IDLE    0xe6e9ef
#define TH_TITLE_TEXT    0x1a1d23
#define TH_TITLE_TEXT_2  0x8a919b   /* inactive title text               */
#define TH_CAPTION_HOVER 0xdde1e7   /* minimize button hover             */
#define TH_CLOSE_HOVER   0xe81123   /* close button hover                */
#define TH_TASKBAR       0x0b1020   /* taskbar tint (blended)            */
#define TH_PANEL         0x161b2c   /* start menu panel (blended)        */
#define TH_TEXT_LIGHT    0xf3f5f9
#define TH_TEXT_DIM      0x9aa3b2

/* ---- pixels ---------------------------------------------------------- */
static inline uint32_t th_blend(uint32_t dst, uint32_t src, unsigned a)
{
    uint32_t rb, g;
    if (a >= 255) return src | 0xff000000u;
    if (a == 0)   return dst | 0xff000000u;
    rb = ((src & 0xff00ff) * a + (dst & 0xff00ff) * (255 - a) + 0x800080) >> 8;
    g  = ((src & 0x00ff00) * a + (dst & 0x00ff00) * (255 - a) + 0x008000) >> 8;
    return 0xff000000u | (rb & 0xff00ff) | (g & 0x00ff00);
}

/* blend one pixel, clipped to the raster */
static inline void th_px(struct raster *r, int x, int y, uint32_t rgb, unsigned a)
{
    if (x >= 0 && y >= 0 && x < r->w && y < r->h) {
        uint32_t *p = r->bits + (size_t)y * (size_t)r->stride + (size_t)x;
        *p = th_blend(*p, rgb, a);
    }
}

/* colour c lightened (amt > 0) or darkened (amt < 0), amt in -255..255 */
uint32_t th_shade(uint32_t c, int amt);

/* ---- shapes ---------------------------------------------------------- */
/* fill w x h with rgb at alpha a (clipped to the raster) */
void th_fill_a(struct raster *r, int x, int y, int w, int h, uint32_t rgb,
               unsigned a);
/* filled rounded rectangle, anti-aliased corners, blended at alpha a */
void th_round_rect(struct raster *r, int x, int y, int w, int h, int rad,
                   uint32_t rgb, unsigned a);
/* soft drop shadow of the rounded rect (x, y+dy, w, h): darkens pixels
 * outside (x, y, w, h), fading out over `blur` px; strength = max alpha */
void th_shadow(struct raster *r, int x, int y, int w, int h, int rad,
               int blur, int dy, unsigned strength);
/* frosted glass: blur the pixels of (x, y, w, h) in place (box blur of
 * radius `blur`, two passes ~ gaussian). Pixels outside a rounded rect of
 * corner radius rad (<= 16) stay sharp, so rounded panels look clean.   */
void th_frost(struct raster *r, int x, int y, int w, int h, int rad, int blur);
/* coverage (0..255) of pixel (px, py) of a top-left corner square of a
 * rounded rect with radius rad (mirror the coordinates for the others) */
unsigned th_corner_cov(int px, int py, int rad);

/* ---- text (8x8 font, transparent background) ----------------------- */
int  th_text(struct raster *r, const char *s, int x, int y, uint32_t rgb);
int  th_text_bold(struct raster *r, const char *s, int x, int y, uint32_t rgb);
/* clipped to max_w pixels; returns the width drawn */
int  th_text_clip(struct raster *r, const char *s, int x, int y, uint32_t rgb,
                  int max_w);
/* 16 px tall headings: the 8x8 font scaled 2x with Scale2x smoothing;
 * 14 px advance. Returns the width. */
int  th_text2x(struct raster *r, const char *s, int x, int y, uint32_t rgb);
int  th_text2x_width(const char *s);

/* ---- artwork ---------------------------------------------------------- */
/* the modern wallpaper, rendered once into w x h pixels */
void th_wallpaper(uint32_t *px, int w, int h);
/* app icon: rounded square with a gentle gradient and the initial */
void th_icon(struct raster *r, int x, int y, int size, uint32_t rgb, char letter);
/* the OmniOS logo: a blue-to-violet ring, anti-aliased */
void th_logo(struct raster *r, int cx, int cy, int radius);

#endif /* OMNI_OS_GUI_THEME_H */
