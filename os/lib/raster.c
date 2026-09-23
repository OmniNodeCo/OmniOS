/*
 * OmniOS — os/lib/raster.c
 *
 * Tiny 2-D software rasterizer over a 32-bit BGRA framebuffer. All drawing
 * is clipped; blits support an extra clip window so window contents can be
 * limited to their parent surface (used by the windowing layer).
 */
#include <string.h>

#include "omni.h"

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

void raster_init(struct raster *r, void *bits, int w, int h, int stride)
{
    r->bits = (uint32_t *)bits;
    r->w = w;
    r->h = h;
    r->stride = stride > 0 ? stride : w;
}

void raster_px(struct raster *r, int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || x >= r->w || y >= r->h)
        return;
    r->bits[(size_t)y * (size_t)r->stride + (size_t)x] = c;
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
        uint32_t *row = r->bits + (size_t)(y + yy) * (size_t)r->stride + (size_t)x;
        int xx;
        for (xx = 0; xx < w; xx++)
            row[xx] = c;
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
    for (x = x0; x <= x1; x++)
        r->bits[(size_t)y * (size_t)r->stride + (size_t)x] = c;
}

void raster_vline(struct raster *r, int x, int y0, int y1, uint32_t c)
{
    int y;
    if (x < 0 || x >= r->w)
        return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (y0 < 0) y0 = 0;
    if (y1 >= r->h) y1 = r->h - 1;
    for (y = y0; y <= y1; y++)
        r->bits[(size_t)y * (size_t)r->stride + (size_t)x] = c;
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
    if (dy == 0)
        return;
    if (dy > 0 && dy < r->h)
        memmove(r->bits,
                r->bits + (size_t)dy * (size_t)r->stride,
                (size_t)(r->h - dy) * (size_t)r->stride * sizeof(uint32_t));
    else if (dy < 0 && -dy < r->h)
        memmove(r->bits + (size_t)(-dy) * (size_t)r->stride,
                r->bits,
                (size_t)(r->h + dy) * (size_t)r->stride * sizeof(uint32_t));
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
        uint32_t *row = r->bits + (size_t)(y + yy) * (size_t)r->stride + (size_t)x;
        for (xx = 0; xx < w; xx++)
            row[xx] = c;
    }
}

void raster_blit_clip(struct raster *dst, const struct raster *src,
                      int dx, int dy, int cx, int cy, int cw, int ch)
{
    int sy, sx;

    /* intersect the src rect (cx,cy,cw,ch) against src bounds */
    if (cx < 0) { cw += cx; cx = 0; }
    if (cy < 0) { ch += cy; cy = 0; }
    if (cx + cw > src->w) cw = src->w - cx;
    if (cy + ch > src->h) ch = src->h - cy;
    if (cw <= 0 || ch <= 0)
        return;

    sy = cy;
    for (; sy < cy + ch; sy++) {
        int dsty = sy - cy + dy;
        if (dsty < 0 || dsty >= dst->h)
            continue;
        uint32_t *drow = dst->bits + (size_t)dsty * (size_t)dst->stride;
        uint32_t *srow = src->bits + (size_t)sy * (size_t)src->stride;
        for (sx = cx; sx < cx + cw; sx++) {
            int dstx = sx - cx + dx;
            if (dstx < 0 || dstx >= dst->w)
                continue;
            drow[dstx] = srow[sx];
        }
    }
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
