/*
 * OmniOS — os/gui/icons.c
 *
 * Pictogram app icons drawn from vector shapes (see icons.h).
 *
 * Every icon is a short list of shapes — rounded rectangles, circles, ring
 * arcs, polygons and round-capped strokes — on a 48x48 design grid, filled
 * with flat colours or linear gradients. An icon is rendered once per size
 * at 4x resolution into a premultiplied-alpha canvas, box-filtered down
 * (16 samples per pixel, so edges are smooth), and cached; drawing a cached
 * icon is a plain alpha blend.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "icons.h"
#include "theme.h"

#define SS        4             /* supersampling factor per axis        */
#define ICON_MAX  128           /* largest size rendered (px)           */
#define CACHE_N   96

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- canvas + paints ------------------------------------------------- */

struct ic {
    uint32_t *px;               /* premultiplied ARGB, n x n            */
    int n;
    float k;                    /* hi-res pixels per design unit        */
};

/* linear gradient from c0 at (x0,y0) to c1 at (x1,y1); ARGB colours */
struct paint { uint32_t c0, c1; float x0, y0, x1, y1; };

static struct paint P(uint32_t argb)
{
    struct paint p = { argb, argb, 0, 0, 0, 48 };
    return p;
}
static struct paint V(uint32_t top, uint32_t bot, float y0, float y1)   /* vertical */
{
    struct paint p = { top, bot, 0, y0, 0, y1 };
    return p;
}
static struct paint D(uint32_t a, uint32_t b)                           /* diagonal */
{
    struct paint p = { a, b, 4, 4, 44, 44 };
    return p;
}

static uint32_t paint_at(const struct paint *p, float x, float y)
{
    float dx = p->x1 - p->x0, dy = p->y1 - p->y0, t, l2 = dx * dx + dy * dy;
    uint32_t out = 0;
    int s;
    if (p->c0 == p->c1 || l2 <= 0.0f)
        return p->c0;
    t = ((x - p->x0) * dx + (y - p->y0) * dy) / l2;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    for (s = 0; s < 32; s += 8) {
        float a = (float)((p->c0 >> s) & 255), b = (float)((p->c1 >> s) & 255);
        out |= (uint32_t)(a + (b - a) * t + 0.5f) << s;
    }
    return out;
}

/* source-over of a straight-alpha colour onto the premultiplied canvas */
static void put(struct ic *c, int i, int j, uint32_t argb)
{
    uint32_t *d = &c->px[j * c->n + i], v = *d;
    unsigned sa = argb >> 24, inv = 255 - sa;
    unsigned r = ((argb >> 16) & 255) * sa / 255 + ((v >> 16) & 255) * inv / 255;
    unsigned g = ((argb >> 8) & 255) * sa / 255 + ((v >> 8) & 255) * inv / 255;
    unsigned b = (argb & 255) * sa / 255 + (v & 255) * inv / 255;
    unsigned a = sa + (v >> 24) * inv / 255;
    *d = (a << 24) | (r << 16) | (g << 8) | b;
}

/* ---- shapes ---------------------------------------------------------- */

enum { SH_RRECT, SH_CIRCLE, SH_RING, SH_POLY, SH_LINE };

struct shape {
    int type;
    float x, y, w, h, r;        /* rrect: box + radius; circle: x,y,r   */
    float r2, a0, a1;           /* ring: inner radius, arc a0..a1 (deg) */
    float x1, y1;               /* line end                             */
    const float *pts;           /* poly: x,y pairs                      */
    int npts;
};

static int in_arc(float ang, float a0, float a1)
{
    if (a0 == a1)
        return 1;                               /* full ring */
    if (a0 < a1)
        return ang >= a0 && ang <= a1;
    return ang >= a0 || ang <= a1;              /* wraps through 0 */
}

