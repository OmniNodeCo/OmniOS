/*
 * OmniOS — os/gui/icons.h
 *
 * Pictogram app icons, drawn from vector shapes (no image files). Each icon
 * is designed on a 48x48 grid, rendered at 4x with coverage sampling for
 * smooth edges, and cached per (icon, size). Icons are keyed by the one-
 * character glyph that the app catalog already carries, so the window
 * protocol (ICON verb) and every caller of th_icon() get them for free.
 */
#ifndef OMNI_OS_GUI_ICONS_H
#define OMNI_OS_GUI_ICONS_H

#include "../lib/omni.h"

/* Draw pictogram `id` with its top-left at (x, y), `size` px square.
 * Returns 1 if `id` has a pictogram (and it was drawn), 0 otherwise. */
int icon_draw(struct raster *r, int x, int y, int size, char id);

/* 1 if `id` names a pictogram */
int icon_exists(char id);

#endif /* OMNI_OS_GUI_ICONS_H */
