/*
 * OmniOS — os/gui/theme.c
 *
 * Drawing toolkit for the modern look (see theme.h). Written from scratch.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "theme.h"
#include "icons.h"

uint32_t th_accent = 0x3b82f6;

static inline uint32_t *px_at(struct raster *r, int x, int y)
{
    return r->bits + (size_t)y * (size_t)r->stride + (size_t)x;
}

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

uint32_t th_shade(uint32_t c, int amt)
{
    int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
    if (amt >= 0) {
        r += (255 - r) * amt / 255;
        g += (255 - g) * amt / 255;
        b += (255 - b) * amt / 255;
    } else {
        r = r * (255 + amt) / 255;
        g = g * (255 + amt) / 255;
        b = b * (255 + amt) / 255;
    }
    return ((uint32_t)clampi(r, 0, 255) << 16) | ((uint32_t)clampi(g, 0, 255) << 8) |
           (uint32_t)clampi(b, 0, 255);
}

/* ------------------------------------------------------------------ */
/* shapes                                                             */
/* ------------------------------------------------------------------ */

void th_fill_a(struct raster *r, int x, int y, int w, int h, uint32_t rgb,
               unsigned a)
{
    int yy, xx;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > r->w) w = r->w - x;
    if (y + h > r->h) h = r->h - y;
    if (w <= 0 || h <= 0 || a == 0)
        return;
    for (yy = y; yy < y + h; yy++) {
        uint32_t *p = px_at(r, x, yy);
        if (a >= 255)
            for (xx = 0; xx < w; xx++) p[xx] = 0xff000000u | rgb;
        else
            for (xx = 0; xx < w; xx++) p[xx] = th_blend(p[xx], rgb, a);
    }
}

unsigned th_corner_cov(int px, int py, int rad)
{
    float dx = (float)rad - ((float)px + 0.5f);
    float dy = (float)rad - ((float)py + 0.5f);
    float cov = (float)rad - sqrtf(dx * dx + dy * dy) + 0.5f;
    if (cov <= 0.0f) return 0;
    if (cov >= 1.0f) return 255;
    return (unsigned)(cov * 255.0f + 0.5f);
}

void th_round_rect(struct raster *r, int x, int y, int w, int h, int rad,
                   uint32_t rgb, unsigned a)
{
    int j, i;
    if (w <= 0 || h <= 0 || a == 0)
        return;
    if (rad * 2 > w) rad = w / 2;
    if (rad * 2 > h) rad = h / 2;
    for (j = 0; j < h; j++) {
        int yy = y + j;
        int cy = (j < rad) ? j : (j >= h - rad ? h - 1 - j : -1);
        if (yy < 0 || yy >= r->h)
            continue;
        if (cy < 0) {                           /* straight rows */
            th_fill_a(r, x, yy, w, 1, rgb, a);
            continue;
        }
        for (i = 0; i < w; i++) {
            int xx = x + i;
            int cx = (i < rad) ? i : (i >= w - rad ? w - 1 - i : -1);
            unsigned cov = (cx >= 0) ? th_corner_cov(cx, cy, rad) : 255;
            uint32_t *p;
            if (xx < 0 || xx >= r->w || cov == 0)
                continue;
            p = px_at(r, xx, yy);
            *p = th_blend(*p, rgb, a * cov / 255);
        }
    }
}

/* one shadow pixel: distance d outside the shape -> darkening */
static inline void shadow_px(uint32_t *p, float d, float blur, unsigned strength)
{
    float t, f;
    if (d >= blur)
        return;
    t = d <= 0.0f ? 0.0f : d / blur;
    f = 1.0f - t * t * (3.0f - 2.0f * t);       /* smooth falloff */
    *p = th_blend(*p, 0x000000, (unsigned)((float)strength * f * f));
}