static int inside(const struct shape *s, float px, float py)
{
    float dx, dy, qx, qy;
    int i, j, in = 0;

    switch (s->type) {
    case SH_RRECT:
        if (px < s->x || py < s->y || px > s->x + s->w || py > s->y + s->h)
            return 0;
        qx = px < s->x + s->r ? s->x + s->r : (px > s->x + s->w - s->r ? s->x + s->w - s->r : px);
        qy = py < s->y + s->r ? s->y + s->r : (py > s->y + s->h - s->r ? s->y + s->h - s->r : py);
        dx = px - qx; dy = py - qy;
        return dx * dx + dy * dy <= s->r * s->r;
    case SH_CIRCLE:
        dx = px - s->x; dy = py - s->y;
        return dx * dx + dy * dy <= s->r * s->r;
    case SH_RING: {
        float d2, ang;
        dx = px - s->x; dy = py - s->y;
        d2 = dx * dx + dy * dy;
        if (d2 > s->r * s->r || d2 < s->r2 * s->r2)
            return 0;
        ang = (float)(atan2(-dy, dx) * 180.0 / M_PI);     /* y up, degrees */
        if (ang < 0.0f)
            ang += 360.0f;
        return in_arc(ang, s->a0, s->a1);
    }
    case SH_POLY:
        for (i = 0, j = s->npts - 1; i < s->npts; j = i++) {
            float xi = s->pts[2 * i], yi = s->pts[2 * i + 1];
            float xj = s->pts[2 * j], yj = s->pts[2 * j + 1];
            if (((yi > py) != (yj > py)) &&
                (px < (xj - xi) * (py - yi) / (yj - yi) + xi))
                in = !in;
        }
        return in;
    case SH_LINE: {
        float vx = s->x1 - s->x, vy = s->y1 - s->y, l2 = vx * vx + vy * vy, t;
        t = l2 > 0.0f ? ((px - s->x) * vx + (py - s->y) * vy) / l2 : 0.0f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        dx = px - (s->x + vx * t); dy = py - (s->y + vy * t);
        return dx * dx + dy * dy <= s->r * s->r;
    }
    }
    return 0;
}

static void bbox(const struct shape *s, float *x0, float *y0, float *x1, float *y1)
{
    int i;
    switch (s->type) {
    case SH_RRECT:
        *x0 = s->x; *y0 = s->y; *x1 = s->x + s->w; *y1 = s->y + s->h;
        return;
    case SH_CIRCLE: case SH_RING:
        *x0 = s->x - s->r; *y0 = s->y - s->r; *x1 = s->x + s->r; *y1 = s->y + s->r;
        return;
    case SH_LINE:
        *x0 = (s->x < s->x1 ? s->x : s->x1) - s->r;
        *x1 = (s->x > s->x1 ? s->x : s->x1) + s->r;
        *y0 = (s->y < s->y1 ? s->y : s->y1) - s->r;
        *y1 = (s->y > s->y1 ? s->y : s->y1) + s->r;
        return;
    case SH_POLY:
        *x0 = *x1 = s->pts[0]; *y0 = *y1 = s->pts[1];
        for (i = 1; i < s->npts; i++) {
            float x = s->pts[2 * i], y = s->pts[2 * i + 1];
            if (x < *x0) *x0 = x;
            if (x > *x1) *x1 = x;
            if (y < *y0) *y0 = y;
            if (y > *y1) *y1 = y;
        }
        return;
    }
    *x0 = *y0 = 0; *x1 = *y1 = 48;
}

/* fill a shape (clear != 0 punches a transparent hole instead) */
static void fill(struct ic *c, const struct shape *s, struct paint p, int clear)
{
    float fx0, fy0, fx1, fy1;
    int i0, j0, i1, j1, i, j;
    bbox(s, &fx0, &fy0, &fx1, &fy1);
    i0 = (int)floorf(fx0 * c->k) - 1; j0 = (int)floorf(fy0 * c->k) - 1;
    i1 = (int)ceilf(fx1 * c->k) + 1;  j1 = (int)ceilf(fy1 * c->k) + 1;
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > c->n) i1 = c->n;
    if (j1 > c->n) j1 = c->n;
    for (j = j0; j < j1; j++) {
        float py = ((float)j + 0.5f) / c->k;
        for (i = i0; i < i1; i++) {
            float px = ((float)i + 0.5f) / c->k;
            if (!inside(s, px, py))
                continue;
            if (clear)
                c->px[j * c->n + i] = 0;
            else
                put(c, i, j, paint_at(&p, px, py));
        }
    }
}

