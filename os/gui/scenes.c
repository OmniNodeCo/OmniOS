/*
 * OmniOS — os/gui/scenes.c
 *
 * The painted wallpapers: Dunes, Peaks, Night, Lagoon, Prism and Bokeh
 * (th_wallpaper_style() hands these styles over). Like the Bloom family in
 * theme.c they are computed, not stored as pictures: a few layers of
 * curves, noise and gradients, cheap enough to draw at start-up (well
 * under 100 ms at 1280x800) and sharp at any screen size.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "theme.h"

static inline uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static inline float rnd(uint32_t seed)             /* 0..1 */
{
    return (float)(hash32(seed) & 0xffffff) / 16777216.0f;
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static inline float smooth(float e0, float e1, float x)
{
    float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static inline uint32_t mix(uint32_t a, uint32_t b, float t)
{
    return th_blend(a, b, (unsigned)(clampf(t, 0.0f, 1.0f) * 255.0f + 0.5f)) & 0xffffff;
}

/* add light (screen-like): c + k * light, per channel */
static inline uint32_t add(uint32_t c, uint32_t light, float k)
{
    int r = (int)((c >> 16) & 255) + (int)(((light >> 16) & 255) * k);
    int g = (int)((c >> 8) & 255) + (int)(((light >> 8) & 255) * k);
    int b = (int)(c & 255) + (int)((light & 255) * k);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (uint32_t)(r << 16 | g << 8 | b);
}

/* colour at t along n stops */
static uint32_t ramp(const uint32_t *c, const float *at, int n, float t)
{
    int i;
    if (t <= at[0])
        return c[0];
    for (i = 1; i < n; i++)
        if (t <= at[i])
            return mix(c[i - 1], c[i], (t - at[i - 1]) / (at[i] - at[i - 1]));
    return c[n - 1];
}

/* smooth 1-D value noise and its octave sum */
static float noise1(float x, uint32_t seed)
{
    float fi = floorf(x), f = x - fi;
    int i = (int)fi;
    float a = rnd((uint32_t)i * 7919u + seed), b = rnd((uint32_t)(i + 1) * 7919u + seed);
    f = f * f * (3.0f - 2.0f * f);
    return a + (b - a) * f;
}

static float fbm1(float x, uint32_t seed, int oct, int ridged)
{
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    int o;
    for (o = 0; o < oct; o++) {
        float n = noise1(x, seed + (uint32_t)o * 131u);
        if (ridged)
            n = 1.0f - fabsf(n * 2.0f - 1.0f);
        sum += n * amp;
        norm += amp;
        amp *= 0.5f;
        x *= 2.03f;
    }
    return sum / norm;
}

/* 2-D value noise (for the Milky Way and clouds) */
static float noise2(float x, float y, uint32_t seed)
{
    float fx = floorf(x), fy = floorf(y), tx = x - fx, ty = y - fy;
    uint32_t ix = (uint32_t)(int)fx, iy = (uint32_t)(int)fy;
    float a = rnd(ix * 7919u + iy * 104729u + seed);
    float b = rnd((ix + 1) * 7919u + iy * 104729u + seed);
    float c = rnd(ix * 7919u + (iy + 1) * 104729u + seed);
    float d = rnd((ix + 1) * 7919u + (iy + 1) * 104729u + seed);
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

/* ---- Dunes: a desert at sunset ------------------------------------------ */
static void dunes(uint32_t *px, int w, int h)
{
    static const uint32_t sky[] = { 0x1d0b3f, 0x52205f, 0xb2406a, 0xf2875f, 0xfcc583 };
    static const float at[] = { 0.0f, 0.22f, 0.42f, 0.55f, 0.62f };
    static const uint32_t crest[5] = { 0xd88a7c, 0xc0606f, 0x9a3f63, 0x6c2752, 0x3a1236 };
    static const uint32_t base[5]  = { 0xb7686f, 0x96455f, 0x702a52, 0x4a1842, 0x230a25 };
    float *hy = malloc(sizeof(float) * (size_t)w * 5);
    float sx = 0.70f * (float)w, sy = 0.56f * (float)h, sr = 0.055f * (float)h;
    int x, y, k;
    if (!hy)
        return;
    for (k = 0; k < 5; k++)
        for (x = 0; x < w; x++) {
            float u = (float)x / (float)w;
            float y0 = 0.60f + 0.085f * (float)k, a = 0.035f + 0.012f * (float)k;
            float v = 0.6f * sinf(6.2831853f * (u * (0.8f + 0.25f * k) + 0.13f * k))
                    + 0.4f * sinf(6.2831853f * (u * (1.9f + 0.4f * k) + 0.71f * k))
                    + 0.5f * (fbm1(u * (3.0f + k), 11u + (uint32_t)k, 3, 0) - 0.5f);
            hy[k * w + x] = (y0 - a * v) * (float)h;
        }
    for (y = 0; y < h; y++) {
        float t = (float)y / (float)h;
        uint32_t s = ramp(sky, at, 5, t);
        for (x = 0; x < w; x++) {
            float dx = (float)x - sx, dy = (float)y - sy, d = sqrtf(dx * dx + dy * dy);
            uint32_t c = s;
            /* the sun: a soft disc and a wide glow */
            c = add(c, 0xffb36b, 0.55f * expf(-d / (sr * 3.0f)));
            if (d < sr + 1.0f)
                c = mix(c, 0xfff1c9, clampf(sr + 1.0f - d, 0.0f, 1.0f));
            for (k = 0; k < 5; k++) {
                float top = hy[k * w + x], cov = clampf((float)y - top + 0.5f, 0.0f, 1.0f);
                if (cov > 0.0f) {
                    int xa = x > 3 ? x - 3 : 0, xb = x + 3 < w ? x + 3 : w - 1;
                    float slope = (hy[k * w + xb] - hy[k * w + xa]) / (float)(xb - xa + 1) * 2.0f;
                    uint32_t lc = mix(crest[k], base[k], ((float)y - top) / (0.22f * (float)h));
                    /* sunlit faces lighter, the far side darker */
                    lc = slope < 0 ? add(lc, 0xffc9a0, clampf(-slope * 0.6f, 0.0f, 0.18f))
                                   : mix(lc, 0x12061a, clampf(slope * 0.5f, 0.0f, 0.22f));
                    c = mix(c, lc, cov);
                }
            }
            px[(size_t)y * w + x] = 0xff000000u | c;
        }
    }
    free(hy);
}

/* ---- Peaks: mountains at blue hour -------------------------------------- */
static void peaks(uint32_t *px, int w, int h)
{
    static const uint32_t sky[] = { 0x07142c, 0x163a63, 0x3f77a3, 0x9cc3d8, 0xf0dcc0 };
    static const float at[] = { 0.0f, 0.25f, 0.45f, 0.60f, 0.70f };
    static const uint32_t col[5] = { 0x8fa9c4, 0x5f7f9f, 0x3d5878, 0x233a55, 0x0e1a2c };
    float *hy = malloc(sizeof(float) * (size_t)w * 5);
    int x, y, k;
    if (!hy)
        return;
    for (k = 0; k < 5; k++)
        for (x = 0; x < w; x++) {
            float u = (float)x / (float)w;
            float n = fbm1(u * (2.2f + 1.1f * k) + 3.7f * k, 101u + (uint32_t)k * 17u, 5, 1);
            hy[k * w + x] = (0.70f + 0.06f * k - (0.30f - 0.035f * k) * n) * (float)h;
        }
    for (y = 0; y < h; y++) {
        float t = (float)y / (float)h;
        uint32_t s = ramp(sky, at, 5, t);
        for (x = 0; x < w; x++) {
            uint32_t c = s;
            uint32_t r = hash32((uint32_t)(y * 9973 + x * 31337));
            if (t < 0.45f && (r & 1023) < 2)              /* a few stars */
                c = add(c, 0xffffff, (0.45f - t) * 1.6f * rnd(r));
            for (k = 0; k < 5; k++) {
                float top = hy[k * w + x], cov = clampf((float)y - top + 0.5f, 0.0f, 1.0f);
                if (cov > 0.0f) {
                    float below = ((float)y - top) / (0.25f * (float)h);
                    uint32_t lc = mix(add(col[k], 0x1a2a40, 0.6f), col[k], below * 3.0f);
                    /* mist gathers at the foot of each ridge */
                    lc = mix(lc, 0xc9d8e4, (0.35f - 0.06f * k) * smooth(0.15f, 1.0f, below));
                    c = mix(c, lc, cov);
                }
            }
            px[(size_t)y * w + x] = 0xff000000u | c;
        }
    }
    free(hy);
}

/* ---- Night: stars, the Milky Way, a dark horizon ------------------------ */
static void night(uint32_t *px, int w, int h)
{
    static const uint32_t sky[] = { 0x01030d, 0x050b24, 0x0f1640, 0x2a2257, 0x4a2f63 };
    static const float at[] = { 0.0f, 0.35f, 0.65f, 0.82f, 0.90f };
    float *hill = malloc(sizeof(float) * (size_t)w);
    float aspect = (float)w / (float)h;
    int x, y, i, nstars = w * h / 900;
    if (!hill)
        return;
    for (x = 0; x < w; x++) {
        float u = (float)x / (float)w;
        hill[x] = (0.86f - 0.07f * fbm1(u * 3.5f, 77u, 4, 0)) * (float)h;
    }
    for (y = 0; y < h; y++) {
        float t = (float)y / (float)h;
        uint32_t s = ramp(sky, at, 5, t);
        for (x = 0; x < w; x++) {
            float u = (float)x / (float)w * aspect, v = t;
            /* the band: distance from a tilted line, broken up by noise */
            float d = fabsf((v - 0.75f) + 0.45f * (u - 0.2f * aspect)) ;
            float n = noise2(u * 6.0f, v * 6.0f, 5u) * 0.6f + noise2(u * 17.0f, v * 17.0f, 9u) * 0.4f;
            float band = expf(-d * d * 18.0f) * (0.35f + 0.65f * n);
            uint32_t c = add(s, 0x8f7fd0, 0.28f * band);
            c = add(c, 0xffd9b0, 0.10f * band * band);
            if (y >= (int)hill[x] - 1) {
                float cov = clampf((float)y - hill[x] + 0.5f, 0.0f, 1.0f);
                c = mix(c, mix(0x0b0a1c, 0x05040d, (t - 0.80f) * 4.0f), cov);
            }
            px[(size_t)y * w + x] = 0xff000000u | c;
        }
    }
    for (i = 0; i < nstars; i++) {                /* stars, a few bright */
        int sx = (int)(rnd((uint32_t)i * 3u + 1u) * (float)w);
        int sy = (int)(rnd((uint32_t)i * 3u + 2u) * (float)h);
        float b = rnd((uint32_t)i * 3u + 3u);
        int dx, dy, rad;
        if (sy >= (int)hill[sx] - 2)
            continue;
        b = b * b * b;
        rad = w < 400 ? 0 : (b > 0.6f && w >= 640) ? 2 : 1;   /* small: dots */
        for (dy = -rad; dy <= rad; dy++)
            for (dx = -rad; dx <= rad; dx++) {
                int xx = sx + dx, yy = sy + dy;
                float k = (dx == 0 && dy == 0) ? 1.0f : (dx == 0 || dy == 0) ? 0.35f : 0.12f;
                if (xx >= 0 && yy >= 0 && xx < w && yy < h) {
                    uint32_t *p = &px[(size_t)yy * w + xx];
                    *p = 0xff000000u | add(*p & 0xffffff, (i % 7) ? 0xeef2ff : 0xffe2b8,
                                           (0.25f + 0.9f * b) * k);
                }
            }
    }
    free(hill);
}

/* ---- Lagoon: layered waves ---------------------------------------------- */
static void lagoon(uint32_t *px, int w, int h)
{
    static const uint32_t col[7] = { 0xa5f3fc, 0x67e8f9, 0x22c3dd, 0x0ea0c6,
                                     0x0b74a8, 0x0b4f86, 0x082f5c };
    float *hy = malloc(sizeof(float) * (size_t)w * 7);
    int x, y, k;
    if (!hy)
        return;
    for (k = 0; k < 7; k++)
        for (x = 0; x < w; x++) {
            float u = (float)x / (float)w;
            hy[k * w + x] = (0.08f + 0.13f * k
                + 0.035f * sinf(6.2831853f * (u * (0.9f + 0.17f * k) + 0.37f * k))
                + 0.020f * sinf(6.2831853f * (u * (2.3f + 0.29f * k) + 0.11f * k))) * (float)h;
        }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t c = mix(0xe0fbff, 0xbdeffa, (float)y / (float)h);
            for (k = 0; k < 7; k++) {
                float top = hy[k * w + x], dy = (float)y - top;
                float cov = clampf(dy + 0.5f, 0.0f, 1.0f);
                if (dy > -14.0f && dy < 0.0f)               /* shadow above */
                    c = mix(c, 0x062a4d, 0.10f * (1.0f + dy / 14.0f));
                if (cov > 0.0f) {
                    uint32_t lc = mix(add(col[k], 0xffffff, 0.10f), col[k], dy / (0.10f * (float)h));
                    if (dy < 2.5f)                          /* a lit edge */
                        lc = add(lc, 0xffffff, 0.22f * (1.0f - dy / 2.5f));
                    c = mix(c, lc, cov);
                }
            }
            px[(size_t)y * w + x] = 0xff000000u | c;
        }
    }
    free(hy);
}

/* ---- Prism: low-poly facets ---------------------------------------------- */
static void tri(uint32_t *px, int w, int h, const float *a, const float *b,
                const float *c, uint32_t col)
{
    int x0 = (int)floorf(fminf(a[0], fminf(b[0], c[0]))), x1 = (int)ceilf(fmaxf(a[0], fmaxf(b[0], c[0])));
    int y0 = (int)floorf(fminf(a[1], fminf(b[1], c[1]))), y1 = (int)ceilf(fmaxf(a[1], fmaxf(b[1], c[1])));
    float area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    int x, y;
    if (fabsf(area) < 1e-3f)
        return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            float pxf = (float)x + 0.5f, pyf = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (pyf - a[1]) - (b[1] - a[1]) * (pxf - a[0]);
            float w1 = (c[0] - b[0]) * (pyf - b[1]) - (c[1] - b[1]) * (pxf - b[0]);
            float w2 = (a[0] - c[0]) * (pyf - c[1]) - (a[1] - c[1]) * (pxf - c[0]);
            if (area < 0) { w0 = -w0; w1 = -w1; w2 = -w2; }
            if (w0 >= 0 && w1 >= 0 && w2 >= 0)
                px[(size_t)y * w + x] = 0xff000000u | col;
        }
}

