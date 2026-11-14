/*
 * OmniOS — os/gui/font.c
 *
 * TrueType text with stb_truetype (see font.h). The font files are read
 * once into memory; each glyph is rasterized the first time it is needed
 * at a given size and quarter-pixel offset, then kept in a hash table
 * (cleared when it fills up).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "../vendor/stb/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define FONT_DIR   "/usr/share/fonts/omnios"
#define CACHE_SIZE 8192             /* slots (power of two)               */
#define CACHE_FILL 6000             /* clear the cache beyond this        */

static const char *const g_files[FONT_NFACES] = {
    "Selawik-Regular.ttf", "Selawik-Bold.ttf", "Selawik-Light.ttf",
};

struct face {
    stbtt_fontinfo info;
    unsigned char *data;
    int ok;
};

struct glyph {
    uint32_t key;                   /* 0 = empty slot                     */
    short    w, h, xoff, yoff;      /* bitmap size and offset             */
    int      adv;                   /* advance, 1/64 px                   */
    unsigned char *bmp;
};

static struct face  g_face[FONT_NFACES];
static struct glyph g_cache[CACHE_SIZE];
static int g_cached, g_inited, g_ok;
static float g_ascent_em = 0.99f, g_line_em = 1.2f;
static unsigned char g_gamma[256];

static unsigned char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf = NULL;
    long n;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        buf = malloc((size_t)n);
        if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
            free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    return buf;
}

int font_init(void)
{
    const char *dir = getenv("OMNI_FONT_DIR");
    int i;
    if (g_inited)
        return g_ok;
    g_inited = 1;
    if (!dir || !*dir)
        dir = FONT_DIR;
    for (i = 0; i < FONT_NFACES; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, g_files[i]);
        g_face[i].data = slurp(path);
        if (g_face[i].data &&
            stbtt_InitFont(&g_face[i].info, g_face[i].data,
                           stbtt_GetFontOffsetForIndex(g_face[i].data, 0)))
            g_face[i].ok = 1;
    }
    g_ok = g_face[FONT_REGULAR].ok;
    if (g_ok) {
        int asc, desc, gap;
        const stbtt_fontinfo *f = &g_face[FONT_REGULAR].info;
        float per_em = stbtt_ScaleForMappingEmToPixels(f, 1.0f);
        stbtt_GetFontVMetrics(f, &asc, &desc, &gap);
        g_ascent_em = (float)asc * per_em;
        g_line_em = (float)(asc - desc + gap) * per_em;
    }
    /* slightly heavier coverage: thin strokes stay legible on the dark
     * taskbar and glass panels */
    for (i = 0; i < 256; i++)
        g_gamma[i] = (unsigned char)(255.0 * pow(i / 255.0, 1.0 / 1.2) + 0.5);
    return g_ok;
}

int font_ok(void)
{
    return font_init();
}

static const stbtt_fontinfo *face_info(int face)
{
    if (face < 0 || face >= FONT_NFACES || !g_face[face].ok)
        face = FONT_REGULAR;
    return &g_face[face].info;
}

static int clamp_px(int px)
{
    return px < 4 ? 4 : px > 127 ? 127 : px;
}

static void cache_clear(void)
{
    int i;
    for (i = 0; i < CACHE_SIZE; i++) {
        free(g_cache[i].bmp);
        g_cache[i].bmp = NULL;
        g_cache[i].key = 0;
    }
    g_cached = 0;
}

/* face 2 bits, sub-pixel 2 bits, size 7 bits, code point 21 bits */
static struct glyph *glyph_get(int face, int px, uint32_t cp, int sub)
{
    uint32_t key = ((uint32_t)face << 30) | ((uint32_t)sub << 28) |
                   ((uint32_t)px << 21) | (cp & 0x1fffff);
    uint32_t h = (key * 2654435761u) & (CACHE_SIZE - 1);
    const stbtt_fontinfo *f;
    struct glyph *g;
    float scale;
    int adv, lsb, x0, y0, x1, y1, gi;