static void rrect(struct ic *c, float x, float y, float w, float h, float r, struct paint p)
{
    struct shape s;
    memset(&s, 0, sizeof(s));
    s.type = SH_RRECT; s.x = x; s.y = y; s.w = w; s.h = h;
    s.r = r > w / 2 ? w / 2 : (r > h / 2 ? h / 2 : r);
    fill(c, &s, p, 0);
}
static void circle(struct ic *c, float x, float y, float r, struct paint p)
{
    struct shape s;
    memset(&s, 0, sizeof(s));
    s.type = SH_CIRCLE; s.x = x; s.y = y; s.r = r;
    fill(c, &s, p, 0);
}
static void hole(struct ic *c, float x, float y, float r)
{
    struct shape s;
    memset(&s, 0, sizeof(s));
    s.type = SH_CIRCLE; s.x = x; s.y = y; s.r = r;
    fill(c, &s, P(0), 1);
}
/* ring between radii rin..rout; arc from a0 to a1 degrees, counter-
 * clockwise with 0 = right, 90 = up (a0 == a1: full ring) */
static void ring(struct ic *c, float x, float y, float rin, float rout,
                 float a0, float a1, struct paint p)
{
    struct shape s;
    memset(&s, 0, sizeof(s));
    s.type = SH_RING; s.x = x; s.y = y; s.r = rout; s.r2 = rin; s.a0 = a0; s.a1 = a1;
    fill(c, &s, p, 0);
}
static void line(struct ic *c, float x0, float y0, float x1, float y1, float w, struct paint p)
{
    struct shape s;
    memset(&s, 0, sizeof(s));
    s.type = SH_LINE; s.x = x0; s.y = y0; s.x1 = x1; s.y1 = y1; s.r = w / 2;
    fill(c, &s, p, 0);
}
static void poly(struct ic *c, const float *pts, int n, struct paint p)
{
    struct shape s;
    memset(&s, 0, sizeof(s));
    s.type = SH_POLY; s.pts = pts; s.npts = n;
    fill(c, &s, p, 0);
}
/* erase everything outside the circle (clips earlier shapes to it) */
static void clip_circle(struct ic *c, float x, float y, float r)
{
    int i, j;
    for (j = 0; j < c->n; j++)
        for (i = 0; i < c->n; i++) {
            float dx = ((float)i + 0.5f) / c->k - x, dy = ((float)j + 0.5f) / c->k - y;
            if (dx * dx + dy * dy > r * r)
                c->px[j * c->n + i] = 0;
        }
}
/* point at angle deg (0 = right, 90 = up) and radius r around (cx, cy) */
static float ax(float cx, float deg, float r) { return cx + r * (float)cos(deg * M_PI / 180.0); }
static float ay(float cy, float deg, float r) { return cy - r * (float)sin(deg * M_PI / 180.0); }

#define RIM 0xff94a3b8          /* light outline so dark icons read on the dark taskbar */

/* ---- the icons ------------------------------------------------------- */

static void ic_terminal(struct ic *c)                   /* '>' */
{
    rrect(c, 3, 6, 42, 36, 8, P(RIM));
    rrect(c, 4, 7, 40, 34, 7, V(0xff3b4658, 0xff1a212d, 7, 41));
    line(c, 13, 17.5, 21, 24.5, 3.8, P(0xffe2e8f0));
    line(c, 21, 24.5, 13, 31.5, 3.8, P(0xffe2e8f0));
    line(c, 25, 32, 35, 32, 3.8, P(0xff4ade80));
}

static void ic_folder(struct ic *c)                     /* 'F' */
{
    rrect(c, 4, 9, 18, 10, 3, P(0xffd97706));
    rrect(c, 4, 13, 40, 27, 4, P(0xffd97706));
    rrect(c, 4, 18, 40, 22, 4, V(0xfffde68a, 0xfff59e0b, 18, 40));
    rrect(c, 7, 19.6, 34, 1.4, 0.7, P(0x90ffffff));
}

