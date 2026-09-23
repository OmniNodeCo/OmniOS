/*
 * OmniOS — os/lib/osfb.c
 *
 * Framebuffer device access: open /dev/fb0, query the fixed/var info via
 * ioctl, mmap it, and expose a BGRA32-oriented view with a stride that may
 * differ from the visible width.
 *
 * The kernel speaks to us through the legacy FBIOGET_* ioctls (also emitted
 * by CONFIG_DRM_SIMPLEDRM / sysfb), so this works for VESA, EFI GOP and
 * simpledrm framebuffers alike.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/fb.h>

#include "omni.h"

int osfb_open(struct osfb *fb, const char *dev)
{
    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;
    size_t mapsz;

    memset(fb, 0, sizeof(*fb));
    fb->fd = -1;

    fb->fd = open(dev, O_RDWR);
    if (fb->fd < 0)
        return -1;

    if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &finfo) < 0)
        goto fail;
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo) < 0)
        goto fail;

    fb->w      = vinfo.xres;
    fb->h      = vinfo.yres;
    fb->depth  = vinfo.bits_per_pixel;
    fb->stride = (int)(finfo.line_length / (unsigned)(fb->depth / 8 ? fb->depth / 8 : 1));
    if (fb->stride <= 0)
        fb->stride = fb->w;

    /* Describe how red/green/blue/alpha sit inside a pixel so the raster
     * layer maps canonical ARGB colours onto the real device layout instead
     * of blindly assuming BGRA32. */
    fb->fmt = omni_pixfmt_from_var((uint32_t)vinfo.bits_per_pixel,
                                   (int)vinfo.red.offset,   (int)vinfo.red.length,
                                   (int)vinfo.green.offset, (int)vinfo.green.length,
                                   (int)vinfo.blue.offset,  (int)vinfo.blue.length,
                                   (int)vinfo.transp.offset,(int)vinfo.transp.length);
    {
        char line[128];
        char fmtbuf[64];
        omni_pixfmt_describe(&fb->fmt, fmtbuf, sizeof(fmtbuf));
        snprintf(line, sizeof(line),
                 "desktop: fb0 %dx%d stride=%d bpp=%d fmt=%s line=%u smem=%u\n",
                 fb->w, fb->h, fb->stride, fb->depth, fmtbuf,
                 (unsigned)finfo.line_length, (unsigned)finfo.smem_len);
        omni_console_puts(line);
    }

    mapsz = (size_t)finfo.smem_len;
    if (mapsz == 0)
        mapsz = (size_t)fb->stride * (size_t)fb->h * 4u;

    fb->mem = mmap(NULL, mapsz, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->mem == MAP_FAILED)
        goto fail;
    fb->size = (uint32_t)mapsz;

    if (finfo.id[0])
        fb->name = strndup(finfo.id, sizeof(finfo.id));
    else
        fb->name = strdup(dev);

    return 0;

fail:
    osfb_close(fb);
    return -1;
}

struct raster fb_raster(const struct osfb *fb)
{
    struct raster r;
    r.bits = fb->mem;
    r.w = fb->w;
    r.h = fb->h;
    r.stride = fb->stride > 0 ? fb->stride : fb->w;
    r.fmt = fb->fmt;
    return r;
}

void osfb_close(struct osfb *fb)
{
    if (fb->mem && fb->mem != MAP_FAILED)
        munmap(fb->mem, fb->size);
    if (fb->fd >= 0)
        close(fb->fd);
    free(fb->name);
    memset(fb, 0, sizeof(*fb));
    fb->fd = -1;
}

int osfb_wait(const char *dev, int retries, int delay_ms)
{
    struct timespec ts;
    int i;

    ts.tv_sec  = (time_t)(delay_ms / 1000);
    ts.tv_nsec = (long)(delay_ms % 1000) * 1000000L;

    for (i = 0; i < retries; i++) {
        int fd = open(dev, O_RDWR);
        if (fd >= 0) {
            close(fd);
            return 0;
        }
        nanosleep(&ts, NULL);
    }
    return -1;
}
