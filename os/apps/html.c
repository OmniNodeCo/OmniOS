/*
 * OmniOS — os/apps/html.c
 *
 * HTML engine of the web browser (see html.h). One pass over the source:
 * the tokenizer hands tags and text to the layout, which keeps a stack of
 * open elements (their font, colour, link, indent), breaks text into lines
 * and places boxes. Real-world HTML is messy, so it forgives: unknown tags
 * are ignored, end tags close whatever they match, <p>/<li>/<td> close
 * themselves, and nothing is ever trusted to be well formed.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "html.h"
#include "../gui/font.h"

#define C_TEXT   0x1f2328
#define C_LINK   0x0b57d0
#define C_DIM    0x59636e
#define C_RULE   0xd8dee4
#define C_PREBG  0xf3f4f6
#define C_BAR    0xd0d7de
#define BODY_PX  15
#define MONO_W   8                   /* bitmap font for code: 8x16 cells */
#define MONO_H   16
#define MAXDEPTH 96

/* ---- small growable arrays ------------------------------------------------ */
static void *grow(void *p, int *cap, int need, size_t elem)
{
    if (need <= *cap)
        return p;
    {
        int nc = *cap ? *cap * 2 : 64;
        void *q;
        while (nc < need)
            nc *= 2;
        q = realloc(p, (size_t)nc * elem);
        if (!q)
            return NULL;
        *cap = nc;
        return q;
    }
}

static int text_add(struct html_doc *d, const char *s, int n)
{
    char *t = grow(d->text, &d->captext, d->ntext + n + 1, 1);
    int at = d->ntext;
    if (!t)
        return -1;
    d->text = t;
    memcpy(d->text + d->ntext, s, (size_t)n);
    d->ntext += n;
    d->text[d->ntext] = '\0';
    return at;
}

/* ---- URLs ------------------------------------------------------------------- */
static int has_scheme(const char *s)
{
    const char *p = s;
    if (!isalpha((unsigned char)*p))
        return 0;
    while (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.')
        p++;
    return *p == ':';
}

void html_resolve(const char *base, const char *rel, char *out, size_t n)
{
    char tmp[4096], *q;
    const char *host_end, *path;
    size_t blen;

    while (*rel == ' ' || *rel == '\t' || *rel == '\n' || *rel == '\r')
        rel++;
    if (has_scheme(rel) || !base || !*base) {
        snprintf(out, n, "%s", rel);
        return;
    }
    if (rel[0] == '/' && rel[1] == '/') {                /* //host/path */
        const char *c = strchr(base, ':');
        snprintf(out, n, "%.*s%s", c ? (int)(c - base + 1) : 0, base, rel);
        return;
    }
    /* scheme://host part of base */
    host_end = strstr(base, "://");
    host_end = host_end ? host_end + 3 : base;
    while (*host_end && *host_end != '/' && *host_end != '?' && *host_end != '#')
        host_end++;
    if (rel[0] == '/') {
        snprintf(out, n, "%.*s%s", (int)(host_end - base), base, rel);
        return;
    }
    if (rel[0] == '#' || rel[0] == '?' || rel[0] == '\0') {
        const char *cut = strpbrk(host_end, rel[0] == '?' ? "?#" : "#");
        blen = cut ? (size_t)(cut - base) : strlen(base);
        snprintf(out, n, "%.*s%s", (int)blen, base, rel);
        return;
    }
    /* relative path: base directory + rel, then fold . and .. */
    path = host_end;
    {
        const char *end = strpbrk(path, "?#");
        const char *slash;
        size_t plen = end ? (size_t)(end - path) : strlen(path);
        slash = NULL;
        for (q = (char *)path; q < path + plen; q++)
            if (*q == '/')
                slash = q;
        if (slash)
            snprintf(tmp, sizeof(tmp), "%.*s%s", (int)(slash - path + 1), path, rel);
        else
            snprintf(tmp, sizeof(tmp), "/%s", rel);
    }
    {
        char res[4096], *segs[256];
        int ns = 0, i;
        char *qs = strpbrk(tmp, "?#"), tail[2048] = "";
        char *tok, *save = NULL;
        int trailing;
        if (qs) {
            snprintf(tail, sizeof(tail), "%s", qs);
            *qs = '\0';
        }
        trailing = tmp[0] && tmp[strlen(tmp) - 1] == '/';
        for (tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
            if (strcmp(tok, ".") == 0)
                continue;
            if (strcmp(tok, "..") == 0) {
                if (ns > 0)
                    ns--;
                continue;
            }
            if (ns < 256)
                segs[ns++] = tok;
        }
        res[0] = '\0';
        for (i = 0; i < ns; i++) {
            size_t l = strlen(res);
            snprintf(res + l, sizeof(res) - l, "/%s", segs[i]);
        }
        if (!res[0] || trailing) {
            size_t l = strlen(res);
            snprintf(res + l, sizeof(res) - l, "/");
        }
        snprintf(out, n, "%.*s%s%s", (int)(host_end - base), base, res, tail);
    }
}

void html_urlencode(const char *s, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else if (c == ' ') {
            out[o++] = '+';
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = '\0';
}

/* ---- entities --------------------------------------------------------------- */
static int put_utf8(char *o, uint32_t cp)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xc0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xe0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 63));
        o[2] = (char)(0x80 | (cp & 63)); return 3;
    }
    o[0] = (char)(0xf0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 63));
    o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63));
    return 4;
}

static const struct { const char *n; uint32_t cp; } g_ent[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
    { "nbsp", 0xa0 }, { "copy", 0xa9 }, { "reg", 0xae }, { "trade", 0x2122 },
    { "hellip", 0x2026 }, { "mdash", 0x2014 }, { "ndash", 0x2013 },
    { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "ldquo", 0x201c }, { "rdquo", 0x201d },
    { "sbquo", 0x201a }, { "bdquo", 0x201e }, { "bull", 0x2022 }, { "middot", 0xb7 },
    { "laquo", 0xab }, { "raquo", 0xbb }, { "lsaquo", 0x2039 }, { "rsaquo", 0x203a },
    { "euro", 0x20ac }, { "pound", 0xa3 }, { "yen", 0xa5 }, { "cent", 0xa2 },
    { "deg", 0xb0 }, { "plusmn", 0xb1 }, { "times", 0xd7 }, { "divide", 0xf7 },
    { "frac12", 0xbd }, { "frac14", 0xbc }, { "frac34", 0xbe }, { "sup1", 0xb9 },
    { "sup2", 0xb2 }, { "sup3", 0xb3 }, { "para", 0xb6 }, { "sect", 0xa7 },
    { "iexcl", 0xa1 }, { "iquest", 0xbf }, { "shy", 0xad }, { "micro", 0xb5 },
    { "larr", 0x2190 }, { "rarr", 0x2192 }, { "uarr", 0x2191 }, { "darr", 0x2193 },
    { "harr", 0x2194 }, { "hearts", 0x2665 }, { "zwj", 0x200d }, { "zwnj", 0x200c },
    { "thinsp", 0x2009 }, { "ensp", 0x2002 }, { "emsp", 0x2003 }, { "prime", 0x2032 },
    { "eacute", 0xe9 }, { "egrave", 0xe8 }, { "aacute", 0xe1 }, { "agrave", 0xe0 },
    { "ouml", 0xf6 }, { "uuml", 0xfc }, { "auml", 0xe4 }, { "szlig", 0xdf },
    { "ccedil", 0xe7 }, { "ntilde", 0xf1 }, { "oacute", 0xf3 }, { "iacute", 0xed },
    { "uacute", 0xfa }, { "Eacute", 0xc9 }, { "Ouml", 0xd6 }, { "Uuml", 0xdc },
    { "Auml", 0xc4 }, { "aring", 0xe5 }, { "oslash", 0xf8 }, { "aelig", 0xe6 },
};

/* decode entities of s[0..n) into a new NUL-terminated buffer */
static char *decode(const char *s, int n, int *outn)
{
    char *o = malloc((size_t)n * 2 + 8);
    int i = 0, k = 0;
    if (!o)
        return NULL;
    while (i < n) {
        if (s[i] == '&') {
            int j = i + 1, m;
            uint32_t cp = 0;
            if (j < n && s[j] == '#') {
                j++;
                if (j < n && (s[j] == 'x' || s[j] == 'X')) {
                    for (j++; j < n && isxdigit((unsigned char)s[j]) && j - i < 10; j++)
                        cp = cp * 16 + (uint32_t)(isdigit((unsigned char)s[j]) ? s[j] - '0'
                                                  : (tolower((unsigned char)s[j]) - 'a' + 10));
                } else {
                    for (; j < n && isdigit((unsigned char)s[j]) && j - i < 10; j++)
                        cp = cp * 10 + (uint32_t)(s[j] - '0');
                }
                if (cp > 0 && cp < 0x110000) {
                    if (cp >= 0x80 && cp < 0xa0) {      /* windows-1252 range */
                        static const uint16_t w[32] = {
                            0x20ac, 0, 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021,
                            0x2c6, 0x2030, 0x160, 0x2039, 0x152, 0, 0x17d, 0, 0,
                            0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
                            0x2dc, 0x2122, 0x161, 0x203a, 0x153, 0, 0x17e, 0x178 };
                        cp = w[cp - 0x80] ? w[cp - 0x80] : '?';
                    }
                    if (j < n && s[j] == ';')
                        j++;
                    k += put_utf8(o + k, cp);
                    i = j;
                    continue;
                }
            } else {
                for (m = 0; m < (int)(sizeof(g_ent) / sizeof(g_ent[0])); m++) {
                    size_t l = strlen(g_ent[m].n);
                    if (i + 1 + (int)l <= n && strncmp(s + i + 1, g_ent[m].n, l) == 0 &&
                        (i + 1 + (int)l == n || !isalnum((unsigned char)s[i + 1 + l]))) {
                        j = i + 1 + (int)l;
                        if (j < n && s[j] == ';')
                            j++;
                        k += put_utf8(o + k, g_ent[m].cp);
                        i = j;
                        break;
                    }
                }
                if (m < (int)(sizeof(g_ent) / sizeof(g_ent[0])))
                    continue;
            }
        }
        o[k++] = s[i++];
    }
    o[k] = '\0';
    *outn = k;
    return o;
}

