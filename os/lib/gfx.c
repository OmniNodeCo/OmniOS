/*
 * OmniOS — os/lib/gfx.c
 *
 * Image loading helper wrapping stb_image (public domain). PNG, JPEG, BMP,
 * TGA decode into a BGRA32 surface for the raster layer.
 */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_GIF
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../vendor/stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <stdlib.h>
#include <string.h>

#include "omni.h"

struct osimage {
    struct raster r;
    uint32_t *owned;   /* malloc'd pixel buffer (NULL for borrowed) */
};

/* Load an image file into a newly allocated surface (BGRA32). */
struct osimage *omni_image_load(const char *path)
{
    struct osimage *im;
    int w, h, comp;

    unsigned char *px = stbi_load(path, &w, &h, &comp, 4);
    if (!px)
        return NULL;

    im = calloc(1, sizeof(*im));
    if (!im) {
        stbi_image_free(px);
        return NULL;
    }
    im->owned = (uint32_t *)px;
    im->r.bits = im->owned;
    im->r.w = w;
    im->r.h = h;
    im->r.stride = w;
    return im;
}

struct osimage *omni_image_from_mem(const unsigned char *buf, int len)
{
    struct osimage *im;
    int w, h, comp;

    unsigned char *px = stbi_load_from_memory(buf, len, &w, &h, &comp, 4);
    if (!px)
        return NULL;

    im = calloc(1, sizeof(*im));
    if (!im) {
        stbi_image_free(px);
        return NULL;
    }
    im->owned = (uint32_t *)px;
    im->r.bits = im->owned;
    im->r.w = w;
    im->r.h = h;
    im->r.stride = w;
    return im;
}

void omni_image_free(struct osimage *im)
{
    if (!im)
        return;
    if (im->owned)
        stbi_image_free(im->owned);
    free(im);
}

/* Draw image (optionally alpha-blended) centered in the rect x,y,w,h. */
void omni_image_draw(struct raster *dst, const struct osimage *im,
                     int x, int y, int w, int h, int blend)
{
    int iw = im->r.w, ih = im->r.h;
    int sw, sh;
    int dx, dy;

    /* fit within the box, aspect-preserving, centered */
    if (w * ih > h * iw) {   /* box is wider relative */
        sw = h * iw / ih;
        sh = h;
    } else {
        sw = w;
        sh = w * ih / iw;
    }
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    dx = x + (w - sw) / 2;
    dy = y + (h - sh) / 2;

    /* simple nearest-neighbour scale by sampling source */
    {
        int oy;
        for (oy = 0; oy < sh; oy++) {
            int src_y = oy * ih / sh;
            int ox;
            if (src_y >= ih) src_y = ih - 1;
            for (ox = 0; ox < sw; ox++) {
                int src_x = ox * iw / sw;
                uint32_t c;
                uint32_t *drow;
                int ddx = dx + ox, ddy = dy + oy;
                if (ddx < 0 || ddy < 0 || ddx >= dst->w || ddy >= dst->h)
                    continue;
                if (src_x >= iw) src_x = iw - 1;
                c = im->r.bits[(size_t)src_y * (size_t)im->r.stride + (size_t)src_x];
                drow = dst->bits + (size_t)ddy * (size_t)dst->stride;
                drow[ddx] = blend ? omni_mix(drow[ddx], c) : c;
            }
        }
    }
}