static void ic_editor(struct ic *c)                     /* 'E' */
{
    rrect(c, 8, 4, 32, 40, 5, P(RIM));
    rrect(c, 9, 5, 30, 38, 4, V(0xffffffff, 0xffe8edf3, 5, 43));
    rrect(c, 9, 5, 30, 10, 4, P(0xff0ea5e9));
    rrect(c, 9, 10, 30, 5, 0, P(0xff0ea5e9));
    line(c, 14, 21, 34, 21, 2.4, P(0xff94a3b8));
    line(c, 14, 27, 34, 27, 2.4, P(0xff94a3b8));
    line(c, 14, 33, 27, 33, 2.4, P(0xff94a3b8));
}

static void ic_calc(struct ic *c)                       /* '+' */
{
    int r, k;
    rrect(c, 8, 3, 32, 42, 7, P(RIM));
    rrect(c, 9, 4, 30, 40, 6, V(0xff4b5563, 0xff27303b, 4, 44));
    rrect(c, 13, 8, 22, 9, 2, P(0xffd1fae5));
    for (r = 0; r < 3; r++)
        for (k = 0; k < 3; k++)
            rrect(c, 13.0f + (float)k * 8.0f, 21.0f + (float)r * 7.5f, 6, 5.5f, 1.5f,
                  (r == 2 && k == 2) ? P(0xff10b981) : P(0xffe5e7eb));
}

static void ic_gauge(struct ic *c)                      /* 'i' System Info */
{
    circle(c, 24, 26, 20, P(RIM));
    circle(c, 24, 26, 19, V(0xff475569, 0xff1e293b, 7, 45));
    ring(c, 24, 26, 12, 16, 130, 210, P(0xff22c55e));
    ring(c, 24, 26, 12, 16, 50, 130, P(0xfff59e0b));
    ring(c, 24, 26, 12, 16, 330, 50, P(0xffef4444));
    line(c, 24, 26, ax(24, 60, 11), ay(26, 60, 11), 3, P(0xfff8fafc));
    circle(c, 24, 26, 3.4f, P(0xfff8fafc));
}

static void clock_face(struct ic *c, uint32_t top, uint32_t bot)
{
    int k;
    circle(c, 24, 24, 21, V(top, bot, 3, 45));
    circle(c, 24, 24, 17, P(0xffffffff));
    for (k = 0; k < 12; k++) {
        float a = (float)k * 30.0f, r0 = (k % 3 == 0) ? 12.3f : 13.6f;
        line(c, ax(24, a, r0), ay(24, a, r0), ax(24, a, 15.2f), ay(24, a, 15.2f),
             (k % 3 == 0) ? 2.2f : 1.4f, P(0xff64748b));
    }
    line(c, 24, 24, ax(24, 145, 8.5f), ay(24, 145, 8.5f), 3.2f, P(0xff1f2937));
    line(c, 24, 24, ax(24, 30, 12.5f), ay(24, 30, 12.5f), 2.6f, P(0xff1f2937));
    circle(c, 24, 24, 2.4f, P(bot));
}
static void ic_clock(struct ic *c)    { clock_face(c, 0xff818cf8, 0xff4f46e5); }   /* 'C' */
static void ic_timelang(struct ic *c) { clock_face(c, 0xff2dd4bf, 0xff0d9488); }   /* 't' */

static void ic_snake(struct ic *c)                      /* 'S' */
{
    struct paint g = V(0xff86efac, 0xff16a34a, 6, 42);
    ring(c, 24, 16, 4, 10, 30, 270, g);
    ring(c, 24, 31, 4, 10, 210, 90, g);
    circle(c, ax(24, 210, 7), ay(31, 210, 7), 3, g);
    line(c, 33.5f, 10.2f, 37.5f, 8, 1.3f, P(0xffef4444));
    circle(c, 30.4f, 12.3f, 4.4f, P(0xff15803d));
    circle(c, 31.4f, 11.1f, 1.2f, P(0xffffffff));
}

static void ic_store(struct ic *c)                      /* 'A' */
{
    ring(c, 24, 15, 5.5f, 8.5f, 0, 180, P(0xff1e40af));
    rrect(c, 7, 14, 34, 30, 6, V(0xff60a5fa, 0xff1d4ed8, 14, 44));
    rrect(c, 17.5f, 21.5f, 6, 6, 1.5f, P(0xffffffff));
    rrect(c, 24.5f, 21.5f, 6, 6, 1.5f, P(0xffffffff));
    rrect(c, 17.5f, 28.5f, 6, 6, 1.5f, P(0xffffffff));
    rrect(c, 24.5f, 28.5f, 6, 6, 1.5f, P(0xb0ffffff));
}