/* ---- tags -------------------------------------------------------------------- */
enum {
    T_NONE, T_HTML, T_HEAD, T_BODY, T_TITLE, T_BASE, T_META, T_LINK, T_SCRIPT,
    T_STYLE, T_NOSCRIPT, T_TEMPLATE, T_SVG, T_IFRAME, T_OBJECT, T_CANVAS,
    T_H1, T_H2, T_H3, T_H4, T_H5, T_H6, T_P, T_DIV, T_BR, T_HR, T_PRE,
    T_BLOCKQUOTE, T_UL, T_OL, T_LI, T_DL, T_DT, T_DD, T_TABLE, T_TR, T_TD,
    T_TH, T_CAPTION, T_A, T_B, T_I, T_U, T_S, T_CODE, T_SMALL, T_BIG, T_SUB,
    T_SUP, T_MARK, T_FONT, T_SPAN, T_Q, T_IMG, T_INPUT, T_BUTTON, T_TEXTAREA,
    T_SELECT, T_OPTION, T_FORM, T_LABEL, T_CENTER, T_SECTION, T_FIGURE,
    T_FIGCAPTION, T_ADDRESS, T_NAV, T_HEADER, T_FOOTER, T_MAIN, T_ARTICLE,
    T_ASIDE, T_DETAILS, T_SUMMARY, T_FIELDSET, T_LEGEND, T_THEAD, T_TBODY,
    T_TFOOT, T_WBR, T_AREA, T_COL, T_SOURCE, T_PICTURE, T_VIDEO, T_AUDIO,
    T_MENU, T_HGROUP, T_DIALOG, T_MATH, T_KBD,
};

static const struct { const char *n; unsigned char t; } g_tags[] = {
    { "html", T_HTML }, { "head", T_HEAD }, { "body", T_BODY }, { "title", T_TITLE },
    { "base", T_BASE }, { "meta", T_META }, { "link", T_LINK }, { "script", T_SCRIPT },
    { "style", T_STYLE }, { "noscript", T_NOSCRIPT }, { "template", T_TEMPLATE },
    { "svg", T_SVG }, { "iframe", T_IFRAME }, { "object", T_OBJECT }, { "embed", T_OBJECT },
    { "canvas", T_CANVAS }, { "h1", T_H1 }, { "h2", T_H2 }, { "h3", T_H3 },
    { "h4", T_H4 }, { "h5", T_H5 }, { "h6", T_H6 }, { "p", T_P }, { "div", T_DIV },
    { "br", T_BR }, { "hr", T_HR }, { "pre", T_PRE }, { "listing", T_PRE },
    { "xmp", T_PRE }, { "blockquote", T_BLOCKQUOTE }, { "ul", T_UL }, { "ol", T_OL },
    { "menu", T_MENU }, { "dir", T_UL }, { "li", T_LI }, { "dl", T_DL }, { "dt", T_DT },
    { "dd", T_DD }, { "table", T_TABLE }, { "tr", T_TR }, { "td", T_TD }, { "th", T_TH },
    { "caption", T_CAPTION }, { "a", T_A }, { "b", T_B }, { "strong", T_B },
    { "i", T_I }, { "em", T_I }, { "cite", T_I }, { "dfn", T_I }, { "var", T_I },
    { "u", T_U }, { "ins", T_U }, { "s", T_S }, { "strike", T_S }, { "del", T_S },
    { "code", T_CODE }, { "tt", T_CODE }, { "samp", T_CODE }, { "kbd", T_KBD },
    { "small", T_SMALL }, { "big", T_BIG }, { "sub", T_SUB }, { "sup", T_SUP },
    { "mark", T_MARK }, { "font", T_FONT }, { "span", T_SPAN }, { "abbr", T_SPAN },
    { "time", T_SPAN }, { "bdi", T_SPAN }, { "bdo", T_SPAN }, { "data", T_SPAN },
    { "q", T_Q }, { "img", T_IMG }, { "input", T_INPUT }, { "button", T_BUTTON },
    { "textarea", T_TEXTAREA }, { "select", T_SELECT }, { "option", T_OPTION },
    { "form", T_FORM }, { "label", T_LABEL }, { "center", T_CENTER },
    { "section", T_SECTION }, { "figure", T_FIGURE }, { "figcaption", T_FIGCAPTION },
    { "address", T_ADDRESS }, { "nav", T_NAV }, { "header", T_HEADER },
    { "footer", T_FOOTER }, { "main", T_MAIN }, { "article", T_ARTICLE },
    { "aside", T_ASIDE }, { "details", T_DETAILS }, { "summary", T_SUMMARY },
    { "fieldset", T_FIELDSET }, { "legend", T_LEGEND }, { "thead", T_THEAD },
    { "tbody", T_TBODY }, { "tfoot", T_TFOOT }, { "wbr", T_WBR }, { "area", T_AREA },
    { "col", T_COL }, { "colgroup", T_COL }, { "source", T_SOURCE }, { "track", T_SOURCE },
    { "param", T_SOURCE }, { "picture", T_PICTURE }, { "video", T_VIDEO },
    { "audio", T_AUDIO }, { "hgroup", T_HGROUP }, { "dialog", T_DIALOG },
    { "math", T_MATH }, { "search", T_DIV }, { "output", T_SPAN },
};

static int tag_id(const char *s, int n)
{
    int i;
    for (i = 0; i < (int)(sizeof(g_tags) / sizeof(g_tags[0])); i++)
        if ((int)strlen(g_tags[i].n) == n && strncasecmp(g_tags[i].n, s, (size_t)n) == 0)
            return g_tags[i].t;
    return T_NONE;
}

static int is_void(int t)
{
    return t == T_BR || t == T_HR || t == T_IMG || t == T_INPUT || t == T_META ||
           t == T_LINK || t == T_BASE || t == T_WBR || t == T_AREA || t == T_COL ||
           t == T_SOURCE;
}

static int is_block(int t)
{
    switch (t) {
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6: case T_P:
    case T_DIV: case T_PRE: case T_BLOCKQUOTE: case T_UL: case T_OL: case T_LI:
    case T_DL: case T_DT: case T_DD: case T_TABLE: case T_TR: case T_CAPTION:
    case T_FORM: case T_CENTER: case T_SECTION: case T_FIGURE: case T_FIGCAPTION:
    case T_ADDRESS: case T_NAV: case T_HEADER: case T_FOOTER: case T_MAIN:
    case T_ARTICLE: case T_ASIDE: case T_DETAILS: case T_SUMMARY: case T_FIELDSET:
    case T_LEGEND: case T_HR: case T_MENU: case T_HGROUP: case T_DIALOG:
    case T_THEAD: case T_TBODY: case T_TFOOT:
        return 1;
    default:
        return 0;
    }
}

/* ---- attributes --------------------------------------------------------------- */
struct attr { const char *k; int kn; const char *v; int vn; };

static int attr_get(const struct attr *a, int na, const char *key, char *out, size_t n)
{
    int i;
    for (i = 0; i < na; i++)
        if ((int)strlen(key) == a[i].kn && strncasecmp(a[i].k, key, (size_t)a[i].kn) == 0) {
            int dn = 0;
            char *dec = decode(a[i].v, a[i].vn, &dn);
            if (out && n) {
                snprintf(out, n, "%s", dec ? dec : "");
            }
            free(dec);
            return 1;
        }
    if (out && n)
        out[0] = '\0';
    return 0;
}

/* #rgb / #rrggbb / a few names -> colour, or -1 */
static long parse_color(const char *s)
{
    static const struct { const char *n; uint32_t c; } names[] = {
        { "black", 0x000000 }, { "white", 0xffffff }, { "red", 0xcc0000 },
        { "green", 0x008000 }, { "blue", 0x0000cc }, { "gray", 0x808080 },
        { "grey", 0x808080 }, { "navy", 0x000080 }, { "maroon", 0x800000 },
        { "purple", 0x800080 }, { "teal", 0x008080 }, { "orange", 0xe07000 },
        { "darkred", 0x8b0000 }, { "darkgreen", 0x006400 }, { "darkblue", 0x00008b },
    };
    size_t i;
    while (*s == ' ')
        s++;
    if (*s == '#') {
        unsigned v = 0;
        int n = 0;
        for (s++; isxdigit((unsigned char)s[n]) && n < 6; n++)
            v = v * 16 + (unsigned)(isdigit((unsigned char)s[n]) ? s[n] - '0'
                                    : tolower((unsigned char)s[n]) - 'a' + 10);
        if (n == 3)
            return (long)(((v >> 8) & 15) * 0x110000 + ((v >> 4) & 15) * 0x1100 + (v & 15) * 0x11);
        return n == 6 ? (long)v : -1;
    }
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strncasecmp(s, names[i].n, strlen(names[i].n)) == 0)
            return (long)names[i].c;
    return -1;
}