    for (;;) {
        g = &g_cache[h];
        if (g->key == key)
            return g;
        if (g->key == 0)
            break;
        h = (h + 1) & (CACHE_SIZE - 1);
    }
    if (g_cached >= CACHE_FILL) {
        cache_clear();
        return glyph_get(face, px, cp, sub);
    }
    f = face_info(face);
    scale = stbtt_ScaleForMappingEmToPixels(f, (float)px);
    gi = stbtt_FindGlyphIndex(f, (int)cp);
    if (gi == 0 && cp > 127) {      /* not in the font: a few stand-ins */
        uint32_t alt = cp == 0x2192 ? '>' : cp == 0x2190 ? '<' : cp == 0x2713 ? 'v'
                     : cp == 0x2022 || cp == 0x25cf ? 0xb7 : '?';
        gi = stbtt_FindGlyphIndex(f, (int)alt);
    }
    stbtt_GetGlyphHMetrics(f, gi, &adv, &lsb);
    stbtt_GetGlyphBitmapBoxSubpixel(f, gi, scale, scale, sub * 0.25f, 0.0f,
                                    &x0, &y0, &x1, &y1);
    g->key = key;
    g->adv = (int)lroundf(adv * scale * 64.0f);
    g->w = (short)(x1 - x0);
    g->h = (short)(y1 - y0);
    g->xoff = (short)x0;
    g->yoff = (short)y0;
    g->bmp = NULL;
    if (g->w > 0 && g->h > 0) {
        g->bmp = malloc((size_t)g->w * (size_t)g->h);
        if (g->bmp)
            stbtt_MakeGlyphBitmapSubpixel(f, g->bmp, g->w, g->h, g->w, scale, scale,
                                          sub * 0.25f, 0.0f, gi);
    }
    g_cached++;
    return g;
}

static int kern(int face, int px, uint32_t a, uint32_t b)
{
    const stbtt_fontinfo *f = face_info(face);
    int k;
    if (!a || px < 9)
        return 0;
    k = stbtt_GetCodepointKernAdvance(f, (int)a, (int)b);
    return k ? (int)lroundf(k * stbtt_ScaleForMappingEmToPixels(f, (float)px) * 64.0f) : 0;
}

static inline uint32_t blend(uint32_t dst, uint32_t src, unsigned a)
{
    uint32_t rb, g;
    if (a >= 255) return src | 0xff000000u;
    rb = ((src & 0xff00ff) * a + (dst & 0xff00ff) * (255 - a) + 0x800080) >> 8;
    g  = ((src & 0x00ff00) * a + (dst & 0x00ff00) * (255 - a) + 0x008000) >> 8;
    return 0xff000000u | (rb & 0xff00ff) | (g & 0x00ff00);
}

static void blit(struct raster *r, const struct glyph *g, int x, int y, uint32_t rgb,
                 int italic)
{
    int i, j, i0 = 0, j0 = 0, i1 = g->w, j1 = g->h;
    if (!g->bmp || !r || !r->bits)
        return;
    if (italic) {                   /* shear: each row shifted by its height */
        for (j = 0; j < g->h; j++) {
            int yy = y + g->yoff + j, xx = x + g->xoff + (int)lroundf(-(g->yoff + j) * 0.2f);
            const unsigned char *src = g->bmp + (size_t)j * (size_t)g->w;
            if (yy < 0 || yy >= r->h)
                continue;
            for (i = 0; i < g->w; i++)
                if (src[i] && xx + i >= 0 && xx + i < r->w) {
                    uint32_t *d = r->bits + (size_t)yy * (size_t)r->stride + (size_t)(xx + i);
                    *d = blend(*d, rgb, g_gamma[src[i]]);
                }
        }
        return;
    }
    x += g->xoff;
    y += g->yoff;
    if (x < 0) i0 = -x;
    if (y < 0) j0 = -y;
    if (x + i1 > r->w) i1 = r->w - x;
    if (y + j1 > r->h) j1 = r->h - y;
    for (j = j0; j < j1; j++) {
        const unsigned char *src = g->bmp + (size_t)j * (size_t)g->w;
        uint32_t *dst = r->bits + (size_t)(y + j) * (size_t)r->stride + (size_t)x;
        for (i = i0; i < i1; i++)
            if (src[i])
                dst[i] = blend(dst[i], rgb, g_gamma[src[i]]);
    }
}