void th_shadow(struct raster *r, int x, int y, int w, int h, int rad,
               int blur, int dy, unsigned strength)
{
    int sy = y + dy;                            /* the shadow's shape */
    int x0 = x - blur, x1 = x + w + blur, y0 = sy - blur, y1 = sy + h + blur;
    int ix0 = x + rad, ix1 = x + w - 1 - rad, iy0 = sy + rad, iy1 = sy + h - 1 - rad;
    int yy, xx;

    if (blur <= 0 || strength == 0)
        return;
    x0 = clampi(x0, 0, r->w); x1 = clampi(x1, 0, r->w);
    y0 = clampi(y0, 0, r->h); y1 = clampi(y1, 0, r->h);
    for (yy = y0; yy < y1; yy++) {
        int qy = yy < iy0 ? iy0 - yy : (yy > iy1 ? yy - iy1 : 0);
        int under = (yy >= y && yy < y + h);    /* row crosses the window */
        /* in the window's top/bottom `rad` rows its rounded corners let the
         * shadow show through, so only the middle part is skipped there */
        int corner_row = under && (yy < y + rad || yy >= y + h - rad);
        int skip0 = corner_row ? x + rad : x, skip1 = corner_row ? x + w - rad : x + w;
        uint32_t *row = px_at(r, 0, yy);
        for (xx = x0; xx < x1; xx++) {
            int qx;
            float d;
            if (under && xx >= skip0 && xx < skip1) {
                xx = skip1 - 1;                 /* skip what the window covers */
                continue;
            }
            qx = xx < ix0 ? ix0 - xx : (xx > ix1 ? xx - ix1 : 0);
            if (qx == 0)      d = (float)qy;
            else if (qy == 0) d = (float)qx;
            else              d = sqrtf((float)(qx * qx + qy * qy));
            shadow_px(&row[xx], d - (float)rad, (float)blur, strength);
        }
    }
}

/* ------------------------------------------------------------------ */
/* text                                                               */
/* ------------------------------------------------------------------ */

/* --- frosted glass ---------------------------------------------------- */

static uint32_t *frost_tmp;         /* one line (horizontal pass)        */
static int frost_cap;
static uint32_t *frost_src;         /* region copy (vertical pass)       */
static size_t frost_src_cap;
static int *frost_sum;              /* per-column running sums, 3 x w    */
static int frost_sum_cap;

/* running-sum box blur of one contiguous row of n pixels: the row is
 * copied into tmp padded with its edge pixels, so no clamping per pixel */
static void blur_row(uint32_t *p, int n, int rad, uint32_t *tmp)
{
    int i, sr = 0, sg = 0, sb = 0;
    unsigned inv = 65536u / (unsigned)(2 * rad + 1);
    const uint32_t *q;

    for (i = 0; i < rad; i++) {
        tmp[i] = p[0];
        tmp[rad + n + i] = p[n - 1];
    }
    tmp[2 * rad + n] = p[n - 1];
    memcpy(tmp + rad, p, (size_t)n * sizeof(uint32_t));
    for (i = 0; i <= 2 * rad; i++) {
        sr += (int)((tmp[i] >> 16) & 255);
        sg += (int)((tmp[i] >> 8) & 255);
        sb += (int)(tmp[i] & 255);
    }
    q = tmp;                            /* window is q[i .. i + 2 rad]  */
    for (i = 0; i < n; i++) {
        uint32_t a = q[i + 2 * rad + 1], d = q[i];
        p[i] = 0xff000000u | ((((unsigned)sr * inv) >> 16) << 16) |
               ((((unsigned)sg * inv) >> 16) << 8) | (((unsigned)sb * inv) >> 16);
        sr += (int)((a >> 16) & 255) - (int)((d >> 16) & 255);
        sg += (int)((a >> 8) & 255) - (int)((d >> 8) & 255);
        sb += (int)(a & 255) - (int)(d & 255);
    }
}

/* vertical box blur of a region, sliding per-column sums down row by
 * row so every memory access is sequential (cache friendly) */