static void ic_about(struct ic *c)                      /* 'O' — the OmniOS ring */
{
    ring(c, 24, 24, 11, 20, 0, 0, D(0xff4f8cff, 0xffa855f7));
    circle(c, 24, 24, 5, P(0x60ffffff));
}

static void ic_settings(struct ic *c)                   /* 'G' — gear */
{
    struct paint g = V(0xffcbd5e1, 0xff64748b, 3, 45);
    int k;
    for (k = 0; k < 8; k++) {
        float a = (float)k * 45.0f, ux = (float)cos(a * M_PI / 180.0), uy = -(float)sin(a * M_PI / 180.0);
        float vx = -uy, vy = ux;
        float pts[8];
        pts[0] = 24 + ux * 12 + vx * 4.4f;   pts[1] = 24 + uy * 12 + vy * 4.4f;
        pts[2] = 24 + ux * 20.5f + vx * 3.2f; pts[3] = 24 + uy * 20.5f + vy * 3.2f;
        pts[4] = 24 + ux * 20.5f - vx * 3.2f; pts[5] = 24 + uy * 20.5f - vy * 3.2f;
        pts[6] = 24 + ux * 12 - vx * 4.4f;   pts[7] = 24 + uy * 12 - vy * 4.4f;
        poly(c, pts, 4, g);
    }
    circle(c, 24, 24, 15.5f, g);
    ring(c, 24, 24, 6.5f, 9, 0, 0, P(0x40000000));
    hole(c, 24, 24, 6.5f);
}

static void ic_taskmgr(struct ic *c)                    /* 'T' */
{
    static const float area[] = { 8, 34, 15, 27, 21, 30, 28, 17, 34, 23, 40, 13, 40, 37, 8, 37 };
    int i;
    rrect(c, 3, 6, 42, 36, 8, P(RIM));
    rrect(c, 4, 7, 40, 34, 7, V(0xff134e4a, 0xff0b2b29, 7, 41));
    line(c, 8, 19, 40, 19, 0.8f, P(0x40ffffff));
    line(c, 8, 29, 40, 29, 0.8f, P(0x40ffffff));
    poly(c, area, 8, P(0x505eead4));
    for (i = 0; i < 5; i++)
        line(c, area[2 * i], area[2 * i + 1], area[2 * i + 2], area[2 * i + 3], 3, P(0xff5eead4));
}

static void ic_calendar(struct ic *c)                   /* 'D' */
{
    int r, k;
    rrect(c, 5, 7, 38, 36, 6, P(RIM));
    rrect(c, 6, 8, 36, 34, 5, V(0xffffffff, 0xffe5e7eb, 8, 42));
    rrect(c, 6, 8, 36, 11, 5, P(0xffef4444));
    rrect(c, 6, 13, 36, 6, 0, P(0xffef4444));
    rrect(c, 13, 4, 3.5f, 8, 1.75f, P(0xff475569));
    rrect(c, 31.5f, 4, 3.5f, 8, 1.75f, P(0xff475569));
    for (r = 0; r < 3; r++)
        for (k = 0; k < 4; k++)
            rrect(c, 11.0f + (float)k * 7.0f, 23.0f + (float)r * 6.0f, 5, 3.8f, 1,
                  (r == 1 && k == 2) ? P(0xffef4444) : P(0xff9ca3af));
}

static void ic_mines(struct ic *c)                      /* '*' */
{
    int k;
    rrect(c, 4, 4, 40, 40, 9, V(0xffe5e7eb, 0xffc4c9d1, 4, 44));
    for (k = 0; k < 4; k++) {
        float a = (float)k * 45.0f;
        line(c, ax(24, a, 15), ay(24, a, 15), ax(24, a + 180, 15), ay(24, a + 180, 15),
             3.2f, P(0xff111827));
    }
    circle(c, 24, 24, 10.5f, V(0xff4b5563, 0xff0f172a, 13, 35));
    circle(c, 20.5f, 20.5f, 3, P(0xd0ffffff));
}