/* ---- layout state ------------------------------------------------------------- */
struct st {
    unsigned char tag, size, face, flags;
    unsigned char mono, pre, center, hidden;
    uint32_t color;
    int link, indent;
};

struct lay {
    struct html_doc *d;
    int width;                      /* content width                       */
    int y;                          /* top of the line being built          */
    int pen;                        /* x of the next item, from the indent  */
    int line0;                      /* first box of the line                */
    int line_has;                   /* the line has content                 */
    int space;                      /* a space before the next word         */
    int margin;                     /* collapsed margin before next content */
    int started;                    /* any content yet (top margin ignored) */
    struct st st[MAXDEPTH];
    int sp;
    int list_kind[16], list_n[16], lsp;
    int bullet;                     /* li started: a marker is due          */
    int cell;                       /* cells so far in this table row       */
    int form;                       /* current form or -1                   */
    int nin;                        /* inputs met so far                    */
    int in_select, in_option, sel_input;
    int pre_y0[8], pre_sp;          /* tops of open <pre> / <blockquote>    */
    int bq_y0[8], bq_ind[8], bq_sp;
    int raw_skip;                   /* in <script>/<style>                  */
    struct { int on, ncol, cell, colx[24]; } tbl[4];   /* open tables */
    int tsp;
    const char *pos, *end;          /* the tokenizer's position (pre-scans) */
};

static struct st *cur(struct lay *L) { return &L->st[L->sp]; }

static int avail(struct lay *L)
{
    int a = L->width - cur(L)->indent;
    return a < 40 ? 40 : a;
}

static int text_w(const struct st *s, const char *t, int n)
{
    if (s->mono) {
        int c = 0;
        const char *p = t, *e = t + n;
        while (p < e && *p) {
            omni_utf8_next(&p);
            c++;
        }
        return c * MONO_W;
    }
    return font_width_n(t, n, s->size, s->face);
}

static int lead(const struct st *s)        /* line height of the style */
{
    if (s->mono)
        return 19;
    return s->size >= 19 ? s->size * 13 / 10 : s->size * 3 / 2;
}

static struct html_box *new_box(struct lay *L)
{
    struct html_doc *d = L->d;
    struct html_box *b = grow(d->box, &d->capbox, d->nbox + 1, sizeof(*b));
    if (!b)
        return NULL;
    d->box = b;
    b = &d->box[d->nbox++];
    memset(b, 0, sizeof(*b));
    b->link = -1;
    b->ref = -1;
    return b;
}

/* finish the line: vertical positions of its boxes, centring */
static void end_line(struct lay *L, int force)
{
    struct html_doc *d = L->d;
    int asc = 0, desc = 0, i, shift = 0;
    if (!L->line_has) {
        if (force) {                                  /* <br> on an empty line */
            L->y += lead(cur(L));
        }
        L->pen = 0;
        L->space = 0;
        return;
    }
    for (i = L->line0; i < d->nbox; i++) {
        struct html_box *b = &d->box[i];
        int a, ds;
        if (b->type == BOX_BG || b->type == BOX_BAR)
            continue;
        if (b->type == BOX_TEXT || b->type == BOX_MONO) {
            int l = b->type == BOX_MONO ? 19 : (b->size >= 19 ? b->size * 13 / 10 : b->size * 3 / 2);
            int fa = b->type == BOX_MONO ? 13 : font_ascent(b->size);
            int fl = b->type == BOX_MONO ? MONO_H : font_line(b->size);
            int half = (l - fl) / 2;
            a = fa + half;
            ds = l - a;
        } else {
            a = b->h - (b->type == BOX_INPUT ? 7 : 0);
            ds = b->h - a + (b->type == BOX_IMG ? 2 : 0);
        }
        if (a > asc) asc = a;
        if (ds > desc) desc = ds;
    }
    if (cur(L)->center && L->pen < avail(L))
        shift = (avail(L) - L->pen) / 2;
    for (i = L->line0; i < d->nbox; i++) {
        struct html_box *b = &d->box[i];
        if (b->type == BOX_BG || b->type == BOX_BAR)
            continue;
        b->x += shift;
        if (b->type == BOX_TEXT || b->type == BOX_MONO) {
            int fa = b->type == BOX_MONO ? 13 : font_ascent(b->size);
            b->y = L->y + asc - fa;
            b->base = (short)fa;
            b->h = b->type == BOX_MONO ? MONO_H : font_line(b->size);
        } else {
            int a = b->h - (b->type == BOX_INPUT ? 7 : 0);
            b->y = L->y + asc - a;
        }
    }
    L->y += asc + desc;
    L->pen = 0;
    L->space = 0;
    L->line_has = 0;
    L->line0 = d->nbox;
}

static void block(struct lay *L, int margin)
{
    end_line(L, 0);
    if (margin > L->margin)
        L->margin = margin;
}

/* content is about to be placed: margins and list markers first */
static void content(struct lay *L)
{
    if (!L->line_has) {
        if (L->started)
            L->y += L->margin;
        L->margin = 0;
        L->line0 = L->d->nbox;
        L->started = 1;
        L->line_has = 1;
        if (L->bullet) {
            struct st *s = cur(L);
            char mark[16];
            int k = L->lsp > 0 ? L->lsp - 1 : 0, ord = L->lsp > 0 && L->list_kind[k];
            struct html_box *b;
            int at;
            L->bullet = 0;
            if (ord)
                snprintf(mark, sizeof(mark), "%d.", ++L->list_n[k]);
            else
                snprintf(mark, sizeof(mark), "%s", (k % 2) ? "\xe2\x80\x93" : "\xe2\x80\xa2");
            at = text_add(L->d, mark, (int)strlen(mark));
            b = at >= 0 ? new_box(L) : NULL;
            if (b) {
                b->type = BOX_TEXT;
                b->size = (unsigned char)(s->size > 15 ? 15 : s->size);
                b->face = FONT_REGULAR;
                b->color = C_DIM;
                b->text = at;
                b->len = (int)strlen(mark);
                b->w = font_width(mark, b->size, b->face);
                b->x = s->indent - b->w - 8;
            }
        }
    }
}

static void put_text(struct lay *L, const char *w, int n, int ww, int sp)
{
    struct html_doc *d = L->d;
    struct st *s = cur(L);
    struct html_box *last = d->nbox > L->line0 && L->line_has ? &d->box[d->nbox - 1] : NULL;
    int type = s->mono ? BOX_MONO : BOX_TEXT;
    content(L);
    if (last && last->type == type && last->size == s->size && last->face == s->face &&
        last->color == s->color && last->link == s->link &&
        last->flags == (unsigned char)(s->flags | (s->mono && !s->pre ? BF_CODE : 0)) &&
        last->text + last->len == d->ntext && last->x + last->w == s->indent + L->pen) {
        if (sp) {
            text_add(d, " ", 1);
            last->len++;
        }
        if (text_add(d, w, n) >= 0)
            last->len += n;
        last->w += sp + ww;
    } else {
        struct html_box *b;
        int at = text_add(d, w, n);
        if (at < 0 || !(b = new_box(L)))
            return;
        b->type = (unsigned char)type;
        b->size = s->size;
        b->face = s->face;
        b->flags = (unsigned char)(s->flags | (s->mono && !s->pre ? BF_CODE : 0));
        b->color = s->color;
        b->link = s->link;
        b->text = at;
        b->len = n;
        b->x = s->indent + L->pen + sp;
        b->w = ww;
    }
    L->pen += sp + ww;
    L->space = 0;
}

static void word(struct lay *L, const char *w, int n)
{
    struct st *s = cur(L);
    int ww = text_w(s, w, n);
    int sp = (L->space && L->pen > 0) ? text_w(s, " ", 1) : 0;
    if (L->pen > 0 && L->pen + sp + ww > avail(L)) {
        end_line(L, 0);
        sp = 0;
    }
    while (ww > avail(L) && n > 1) {                 /* a very long word */
        int k = s->mono ? (avail(L) - L->pen) / MONO_W : font_fit(w, s->size, s->face, avail(L) - L->pen);
        if (k <= 0) {
            if (L->pen == 0)
                k = 1;
            else {
                end_line(L, 0);
                continue;
            }
        }
        while (k < n && (w[k] & 0xc0) == 0x80)       /* whole characters */
            k++;
        put_text(L, w, k, text_w(s, w, k), sp);
        w += k;
        n -= k;
        sp = 0;
        end_line(L, 0);
        ww = text_w(s, w, n);
    }
    if (n > 0)
        put_text(L, w, n, ww, sp);
}

static void text(struct lay *L, const char *t, int n)
{
    struct st *s = cur(L);
    int i = 0;
    if (s->hidden || L->raw_skip)
        return;
    if (L->in_select) {                               /* option text: the label */
        if (L->in_option && L->sel_input >= 0 && L->sel_input < L->d->nin) {
            struct html_input *in = &L->d->in[L->sel_input];
            if (!in->placeholder[0]) {
                int k = 0;
                while (k < n && isspace((unsigned char)t[k])) k++;
                snprintf(in->placeholder, sizeof(in->placeholder), "%.*s", n - k, t + k);
            }
        }
        return;
    }
    if (s->pre) {
        while (i < n) {
            int j = i;
            while (j < n && t[j] != '\n')
                j++;
            if (j > i) {
                char buf[1024];
                int k, m = 0;
                for (k = i; k < j && m < (int)sizeof(buf) - 9; k++) {
                    if (t[k] == '\t') {
                        do buf[m++] = ' '; while (m % 8);
                    } else if (t[k] != '\r') {
                        buf[m++] = t[k];
                    }
                }
                word(L, buf, m);
            }
            if (j < n)
                end_line(L, 1);
            i = j + 1;
        }
        return;
    }
    while (i < n) {
        int j;
        if (isspace((unsigned char)t[i])) {
            L->space = 1;
            i++;
            continue;
        }
        for (j = i; j < n && !isspace((unsigned char)t[j]); j++)
            ;
        word(L, t + i, j - i);
        i = j;
    }
}