static int blur_cols(struct raster *r, int x, int y, int w, int h, int rad)
{
    size_t need = (size_t)w * (size_t)h;
    unsigned inv = 65536u / (unsigned)(2 * rad + 1);
    int *sr, *sg, *sb, i, j;

    if (frost_src_cap < need) {
        uint32_t *t = realloc(frost_src, need * sizeof(uint32_t));
        if (!t)
            return -1;
        frost_src = t;
        frost_src_cap = need;
    }
    if (frost_sum_cap < 3 * w) {
        int *t = realloc(frost_sum, (size_t)(3 * w) * sizeof(int));
        if (!t)
            return -1;
        frost_sum = t;
        frost_sum_cap = 3 * w;
    }
    for (j = 0; j < h; j++)
        memcpy(frost_src + (size_t)j * (size_t)w, px_at(r, x, y + j),
               (size_t)w * sizeof(uint32_t));
    sr = frost_sum;
    sg = sr + w;
    sb = sg + w;
    memset(frost_sum, 0, (size_t)(3 * w) * sizeof(int));
    for (j = -rad; j <= rad; j++) {
        const uint32_t *row = frost_src + (size_t)clampi(j, 0, h - 1) * (size_t)w;
        for (i = 0; i < w; i++) {
            sr[i] += (int)((row[i] >> 16) & 255);
            sg[i] += (int)((row[i] >> 8) & 255);
            sb[i] += (int)(row[i] & 255);
        }
    }
    for (j = 0; j < h; j++) {
        uint32_t *out = px_at(r, x, y + j);
        const uint32_t *add = frost_src + (size_t)clampi(j + rad + 1, 0, h - 1) * (size_t)w;
        const uint32_t *sub = frost_src + (size_t)clampi(j - rad, 0, h - 1) * (size_t)w;
        for (i = 0; i < w; i++) {
            out[i] = 0xff000000u | ((((unsigned)sr[i] * inv) >> 16) << 16) |
                     ((((unsigned)sg[i] * inv) >> 16) << 8) |
                     (((unsigned)sb[i] * inv) >> 16);
            sr[i] += (int)((add[i] >> 16) & 255) - (int)((sub[i] >> 16) & 255);
            sg[i] += (int)((add[i] >> 8) & 255) - (int)((sub[i] >> 8) & 255);
            sb[i] += (int)(add[i] & 255) - (int)(sub[i] & 255);
        }
    }
    return 0;
}

void th_frost(struct raster *r, int x, int y, int w, int h, int rad, int blur)
{
    uint32_t save[4][16 * 16];
    int ox = x, oy = y, ow = w, oh = h, k, i, j, pass;

    if (rad > 16) rad = 16;
    if (rad < 0) rad = 0;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > r->w) w = r->w - x;
    if (y + h > r->h) h = r->h - y;
    if (w <= 1 || h <= 1 || blur <= 0)
        return;
    if (frost_cap < w + 2 * blur + 2) {
        int cap = w + 2 * blur + 2;
        uint32_t *t = realloc(frost_tmp, (size_t)cap * sizeof(uint32_t));
        if (!t)
            return;
        frost_tmp = t;
        frost_cap = cap;
    }

    /* keep the corners (of the unclipped rect) as they are */
    for (k = 0; k < 4 && rad; k++) {
        int cx0 = (k & 1) ? ox + ow - rad : ox, cy0 = (k & 2) ? oy + oh - rad : oy;
        for (j = 0; j < rad; j++)
            for (i = 0; i < rad; i++) {
                int xx = cx0 + i, yy = cy0 + j;
                save[k][j * rad + i] = (xx >= 0 && yy >= 0 && xx < r->w && yy < r->h)
                                           ? *px_at(r, xx, yy) : 0;
            }
    }

    for (pass = 0; pass < 2; pass++) {
        for (j = 0; j < h; j++)
            blur_row(px_at(r, x, y + j), w, blur, frost_tmp);
        if (blur_cols(r, x, y, w, h, blur) != 0)
            break;
    }

    /* outside the rounded shape: back to sharp, blended by coverage */
    for (k = 0; k < 4 && rad; k++) {
        int cx0 = (k & 1) ? ox + ow - rad : ox, cy0 = (k & 2) ? oy + oh - rad : oy;
        for (j = 0; j < rad; j++)
            for (i = 0; i < rad; i++) {
                int xx = cx0 + i, yy = cy0 + j;
                int cx = (k & 1) ? rad - 1 - i : i, cy = (k & 2) ? rad - 1 - j : j;
                unsigned cov = th_corner_cov(cx, cy, rad);
                if (cov < 255 && xx >= 0 && yy >= 0 && xx < r->w && yy < r->h) {
                    uint32_t *p = px_at(r, xx, yy);
                    *p = th_blend(save[k][j * rad + i], *p, cov);
                }
            }
    }
}

static void glyph(struct raster *r, uint32_t ch, int x, int y, uint32_t c)
{
    const unsigned char *g = omni_glyph8(ch);
    int row, b;
    for (row = 0; row < 8; row++) {
        int yy = y + row;
        if (yy < 0 || yy >= r->h || !g[row])
            continue;
        for (b = 0; b < 8; b++) {
            int xx = x + b;
            if ((g[row] >> b) & 1 && xx >= 0 && xx < r->w)
                *px_at(r, xx, yy) = c;
        }
    }
}

