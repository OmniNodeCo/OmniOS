/*
 * OmniOS — os/gui/font.h
 *
 * Anti-aliased TrueType text: the Selawik family (Microsoft's open-source
 * font with Segoe UI's metrics, os/vendor/fonts/selawik) drawn with
 * stb_truetype. Glyphs are cached per face, size and quarter-pixel
 * position, so text is spaced as the font intends even at small sizes.
 *
 * Fonts live in /usr/share/fonts/omnios (OMNI_FONT_DIR overrides it, for
 * tests). Without them every function still works: font_ok() is 0 and
 * callers fall back to the 8x8 bitmap font.
 *
 * Sizes are em sizes in pixels, as in CSS (15 = body text, 13 = small
 * labels). y is the top of the line box: the baseline is at
 * y + font_ascent(px).
 */
#ifndef OMNI_OS_GUI_FONT_H
#define OMNI_OS_GUI_FONT_H

#include "../lib/omni.h"

enum { FONT_REGULAR, FONT_BOLD, FONT_LIGHT, FONT_NFACES };
/* or'ed into a face: slanted (Selawik has no italic; the glyphs are sheared) */
#define FONT_ITALIC 0x10

/* load the fonts (once); returns 1 if TrueType text is available */
int  font_init(void);
int  font_ok(void);

/* draw UTF-8 text; returns the advance width in pixels */
int  font_draw(struct raster *r, const char *s, int x, int y, int px, int face,
               uint32_t rgb);
/* the same, with the baseline at y */
int  font_draw_base(struct raster *r, const char *s, int x, int base, int px,
                    int face, uint32_t rgb);
/* the first n bytes of s, baseline at base */
int  font_draw_n(struct raster *r, const char *s, int n, int x, int base, int px,
                 int face, uint32_t rgb);
/* at most max_w wide: what doesn't fit is cut and ends in an ellipsis */
int  font_draw_clip(struct raster *r, const char *s, int x, int y, int px,
                    int face, uint32_t rgb, int max_w);

int  font_width(const char *s, int px, int face);
/* width of the first n bytes of s */
int  font_width_n(const char *s, int n, int px, int face);
/* how many bytes of s fit in max_w pixels (whole characters) */
int  font_fit(const char *s, int px, int face, int max_w);
int  font_ascent(int px);
int  font_line(int px);              /* line height (ascent + descent)  */

#endif /* OMNI_OS_GUI_FONT_H */