static void push(struct lay *L, int tag)
{
    if (L->sp + 1 >= MAXDEPTH)
        return;
    L->st[L->sp + 1] = L->st[L->sp];
    L->sp++;
    L->st[L->sp].tag = (unsigned char)tag;
}

static void close_block_end(struct lay *L, int tag);

/* pop up to and including the nearest `tag`; returns 1 if found */
static int pop_to(struct lay *L, int tag)
{
    int i;
    for (i = L->sp; i > 0; i--)
        if (L->st[i].tag == tag)
            break;
    if (i == 0)
        return 0;
    while (L->sp >= i) {
        int t = L->st[L->sp].tag;
        if (is_block(t))
            end_line(L, 0);             /* its last line, in its own style */
        L->sp--;
        close_block_end(L, t);
    }
    return 1;
}

static int open_has(struct lay *L, int tag)
{
    int i;
    for (i = L->sp; i > 0; i--)
        if (L->st[i].tag == tag)
            return 1;
    return 0;
}

static int heading_px(int t)
{
    static const int px[6] = { 30, 24, 19, 16, 14, 13 };
    return px[t - T_H1];
}

/* what an element's end does to the layout (after it was popped) */
static void close_block_end(struct lay *L, int tag)
{
    struct html_doc *d = L->d;
    switch (tag) {
    case T_H1: case T_H2:
        block(L, 14);
        break;
    case T_H3: case T_H4: case T_H5: case T_H6: case T_P: case T_FIGURE:
        block(L, 12);
        break;
    case T_UL: case T_OL: case T_MENU:
        if (L->lsp > 0)
            L->lsp--;
        block(L, L->lsp ? 0 : 12);
        break;
    case T_PRE:
        block(L, 12);
        if (L->pre_sp > 0) {
            int y0 = L->pre_y0[--L->pre_sp];
            struct html_box *b = new_box(L);
            if (b) {
                b->type = BOX_BG;
                b->x = cur(L)->indent - 10;
                b->y = y0 - 8;
                b->w = L->width - cur(L)->indent + 20;
                b->h = L->y - y0 + 16;
                b->color = C_PREBG;
            }
            L->margin = 20;
        }
        break;
    case T_BLOCKQUOTE:
        block(L, 12);
        if (L->bq_sp > 0) {
            int y0 = L->bq_y0[--L->bq_sp];
            struct html_box *b = new_box(L);
            if (b) {
                b->type = BOX_BAR;
                b->x = L->bq_ind[L->bq_sp];
                b->y = y0;
                b->w = 3;
                b->h = L->y - y0;
                b->color = C_BAR;
            }
        }
        break;
    case T_TABLE: case T_CAPTION:
        block(L, 8);
        if (tag == T_TABLE && L->tsp > 0)
            L->tsp--;
        break;
    case T_A: case T_SELECT:
        if (tag == T_SELECT)
            L->in_select = 0;
        break;
    case T_OPTION:
        L->in_option = 0;
        break;
    default:
        if (is_block(tag))
            block(L, 0);
        break;
    }
    (void)d;
}

/* Column positions for the table starting at p (just after <table>): the
 * widest one-line cell of each column. 0 if a cell holds a nested table or
 * the columns don't fit side by side; the cells then follow each other. */
static int table_scan(struct lay *L, const char *p, const char *end, int *colx, int *ncol)
{
    int colw[24] = { 0 }, depth = 1, cell = -1, span = 1, n = 0, th = 0, i, x;
    int linew = 0, cellw = 0, size = cur(L)->size, room = avail(L);
    const char *tstart = NULL;
    while (p < end && depth > 0) {
        if (*p == '<') {
            const char *q = p + 1, *nm;
            int close = 0, t;
            if (q < end && *q == '!') {                  /* comments etc. */
                const char *g = (p + 4 <= end && strncmp(p, "<!--", 4) == 0) ? strstr(p, "-->") : memchr(p, '>', (size_t)(end - p));
                p = g ? g + 1 : end;
                continue;
            }
            if (q < end && *q == '/') { close = 1; q++; }
            nm = q;
            while (q < end && isalnum((unsigned char)*q))
                q++;
            t = tag_id(nm, (int)(q - nm));
            if (tstart && cell >= 0) {                   /* text before this tag */
                int dn = 0;
                char *dec = decode(tstart, (int)(p - tstart), &dn), *c;
                if (dec) {
                    int sp = 0, k = 0;
                    for (c = dec; *c; c++) {             /* collapse spaces */
                        if (isspace((unsigned char)*c)) {
                            if (!sp && k) dec[k++] = ' ';
                            sp = 1;
                        } else {
                            dec[k++] = *c;
                            sp = 0;
                        }
                    }
                    dec[k] = '\0';
                    linew += font_width(dec, size, th ? FONT_BOLD : FONT_REGULAR);
                    free(dec);
                }
                tstart = NULL;
            }
            if (t == T_TABLE) {
                if (close) depth--;
                else if (++depth > 1)
                    return 0;                            /* nested: no columns */
            } else if (depth == 1 && !close && (t == T_TR || t == T_TD || t == T_TH)) {
                if (cell >= 0 && span == 1) {            /* the cell just ended */
                    if (linew > cellw) cellw = linew;
                    if (cell < 24 && cellw > colw[cell]) colw[cell] = cellw;
                }
                if (t == T_TR) {
                    cell = -1;
                    span = 1;
                } else {
                    char sv[16] = "";
                    const char *a = strstr(q, "colspan");
                    const char *g = memchr(q, '>', (size_t)(end - q));
                    cell += span;
                    span = 1;
                    if (a && g && a < g) {
                        a += 7;
                        while (*a == '=' || *a == '"' || *a == '\'' || *a == ' ')
                            a++;
                        snprintf(sv, sizeof(sv), "%.*s", 4, a);
                        span = atoi(sv) > 1 ? atoi(sv) : 1;
                    }
                    if (cell < 0) cell = 0;
                    if (cell + span > n) n = cell + span;
                    if (n > 24) return 0;
                    th = t == T_TH;
                }
                linew = cellw = 0;
            } else if (depth == 1 && t == T_BR) {
                if (linew > cellw) cellw = linew;
                linew = 0;
            } else if (depth == 1 && t == T_IMG && !close) {
                const char *a = strstr(q, "width"), *g = memchr(q, '>', (size_t)(end - q));
                if (a && g && a < g) {
                    a += 5;
                    while (*a == '=' || *a == '"' || *a == '\'' || *a == ' ')
                        a++;
                    linew += atoi(a) + 4;
                }
            } else if (depth == 1 && !close && (is_block(t) && t != T_TR)) {
                return 0;                                /* block content in a cell */
            }
            {
                const char *g = memchr(p, '>', (size_t)(end - p));
                p = g ? g + 1 : end;
            }
            if (t == T_SCRIPT || t == T_STYLE)
                return 0;
            continue;
        }
        if (!tstart)
            tstart = p;
        p++;
    }
    if (cell >= 0 && span == 1) {
        if (linew > cellw) cellw = linew;
        if (cell < 24 && cellw > colw[cell]) colw[cell] = cellw;
    }
    if (n < 2)
        return 0;
    for (i = 0, x = 0; i < n; i++) {
        colx[i] = x;
        x += colw[i] + (colw[i] ? 18 : 6);
    }
    *ncol = n;
    return x <= room;
}

static void add_anchor(struct lay *L, const char *name)
{
    struct html_doc *d = L->d;
    struct html_anchor *a;
    if (!name[0] || !(a = grow(d->anc, &d->capanc, d->nanc + 1, sizeof(*a))))
        return;
    d->anc = a;
    snprintf(d->anc[d->nanc].name, sizeof(d->anc[0].name), "%s", name);
    d->anc[d->nanc++].y = L->y;
}

static int find_image(struct html_doc *d, const char *url)
{
    struct html_image *im;
    int i;
    for (i = 0; i < d->nimg; i++)
        if (strcmp(d->img[i].url, url) == 0)
            return i;
    if (!(im = grow(d->img, &d->capimg, d->nimg + 1, sizeof(*im))))
        return -1;
    d->img = im;
    memset(&d->img[d->nimg], 0, sizeof(*im));
    d->img[d->nimg].url = strdup(url);
    return d->img[d->nimg].url ? d->nimg++ : -1;
}

static int new_input(struct lay *L, int type, const struct attr *a, int na)
{
    struct html_doc *d = L->d;
    struct html_input *in;
    int i = L->nin++;
    char v[512];
    if (i < d->nin && d->in[i].type == type)          /* laid out again: keep */
        return i;
    if (i >= d->nin) {
        if (!(in = grow(d->in, &d->capin, i + 1, sizeof(*in))))
            return -1;
        d->in = in;
        d->nin = i + 1;
    }
    in = &d->in[i];
    memset(in, 0, sizeof(*in));
    in->type = type;
    in->form = L->form;
    attr_get(a, na, "name", in->name, sizeof(in->name));
    if (attr_get(a, na, "value", v, sizeof(v)))
        snprintf(in->value, sizeof(in->value), "%s", v);
    attr_get(a, na, "placeholder", in->placeholder, sizeof(in->placeholder));
    in->checked = attr_get(a, na, "checked", NULL, 0);
    return i;
}