int th_text(struct raster *r, const char *s, int x, int y, uint32_t rgb)
{
    const char *p = s;
    int cx = x;
    while (*p) {
        glyph(r, omni_utf8_next(&p), cx, y, 0xff000000u | rgb);
        cx += 8;
    }
    return cx - x;
}

int th_text_bold(struct raster *r, const char *s, int x, int y, uint32_t rgb)
{
    th_text(r, s, x + 1, y, rgb);
    return th_text(r, s, x, y, rgb) + 1;
}

int th_text_clip(struct raster *r, const char *s, int x, int y, uint32_t rgb,
                 int max_w)
{
    int n = (int)strlen(s), fit = max_w / 8, i;
    if (fit <= 0)
        return 0;
    if (n <= fit)
        return th_text(r, s, x, y, rgb);
    for (i = 0; i < fit - 2 && s[i]; i++)       /* cut + ".." */
        glyph(r, (unsigned char)s[i], x + i * 8, y, 0xff000000u | rgb);
    glyph(r, '.', x + i * 8 - 2, y, 0xff000000u | rgb);
    glyph(r, '.', x + i * 8 + 3, y, 0xff000000u | rgb);
    return fit * 8;
}

/* 8x8 glyph -> 16x16 with Scale2x (EPX): diagonals come out smooth */
static void glyph2x(struct raster *r, uint32_t ch, int x, int y, uint32_t c)
{
    const unsigned char *g = omni_glyph8(ch);
    int sx, sy;
#define SP(xx, yy) (((xx) < 0 || (yy) < 0 || (xx) > 7 || (yy) > 7) ? 0 \
                    : ((g[(yy)] >> (xx)) & 1))
    for (sy = 0; sy < 8; sy++) {
        for (sx = 0; sx < 8; sx++) {
            int P = SP(sx, sy), A = SP(sx, sy - 1), B = SP(sx + 1, sy);
            int C = SP(sx - 1, sy), D = SP(sx, sy + 1);
            int e[4], k;
            e[0] = (C == A && C != D && A != B) ? A : P;
            e[1] = (A == B && A != C && B != D) ? B : P;
            e[2] = (D == C && D != B && C != A) ? C : P;
            e[3] = (B == D && B != A && D != C) ? D : P;
            for (k = 0; k < 4; k++) {
                int xx = x + 2 * sx + (k & 1), yy = y + 2 * sy + (k >> 1);
                if (e[k] && xx >= 0 && yy >= 0 && xx < r->w && yy < r->h)
                    *px_at(r, xx, yy) = c;
            }
        }
    }
#undef SP
}

/* one Scale2x (EPX) pass over a 0/1 bitmap: w x h -> 2w x 2h */
static unsigned char *scale2x_bits(const unsigned char *a, int w, int h)
{
    unsigned char *b = malloc((size_t)w * (size_t)h * 4);
    int x, y;
#define AT(xx, yy) (((xx) < 0 || (yy) < 0 || (xx) >= w || (yy) >= h) ? 0 : a[(yy) * w + (xx)])
    if (!b)
        return NULL;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int P = AT(x, y), A = AT(x, y - 1), B = AT(x + 1, y), C = AT(x - 1, y), D = AT(x, y + 1);
            unsigned char *o = &b[(2 * y) * (2 * w) + 2 * x];
            o[0]         = (unsigned char)((C == A && C != D && A != B) ? A : P);
            o[1]         = (unsigned char)((A == B && A != C && B != D) ? B : P);
            o[2 * w]     = (unsigned char)((D == C && D != B && C != A) ? C : P);
            o[2 * w + 1] = (unsigned char)((B == D && B != A && D != C) ? D : P);
        }
#undef AT
    return b;
}

int th_text_big_width(const char *s, int scale)
{
    const char *p = s;
    int n = 0;
    while (*p) {
        omni_utf8_next(&p);
        n++;
    }
    return n * 7 * scale;
}

