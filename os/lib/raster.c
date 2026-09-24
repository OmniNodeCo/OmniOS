/*
 * OmniOS — os/lib/raster.c
 *
 * Tiny 2-D software rasterizer.  All colours are held canonically as
 * 32-bit ARGB (omni_rgb()/omni_rgba()); a raster knows how those colours
 * map onto the device pixel format described by fb_var_screeninfo (see
 * omni_pixfmt*), so a blue-tinted window on a BGR framebuffer works just
 * as well as on an RGB one.  Window backing stores are plain native 32-bit
 * rasters.  All drawing is clipped; blits support an extra clip window so
 * window contents can be limited to their parent surface.
 */
#include <stdio.h>
#include <string.h>

#include "omni.h"

/* conversion row buffer for the non-native pixel-format path            */
#define FILL_ROW_COLS 256

/* ------------------------------------------------------------------ */
/* color helpers                                                      */
/* ------------------------------------------------------------------ */

uint32_t omni_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

uint32_t omni_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return omni_rgba(r, g, b, 0xff);
}

uint32_t omni_mix(uint32_t dst, uint32_t src)
{
    uint32_t a = (src >> 24) & 0xff;
    uint32_t sr = (src >> 16) & 0xff, sg = (src >> 8) & 0xff, sb = src & 0xff;
    uint32_t dr = (dst >> 16) & 0xff, dg = (dst >> 8) & 0xff, db = dst & 0xff;

    dr = (sr * a + dr * (255 - a)) / 255;
    dg = (sg * a + dg * (255 - a)) / 255;
    db = (sb * a + db * (255 - a)) / 255;
    return 0xff000000u | (dr << 16) | (dg << 8) | db;
}

/* ------------------------------------------------------------------ */
/* device pixel-format matching                                       */
/* ------------------------------------------------------------------ */

struct omni_pixfmt omni_pixfmt_from_var(uint32_t bpp,
                                        int r_off, int r_len,
                                        int g_off, int g_len,
                                        int b_off, int b_len,
                                        int a_off, int a_len)
{
    struct omni_pixfmt pf;

    memset(&pf, 0, sizeof(pf));

    /* Which bytes form one addressable pixel?  DRM framebuffers are 16 or
     * 32 bpp in practice; 24 bpp (packed) is supported too. */
    if (bpp == 32)
        pf.bytes = 4;
    else if (bpp == 16)
        pf.bytes = 2;
    else if (bpp == 24)
        pf.bytes = 3;
    else if (bpp == 8 || bpp == 1)
        pf.bytes = 1;
    else
        pf.bytes = 4;                  /* unknown: treat as native 32-bit   */

    pf.bpp = (int)bpp;

    pf.b_shift = b_off;
    pf.b_len   = b_len;
    pf.b_loss  = 8 - b_len;
    pf.g_shift = g_off;
    pf.g_len   = g_len;
    pf.g_loss  = 8 - g_len;
    pf.r_shift = r_off;
    pf.r_len   = r_len;
    pf.r_loss  = 8 - r_len;

    /* alpha channel (transp): clamp to what the packer understands, or
     * mark it absent so the caller knows not to preserve it. */
    if (a_len <= 0 || a_off < 0) {
        pf.a_shift = -1;
        pf.a_len   = 0;
        pf.a_loss  = 8;
    } else {
        pf.a_shift = a_off;
        pf.a_len   = a_len > 8 ? 8 : a_len;
        pf.a_loss  = 8 - pf.a_len;
    }

    /* Layouts we cannot describe with plain shifts force the native
     * fallback in the raster layer. */
    if ((pf.bytes != 4 && pf.bytes != 2 && pf.bytes != 3) ||
        r_len <= 0 || g_len <= 0 || b_len <= 0 ||
        r_len > 8 || g_len > 8 || b_len > 8 ||
        r_off < 0 || g_off < 0 || b_off < 0) {
        pf.bpp = 0;
        pf.bytes = 0;
    }

    return pf;
}

int omni_pixfmt_native(const struct omni_pixfmt *pf)
{
    /* the "nothing to do" case, or the exact layout omni_rgb() emits on a
     * little-endian CPU: blue LSB, green << 8, red << 16, 8 bits each. */
    if (!pf || pf->bpp == 0 || pf->bytes == 0)
        return 1;
    if (pf->bpp != 32 || pf->bytes != 4)
        return 0;
    return (pf->b_shift == 0 && pf->b_len == 8 &&
            pf->g_shift == 8 && pf->g_len == 8 &&
            pf->r_shift == 16 && pf->r_len == 8);
}