static void put_box(struct lay *L, int type, int w, int h, int ref)
{
    struct html_box *b;
    int sp = (L->space && L->pen > 0) ? 4 : 0;
    if (L->pen > 0 && L->pen + sp + w > avail(L)) {
        end_line(L, 0);
        sp = 0;
    }
    content(L);
    if (!(b = new_box(L)))
        return;
    b->type = (unsigned char)type;
    b->x = cur(L)->indent + L->pen + sp;
    b->w = w;
    b->h = h;
    b->ref = ref;
    b->link = cur(L)->link;
    L->pen += sp + w;
    L->space = 0;
}

static void start_tag(struct lay *L, int t, const struct attr *a, int na, int selfclose)
{
    struct html_doc *d = L->d;
    struct st *s;
    char v[2048];

    (void)selfclose;
    if (t == T_NONE) {                                /* still: ids are anchors */
        if (attr_get(a, na, "id", v, 64))
            add_anchor(L, v);
        return;
    }
    if (t == T_BASE) {
        if (attr_get(a, na, "href", v, sizeof(v))) {
            char abs[2048];
            html_resolve(d->base, v, abs, sizeof(abs));
            snprintf(d->base, sizeof(d->base), "%s", abs);
        }
        return;
    }
    if (t == T_HEAD || t == T_META || t == T_LINK || t == T_HTML || t == T_BODY ||
        t == T_COL || t == T_SOURCE || t == T_AREA || t == T_WBR)
        return;

    /* implied ends */
    if (is_block(t) && t != T_LI && t != T_DT && t != T_DD && open_has(L, T_P))
        pop_to(L, T_P);
    if (t == T_LI && open_has(L, T_LI)) {
        int i, stop = 0;
        for (i = L->sp; i > 0 && !stop; i--) {
            if (L->st[i].tag == T_UL || L->st[i].tag == T_OL || L->st[i].tag == T_MENU)
                stop = 1;
            else if (L->st[i].tag == T_LI) {
                pop_to(L, T_LI);
                stop = 1;
            }
        }
    }
    if ((t == T_DT || t == T_DD) && (open_has(L, T_DT) || open_has(L, T_DD))) {
        if (!pop_to(L, T_DD))
            pop_to(L, T_DT);
    }
    if ((t == T_TD || t == T_TH) && (open_has(L, T_TD) || open_has(L, T_TH))) {
        if (!pop_to(L, T_TD))
            pop_to(L, T_TH);
    }
    if (t == T_TR && open_has(L, T_TR))
        pop_to(L, T_TR);
    if (t == T_OPTION && L->in_option)
        pop_to(L, T_OPTION);

    if (attr_get(a, na, "id", v, 64))
        add_anchor(L, v);

    if (t == T_BR) {
        if (!cur(L)->hidden)
            end_line(L, 1);
        return;
    }
    if (t == T_HR) {
        struct html_box *b;
        if (cur(L)->hidden)
            return;
        block(L, 10);
        content(L);
        if ((b = new_box(L)) != NULL) {
            b->type = BOX_RULE;
            b->x = cur(L)->indent;
            b->y = L->y;
            b->w = avail(L);
            b->h = 1;
            b->color = C_RULE;
        }
        L->line_has = 0;
        L->line0 = d->nbox;
        L->y += 1;
        L->margin = 10;
        return;
    }
    if (t == T_IMG) {
        char src[2048], abs[2048], wv[16], hv[16];
        int w, h, i;
        if (cur(L)->hidden || !attr_get(a, na, "src", src, sizeof(src)) || !src[0] ||
            strncmp(src, "data:", 5) == 0)
            return;
        html_resolve(d->base, src, abs, sizeof(abs));
        i = find_image(d, abs);
        if (i < 0)
            return;
        w = attr_get(a, na, "width", wv, sizeof(wv)) ? atoi(wv) : 0;
        h = attr_get(a, na, "height", hv, sizeof(hv)) ? atoi(hv) : 0;
        if (strchr(wv, '%'))
            w = avail(L) * w / 100;
        if (strchr(hv, '%'))
            h = 0;
        if (d->img[i].nat_w > 0) {
            if (!w && !h) { w = d->img[i].nat_w; h = d->img[i].nat_h; }
            else if (!h)  h = w * d->img[i].nat_h / d->img[i].nat_w;
            else if (!w)  w = h * d->img[i].nat_w / d->img[i].nat_h;
        }
        if (d->img[i].state == IMG_FAILED && (w <= 0 || h <= 0)) {
            if (attr_get(a, na, "alt", v, 256) && v[0]) {   /* broken: its words */
                push(L, T_I);
                cur(L)->face |= FONT_ITALIC;
                cur(L)->color = C_DIM;
                text(L, v, (int)strlen(v));
                L->sp--;
            }
            return;
        }
        if (w <= 0 || h <= 0)
            w = h = 20;                               /* placeholder until loaded */
        if (w > avail(L)) {
            h = (int)((long)h * avail(L) / w);
            w = avail(L);
        }
        if (h > 4000)
            h = 4000;
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        put_box(L, BOX_IMG, w, h, i);
        return;
    }
    if (t == T_INPUT) {
        char ty[24];
        int type = IN_TEXT, i, w = 220, h = 30;
        if (cur(L)->hidden)
            return;
        attr_get(a, na, "type", ty, sizeof(ty));
        if (!strcasecmp(ty, "hidden")) type = IN_HIDDEN;
        else if (!strcasecmp(ty, "password")) type = IN_PASSWORD;
        else if (!strcasecmp(ty, "submit") || !strcasecmp(ty, "image")) type = IN_SUBMIT;
        else if (!strcasecmp(ty, "button") || !strcasecmp(ty, "reset")) type = IN_BUTTON;
        else if (!strcasecmp(ty, "checkbox")) type = IN_CHECK;
        else if (!strcasecmp(ty, "radio")) type = IN_RADIO;
        i = new_input(L, type, a, na);
        if (i < 0 || type == IN_HIDDEN)
            return;
        if (type == IN_SUBMIT || type == IN_BUTTON) {
            const char *lab = d->in[i].value[0] ? d->in[i].value : (type == IN_SUBMIT ? "Submit" : "Button");
            if (!d->in[i].value[0])
                snprintf(d->in[i].value, sizeof(d->in[i].value), "%s", lab);
            w = font_width(lab, 14, FONT_REGULAR) + 28;
        } else if (type == IN_CHECK || type == IN_RADIO) {
            w = h = 18;
        } else if (attr_get(a, na, "size", v, 16) && atoi(v) > 0) {
            w = atoi(v) * 8 + 16;
            if (w > avail(L)) w = avail(L);
        }
        put_box(L, BOX_INPUT, w, h, i);
        return;
    }

    if (is_block(t))
        end_line(L, 0);                 /* the line before, in its style */
    push(L, t);
    s = cur(L);
    if (attr_get(a, na, "hidden", NULL, 0))
        s->hidden = 1;
    /* what style sheets would hide: screen-reader text, utility classes,
     * decorations, closed dropdowns and dialogs */
    if (attr_get(a, na, "aria-hidden", v, 16) && !strcasecmp(v, "true"))
        s->hidden = 1;
    if (attr_get(a, na, "class", v, sizeof(v))) {
        static const char *const hide[] = { "sr-only", "visually-hidden", "screen-reader-text",
            "d-none", "hidden", "hide", "invisible", "is-hidden", "u-hide", "show-on-focus",
            "noprint", "mw-jump-link", "skip-link", "vector-menu-content", "js-hidden", NULL };
        char *tok, *save = NULL;
        for (tok = strtok_r(v, " \t\n", &save); tok && !s->hidden; tok = strtok_r(NULL, " \t\n", &save)) {
            int k;
            for (k = 0; hide[k]; k++)
                if (strcasecmp(tok, hide[k]) == 0)
                    s->hidden = 1;
        }
    }
    if ((t == T_DETAILS || t == T_DIALOG) && !attr_get(a, na, "open", NULL, 0))
        s->hidden = 2;                                /* closed: its summary only */
    if (t == T_SUMMARY && L->sp > 0 && L->st[L->sp - 1].hidden == 2 &&
        (L->sp < 2 || !L->st[L->sp - 2].hidden))
        s->hidden = 0;
    if (attr_get(a, na, "style", v, sizeof(v))) {
        char *p;
        for (p = v; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        if (strstr(v, "display:none") || strstr(v, "display: none") ||
            strstr(v, "visibility:hidden") || strstr(v, "visibility: hidden"))
            s->hidden = 1;
        if ((p = strstr(v, "color:")) != NULL && (p == v || p[-1] == ';' || p[-1] == ' ')) {
            long c = parse_color(p + 6);
            if (c >= 0 && t != T_A)
                s->color = (uint32_t)c;
        }
        if (strstr(v, "text-align:center") || strstr(v, "text-align: center"))
            s->center = 1;
    }
    if (attr_get(a, na, "align", v, 16) && !strcasecmp(v, "center") && is_block(t))
        s->center = 1;

    switch (t) {
    case T_TITLE:
        break;
    case T_SCRIPT: case T_STYLE: case T_TEMPLATE: case T_SVG: case T_IFRAME:
    case T_OBJECT: case T_MATH: case T_AUDIO: case T_VIDEO: case T_CANVAS:
        s->hidden = 1;
        break;
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6:
        block(L, t <= T_H2 ? 20 : 16);
        s->size = (unsigned char)heading_px(t);
        s->face = (unsigned char)((s->face & FONT_ITALIC) | FONT_BOLD);
        break;
    case T_P: case T_FIGURE:
        block(L, 12);
        break;
    case T_PRE:
        block(L, 12);
        s->pre = 1;
        s->mono = 1;
        if (L->pre_sp < 8)
            L->pre_y0[L->pre_sp++] = L->y + (L->started ? L->margin : 0);
        break;
    case T_BLOCKQUOTE:
        block(L, 12);
        if (L->bq_sp < 8) {
            L->bq_y0[L->bq_sp] = L->y + (L->started ? L->margin : 0);
            L->bq_ind[L->bq_sp++] = s->indent + 4;
        }
        s->indent += 28;
        s->color = 0x3d444d;
        break;
    case T_UL: case T_OL: case T_MENU:
        block(L, L->lsp ? 0 : 12);
        s->indent += 28;
        if (L->lsp < 16) {
            L->list_kind[L->lsp] = t == T_OL;
            L->list_n[L->lsp] = 0;
            if (t == T_OL && attr_get(a, na, "start", v, 16))
                L->list_n[L->lsp] = atoi(v) - 1;
            L->lsp++;
        }
        break;
    case T_LI:
        block(L, 3);
        if (L->lsp == 0)
            s->indent += 28;
        L->bullet = 1;
        break;
    case T_DT:
        block(L, 6);
        s->face = (unsigned char)((s->face & FONT_ITALIC) | FONT_BOLD);
        break;
    case T_DD:
        block(L, 0);
        s->indent += 32;
        break;
    case T_TABLE:
        block(L, 8);
        if (L->tsp < 4) {
            int k = L->tsp++;
            memset(&L->tbl[k], 0, sizeof(L->tbl[k]));
            if (!s->hidden && L->pos)
                L->tbl[k].on = table_scan(L, L->pos, L->end, L->tbl[k].colx, &L->tbl[k].ncol);
        }
        break;
    case T_TR:
        block(L, 2);
        L->cell = 0;
        if (L->tsp > 0)
            L->tbl[L->tsp - 1].cell = 0;
        break;
    case T_TD: case T_TH: {
        int k = L->tsp - 1;
        if (k >= 0 && L->tbl[k].on) {                 /* the column's place */
            int col = L->tbl[k].cell, sp = 1;
            if (attr_get(a, na, "colspan", v, 8) && atoi(v) > 1)
                sp = atoi(v);
            if (col < L->tbl[k].ncol) {
                int want = L->tbl[k].colx[col];
                L->pen = L->pen > want ? L->pen + 12 : want;
                if (L->pen > 0 && !L->line_has)
                    content(L);                         /* the row has started */
            }
            L->tbl[k].cell += sp;
        } else if (L->cell > 0 && L->line_has) {
            L->pen += 18;                             /* a gap between cells */
        }
        L->cell++;
        L->space = 0;
        if (t == T_TH)
            s->face = (unsigned char)((s->face & FONT_ITALIC) | FONT_BOLD);
        break;
    }
    case T_CAPTION:
        block(L, 6);
        s->center = 1;
        s->face = (unsigned char)((s->face & FONT_ITALIC) | FONT_BOLD);
        break;
    case T_CENTER:
        block(L, 0);
        s->center = 1;
        break;
    case T_A:
        if (attr_get(a, na, "name", v, 64))
            add_anchor(L, v);
        if (attr_get(a, na, "href", v, sizeof(v)) && v[0] &&
            strncasecmp(v, "javascript:", 11) != 0) {
            char abs[2048];
            char **lk = grow(d->link, &d->caplink, d->nlink + 1, sizeof(char *));
            html_resolve(d->base, v, abs, sizeof(abs));
            if (lk) {
                d->link = lk;
                d->link[d->nlink] = strdup(abs);
                if (d->link[d->nlink]) {
                    s->link = d->nlink++;
                    s->color = C_LINK;
                    s->flags |= BF_UNDER;
                }
            }
        }
        break;
    case T_B:
        s->face = (unsigned char)((s->face & FONT_ITALIC) | FONT_BOLD);
        break;
    case T_I:
        s->face |= FONT_ITALIC;
        break;
    case T_U:
        s->flags |= BF_UNDER;
        break;
    case T_S:
        s->flags |= BF_STRIKE;
        break;
    case T_CODE: case T_KBD:
        s->mono = 1;
        break;
    case T_SMALL:
        if (s->size > 11) s->size = (unsigned char)(s->size - 2);
        break;
    case T_BIG:
        s->size = (unsigned char)(s->size + 3);
        break;
    case T_SUB: case T_SUP:
        if (s->size > 11) s->size = (unsigned char)(s->size - 3);
        break;
    case T_MARK:
        s->flags |= BF_MARK;
        break;
    case T_FONT:
        if (attr_get(a, na, "color", v, 32)) {
            long c = parse_color(v);
            if (c >= 0)
                s->color = (uint32_t)c;
        }
        break;
    case T_Q:
        text(L, "\xe2\x80\x9c", 3);
        break;
    case T_FORM: {
        struct html_form *f = grow(d->form, &d->capform, d->nform + 1, sizeof(*f));
        block(L, 0);
        if (f) {
            char act[2048];
            d->form = f;
            f = &d->form[d->nform];
            memset(f, 0, sizeof(*f));
            attr_get(a, na, "action", act, sizeof(act));
            html_resolve(d->base, act[0] ? act : "", f->action, sizeof(f->action));
            if (attr_get(a, na, "method", v, 16) && !strcasecmp(v, "post"))
                f->post = 1;
            L->form = d->nform++;
        }
        break;
    }
    case T_BUTTON: {
        char ty[16];
        int i;
        attr_get(a, na, "type", ty, sizeof(ty));
        i = new_input(L, strcasecmp(ty, "button") == 0 || strcasecmp(ty, "reset") == 0
                         ? IN_BUTTON : IN_SUBMIT, a, na);
        s->hidden = 1;                                /* its label: below */
        if (i >= 0) {
            /* the label is the button's text, collected as it comes */
            d->in[i].placeholder[0] = '\0';
            L->sel_input = i;
        }
        break;
    }
    case T_TEXTAREA: {
        int i = new_input(L, IN_AREA, a, na), cols = 40, rows = 3;
        if (attr_get(a, na, "cols", v, 16) && atoi(v) > 0) cols = atoi(v);
        if (attr_get(a, na, "rows", v, 16) && atoi(v) > 0) rows = atoi(v);
        if (i >= 0 && !s->hidden && !L->st[L->sp - 1].hidden) {
            int w = cols * 8 + 16;
            if (w > avail(L)) w = avail(L);
            put_box(L, BOX_INPUT, w, rows * 18 + 12, i);
        }
        s->hidden = 1;
        break;
    }
    case T_SELECT: {
        int i = new_input(L, IN_SELECT, a, na);
        if (i >= 0 && !s->hidden)
            put_box(L, BOX_INPUT, 180, 30, i);
        L->in_select = 1;
        L->sel_input = i;
        break;
    }
    case T_OPTION:
        L->in_option = 1;
        if (L->in_select && L->sel_input >= 0 && L->sel_input < d->nin) {
            struct html_input *in = &d->in[L->sel_input];
            int sel = attr_get(a, na, "selected", NULL, 0);
            if ((!in->value[0] || sel) && attr_get(a, na, "value", v, sizeof(in->value)))
                snprintf(in->value, sizeof(in->value), "%s", v);
            if (sel)
                in->placeholder[0] = '\0';            /* this one's text */
        }
        break;
    case T_SUMMARY:
        block(L, 4);
        s->face = (unsigned char)((s->face & FONT_ITALIC) | FONT_BOLD);
        if (!s->hidden)
            text(L, "\xe2\x80\xba ", 4);           /* › */
        break;
    case T_LEGEND: case T_FIGCAPTION:
        block(L, 4);
        s->color = C_DIM;
        if (s->size > 13) s->size = 13;
        break;
    default:
        if (is_block(t))
            block(L, 0);
        break;
    }
}

static void end_tag(struct lay *L, int t)
{
    if (t == T_NONE || is_void(t) || t == T_HEAD || t == T_HTML || t == T_BODY)
        return;
    if (t == T_Q && open_has(L, T_Q))
        text(L, "\xe2\x80\x9d", 3);
    if (t == T_BUTTON && open_has(L, T_BUTTON)) {
        /* the button's box, now that its label is known */
        int i = L->sel_input;
        pop_to(L, T_BUTTON);
        if (i >= 0 && i < L->d->nin && !cur(L)->hidden) {
            struct html_input *in = &L->d->in[i];
            const char *lab = in->placeholder[0] ? in->placeholder : NULL;
            if (!in->value[0] && lab)
                snprintf(in->value, sizeof(in->value), "%s", lab);
            if (lab)                                  /* icon-only buttons: skipped */
                put_box(L, BOX_INPUT, font_width(lab, 14, FONT_REGULAR) + 28, 30, i);
        }
        L->sel_input = -1;
        return;
    }
    if (t == T_TEXTAREA || t == T_SELECT || t == T_FORM) {
        if (t == T_FORM)
            L->form = -1;
    }
    pop_to(L, t);
}

/* text of a <button> or <textarea>: into its input */
static void hidden_text(struct lay *L, const char *t, int n)
{
    int i = L->sel_input;
    if (open_has(L, T_BUTTON) && i >= 0 && i < L->d->nin) {
        struct html_input *in = &L->d->in[i];
        size_t l = strlen(in->placeholder);
        int k;
        for (k = 0; k < n && l + 2 < sizeof(in->placeholder); k++) {
            char c = isspace((unsigned char)t[k]) ? ' ' : t[k];
            if (c == ' ' && (l == 0 || in->placeholder[l - 1] == ' '))
                continue;
            in->placeholder[l++] = c;
        }
        in->placeholder[l] = '\0';
    }
}

void html_free(struct html_doc *d)
{
    int i;
    for (i = 0; i < d->nlink; i++)
        free(d->link[i]);
    for (i = 0; i < d->nimg; i++) {
        free(d->img[i].url);
        free(d->img[i].px);
        free(d->img[i].scaled);
    }
    free(d->box);
    free(d->text);
    free(d->link);
    free(d->img);
    free(d->in);
    free(d->form);
    free(d->anc);
    memset(d, 0, sizeof(*d));
}

void html_layout(struct html_doc *d, const char *src, size_t n, const char *base_url,
                 int width)
{
    static struct lay L0;
    struct lay *L = &L0;
    const char *p = src, *end = src + n;
    int i;

    for (i = 0; i < d->nlink; i++)
        free(d->link[i]);
    d->nlink = d->nbox = d->ntext = d->nform = d->nanc = 0;
    d->title[0] = '\0';
    snprintf(d->base, sizeof(d->base), "%s", base_url ? base_url : "");
    d->width = width;
    d->relayout = 0;

    memset(L, 0, sizeof(*L));
    L->d = d;
    L->width = width;
    L->form = -1;
    L->sel_input = -1;
    L->st[0].size = BODY_PX;
    L->st[0].face = FONT_REGULAR;
    L->st[0].color = C_TEXT;
    L->st[0].link = -1;

    while (p < end) {
        if (*p == '<' && p + 1 < end) {
            if (p + 4 <= end && strncmp(p, "<!--", 4) == 0) {
                const char *q = strstr(p + 4, "-->");
                p = q ? q + 3 : end;
                continue;
            }
            if (p[1] == '!' || p[1] == '?') {
                const char *q = memchr(p, '>', (size_t)(end - p));
                p = q ? q + 1 : end;
                continue;
            }
            if (p[1] == '/' && p + 2 < end && isalpha((unsigned char)p[2])) {
                const char *q = p + 2, *nm = q;
                while (q < end && (isalnum((unsigned char)*q) || *q == '-' || *q == ':'))
                    q++;
                end_tag(L, tag_id(nm, (int)(q - nm)));
                q = memchr(q, '>', (size_t)(end - q));
                p = q ? q + 1 : end;
                continue;
            }
            if (isalpha((unsigned char)p[1])) {
                struct attr a[32];
                int na = 0, t, selfclose = 0;
                const char *q = p + 1, *nm = q;
                while (q < end && (isalnum((unsigned char)*q) || *q == '-' || *q == ':'))
                    q++;
                t = tag_id(nm, (int)(q - nm));
                while (q < end && *q != '>') {        /* attributes */
                    const char *k, *v = NULL;
                    int kn, vn = 0;
                    while (q < end && (isspace((unsigned char)*q) || *q == '/')) {
                        if (*q == '/' && q + 1 < end && q[1] == '>')
                            selfclose = 1;
                        q++;
                    }
                    if (q >= end || *q == '>')
                        break;
                    k = q;
                    while (q < end && !isspace((unsigned char)*q) && *q != '=' && *q != '>' &&
                           !(*q == '/' && q + 1 < end && q[1] == '>'))
                        q++;
                    kn = (int)(q - k);
                    while (q < end && isspace((unsigned char)*q))
                        q++;
                    if (q < end && *q == '=') {
                        q++;
                        while (q < end && isspace((unsigned char)*q))
                            q++;
                        if (q < end && (*q == '"' || *q == '\'')) {
                            char qc = *q++;
                            v = q;
                            while (q < end && *q != qc)
                                q++;
                            vn = (int)(q - v);
                            if (q < end)
                                q++;
                        } else {
                            v = q;
                            while (q < end && !isspace((unsigned char)*q) && *q != '>')
                                q++;
                            vn = (int)(q - v);
                        }
                    }
                    if (kn > 0 && na < 32) {
                        a[na].k = k;
                        a[na].kn = kn;
                        a[na].v = v ? v : "";
                        a[na].vn = vn;
                        na++;
                    }
                }
                p = q < end ? q + 1 : end;
                L->pos = p;
                L->end = end;
                if (t == T_SCRIPT || t == T_STYLE || t == T_TITLE || t == T_TEXTAREA) {
                    /* raw text up to the matching end tag */
                    const char *e = p, *close = NULL;
                    size_t tl;
                    {
                        const char *name = t == T_SCRIPT ? "script" : t == T_STYLE ? "style"
                                         : t == T_TITLE ? "title" : "textarea";
                        tl = strlen(name);
                        while (e + 2 + tl <= end) {
                            if (e[0] == '<' && e[1] == '/' && strncasecmp(e + 2, name, tl) == 0) {
                                close = e;
                                break;
                            }
                            e++;
                        }
                    }
                    if (!close)
                        close = end;
                    if (t == T_TITLE && !d->title[0]) {
                        int dn = 0, k = 0, o = 0;
                        char *dec = decode(p, (int)(close - p), &dn);
                        if (dec) {
                            for (k = 0; k < dn && o < (int)sizeof(d->title) - 1; k++) {
                                char c = isspace((unsigned char)dec[k]) ? ' ' : dec[k];
                                if (c == ' ' && (o == 0 || d->title[o - 1] == ' '))
                                    continue;
                                d->title[o++] = c;
                            }
                            while (o > 0 && d->title[o - 1] == ' ')
                                o--;
                            d->title[o] = '\0';
                            free(dec);
                        }
                    } else if (t == T_TEXTAREA) {
                        start_tag(L, t, a, na, 0);
                        if (L->nin > 0) {
                            struct html_input *in = &d->in[L->nin - 1];
                            if (in->type == IN_AREA && !in->value[0]) {
                                int dn = 0;
                                char *dec = decode(p, (int)(close - p), &dn);
                                if (dec) {
                                    snprintf(in->value, sizeof(in->value), "%s", dec);
                                    free(dec);
                                }
                            }
                        }
                        pop_to(L, T_TEXTAREA);
                    }
                    p = close;
                    if (p < end) {
                        const char *g = memchr(p, '>', (size_t)(end - p));
                        p = g ? g + 1 : end;
                    }
                    continue;
                }
                start_tag(L, t, a, na, selfclose);
                if (selfclose && !is_void(t) && t != T_NONE && L->st[L->sp].tag == t)
                    end_tag(L, t);
                continue;
            }
        }
        {                                             /* text up to the next tag */
            const char *q = p + 1;
            int dn = 0;
            char *dec;
            while (q < end && *q != '<')
                q++;
            dec = decode(p, (int)(q - p), &dn);
            if (dec) {
                if (cur(L)->hidden)
                    hidden_text(L, dec, dn);
                else
                    text(L, dec, dn);
                free(dec);
            }
            p = q;
        }
    }
    end_line(L, 0);
    d->height = L->y + 24;
}

/* ---- drawing -------------------------------------------------------------------- */
static inline uint32_t blend(uint32_t dst, uint32_t src, unsigned a)
{
    uint32_t rb, g;
    if (a >= 255) return src | 0xff000000u;
    rb = ((src & 0xff00ff) * a + (dst & 0xff00ff) * (255 - a) + 0x800080) >> 8;
    g  = ((src & 0x00ff00) * a + (dst & 0x00ff00) * (255 - a) + 0x008000) >> 8;
    return 0xff000000u | (rb & 0xff00ff) | (g & 0x00ff00);
}

static void fill(struct raster *r, int x, int y, int w, int h, uint32_t c, int clip_y0, int clip_y1)
{
    int i, j;
    if (y < clip_y0) { h -= clip_y0 - y; y = clip_y0; }
    if (y + h > clip_y1) h = clip_y1 - y;
    if (x < 0) { w += x; x = 0; }
    if (x + w > r->w) w = r->w - x;
    for (j = 0; j < h; j++) {
        uint32_t *row = r->bits + (size_t)(y + j) * (size_t)r->stride + (size_t)x;
        for (i = 0; i < w; i++)
            row[i] = 0xff000000u | c;
    }
}

static void frame(struct raster *r, int x, int y, int w, int h, uint32_t c, int cy0, int cy1)
{
    fill(r, x, y, w, 1, c, cy0, cy1);
    fill(r, x, y + h - 1, w, 1, c, cy0, cy1);
    fill(r, x, y, 1, h, c, cy0, cy1);
    fill(r, x + w - 1, y, 1, h, c, cy0, cy1);
}

static void mono_text(struct raster *r, const char *s, int n, int x, int y, uint32_t c,
                      int cy0, int cy1)
{
    const char *p = s, *e = s + n;
    while (p < e && *p) {
        uint32_t cp = omni_utf8_next(&p);
        const unsigned char *g = omni_glyph8(cp < 128 ? cp : '?');
        int gx, gy;
        for (gy = 0; gy < 16; gy++) {
            int yy = y + gy;
            unsigned bits = g[gy / 2];
            if (yy < cy0 || yy >= cy1)
                continue;
            for (gx = 0; gx < 8; gx++)
                if ((bits >> gx) & 1) {
                    int xx = x + gx;
                    if (xx >= 0 && xx < r->w)
                        r->bits[(size_t)yy * (size_t)r->stride + (size_t)xx] = 0xff000000u | c;
                }
        }
        x += MONO_W;
    }
}

/* image i at w x h: cached, box-filtered when shrinking */
static const uint32_t *scaled(struct html_image *im, int w, int h)
{
    int x, y;
    if (!im->px || w <= 0 || h <= 0)
        return NULL;
    if (im->scaled && im->sw == w && im->sh == h)
        return im->scaled;
    free(im->scaled);
    im->scaled = malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!im->scaled)
        return NULL;
    im->sw = w;
    im->sh = h;
    for (y = 0; y < h; y++) {
        int sy0 = (int)((long)y * im->nat_h / h), sy1 = (int)((long)(y + 1) * im->nat_h / h);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > im->nat_h) sy1 = im->nat_h;
        for (x = 0; x < w; x++) {
            int sx0 = (int)((long)x * im->nat_w / w), sx1 = (int)((long)(x + 1) * im->nat_w / w);
            unsigned r = 0, g = 0, b = 0, cnt = 0;
            int i, j;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > im->nat_w) sx1 = im->nat_w;
            for (j = sy0; j < sy1; j += 1 + (sy1 - sy0) / 4)
                for (i = sx0; i < sx1; i += 1 + (sx1 - sx0) / 4) {
                    uint32_t c = im->px[(size_t)j * (size_t)im->nat_w + (size_t)i];
                    r += (c >> 16) & 255;
                    g += (c >> 8) & 255;
                    b += c & 255;
                    cnt++;
                }
            if (!cnt) cnt = 1;
            im->scaled[(size_t)y * w + x] = 0xff000000u | (r / cnt) << 16 | (g / cnt) << 8 | (b / cnt);
        }
    }
    return im->scaled;
}