int th_text_big(struct raster *r, const char *s, int x, int y, int scale, uint32_t rgb)
{
    const char *p = s;
    uint32_t chs[48];
    unsigned char *a;
    int n = 0, w, h = 8, i, j, k, passes = 0;

    while (*p && n < 48)
        chs[n++] = omni_utf8_next(&p);
    if (n == 0 || scale < 2)
        return 0;
    w = n * 7 + 1;
    a = calloc((size_t)w * (size_t)h, 1);
    if (!a)
        return 0;
    for (i = 0; i < n; i++) {
        const unsigned char *g = omni_glyph8(chs[i]);
        for (j = 0; j < 8; j++)
            for (k = 0; k < 8; k++)
                if ((g[j] >> k) & 1)
                    a[j * w + i * 7 + k] = 1;
    }
    for (k = 2 * scale; k > 1; k >>= 1) {          /* upscale to 2x the target */
        unsigned char *b = scale2x_bits(a, w, h);
        free(a);
        if (!b)
            return 0;
        a = b;
        w *= 2;
        h *= 2;
        passes++;
    }
    for (j = 0; j + 1 < h; j += 2)                  /* 2x2 box filter = AA */
        for (i = 0; i + 1 < w; i += 2) {
            int cov = a[j * w + i] + a[j * w + i + 1] + a[(j + 1) * w + i] + a[(j + 1) * w + i + 1];
            if (cov)
                th_px(r, x + i / 2, y + j / 2, rgb, (unsigned)(cov * 255 / 4));
        }
    free(a);
    (void)passes;
    return n * 7 * scale;
}

int th_text2x(struct raster *r, const char *s, int x, int y, uint32_t rgb)
{
    const char *p = s;
    int cx = x;
    while (*p) {
        glyph2x(r, omni_utf8_next(&p), cx, y, 0xff000000u | rgb);
        cx += 14;
    }
    return cx - x;
}

int th_text2x_width(const char *s)
{
    return (int)strlen(s) * 14;
}

/* ------------------------------------------------------------------ */
/* artwork                                                            */
/* ------------------------------------------------------------------ */

void th_letter_icon(struct raster *r, int x, int y, int size, uint32_t rgb,
                    char letter)
{
    int rad = size / 4, j, i;
    uint32_t top = th_shade(rgb, 45), bot = th_shade(rgb, -30);
    char s[2] = { letter, 0 };

    for (j = 0; j < size; j++) {
        int yy = y + j;
        int cy = (j < rad) ? j : (j >= size - rad ? size - 1 - j : -1);
        uint32_t c = th_blend(top, bot, (unsigned)(j * 255 / (size > 1 ? size - 1 : 1))) & 0xffffff;
        if (yy < 0 || yy >= r->h)
            continue;
        for (i = 0; i < size; i++) {
            int xx = x + i;
            int cx = (i < rad) ? i : (i >= size - rad ? size - 1 - i : -1);
            unsigned cov = (cy >= 0 && cx >= 0) ? th_corner_cov(cx, cy, rad) : 255;
            uint32_t *p;
            if (xx < 0 || xx >= r->w || !cov)
                continue;
            p = px_at(r, xx, yy);
            *p = th_blend(*p, c, cov);
        }
    }
    if (size >= 30)
        th_text2x(r, s, x + (size - 14) / 2 - 1, y + (size - 16) / 2, 0xffffff);
    else
        th_text_bold(r, s, x + (size - 8) / 2, y + (size - 8) / 2, 0xffffff);
}

/* the app's pictogram if it has one (icons.c), else the lettered tile */
void th_icon(struct raster *r, int x, int y, int size, uint32_t rgb, char letter)
{
    if (!icon_draw(r, x, y, size, letter))
        th_letter_icon(r, x, y, size, rgb, letter);
}

void th_logo(struct raster *r, int cx, int cy, int radius)
{
    float R = (float)radius, Ri = R * 0.56f;
    int yy, xx;
    for (yy = cy - radius - 1; yy <= cy + radius + 1; yy++) {
        if (yy < 0 || yy >= r->h)
            continue;
        for (xx = cx - radius - 1; xx <= cx + radius + 1; xx++) {
            float dx = (float)xx + 0.5f - (float)cx - 0.5f;
            float dy = (float)yy + 0.5f - (float)cy - 0.5f;
            float d = sqrtf(dx * dx + dy * dy);
            float co = R - d + 0.5f, ci = d - Ri + 0.5f, cov, t;
            uint32_t c;
            if (xx < 0 || xx >= r->w)
                continue;
            cov = co < ci ? co : ci;
            if (cov <= 0.0f)
                continue;
            if (cov > 1.0f) cov = 1.0f;
            t = ((dx + dy) / (2.0f * R)) * 0.5f + 0.5f;        /* diagonal */
            t = t < 0 ? 0 : (t > 1 ? 1 : t);
            c = th_blend(0x4f8cff, 0xa855f7, (unsigned)(t * 255.0f)) & 0xffffff;
            *px_at(r, xx, yy) = th_blend(*px_at(r, xx, yy), c, (unsigned)(cov * 255.0f));
        }
    }
}