static void prism(uint32_t *px, int w, int h)
{
    static const uint32_t ramp_c[] = { 0x1e1b4b, 0x4338ca, 0x7c3aed, 0xdb2777, 0xf59e0b };
    static const float ramp_at[] = { 0.0f, 0.30f, 0.55f, 0.80f, 1.0f };
    int cols = 16, rows = (int)(16.0f * (float)h / (float)w + 0.5f), i, j;
    float cw = (float)w / (float)cols, ch;
    float *pt;
    if (rows < 4)
        rows = 4;
    ch = (float)h / (float)rows;
    pt = malloc(sizeof(float) * 2 * (size_t)(cols + 1) * (size_t)(rows + 1));
    if (!pt)
        return;
    for (j = 0; j <= rows; j++)
        for (i = 0; i <= cols; i++) {
            float *p = &pt[(j * (cols + 1) + i) * 2];
            float jx = (i == 0 || i == cols) ? 0.0f : (rnd((uint32_t)(j * 97 + i) * 2u) - 0.5f) * 0.8f;
            float jy = (j == 0 || j == rows) ? 0.0f : (rnd((uint32_t)(j * 97 + i) * 2u + 1u) - 0.5f) * 0.8f;
            p[0] = ((float)i + jx) * cw;
            p[1] = ((float)j + jy) * ch;
        }
    for (j = 0; j < rows; j++)
        for (i = 0; i < cols; i++) {
            const float *p00 = &pt[(j * (cols + 1) + i) * 2], *p10 = p00 + 2;
            const float *p01 = &pt[((j + 1) * (cols + 1) + i) * 2], *p11 = p01 + 2;
            int flip = hash32((uint32_t)(j * 131 + i)) & 1, k;
            for (k = 0; k < 2; k++) {
                /* the cell's two triangles, split along one diagonal or the other */
                const float *a = p00, *b = flip ? p11 : p10, *c = flip ? p01 : p01;
                float cx, cy, t, v;
                if (!flip && k) { a = p10; b = p11; c = p01; }
                if (flip && !k) { a = p00; b = p10; c = p11; }
                if (flip && k)  { a = p00; b = p11; c = p01; }
                cx = (a[0] + b[0] + c[0]) / 3.0f;
                cy = (a[1] + b[1] + c[1]) / 3.0f;
                t = clampf(0.65f * cx / (float)w + 0.35f * cy / (float)h, 0.0f, 1.0f);
                v = rnd((uint32_t)(j * 131 + i) * 4u + (uint32_t)k) - 0.5f;
                tri(px, w, h, a, b, c, v > 0 ? add(ramp(ramp_c, ramp_at, 5, t), 0xffffff, v * 0.16f)
                                             : mix(ramp(ramp_c, ramp_at, 5, t), 0x000000, -v * 0.30f));
            }
        }
    free(pt);
}