static void draw_input(const struct html_doc *d, const struct html_box *b, struct raster *r,
                       int x, int y, int focused, int cy0, int cy1)
{
    const struct html_input *in = &d->in[b->ref];
    struct raster clip = *r;
    int base = y + b->h / 2 + 5;
    if (in->type == IN_SUBMIT || in->type == IN_BUTTON) {
        const char *lab = in->placeholder[0] ? in->placeholder : in->value;
        fill(r, x, y, b->w, b->h, 0xf3f4f6, cy0, cy1);
        frame(r, x, y, b->w, b->h, 0xc4c9d0, cy0, cy1);
        if (base > cy0 && base < cy1)
            font_draw_base(r, lab, x + (b->w - font_width(lab, 14, FONT_REGULAR)) / 2, base, 14,
                           FONT_REGULAR, C_TEXT);
        return;
    }
    if (in->type == IN_CHECK || in->type == IN_RADIO) {
        fill(r, x, y, b->w, b->h, 0xffffff, cy0, cy1);
        frame(r, x, y, b->w, b->h, 0x6e7781, cy0, cy1);
        if (in->checked)
            fill(r, x + 4, y + 4, b->w - 8, b->h - 8, C_LINK, cy0, cy1);
        return;
    }
    fill(r, x, y, b->w, b->h, 0xffffff, cy0, cy1);
    frame(r, x, y, b->w, b->h, focused ? C_LINK : 0x8c959f, cy0, cy1);
    if (focused)
        frame(r, x + 1, y + 1, b->w - 2, b->h - 2, C_LINK, cy0, cy1);
    clip.w = x + b->w - 8 < r->w ? x + b->w - 8 : r->w;   /* keep text inside */
    if (in->type == IN_SELECT) {
        const char *lab = in->placeholder[0] ? in->placeholder : in->value;
        if (base > cy0 && base < cy1) {
            font_draw_base(&clip, lab, x + 8, base, 14, FONT_REGULAR, C_TEXT);
            font_draw_base(r, "\xe2\x96\xbe", x + b->w - 18, base, 14, FONT_REGULAR, C_DIM);
        }
        return;
    }
    if (base > cy0 && base < cy1) {
        char shown[512];
        const char *v = in->value;
        if (in->type == IN_PASSWORD) {
            size_t k, l = strlen(v);
            for (k = 0; k < l && k < sizeof(shown) - 1; k++)
                shown[k] = '*';
            shown[k] = '\0';
            v = shown;
        }
        if (!v[0] && !focused)
            font_draw_base(&clip, in->placeholder, x + 8, base, 14, FONT_REGULAR, 0x8c959f);
        else {
            int tw = font_width(v, 14, FONT_REGULAR), off = 0;
            if (tw > b->w - 20)
                off = tw - (b->w - 20);                   /* show the end */
            font_draw_base(&clip, v, x + 8 - off, base, 14, FONT_REGULAR, C_TEXT);
            if (focused)
                fill(r, x + 8 - off + tw + 1, y + 7, 1, b->h - 14, C_TEXT, cy0, cy1);
        }
    }
}