/* --- wallpaper ------------------------------------------------------- */

struct glow   { float cx, cy, rad, col[3], k; };
struct ribbon { float base, a1, f1, p1, a2, f2, p2, th, thv, fth, inten;
                float c0[3], c1[3]; };

static const struct glow g_glows[] = {
    { 0.10f, 1.00f, 0.60f, { 0.10f, 0.32f, 1.00f }, 0.62f },  /* blue       */
    { 0.86f, 0.18f, 0.52f, { 0.50f, 0.18f, 0.95f }, 0.50f },  /* violet     */
    { 0.58f, 1.08f, 0.42f, { 0.00f, 0.68f, 0.86f }, 0.42f },  /* teal       */
    { 1.04f, 0.94f, 0.32f, { 0.95f, 0.26f, 0.62f }, 0.28f },  /* pink       */
};

static const struct ribbon g_ribbons[] = {
    /* base   a1     f1     p1     a2     f2    p2     th      thv     fth   inten */
    { 0.60f, 0.10f, 0.72f, 0.08f, 0.035f, 2.2f, 0.45f, 0.020f, 0.011f, 1.3f, 0.85f,
      { 0.36f, 0.80f, 1.00f }, { 0.72f, 0.46f, 1.00f } },
    { 0.67f, 0.12f, 0.58f, 0.33f, 0.030f, 1.7f, 0.12f, 0.009f, 0.005f, 2.0f, 0.60f,
      { 0.45f, 0.62f, 1.00f }, { 0.96f, 0.52f, 0.90f } },
    { 0.74f, 0.09f, 0.88f, 0.60f, 0.020f, 2.9f, 0.80f, 0.005f, 0.003f, 1.1f, 0.45f,
      { 0.30f, 0.90f, 0.96f }, { 0.56f, 0.56f, 1.00f } },
    /* wide, faint halo around the first ribbon */
    { 0.60f, 0.10f, 0.72f, 0.08f, 0.035f, 2.2f, 0.45f, 0.090f, 0.030f, 1.3f, 0.16f,
      { 0.30f, 0.55f, 1.00f }, { 0.60f, 0.35f, 1.00f } },
};

#define NGLOW   ((int)(sizeof(g_glows) / sizeof(g_glows[0])))
#define NRIB    ((int)(sizeof(g_ribbons) / sizeof(g_ribbons[0])))
#define GTAB    2048
#define GTAB_MAX 16.0f

static inline uint32_t hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

/* colour mixes for the wallpaper styles: out = M * (r, g, b) */
static const float g_wall_mix[TH_WALL_COUNT][9] = {
    { 1.00f, 0.00f, 0.00f,  0.00f, 1.00f, 0.00f,  0.00f, 0.00f, 1.00f },  /* Bloom    */
    { 0.20f, 0.10f, 0.05f,  0.20f, 0.60f, 0.65f,  0.25f, 0.20f, 0.55f },  /* Aurora   */
    { 0.55f, 0.10f, 0.75f,  0.25f, 0.55f, 0.25f,  0.45f, 0.10f, 0.25f },  /* Sunset   */
    { 0.15f, 0.10f, 0.05f,  0.25f, 0.45f, 0.45f,  0.55f, 0.30f, 0.95f },  /* Ocean    */
    { 0.60f, 0.10f, 0.70f,  0.25f, 0.50f, 0.10f,  0.55f, 0.25f, 0.45f },  /* Rose     */
    { 0.30f, 0.45f, 0.40f,  0.30f, 0.45f, 0.40f,  0.32f, 0.47f, 0.46f },  /* Graphite */
};

void th_wallpaper(uint32_t *px, int w, int h)
{
    th_wallpaper_style(px, w, h, TH_WALL_BLOOM);
}