static void ic_2048(struct ic *c)                       /* '2' */
{
    rrect(c, 3, 3, 42, 42, 8, P(0xffbbada0));
    rrect(c, 7, 7, 16, 16, 3, P(0xffeee4da));
    rrect(c, 25, 7, 16, 16, 3, P(0xfff2b179));
    rrect(c, 7, 25, 16, 16, 3, P(0xfff67c5f));
    rrect(c, 25, 25, 16, 16, 3, P(0xffedc22e));
}

static void ic_tictactoe(struct ic *c)                  /* 'X' */
{
    rrect(c, 3, 3, 42, 42, 9, V(0xfff472b6, 0xffdb2777, 3, 45));
    line(c, 18.5f, 9, 18.5f, 39, 2, P(0x90ffffff));
    line(c, 29.5f, 9, 29.5f, 39, 2, P(0x90ffffff));
    line(c, 9, 18.5f, 39, 18.5f, 2, P(0x90ffffff));
    line(c, 9, 29.5f, 39, 29.5f, 2, P(0x90ffffff));
    line(c, 9.5f, 9.5f, 15.5f, 15.5f, 2.6f, P(0xffffffff));
    line(c, 15.5f, 9.5f, 9.5f, 15.5f, 2.6f, P(0xffffffff));
    ring(c, 24, 24, 2.2f, 4.2f, 0, 0, P(0xffffffff));
    line(c, 32.5f, 32.5f, 38.5f, 38.5f, 2.6f, P(0xffffffff));
    line(c, 38.5f, 32.5f, 32.5f, 38.5f, 2.6f, P(0xffffffff));
}

/* ---- Settings pages / system pictograms ------------------------------ */

static void ic_monitor(struct ic *c)                    /* 'm' System */
{
    rrect(c, 4, 7, 40, 27, 4, P(RIM));
    rrect(c, 5, 8, 38, 25, 3, D(0xff38bdf8, 0xff6366f1));
    rrect(c, 20, 34, 8, 5, 0, P(0xff64748b));
    rrect(c, 13, 38.5f, 22, 3.5f, 1.75f, P(0xff94a3b8));
}

static void ic_palette(struct ic *c)                    /* 'p' Personalization */
{
    circle(c, 24, 24, 20, V(0xfffde68a, 0xfff59e0b, 4, 44));
    hole(c, 31, 31, 4.2f);
    circle(c, 15, 20, 3.4f, P(0xffef4444));
    circle(c, 22, 13, 3.4f, P(0xff3b82f6));
    circle(c, 31, 15, 3.4f, P(0xff22c55e));
    circle(c, 14, 30, 3.4f, P(0xffa855f7));
}

static void ic_apps(struct ic *c)                       /* 'a' Apps */
{
    rrect(c, 6, 6, 16, 16, 4, P(0xff3b82f6));
    rrect(c, 26, 6, 16, 16, 4, P(0xff22c55e));
    rrect(c, 6, 26, 16, 16, 4, P(0xfff59e0b));
    rrect(c, 26, 26, 16, 16, 4, P(0xffef4444));
}

static void ic_user(struct ic *c)                       /* 'u' Accounts / avatar */
{
    circle(c, 24, 24, 21, V(0xff93c5fd, 0xff3b82f6, 3, 45));
    circle(c, 24, 19, 7.5f, P(0xffffffff));
    rrect(c, 11, 29, 26, 18, 8, P(0xffffffff));
    clip_circle(c, 24, 24, 21);                 /* shoulders end at the rim */
}

static void ic_update(struct ic *c)                     /* 'U' OmniOS Update */
{
    static const float h1[] = { 36.5f, 11, 40.5f, 22, 29, 20 };
    static const float h2[] = { 11.5f, 37, 7.5f, 26, 19, 28 };
    circle(c, 24, 24, 21, V(0xff60a5fa, 0xff2563eb, 3, 45));
    ring(c, 24, 24, 9.5f, 13, 20, 160, P(0xffffffff));
    ring(c, 24, 24, 9.5f, 13, 200, 340, P(0xffffffff));
    poly(c, h1, 3, P(0xffffffff));
    poly(c, h2, 3, P(0xffffffff));
}