void html_draw(const struct html_doc *d, struct raster *r, int ox, int oy, int scroll,
               int view_h, int focus, int hover)
{
    int pass, i, cy0 = oy, cy1 = oy + view_h;
    if (cy1 > r->h)
        cy1 = r->h;
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < d->nbox; i++) {
            const struct html_box *b = &d->box[i];
            int x = ox + b->x, y = oy + b->y - scroll;
            if ((pass == 0) != (b->type == BOX_BG || b->type == BOX_BAR))
                continue;
            if (y + b->h < cy0 || y > cy1)
                continue;
            switch (b->type) {
            case BOX_BG: case BOX_BAR: case BOX_RULE:
                fill(r, x, y, b->w, b->h, b->color, cy0, cy1);
                break;
            case BOX_TEXT: case BOX_MONO: {
                uint32_t c = b->color;
                int base = y + b->base;
                struct raster clip = *r;
                if (b->flags & BF_MARK)
                    fill(r, x, y, b->w, b->h, 0xfff3a3, cy0, cy1);
                if (b->link >= 0 && b->link == hover)
                    c = 0x0842a0;
                if (b->type == BOX_MONO) {
                    if (b->flags & BF_CODE)
                        fill(r, x - 2, y, b->w + 4, b->h, 0xeff1f3, cy0, cy1);
                    mono_text(r, d->text + b->text, b->len, x, y, c, cy0, cy1);
                } else {
                    /* glyphs stay inside the page area: a raster cut to it */
                    clip.bits = r->bits + (size_t)cy0 * (size_t)r->stride;
                    clip.h = cy1 - cy0;
                    font_draw_n(&clip, d->text + b->text, b->len, x, base - cy0, b->size,
                                b->face, c);
                }
                if ((b->flags & BF_UNDER) && base + 2 >= cy0 && base + 2 < cy1)
                    fill(r, x, base + 2, b->w, 1,
                         b->link == hover ? c : blend(0xffffff, c, 110) & 0xffffff, cy0, cy1);
                if ((b->flags & BF_STRIKE) && base - b->size / 3 >= cy0 && base - b->size / 3 < cy1)
                    fill(r, x, base - b->size / 3, b->w, 1, c, cy0, cy1);
                break;
            }
            case BOX_IMG: {
                struct html_image *im = &((struct html_doc *)d)->img[b->ref];
                const uint32_t *px = im->state == IMG_OK ? scaled(im, b->w, b->h) : NULL;
                if (px) {
                    int j;
                    for (j = 0; j < b->h; j++) {
                        int yy = y + j, x0 = x < 0 ? -x : 0, x1 = b->w;
                        if (yy < cy0 || yy >= cy1)
                            continue;
                        if (x + x1 > r->w)
                            x1 = r->w - x;
                        if (x1 > x0)
                            memcpy(r->bits + (size_t)yy * (size_t)r->stride + (size_t)(x + x0),
                                   px + (size_t)j * (size_t)b->w + x0,
                                   (size_t)(x1 - x0) * sizeof(uint32_t));
                    }
                } else {
                    fill(r, x, y, b->w, b->h, 0xf0f2f4, cy0, cy1);
                    frame(r, x, y, b->w, b->h, 0xd8dee4, cy0, cy1);
                }
                if (b->link >= 0 && b->link == hover)
                    frame(r, x - 1, y - 1, b->w + 2, b->h + 2, C_LINK, cy0, cy1);
                break;
            }
            case BOX_INPUT:
                draw_input(d, b, r, x, y, b->ref == focus, cy0, cy1);
                break;
            }
        }
    }
}