/* walk s: draw (r != NULL) and/or measure; stops at n bytes (n < 0: all) or
 * once the pen passes max_w (max_w < 0: no limit), returning the bytes
 * used in *used. Returns the width in pixels. */
static int run(struct raster *r, const char *s, int n, int x, int base, int px,
               int face, uint32_t rgb, int max_w, int *used)
{
    const char *p = s, *end = n >= 0 ? s + n : NULL;
    long pen = 0;                   /* 1/64 px */
    uint32_t prev = 0;
    int italic = (face & FONT_ITALIC) != 0;
    face &= ~FONT_ITALIC;
    px = clamp_px(px);
    if (face < 0 || face >= FONT_NFACES || !g_face[face].ok)
        face = FONT_REGULAR;
    while (*p && (!end || p < end)) {
        const char *q = p;
        uint32_t cp = omni_utf8_next(&q);
        long at;
        struct glyph *g;
        int sub;
        if (cp == '\t')
            cp = ' ';
        pen += kern(face, px, prev, cp);
        at = pen < 0 ? 0 : pen;
        sub = px > 40 ? 0 : (int)((at & 63) >> 4);
        g = glyph_get(face, px, cp, sub);
        if (max_w >= 0 && (pen + g->adv + 32) / 64 > max_w)
            break;
        if (r)
            blit(r, g, x + (int)(at >> 6), base, rgb, italic);
        pen += g->adv;
        prev = cp;
        p = q;
    }
    if (used)
        *used = (int)(p - s);
    return (int)((pen + 32) / 64);
}

int font_ascent(int px)
{
    font_init();
    return (int)lroundf(clamp_px(px) * g_ascent_em);
}

int font_line(int px)
{
    font_init();
    return (int)lroundf(clamp_px(px) * g_line_em);
}

int font_draw_base(struct raster *r, const char *s, int x, int base, int px,
                   int face, uint32_t rgb)
{
    if (!font_init() || !s)
        return 0;
    return run(r, s, -1, x, base, px, face, rgb & 0xffffff, -1, NULL);
}

int font_draw_n(struct raster *r, const char *s, int n, int x, int base, int px,
                int face, uint32_t rgb)
{
    if (!font_init() || !s)
        return 0;
    return run(r, s, n, x, base, px, face, rgb & 0xffffff, -1, NULL);
}

int font_draw(struct raster *r, const char *s, int x, int y, int px, int face,
              uint32_t rgb)
{
    return font_draw_base(r, s, x, y + font_ascent(px), px, face, rgb);
}

int font_width(const char *s, int px, int face)
{
    if (!font_init() || !s)
        return 0;
    return run(NULL, s, -1, 0, 0, px, face, 0, -1, NULL);
}

int font_width_n(const char *s, int n, int px, int face)
{
    if (!font_init() || !s)
        return 0;
    return run(NULL, s, n, 0, 0, px, face, 0, -1, NULL);
}

int font_fit(const char *s, int px, int face, int max_w)
{
    int used = 0;
    if (!font_init() || !s)
        return 0;
    run(NULL, s, -1, 0, 0, px, face, 0, max_w < 0 ? 0 : max_w, &used);
    return used;
}

int font_draw_clip(struct raster *r, const char *s, int x, int y, int px,
                   int face, uint32_t rgb, int max_w)
{
    static const char ell[] = "\xe2\x80\xa6";      /* … */
    int w, ew, used;
    if (!font_init() || !s || max_w <= 0)
        return 0;
    w = font_width(s, px, face);
    if (w <= max_w)
        return font_draw(r, s, x, y, px, face, rgb);
    ew = font_width(ell, px, face);
    used = font_fit(s, px, face, max_w - ew);
    while (used > 0 && s[used - 1] == ' ')
        used--;
    w = run(r, s, used, x, y + font_ascent(px), px, face, rgb & 0xffffff, -1, NULL);
    return w + font_draw(r, ell, x + w, y, px, face, rgb);
}
