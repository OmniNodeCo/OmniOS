/*
 * OmniOS — os/gui/widgets.h
 *
 * Small widget set drawn with the raster/canvas layers: buttons, panels and
 * input fields. Used by the desktop shell and the bundled applications.
 */
#ifndef OMNI_OS_GUI_WIDGETS_H
#define OMNI_OS_GUI_WIDGETS_H

#include "../lib/omni.h"

/* A visual button. */
struct omni_button {
    int x, y, w, h;
    const char *label;
    int hover;
    int down;
};

void omni_button_init(struct omni_button *b, int x, int y, int w, int h,
                      const char *label);
int  omni_button_hit(struct omni_button *b, int x, int y);
void omni_button_draw(struct raster *r, struct omni_button *b);

/* engine helpers for the classic 3-D chrome look */
void omni_panel_draw(struct raster *r, int x, int y, int w, int h,
                     uint32_t face, uint32_t light, uint32_t dark);
void omni_bevel(struct raster *r, int x, int y, int w, int h,
                uint32_t light, uint32_t dark);

/* input field (single-line text editing entry) */
struct omni_field {
    int x, y, w, h;
    char text[256];
    int len;
    int cursor;
    int focused;
};

void omni_field_init(struct omni_field *f, int x, int y, int w);
int  omni_field_hit(struct omni_field *f, int x, int y);
int  omni_field_key(struct omni_field *f, int keycode, char ch);
void omni_field_draw(struct raster *r, struct omni_field *f);

#endif /* OMNI_OS_GUI_WIDGETS_H */