/* ---- Bokeh: out-of-focus lights ----------------------------------------- */
static void bokeh(uint32_t *px, int w, int h)
{
    static const uint32_t pal[5] = { 0xf472b6, 0x60a5fa, 0xfbbf24, 0xa78bfa, 0x34d399 };
    int x, y, i, n = 46;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            float t = 0.6f * (float)x / (float)w + 0.4f * (float)y / (float)h;
            px[(size_t)y * w + x] = 0xff000000u | mix(mix(0x1c0a33, 0x3a0d4f, t * 1.6f),
                                                      0x0c2a52, smooth(0.45f, 1.0f, t));
        }
    for (i = 0; i < n; i++) {
        float cx = rnd((uint32_t)i * 5u + 1u) * (float)w, cy = rnd((uint32_t)i * 5u + 2u) * (float)h;
        float r = (0.02f + 0.075f * rnd((uint32_t)i * 5u + 3u)) * (float)h;
        float soft = 1.0f + 12.0f * rnd((uint32_t)i * 5u + 4u);
        float alpha = 0.10f + 0.22f * rnd((uint32_t)i * 5u + 5u);
        uint32_t col = pal[hash32((uint32_t)i) % 5];
        int x0 = (int)(cx - r - soft), x1 = (int)(cx + r + soft) + 1;
        int y0 = (int)(cy - r - soft), y1 = (int)(cy + r + soft) + 1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        for (y = y0; y < y1; y++)
            for (x = x0; x < x1; x++) {
                float dx = (float)x - cx, dy = (float)y - cy, d = sqrtf(dx * dx + dy * dy);
                float a = (1.0f - smooth(r - soft, r + 1.0f, d)) * alpha;
                if (d > r - 2.0f && d < r + 1.0f)       /* bright rim */
                    a *= 1.25f;
                if (a > 0.003f) {
                    uint32_t *p = &px[(size_t)y * w + x];
                    *p = 0xff000000u | add(*p & 0xffffff, col, a);
                }
            }
    }
}

int th_scene(uint32_t *px, int w, int h, int style)
{
    if (w <= 1 || h <= 1)
        return 0;
    switch (style) {
    case TH_WALL_DUNES:  dunes(px, w, h);  return 1;
    case TH_WALL_PEAKS:  peaks(px, w, h);  return 1;
    case TH_WALL_NIGHT:  night(px, w, h);  return 1;
    case TH_WALL_LAGOON: lagoon(px, w, h); return 1;
    case TH_WALL_PRISM:  prism(px, w, h);  return 1;
    case TH_WALL_BOKEH:  bokeh(px, w, h);  return 1;
    default:             return 0;
    }
}