int html_hit(const struct html_doc *d, int x, int y)
{
    int i;
    for (i = d->nbox - 1; i >= 0; i--) {
        const struct html_box *b = &d->box[i];
        if (b->type == BOX_BG || b->type == BOX_BAR || b->type == BOX_RULE)
            continue;
        if (x >= b->x - 2 && x < b->x + b->w + 2 && y >= b->y - 2 && y < b->y + b->h + 2)
            return i;
    }
    return -1;
}

void html_image_set(struct html_doc *d, int i, uint32_t *px, int w, int h)
{
    struct html_image *im;
    int k;
    if (i < 0 || i >= d->nimg) {
        free(px);
        return;
    }
    im = &d->img[i];
    free(im->px);
    free(im->scaled);
    im->scaled = NULL;
    im->px = px;
    im->state = px ? IMG_OK : IMG_FAILED;
    if (!px) {
        d->relayout = 1;
        return;
    }
    /* a box laid out with the placeholder size needs a new layout */
    for (k = 0; k < d->nbox; k++)
        if (d->box[k].type == BOX_IMG && d->box[k].ref == i &&
            (im->nat_w != w || im->nat_h != h) &&
            ((d->box[k].w == 20 && d->box[k].h == 20) || im->nat_w == 0))
            d->relayout = 1;
    im->nat_w = w;
    im->nat_h = h;
}

int html_anchor_y(const struct html_doc *d, const char *name)
{
    int i;
    for (i = 0; i < d->nanc; i++)
        if (strcmp(d->anc[i].name, name) == 0)
            return d->anc[i].y;
    return -1;
}