uint32_t omni_pack_color(const struct omni_pixfmt *pf, uint32_t argb)
{
    uint32_t a, r, g, b, d;

    if (!pf || pf->bpp == 0 || pf->bytes == 0)
        return argb;

    a = (argb >> 24) & 0xff;
    r = (argb >> 16) & 0xff;
    g = (argb >>  8) & 0xff;
    b =  argb         & 0xff;

    d = 0;
    if (pf->b_len > 0)
        d |= ((uint32_t)(b >> pf->b_loss) << pf->b_shift);
    if (pf->g_len > 0)
        d |= ((uint32_t)(g >> pf->g_loss) << pf->g_shift);
    if (pf->r_len > 0)
        d |= ((uint32_t)(r >> pf->r_loss) << pf->r_shift);
    if (pf->bytes >= 4 && pf->a_len > 0 && pf->a_shift >= 0)
        d |= ((uint32_t)(a >> pf->a_loss) << pf->a_shift);

    return d;
}

void omni_pixfmt_describe(const struct omni_pixfmt *pf, char *out, size_t n)
{
    if (!pf || pf->bpp == 0 || pf->bytes == 0)
        snprintf(out, n, "native32");
    else
        snprintf(out, n, "bpp%d r%d/%d g%d/%d b%d/%d",
                 pf->bpp, pf->r_shift, pf->r_len,
                 pf->g_shift, pf->g_len, pf->b_shift, pf->b_len);
}

/* ------------------------------------------------------------------ */
/* raster core (format aware)                                         */
/* ------------------------------------------------------------------ */

/* Fast path?  true when the raster needs no per-pixel conversion: an
 * unset descriptor, or the exact native BGRA/XRGB32 layout.          */
static int raster_fast(const struct raster *r)
{
    return omni_pixfmt_native(&r->fmt);
}

/* Write one canonical row into the device at base pixel offset
 * (y * stride + x, in device pixels of pf->bytes each).               */
static void raster_row_conv(struct raster *r, size_t base_offs,
                            const uint32_t *row, int w)
{
    const struct omni_pixfmt *pf = &r->fmt;
    int bytes = pf->bytes > 0 ? pf->bytes : 4;
    uint8_t *dst = (uint8_t *)r->bits + base_offs * (size_t)bytes;
    int x, p = 0;

    for (x = 0; x < w; x++) {
        uint32_t c = row[x];
        uint32_t a = (c >> 24) & 0xff, rr = (c >> 16) & 0xff;
        uint32_t gg = (c >> 8) & 0xff, bb = c & 0xff;
        uint32_t d = 0;
        int i;

        if (pf->b_len > 0)
            d |= ((uint32_t)(bb >> pf->b_loss) << pf->b_shift);
        if (pf->g_len > 0)
            d |= ((uint32_t)(gg >> pf->g_loss) << pf->g_shift);
        if (pf->r_len > 0)
            d |= ((uint32_t)(rr >> pf->r_loss) << pf->r_shift);
        if (bytes >= 4 && pf->a_len > 0 && pf->a_shift >= 0)
            d |= ((uint32_t)(a >> pf->a_loss) << pf->a_shift);

        for (i = 0; i < bytes; i++)
            dst[p++] = (uint8_t)(d >> (8 * i));
    }
}

void raster_init(struct raster *r, void *bits, int w, int h, int stride)
{
    r->bits = (uint32_t *)bits;
    r->w = w;
    r->h = h;
    r->stride = stride > 0 ? stride : w;
    memset(&r->fmt, 0, sizeof(r->fmt));
}

void raster_px(struct raster *r, int x, int y, uint32_t c)
{
    uint32_t *dst;

    if (x < 0 || y < 0 || x >= r->w || y >= r->h)
        return;
    dst = r->bits + (size_t)y * (size_t)r->stride + (size_t)x;
    if (raster_fast(r))
        *dst = c;
    else
        raster_row_conv(r, (size_t)y * (size_t)r->stride + (size_t)x, &c, 1);
}

