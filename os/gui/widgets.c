/*
 * OmniOS — os/gui/widgets.c
 *
 * Widget rendering and hit-testing. The 3-D bevel look (light top/left,
 * dark bottom/right) gives the shell its classic Windows-style appearance.
 */
#include <string.h>

#include "widgets.h"
#include "wm.h"

void omni_button_init(struct omni_button *b, int x, int y, int w, int h,
                      const char *label)
{
    b->x = x;
    b->y = y;
    b->w = w;
    b->h = h;
    b->label = label;
    b->hover = 0;
    b->down = 0;
}

int omni_button_hit(struct omni_button *b, int x, int y)
{
    return x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h;
}

void omni_bevel(struct raster *r, int x, int y, int w, int h,
                uint32_t light, uint32_t dark)
{
    raster_hline(r, x, x + w - 1, y, light);
    raster_vline(r, x, y, y + h - 1, light);
    raster_hline(r, x, x + w - 1, y + h - 1, dark);
    raster_vline(r, x + w - 1, y, y + h - 1, dark);
}

void omni_panel_draw(struct raster *r, int x, int y, int w, int h,
                     uint32_t face, uint32_t light, uint32_t dark)
{
    raster_fill(r, x, y, w, h, face);
    omni_bevel(r, x, y, w, h, light, dark);
}

void omni_button_draw(struct raster *r, struct omni_button *b)
{
    uint32_t face  = omni_rgb(0xd8, 0xd8, 0xd8);
    uint32_t light = omni_rgb(0xff, 0xff, 0xff);
    uint32_t dark  = omni_rgb(0x80, 0x80, 0x80);

    raster_fill(r, b->x, b->y, b->w, b->h, face);
    if (b->down) {
        omni_bevel(r, b->x, b->y, b->w, b->h, dark, light);
    } else {
        omni_bevel(r, b->x, b->y, b->w, b->h, light, dark);
    }

    if (b->label) {
        struct canvas c;
        int tw = (int)strlen(b->label) * 8;
        canvas_init(&c, r, OMNI_COLOR_TEXT, face);
        canvas_text(&c, b->label,
                    b->x + (b->w - tw) / 2,
                    b->y + (b->h - 8) / 2);
    }
}

void omni_field_init(struct omni_field *f, int x, int y, int w)
{
    memset(f, 0, sizeof(*f));
    f->x = x;
    f->y = y;
    f->w = w;
    f->h = 22;
    f->cursor = 0;
}

int omni_field_hit(struct omni_field *f, int x, int y)
{
    return x >= f->x && x < f->x + f->w && y >= f->y && y < f->y + f->h;
}

int omni_field_key(struct omni_field *f, int keycode, char ch)
{
    if (keycode == OMNI_KEY_BACKSPACE) {
        if (f->cursor > 0) {
            memmove(&f->text[f->cursor - 1], &f->text[f->cursor],
                    (size_t)(f->len - f->cursor) + 1);
            f->len--;
            f->cursor--;
        }
        return 1;
    }
    if (keycode == OMNI_KEY_ENTER) {
        f->text[f->len] = '\n';
        f->len++;
        f->text[f->len] = '\0';
        return 1;
    }
    if (keycode == OMNI_KEY_LEFT) {
        if (f->cursor > 0)
            f->cursor--;
        return 1;
    }
    if (keycode == OMNI_KEY_RIGHT) {
        if (f->cursor < f->len)
            f->cursor++;
        return 1;
    }
    if (ch >= 32 && ch < 127 && f->len < (int)sizeof(f->text) - 1) {
        memmove(&f->text[f->cursor + 1], &f->text[f->cursor],
                (size_t)(f->len - f->cursor) + 1);
        f->text[f->cursor] = ch;
        f->len++;
        f->cursor++;
        return 1;
    }
    return 0;
}

void omni_field_draw(struct raster *r, struct omni_field *f)
{
    struct canvas c;
    raster_fill(r, f->x, f->y, f->w, f->h, OMNI_COLOR_WHITE);
    omni_bevel(r, f->x, f->y, f->w, f->h,
               omni_rgb(0x80, 0x80, 0x80), omni_rgb(0xff, 0xff, 0xff));

    canvas_init(&c, r, OMNI_COLOR_TEXT, OMNI_COLOR_WHITE);
    canvas_set_clip(&c, f->x + 3, f->y + 5, f->w - 8, 8);
    canvas_text(&c, f->text, f->x + 3, f->y + 5);

    if (f->focused) {
        int cx = f->x + 3 + f->cursor * 8;
        if (cx < f->x + f->w - 4)
            raster_vline(r, cx, f->y + 3, f->y + f->h - 4, OMNI_COLOR_TEXT);
    }
}
