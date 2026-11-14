/*
 * OmniOS — os/apps/html.h
 *
 * The web browser's HTML engine, from scratch: a forgiving tokenizer and a
 * one-pass layout that turns a page into a list of positioned boxes (runs
 * of text, images, rules, form fields), then draws any part of it. No
 * scripts and no style sheets: pages look like their plain versions, as in
 * a text-mode browser, but with real fonts, images and forms.
 */
#ifndef OMNI_OS_APPS_HTML_H
#define OMNI_OS_APPS_HTML_H

#include <stddef.h>
#include <stdint.h>

#include "../lib/omni.h"

enum { BOX_TEXT, BOX_MONO, BOX_IMG, BOX_RULE, BOX_BG, BOX_BAR, BOX_INPUT };

/* box flags */
#define BF_UNDER   1
#define BF_STRIKE  2
#define BF_MARK    4                /* highlighted background (<mark>) */
#define BF_CODE    8                /* inline code (shaded)            */

struct html_box {
    int      x, y, w, h;            /* document coordinates               */
    short    base;                  /* baseline, from y (text)            */
    unsigned char type, face, size, flags;
    uint32_t color;
    int      link;                  /* index into links, -1 none          */
    int      text, len;             /* text: bytes in html_doc.text       */
    int      ref;                   /* image or input index               */
};

enum { IMG_WAIT, IMG_LOADING, IMG_OK, IMG_FAILED };

struct html_image {
    char    *url;
    int      state;
    int      nat_w, nat_h;          /* natural size (0 = not known yet)   */
    uint32_t *px;                   /* natural-size pixels (IMG_OK)        */
    uint32_t *scaled;               /* cached copy at sw x sh              */
    int      sw, sh;
};

enum { IN_TEXT, IN_PASSWORD, IN_SUBMIT, IN_BUTTON, IN_HIDDEN, IN_CHECK,
       IN_RADIO, IN_AREA, IN_SELECT };

struct html_input {
    int   type, form;
    char  name[64];
    char  value[512];
    char  placeholder[96];
    int   checked;
};

struct html_form {
    char  action[1024];
    int   post;
};

struct html_anchor {
    char  name[64];
    int   y;
};

struct html_doc {
    char   title[256];
    char   base[2048];              /* links resolve against this         */
    struct html_box *box;    int nbox, capbox;
    char  *text;             int ntext, captext;
    char **link;             int nlink, caplink;
    struct html_image *img;  int nimg, capimg;
    struct html_input *in;   int nin, capin;
    struct html_form *form;  int nform, capform;
    struct html_anchor *anc; int nanc, capanc;
    int    width, height;           /* laid out for width; total height  */
    int    relayout;                /* an image's size became known       */
};

/* lay src out for a content width (pixels). Images found keep their
 * pixels and form fields their values across calls (same document). */
void html_layout(struct html_doc *d, const char *src, size_t n,
                 const char *base_url, int width);
void html_free(struct html_doc *d);

/* draw the part of the page from document row `scroll` into r at (ox, oy),
 * view_h tall; focus = the focused input (-1), hover = the link under the
 * pointer (-1) */
void html_draw(const struct html_doc *d, struct raster *r, int ox, int oy,
               int scroll, int view_h, int focus, int hover);

/* what is at document point (x, y): returns the box index or -1 */
int  html_hit(const struct html_doc *d, int x, int y);

/* give image i its pixels (natural size, 0xAARRGGBB); takes ownership.
 * Sets d->relayout if the page has to be laid out again. */
void html_image_set(struct html_doc *d, int i, uint32_t *px, int w, int h);

/* y of an anchor (#name), or -1 */
int  html_anchor_y(const struct html_doc *d, const char *name);

/* resolve rel against base into out (an absolute URL) */
void html_resolve(const char *base, const char *rel, char *out, size_t n);
/* %-encode s for a query string into out */
void html_urlencode(const char *s, char *out, size_t n);

#endif /* OMNI_OS_APPS_HTML_H */