void raster_fill(struct raster *r, int x, int y, int w, int h, uint32_t c)
{
    int yy;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > r->w) w = r->w - x;
    if (y + h > r->h) h = r->h - y;
    if (w <= 0 || h <= 0)
        return;
    for (yy = 0; yy < h; yy++) {
        size_t off = (size_t)(y + yy) * (size_t)r->stride + (size_t)x;
        if (raster_fast(r)) {
            uint32_t *row = r->bits + off;
            int xx;
            for (xx = 0; xx < w; xx++)
                row[xx] = c;
        } else {
            /* build one canonical row, then convert it in a tight loop */
            static uint32_t copy[FILL_ROW_COLS];   /* reuse across calls */
            int chunk;
            for (chunk = 0; chunk < w; chunk += FILL_ROW_COLS) {
                int n = w - chunk < FILL_ROW_COLS ? w - chunk : FILL_ROW_COLS;
                int i;
                for (i = 0; i < n; i++)
                    copy[i] = c;
                raster_row_conv(r, off + (size_t)chunk, copy, n);
            }
        }
    }
}

void raster_hline(struct raster *r, int x0, int x1, int y, uint32_t c)
{
    int x;
    if (y < 0 || y >= r->h)
        return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x0 < 0) x0 = 0;
    if (x1 >= r->w) x1 = r->w - 1;
    if (x1 < x0)
        return;
    if (raster_fast(r)) {
        uint32_t *row = r->bits + (size_t)y * (size_t)r->stride;
        for (x = x0; x <= x1; x++)
            row[x] = c;
    } else {
        static uint32_t copy[FILL_ROW_COLS];
        int chunk;
        for (chunk = 0; chunk <= x1 - x0; chunk += FILL_ROW_COLS) {
            int n = (x1 - x0 + 1) - chunk;
            int i;
            if (n > FILL_ROW_COLS) n = FILL_ROW_COLS;
            for (i = 0; i < n; i++)
                copy[i] = c;
            raster_row_conv(r, (size_t)y * (size_t)r->stride + (size_t)(x0 + chunk),
                            copy, n);
        }
    }
}

void raster_vline(struct raster *r, int x, int y0, int y1, uint32_t c)
{
    int y;
    if (x < 0 || x >= r->w)
        return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (y0 < 0) y0 = 0;
    if (y1 >= r->h) y1 = r->h - 1;
    if (y1 < y0)
        return;
    if (raster_fast(r)) {
        for (y = y0; y <= y1; y++)
            r->bits[(size_t)y * (size_t)r->stride + (size_t)x] = c;
    } else {
        for (y = y0; y <= y1; y++)
            raster_px(r, x, y, c);
    }
}

void raster_rect(struct raster *r, int x, int y, int w, int h, uint32_t c)
{
    raster_hline(r, x, x + w - 1, y, c);
    raster_hline(r, x, x + w - 1, y + h - 1, c);
    raster_vline(r, x, y, y + h - 1, c);
    raster_vline(r, x + w - 1, y, y + h - 1, c);
}

void raster_scroll(struct raster *r, int dy, uint32_t fill)
{
    int bytes = raster_fast(r) ? 4 : r->fmt.bytes;
    size_t pitch = (size_t)r->stride * (size_t)bytes;

    if (dy == 0)
        return;
    if (dy > 0 && dy < r->h)
        memmove((uint8_t *)r->bits,
                (uint8_t *)r->bits + (size_t)dy * pitch,
                (size_t)(r->h - dy) * pitch);
    else if (dy < 0 && -dy < r->h)
        memmove((uint8_t *)r->bits + (size_t)(-dy) * pitch,
                (uint8_t *)r->bits,
                (size_t)(r->h + dy) * pitch);
    if (dy > 0)
        raster_fill(r, 0, r->h - dy, r->w, dy, fill);
    else
        raster_fill(r, 0, 0, r->w, -dy, fill);
}