void th_wallpaper_style(uint32_t *px, int w, int h, int style)
{
    const float *mix = g_wall_mix[(style >= 0 && style < TH_WALL_COUNT) ? style : 0];
    float aspect = (float)w / (float)(h > 0 ? h : 1);
    float *gx = malloc(sizeof(float) * (size_t)(NGLOW * w));
    float *gy = malloc(sizeof(float) * (size_t)(NGLOW * h));
    float *rc = malloc(sizeof(float) * (size_t)(NRIB * w * 5));  /* centre, 1/th, r,g,b */
    static float gtab[GTAB];
    int x, y, i;

    if (!gx || !gy || !rc) {                    /* no memory: plain gradient */
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                px[(size_t)y * w + x] = th_blend(0x0a1030, 0x1a1040,
                                                 (unsigned)(y * 255 / (h ? h : 1)));
        free(gx); free(gy); free(rc);
        return;
    }
    for (i = 0; i < GTAB; i++)
        gtab[i] = expf(-(float)i * GTAB_MAX / GTAB);

    for (i = 0; i < NGLOW; i++) {
        const struct glow *g = &g_glows[i];
        float r2 = g->rad * g->rad;
        for (x = 0; x < w; x++) {
            float dx = ((float)x / (float)(w - 1) - g->cx) * aspect;
            gx[i * w + x] = expf(-dx * dx / r2) * g->k;
        }
        for (y = 0; y < h; y++) {
            float dy = (float)y / (float)(h - 1) - g->cy;
            gy[i * h + y] = expf(-dy * dy / r2);
        }
    }
    for (i = 0; i < NRIB; i++) {
        const struct ribbon *b = &g_ribbons[i];
        for (x = 0; x < w; x++) {
            float u = (float)x / (float)(w - 1), *o = &rc[(i * w + x) * 5];
            float t = u;
            o[0] = b->base + b->a1 * sinf(6.2831853f * (b->f1 * u + b->p1))
                           + b->a2 * sinf(6.2831853f * (b->f2 * u + b->p2));
            o[1] = 1.0f / (b->th + b->thv * sinf(6.2831853f * (b->fth * u + 0.2f)));
            o[2] = b->c0[0] + (b->c1[0] - b->c0[0]) * t;
            o[3] = b->c0[1] + (b->c1[1] - b->c0[1]) * t;
            o[4] = b->c0[2] + (b->c1[2] - b->c0[2]) * t;
        }
    }

    for (y = 0; y < h; y++) {
        float v = (float)y / (float)(h - 1);
        for (x = 0; x < w; x++) {
            float u = (float)x / (float)(w - 1);
            float t = 0.55f * u + 0.45f * v;
            float cr = 0.018f + 0.050f * t, cg = 0.030f + 0.004f * t, cb = 0.105f + 0.085f * t;
            float du = u - 0.5f, dv = v - 0.42f, vig;
            int R, G, B;
            float n;

            for (i = 0; i < NGLOW; i++) {       /* additive glows */
                float k = gx[i * w + x] * gy[i * h + y];
                cr += g_glows[i].col[0] * k;
                cg += g_glows[i].col[1] * k;
                cb += g_glows[i].col[2] * k;
            }
            for (i = 0; i < NRIB; i++) {        /* light ribbons: screen blend */
                const float *o = &rc[(i * w + x) * 5];
                float d = (v - o[0]) * o[1], d2 = d * d, a;
                if (d2 >= GTAB_MAX)
                    continue;
                a = gtab[(int)(d2 * (GTAB / GTAB_MAX))] * g_ribbons[i].inten;
                cr = 1.0f - (1.0f - cr) * (1.0f - o[2] * a);
                cg = 1.0f - (1.0f - cg) * (1.0f - o[3] * a);
                cb = 1.0f - (1.0f - cb) * (1.0f - o[4] * a);
            }
            if (style > 0) {                    /* re-colour for the style */
                float r0 = cr, g0 = cg, b0 = cb;
                cr = mix[0] * r0 + mix[1] * g0 + mix[2] * b0;
                cg = mix[3] * r0 + mix[4] * g0 + mix[5] * b0;
                cb = mix[6] * r0 + mix[7] * g0 + mix[8] * b0;
            }
            vig = 1.0f - 0.55f * (du * du + dv * dv);
            n = ((float)(hash2(x, y) & 0xff) / 255.0f - 0.5f) * 1.5f;  /* dither */
            R = (int)(cr * vig * 255.0f + n);
            G = (int)(cg * vig * 255.0f + n);
            B = (int)(cb * vig * 255.0f + n);
            px[(size_t)y * w + x] = 0xff000000u | ((uint32_t)clampi(R, 0, 255) << 16) |
                                    ((uint32_t)clampi(G, 0, 255) << 8) | (uint32_t)clampi(B, 0, 255);
        }
    }
    free(gx);
    free(gy);
    free(rc);
}