static void ic_lock(struct ic *c)                       /* 'k' sign-in / lock */
{
    ring(c, 24, 19, 6.5f, 10, 0, 180, P(0xff94a3b8));
    rrect(c, 14, 18, 3.5f, 4, 0, P(0xff94a3b8));
    rrect(c, 30.5f, 18, 3.5f, 4, 0, P(0xff94a3b8));
    rrect(c, 10, 21, 28, 22, 5, V(0xfffbbf24, 0xffd97706, 21, 43));
    circle(c, 24, 30, 3, P(0xff78350f));
    rrect(c, 22.8f, 30, 2.4f, 7, 1.2f, P(0xff78350f));
}

struct icon_def { char id; void (*draw)(struct ic *); };

static const struct icon_def g_icons[] = {
    { '>', ic_terminal }, { 'F', ic_folder },   { 'E', ic_editor },
    { '+', ic_calc },     { 'i', ic_gauge },    { 'C', ic_clock },
    { 'S', ic_snake },    { 'A', ic_store },    { 'O', ic_about },
    { 'G', ic_settings }, { 'T', ic_taskmgr },  { 'D', ic_calendar },
    { '*', ic_mines },    { '2', ic_2048 },     { 'X', ic_tictactoe },
    { 'm', ic_monitor },  { 'p', ic_palette },  { 'a', ic_apps },
    { 'u', ic_user },     { 't', ic_timelang }, { 'U', ic_update },
    { 'k', ic_lock },
};
#define N_ICONS ((int)(sizeof(g_icons) / sizeof(g_icons[0])))

static const struct icon_def *find(char id)
{
    int i;
    for (i = 0; i < N_ICONS; i++)
        if (g_icons[i].id == id)
            return &g_icons[i];
    return NULL;
}

int icon_exists(char id)
{
    return find(id) != NULL;
}

/* ---- render + cache -------------------------------------------------- */

struct cached { char id; int size; uint32_t *px; };     /* straight ARGB */
static struct cached g_cache[CACHE_N];
static int g_cache_next;

static uint32_t *render(const struct icon_def *d, int size)
{
    struct ic c;
    uint32_t *out;
    int x, y, i, j;

    c.n = size * SS;
    c.k = (float)c.n / 48.0f;
    c.px = calloc((size_t)c.n * (size_t)c.n, sizeof(uint32_t));
    out = malloc((size_t)size * (size_t)size * sizeof(uint32_t));
    if (!c.px || !out) {
        free(c.px);
        free(out);
        return NULL;
    }
    d->draw(&c);
    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++) {
            unsigned a = 0, r = 0, g = 0, b = 0;
            for (j = 0; j < SS; j++)
                for (i = 0; i < SS; i++) {
                    uint32_t v = c.px[(y * SS + j) * c.n + x * SS + i];
                    a += v >> 24; r += (v >> 16) & 255; g += (v >> 8) & 255; b += v & 255;
                }
            a /= SS * SS; r /= SS * SS; g /= SS * SS; b /= SS * SS;
            if (a) {                              /* un-premultiply */
                r = r * 255 / a; g = g * 255 / a; b = b * 255 / a;
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
            }
            out[y * size + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    free(c.px);
    return out;
}

int icon_draw(struct raster *r, int x, int y, int size, char id)
{
    const struct icon_def *d = find(id);
    uint32_t *px = NULL;
    int i, j;

    if (!d || size <= 0 || size > ICON_MAX)
        return 0;
    for (i = 0; i < CACHE_N; i++)
        if (g_cache[i].px && g_cache[i].id == id && g_cache[i].size == size) {
            px = g_cache[i].px;
            break;
        }
    if (!px) {
        px = render(d, size);
        if (!px)
            return 0;
        free(g_cache[g_cache_next].px);           /* round-robin eviction */
        g_cache[g_cache_next].id = id;
        g_cache[g_cache_next].size = size;
        g_cache[g_cache_next].px = px;
        g_cache_next = (g_cache_next + 1) % CACHE_N;
    }
    for (j = 0; j < size; j++)
        for (i = 0; i < size; i++) {
            uint32_t v = px[j * size + i];
            if (v >> 24)
                th_px(r, x + i, y + j, v & 0xffffff, v >> 24);
        }
    return 1;
}