void raster_gradient_v(struct raster *r, int x, int y, int w, int h,
                       uint32_t top, uint32_t bottom)
{
    int yy;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > r->w) w = r->w - x;
    if (y + h > r->h) h = r->h - y;
    if (w <= 0 || h <= 0)
        return;
    for (yy = 0; yy < h; yy++) {
        uint32_t t = h > 1 ? (uint32_t)yy * 255u / (uint32_t)(h - 1) : 255u;
        uint32_t ib = 255u - t;
        uint32_t ar = (((top >> 16) & 0xff) * ib + ((bottom >> 16) & 0xff) * t) / 255u;
        uint32_t ag = (((top >>  8) & 0xff) * ib + ((bottom >>  8) & 0xff) * t) / 255u;
        uint32_t ab = (((top >>  0) & 0xff) * ib + ((bottom >>  0) & 0xff) * t) / 255u;
        uint32_t c = 0xff000000u | (ar << 16) | (ag << 8) | ab;
        int xx;

        if (raster_fast(r)) {
            uint32_t *row = r->bits + (size_t)(y + yy) * (size_t)r->stride + (size_t)x;
            for (xx = 0; xx < w; xx++)
                row[xx] = c;
        } else {
            static uint32_t copy[FILL_ROW_COLS];
            int chunk;
            for (chunk = 0; chunk < w; chunk += FILL_ROW_COLS) {
                int n = w - chunk < FILL_ROW_COLS ? w - chunk : FILL_ROW_COLS;
                int i;
                for (i = 0; i < n; i++)
                    copy[i] = c;
                raster_row_conv(r, (size_t)(y + yy) * (size_t)r->stride + (size_t)(x + chunk),
                                copy, n);
            }
        }
    }
}

void raster_blit_clip(struct raster *dst, const struct raster *src,
                      int dx, int dy, int cx, int cy, int cw, int ch)
{
    int sy;

    /* intersect the src rect (cx,cy,cw,ch) against src bounds */
    if (cx < 0) { cw += cx; cx = 0; }
    if (cy < 0) { ch += cy; cy = 0; }
    if (cx + cw > src->w) cw = src->w - cx;
    if (cy + ch > src->h) ch = src->h - cy;
    if (cw <= 0 || ch <= 0)
        return;

    for (sy = cy; sy < cy + ch; sy++) {
        int dsty = sy - cy + dy;
        int sx0, sx1;
        if (dsty < 0 || dsty >= dst->h)
            continue;

        /* restrict the source span to pixels that land inside dst:
         *   dstx = sx - cx + dx, with 0 <= dstx < dst->w
         *   =>  cx - dx <= sx < cx + dst->w - dx                  */
        sx0 = cx - dx;
        if (sx0 < cx) sx0 = cx;
        sx1 = cx + dst->w - dx;
        if (sx1 > cx + cw) sx1 = cx + cw;
        if (sx0 >= sx1)
            continue;

        if (raster_fast(dst)) {
            memcpy(dst->bits + (size_t)dsty * (size_t)dst->stride + (size_t)(sx0 - cx + dx),
                   src->bits + (size_t)sy * (size_t)src->stride + (size_t)sx0,
                   (size_t)(sx1 - sx0) * sizeof(uint32_t));
        } else {
            /* source holds canonical ARGB; convert to device layout */
            raster_row_conv(dst, (size_t)dsty * (size_t)dst->stride + (size_t)(sx0 - cx + dx),
                            src->bits + (size_t)sy * (size_t)src->stride + (size_t)sx0,
                            sx1 - sx0);
        }
    }
}

void raster_put_row(struct raster *dst, int x, int y, const uint32_t *px, int n)
{
    if (y < 0 || y >= dst->h)
        return;
    if (x < 0) {
        px -= x;
        n += x;
        x = 0;
    }
    if (x + n > dst->w)
        n = dst->w - x;
    if (n <= 0)
        return;
    if (raster_fast(dst))
        memcpy(dst->bits + (size_t)y * (size_t)dst->stride + (size_t)x, px,
               (size_t)n * sizeof(uint32_t));
    else
        raster_row_conv(dst, (size_t)y * (size_t)dst->stride + (size_t)x, px, n);
}

void raster_blit(struct raster *dst, const struct raster *src, int dx, int dy)
{
    raster_blit_clip(dst, src, dx, dy, 0, 0, src->w, src->h);
}

void raster_blend(struct raster *dst, const struct raster *src, int dx, int dy)
{
    int sy, sx;
    for (sy = 0; sy < src->h; sy++) {
        int dsty = sy + dy;
        if (dsty < 0 || dsty >= dst->h)
            continue;
        uint32_t *drow = dst->bits + (size_t)dsty * (size_t)dst->stride;
        uint32_t *srow = src->bits + (size_t)sy * (size_t)src->stride;
        for (sx = 0; sx < src->w; sx++) {
            int dstx = sx + dx;
            if (dstx < 0 || dstx >= dst->w)
                continue;
            drow[dstx] = omni_mix(drow[dstx], srow[sx]);
        }
    }
}
