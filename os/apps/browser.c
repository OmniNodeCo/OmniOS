/*
 * OmniOS — os/apps/browser.c
 *
 * Browser: OmniOS's web browser, written from scratch. Tabs, an address
 * bar that also searches (DuckDuckGo), back/forward/reload/home, links,
 * forms (GET and POST), images (PNG, JPEG, GIF, BMP), favorites and
 * downloads (to ~/Downloads, checked by OmniOS Defender). Pages are
 * fetched with BusyBox wget (HTTP and HTTPS) and drawn by html.c; there is
 * no JavaScript or CSS, so sites look like their plain versions.
 *
 *   omnios-browser [URL]            open a window
 *   omnios-browser --dump URL       print the page's title and text, no
 *                                   window (tests, the serial console)
 *
 * Keys: Ctrl+L address bar, Ctrl+T new tab, Ctrl+W close tab, Ctrl+R or
 * F5 reload, Ctrl+D add to favorites, Backspace back, arrows/PgUp/PgDn/
 * Space/Home/End scroll. Middle click opens a link in a new tab.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "html.h"
#include "../gui/client.h"
#include "../gui/font.h"
#include "../gui/theme.h"
#include "../gui/wm.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_TGA
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "../vendor/stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define UA        "Mozilla/5.0 (X11; OmniOS x86_64) OmniOS-Browser/1.0 (like Gecko)"
#define SEARCH    "https://html.duckduckgo.com/html/?q="
#define MAX_TABS  8
#define MAX_HIST  32
#define MAX_IMGS  80
#define TAB_H     40
#define BAR_H     48
#define TOP       (TAB_H + BAR_H)
#define PAGE_MAX  980                /* widest text column */
#define C_STRIP   0xe3e7ec
#define C_ACCENT  0x2563eb
#define C_INK     0x1f2328
#define C_DIM     0x5f6b7a

enum { F_NONE, F_PAGE, F_IMAGE };

struct fetch {
    pid_t pid;                      /* > 0: wget running                   */
    int   kind;
    int   img;                      /* image index (F_IMAGE)               */
    char  url[2048];
    char  out[64], err[64];
    long  t0;
};

struct tab {
    char   url[2048];               /* the page shown (or being loaded)    */
    char   title[256];
    char  *src;                     /* its HTML (built for text/images)    */
    size_t srclen;
    struct html_doc doc;
    int    scroll, focus, laid_w;
    char  *hist[MAX_HIST];
    int    hn, hpos;
    struct fetch page, img;
    int    secure;
    char   frag[128];
    char   notice[300];
    uint32_t notice_bg;
};

static struct omni_client_conn g_c;
static struct tab g_tab[MAX_TABS];
static int g_ntab, g_cur;
static int W, H;                    /* client area                         */
static uint32_t *g_fb;
static struct raster g_r;
static char g_addr[2048];
static int g_addr_focus, g_addr_sel;
static int g_hover_link = -1, g_hover_btn = -1, g_px = -1, g_py = -1;
static int g_dirty = 1;
static char g_home[512];

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct tab *T(void) { return &g_tab[g_cur]; }

/* ---- files ------------------------------------------------------------------ */
static char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    size_t cap = 0, len = 0, r;
    if (!f)
        return NULL;
    for (;;) {
        if (len + 65536 + 1 > cap) {
            char *nb = realloc(buf, cap = cap ? cap * 2 : 131072);
            if (!nb) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
        }
        r = fread(buf + len, 1, cap - len - 1, f);
        if (r == 0)
            break;
        len += r;
        if (len > (size_t)24 << 20)                  /* 24 MB is plenty */
            break;
    }
    fclose(f);
    buf[len] = '\0';
    *n = len;
    return buf;
}

static void mkdir_p(const char *dir)
{
    char tmp[512], *p;
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    mkdir(tmp, 0755);
}

/* ---- fetching ------------------------------------------------------------------ */
static void fetch_stop(struct fetch *f)
{
    if (f->pid > 0) {
        kill(f->pid, SIGTERM);
        waitpid(f->pid, NULL, 0);
    }
    if (f->out[0])
        unlink(f->out);
    if (f->err[0])
        unlink(f->err);
    f->pid = 0;
    f->kind = F_NONE;
}

static int fetch_start(struct fetch *f, int kind, const char *url, const char *post)
{
    static unsigned seq;
    const char *w = getenv("OMNI_WGET");
    pid_t pid;
    fetch_stop(f);
    snprintf(f->url, sizeof(f->url), "%s", url);
    snprintf(f->out, sizeof(f->out), "/tmp/.omnib-%d-%u", (int)getpid(), seq);
    snprintf(f->err, sizeof(f->err), "/tmp/.omnib-%d-%u.h", (int)getpid(), seq++);
    f->kind = kind;
    f->t0 = now_ms();
    if (!w || !*w)
        w = access("/bin/busybox", X_OK) == 0 ? "/bin/busybox" : "wget";
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        const char *argv[16];
        int n = 0, fd = open(f->err, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            dup2(fd, 2);
            dup2(fd, 1);
        }
        if (strstr(w, "busybox"))
            argv[n++] = "busybox";
        argv[n++] = "wget";
        argv[n++] = "-q";
        argv[n++] = "-S";
        argv[n++] = "-T";
        argv[n++] = "25";
        argv[n++] = "-U";
        argv[n++] = UA;
        argv[n++] = "-O";
        argv[n++] = f->out;
        if (post) {
            argv[n++] = "--post-data";
            argv[n++] = post;
        }
        argv[n++] = url;
        argv[n] = NULL;
        execvp(w, (char *const *)argv);
        _exit(127);
    }
    f->pid = pid;
    return 0;
}

struct reply {
    int  ok;                        /* wget succeeded                       */
    int  status;                    /* HTTP status (0 = unknown)            */
    char type[128];                 /* content type                         */
    char final[2048];               /* URL after redirects                  */
    char err[256];                  /* wget's last words                    */
};

static void reply_read(const struct fetch *f, int wstatus, struct reply *r)
{
    FILE *fp = fopen(f->err, "r");
    char line[2048];
    memset(r, 0, sizeof(*r));
    snprintf(r->final, sizeof(r->final), "%s", f->url);
    r->ok = WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0;
    if (!fp)
        return;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line, *e;
        while (*p == ' ')
            p++;
        e = p + strlen(p);
        while (e > p && (e[-1] == '\n' || e[-1] == '\r'))
            *--e = '\0';
        if (strncmp(p, "HTTP/", 5) == 0) {
            char *sp = strchr(p, ' ');
            r->status = sp ? atoi(sp + 1) : 0;
            r->type[0] = '\0';
        } else if (strncasecmp(p, "content-type:", 13) == 0) {
            p += 13;
            while (*p == ' ')
                p++;
            snprintf(r->type, sizeof(r->type), "%s", p);
        } else if (strncasecmp(p, "location:", 9) == 0) {
            char abs[2048];
            p += 9;
            while (*p == ' ')
                p++;
            html_resolve(r->final, p, abs, sizeof(abs));
            snprintf(r->final, sizeof(r->final), "%s", abs);
        } else if (strncmp(p, "wget: ", 6) == 0 && strncmp(p, "wget: note:", 11) != 0) {
            snprintf(r->err, sizeof(r->err), "%s", p + 6);
        }
    }
    fclose(fp);
}

/* ---- text encodings ---------------------------------------------------------------- */
static int valid_utf8(const unsigned char *s, size_t n)
{
    size_t i = 0;
    int bad = 0;
    while (i < n) {
        unsigned c = s[i];
        int k = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
        int j;
        if (k < 0) { bad++; i++; continue; }
        for (j = 1; j <= k; j++)
            if (i + (size_t)j >= n || (s[i + (size_t)j] & 0xc0) != 0x80)
                break;
        if (j <= k) { bad++; i++; continue; }
        i += (size_t)k + 1;
    }
    return bad < 4;
}

/* windows-1252 / latin-1 -> UTF-8 (a new buffer) */
static char *from_cp1252(const char *s, size_t n, size_t *outn)
{
    static const uint16_t w[32] = {
        0x20ac, '?', 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021, 0x2c6, 0x2030,
        0x160, 0x2039, 0x152, '?', 0x17d, '?', '?', 0x2018, 0x2019, 0x201c, 0x201d,
        0x2022, 0x2013, 0x2014, 0x2dc, 0x2122, 0x161, 0x203a, 0x153, '?', 0x17e, 0x178 };
    char *o = malloc(n * 3 + 1);
    size_t i, k = 0;
    if (!o)
        return NULL;
    for (i = 0; i < n; i++) {
        unsigned c = (unsigned char)s[i];
        if (c >= 0x80 && c < 0xa0)
            c = w[c - 0x80];
        if (c < 0x80) o[k++] = (char)c;
        else if (c < 0x800) { o[k++] = (char)(0xc0 | c >> 6); o[k++] = (char)(0x80 | (c & 63)); }
        else { o[k++] = (char)(0xe0 | c >> 12); o[k++] = (char)(0x80 | ((c >> 6) & 63)); o[k++] = (char)(0x80 | (c & 63)); }
    }
    o[k] = '\0';
    *outn = k;
    return o;
}

static char *escape_html(const char *s, size_t n, size_t *outn)
{
    char *o = malloc(n * 5 + 64), *q;
    size_t i;
    if (!o)
        return NULL;
    q = o;
    for (i = 0; i < n; i++) {
        if (s[i] == '<') { memcpy(q, "&lt;", 4); q += 4; }
        else if (s[i] == '>') { memcpy(q, "&gt;", 4); q += 4; }
        else if (s[i] == '&') { memcpy(q, "&amp;", 5); q += 5; }
        else *q++ = s[i];
    }
    *q = '\0';
    *outn = (size_t)(q - o);
    return o;
}

/* ---- favorites ------------------------------------------------------------------------ */
static void fav_path(char *out, size_t n)
{
    snprintf(out, n, "%s/.omnios/favorites", g_home);
}

static int fav_has(const char *url)
{
    char path[600], line[2400];
    FILE *f;
    int found = 0;
    fav_path(path, sizeof(path));
    if (!(f = fopen(path, "r")))
        return 0;
    while (!found && fgets(line, sizeof(line), f)) {
        char *tab = strchr(line, '\t');
        if (tab && (size_t)(tab - line) == strlen(url) && strncmp(line, url, strlen(url)) == 0)
            found = 1;
    }
    fclose(f);
    return found;
}

static void fav_toggle(const char *url, const char *title)
{
    char path[600], tmp[620], line[2400];
    FILE *f, *o;
    int had = fav_has(url);
    char dir[560];
    snprintf(dir, sizeof(dir), "%s/.omnios", g_home);
    mkdir_p(dir);
    fav_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if (!(o = fopen(tmp, "w")))
        return;
    if ((f = fopen(path, "r")) != NULL) {
        while (fgets(line, sizeof(line), f)) {
            char *tab = strchr(line, '\t');
            if (had && tab && (size_t)(tab - line) == strlen(url) && strncmp(line, url, strlen(url)) == 0)
                continue;
            fputs(line, o);
        }
        fclose(f);
    }
    if (!had)
        fprintf(o, "%s\t%s\n", url, title && *title ? title : url);
    fclose(o);
    rename(tmp, path);
}

/* ---- pages made here ------------------------------------------------------------------- */
static void set_src(struct tab *t, char *src, size_t n)
{
    free(t->src);
    t->src = src;
    t->srclen = n;
    html_free(&t->doc);
    t->scroll = 0;
    t->focus = -1;
    t->laid_w = 0;
}

static char *home_html(size_t *n)
{
    static const char *const quick[][2] = {
        { "https://en.m.wikipedia.org/wiki/Special:Random", "Wikipedia (a random article)" },
        { "https://html.duckduckgo.com/html/", "DuckDuckGo (search)" },
        { "https://news.ycombinator.com/", "Hacker News" },
        { "https://text.npr.org/", "NPR News (text)" },
        { "https://lite.cnn.com/", "CNN Lite" },
        { "https://github.com/OmniNodeCo/OmniOS", "OmniOS on GitHub" },
        { "https://example.com/", "example.com" },
    };
    char path[600], line[2400];
    size_t cap = 16384, len = 0, i;
    char *o = malloc(cap);
    FILE *f;
    int nfav = 0;
    if (!o)
        return NULL;
#define ADD(...) do { int k_ = snprintf(o + len, cap - len, __VA_ARGS__); \
                      if (k_ > 0 && len + (size_t)k_ < cap) len += (size_t)k_; } while (0)
    ADD("<html><head><title>New tab</title></head><body>"
        "<center><h1>OmniOS Browser</h1>"
        "<form action=\"https://html.duckduckgo.com/html/\" method=\"get\">"
        "<input type=\"text\" name=\"q\" size=\"52\" placeholder=\"Search the web\"> "
        "<input type=\"submit\" value=\"Search\"></form></center>");
    fav_path(path, sizeof(path));
    if ((f = fopen(path, "r")) != NULL) {
        while (fgets(line, sizeof(line), f) && len + 3000 < cap) {
            char *tab = strchr(line, '\t'), *e;
            if (!tab)
                continue;
            *tab++ = '\0';
            e = tab + strlen(tab);
            while (e > tab && (e[-1] == '\n' || e[-1] == '\r'))
                *--e = '\0';
            if (!nfav++)
                ADD("<h3>Favorites</h3><ul>");
            ADD("<li><a href=\"%s\">%s</a></li>", line, tab);
        }
        fclose(f);
        if (nfav)
            ADD("</ul>");
    }
    ADD("<h3>Get started</h3><ul>");
    for (i = 0; i < sizeof(quick) / sizeof(quick[0]); i++)
        ADD("<li><a href=\"%s\">%s</a></li>", quick[i][0], quick[i][1]);
    ADD("</ul><hr><p><small>OmniOS Browser shows pages without scripts or style sheets, "
        "the way text browsers do: articles, search results and news read well; web apps "
        "won't work. Type an address or words to search in the bar at the top. Ctrl+T opens "
        "a tab, Ctrl+D adds the page to your favorites.</small></p></body></html>");
#undef ADD
    *n = len;
    return o;
}

static char *message_html(const char *title, const char *msg, const char *url, size_t *n)
{
    char *o = malloc(8192);
    char esc_url[2200];
    size_t k;
    if (!o)
        return NULL;
    {
        size_t en = 0;
        char *e = escape_html(url, strlen(url), &en);
        snprintf(esc_url, sizeof(esc_url), "%s", e ? e : "");
        free(e);
    }
    k = (size_t)snprintf(o, 8192, "<html><head><title>%s</title></head><body><h1>%s</h1>"
                         "<p>%s</p><p><small>%s</small></p><p><a href=\"%s\">Try again</a></p>"
                         "</body></html>", title, title, msg, esc_url, esc_url);
    *n = k < 8192 ? k : 8191;
    return o;
}

/* ---- layout + images ---------------------------------------------------------------------- */
static int page_x(void)
{
    int cw = W - 48;
    if (cw > PAGE_MAX)
        cw = PAGE_MAX;
    return (W - cw) / 2;
}

static int page_w(void)
{
    return W - 2 * page_x();
}

static int view_h(void)
{
    return H - TOP - (T()->notice[0] ? 34 : 0);
}

static void layout(struct tab *t)
{
    if (!t->src)
        return;
    html_layout(&t->doc, t->src, t->srclen, t->url, page_w());
    t->laid_w = page_w();
    if (t->doc.title[0])
        snprintf(t->title, sizeof(t->title), "%s", t->doc.title);
}

static void set_title(void)
{
    char s[64];
    const char *t = T()->title[0] ? T()->title : "New tab";
    snprintf(s, sizeof(s), "%.40s%s - Browser", t, strlen(t) > 40 ? "..." : "");
    omni_client_title(&g_c, s);
}

static void next_image(struct tab *t)
{
    int i;
    if (t->img.pid > 0 || t->page.pid > 0)
        return;
    for (i = 0; i < t->doc.nimg && i < MAX_IMGS; i++)
        if (t->doc.img[i].state == IMG_WAIT) {
            const char *u = t->doc.img[i].url;
            if (strncmp(u, "http://", 7) != 0 && strncmp(u, "https://", 8) != 0 &&
                strncmp(u, "file://", 7) != 0) {
                t->doc.img[i].state = IMG_FAILED;
                continue;
            }
            t->doc.img[i].state = IMG_LOADING;
            if (strncmp(u, "file://", 7) == 0) {        /* local: right away */
                int w, h, comp;
                unsigned char *px = stbi_load(u + 7, &w, &h, &comp, 4);
                uint32_t *o = NULL;
                if (px && (size_t)w * (size_t)h < (size_t)16 << 20 &&
                    (o = malloc((size_t)w * (size_t)h * 4)) != NULL) {
                    size_t k;
                    for (k = 0; k < (size_t)w * (size_t)h; k++) {
                        unsigned a = px[k * 4 + 3];
                        uint32_t c = (uint32_t)px[k * 4] << 16 | (uint32_t)px[k * 4 + 1] << 8 | px[k * 4 + 2];
                        o[k] = 0xff000000u | (a == 255 ? c : th_blend(0xffffff, c, a));
                    }
                }
                stbi_image_free(px);
                html_image_set(&t->doc, i, o, w, h);
                continue;
            }
            t->img.img = i;
            fetch_start(&t->img, F_IMAGE, u, NULL);
            return;
        }
}

static void image_done(struct tab *t, int wstatus)
{
    struct reply r;
    int i = t->img.img, w = 0, h = 0, comp;
    uint32_t *o = NULL;
    reply_read(&t->img, wstatus, &r);
    if (r.ok) {
        size_t n = 0;
        char *buf = slurp(t->img.out, &n);
        unsigned char *px = NULL;
        if (buf && stbi_info_from_memory((unsigned char *)buf, (int)n, &w, &h, &comp) &&
            (size_t)w * (size_t)h < (size_t)16 << 20)
            px = stbi_load_from_memory((unsigned char *)buf, (int)n, &w, &h, &comp, 4);
        if (px && (o = malloc((size_t)w * (size_t)h * 4)) != NULL) {
            size_t k;
            for (k = 0; k < (size_t)w * (size_t)h; k++) {
                unsigned a = px[k * 4 + 3];
                uint32_t c = (uint32_t)px[k * 4] << 16 | (uint32_t)px[k * 4 + 1] << 8 | px[k * 4 + 2];
                o[k] = 0xff000000u | (a == 255 ? c : th_blend(0xffffff, c, a));
            }
        }
        stbi_image_free(px);
        free(buf);
    }
    unlink(t->img.out);
    unlink(t->img.err);
    t->img.pid = 0;
    html_image_set(&t->doc, i, o, w, h);
    if (t->doc.relayout)
        layout(t);
    if (t == T())
        g_dirty = 1;
}

/* ---- navigation ---------------------------------------------------------------------------- */
static void show_src(struct tab *t, char *src, size_t n, const char *url)
{
    snprintf(t->url, sizeof(t->url), "%s", url);
    t->secure = strncmp(url, "https://", 8) == 0;
    t->title[0] = '\0';
    set_src(t, src, n);
    layout(t);
    if (!t->title[0])
        snprintf(t->title, sizeof(t->title), "%s", url);
    if (t->frag[0]) {
        int y = html_anchor_y(&t->doc, t->frag);
        if (y >= 0)
            t->scroll = y > 8 ? y - 8 : 0;
        t->frag[0] = '\0';
    }
    if (t == T()) {
        if (!g_addr_focus)
            snprintf(g_addr, sizeof(g_addr), "%s", strcmp(url, "about:home") ? url : "");
        set_title();
    }
    g_dirty = 1;
}

static void hist_push(struct tab *t, const char *url)
{
    int i;
    if (t->hn > 0 && t->hpos >= 0 && t->hpos < t->hn && strcmp(t->hist[t->hpos], url) == 0)
        return;
    for (i = t->hpos + 1; i < t->hn; i++)            /* drop the forward part */
        free(t->hist[i]);
    t->hn = t->hpos + 1;
    if (t->hn == MAX_HIST) {
        free(t->hist[0]);
        memmove(t->hist, t->hist + 1, sizeof(t->hist[0]) * (MAX_HIST - 1));
        t->hn--;
    }
    t->hist[t->hn++] = strdup(url);
    t->hpos = t->hn - 1;
}

static void open_url(struct tab *t, const char *url, const char *post, int push)
{
    char u[2048];
    const char *hash;
    snprintf(u, sizeof(u), "%s", url);
    t->notice[0] = '\0';
    t->focus = -1;
    fetch_stop(&t->img);
    /* only the #fragment changed: scroll there */
    hash = strchr(u, '#');
    if (hash && !post && t->src && strncmp(u, t->url, (size_t)(hash - u)) == 0 &&
        (t->url[hash - u] == '\0' || t->url[hash - u] == '#')) {
        int y = html_anchor_y(&t->doc, hash + 1);
        if (y >= 0)
            t->scroll = y > 8 ? y - 8 : 0;
        snprintf(t->url, sizeof(t->url), "%s", u);
        if (push)
            hist_push(t, u);
        g_dirty = 1;
        return;
    }
    if (push)
        hist_push(t, u);
    t->frag[0] = '\0';
    if (hash) {
        snprintf(t->frag, sizeof(t->frag), "%s", hash + 1);
        u[hash - u] = '\0';
    }
    if (strcmp(u, "about:home") == 0 || u[0] == '\0') {
        size_t n = 0;
        char *s = home_html(&n);
        fetch_stop(&t->page);
        if (s)
            show_src(t, s, n, "about:home");
        snprintf(t->title, sizeof(t->title), "New tab");
        if (t == T())
            set_title();
        return;
    }
    if (strncmp(u, "file://", 7) == 0 || u[0] == '/') {
        const char *path = u[0] == '/' ? u : u + 7;
        size_t n = 0;
        char *s = slurp(path, &n), abs[2100];
        snprintf(abs, sizeof(abs), "file://%s", path);
        fetch_stop(&t->page);
        if (!s) {
            s = message_html("Can't open this file", strerror(errno), abs, &n);
        } else if (!(strstr(path, ".htm") || strstr(path, ".HTM"))) {
            size_t en = 0;
            char *e = escape_html(s, n, &en), *o = e ? malloc(en + 64) : NULL;
            free(s);
            s = NULL;
            if (o) {
                n = (size_t)sprintf(o, "<pre>%s</pre>", e);
                s = o;
            }
            free(e);
        }
        if (s)
            show_src(t, s, n, abs);
        return;
    }
    snprintf(t->url, sizeof(t->url), "%s", u);
    t->secure = strncmp(u, "https://", 8) == 0;
    if (t == T() && !g_addr_focus)
        snprintf(g_addr, sizeof(g_addr), "%s", u);
    fetch_start(&t->page, F_PAGE, u, post);
    g_dirty = 1;
}

static void downloaded(struct tab *t, const struct reply *r)
{
    char name[256], dir[600], dst[900], msg[1400];
    const char *base = strrchr(r->final, '/');
    char *q;
    size_t n = 0;
    int k, verdict = -1;
    struct stat sb;
    snprintf(name, sizeof(name), "%s", base && base[1] ? base + 1 : "download");
    if ((q = strpbrk(name, "?#")) != NULL)
        *q = '\0';
    for (q = name; *q; q++)
        if (*q == '/' || *q == '\\' || (unsigned char)*q < 32)
            *q = '_';
    if (!name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        snprintf(name, sizeof(name), "download");
    snprintf(dir, sizeof(dir), "%s/Downloads", g_home);
    mkdir_p(dir);
    snprintf(dst, sizeof(dst), "%s/%s", dir, name);
    for (k = 1; access(dst, F_OK) == 0 && k < 100; k++)
        snprintf(dst, sizeof(dst), "%s/(%d) %s", dir, k, name);
    if (rename(t->page.out, dst) != 0) {
        snprintf(t->notice, sizeof(t->notice), "Couldn't save %s: %s", name, strerror(errno));
        t->notice_bg = 0xfde2e1;
        return;
    }
    stat(dst, &sb);
    if (access("/usr/bin/omnios-defender", X_OK) == 0) {   /* check it */
        pid_t pid = fork();
        if (pid == 0) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
            execl("/usr/bin/omnios-defender", "omnios-defender", "--scan", dst, (char *)NULL);
            _exit(127);
        }
        if (pid > 0) {
            int st = 0;
            waitpid(pid, &st, 0);
            verdict = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        }
    }
    if (verdict == 1) {
        snprintf(t->notice, sizeof(t->notice),
                 "OmniOS Defender found a threat in %s and quarantined it.", name);
        t->notice_bg = 0xfde2e1;
    } else {
        snprintf(t->notice, sizeof(t->notice), "Downloaded %s (%ld KB) to Downloads%s", name,
                 (long)(sb.st_size + 1023) / 1024,
                 verdict == 0 ? ". OmniOS Defender found no threats." : ".");
        t->notice_bg = 0xdcfce7;
    }
    snprintf(msg, sizeof(msg), "%s", t->notice);
    (void)n;
    (void)msg;
}

static void page_done(struct tab *t, int wstatus)
{
    struct reply r;
    size_t n = 0;
    char *s = NULL;
    reply_read(&t->page, wstatus, &r);
    t->page.pid = 0;
    if (!r.ok) {
        char msg[600];
        const char *why = r.err[0] ? r.err : "the server didn't answer";
        if (r.status >= 400)
            snprintf(msg, sizeof(msg), "The server answered %d (%s).", r.status, why);
        else
            snprintf(msg, sizeof(msg), "OmniOS couldn't load this page: %s. Check that you "
                     "are connected to the internet (Settings &gt; Network &amp; internet).", why);
        s = message_html(r.status == 404 ? "Page not found" : "Can't reach this page", msg,
                         t->page.url, &n);
        unlink(t->page.out);
        unlink(t->page.err);
        if (s)
            show_src(t, s, n, t->page.url);
        return;
    }
    unlink(t->page.err);
    if (r.type[0] && !strstr(r.type, "html") && !strstr(r.type, "xml") &&
        strncasecmp(r.type, "text/", 5) != 0 && strncasecmp(r.type, "image/", 6) != 0) {
        downloaded(t, &r);                            /* not a page: keep it */
        g_dirty = 1;
        return;
    }
    s = slurp(t->page.out, &n);
    if (!s) {
        unlink(t->page.out);
        return;
    }
    if (strncasecmp(r.type, "image/", 6) == 0) {
        /* an image on its own: a page with just it (decoded from the file) */
        char *o = malloc(4400);
        char url[2100];
        int w, h, comp;
        unsigned char *px = stbi_load_from_memory((unsigned char *)s, (int)n, &w, &h, &comp, 4);
        snprintf(url, sizeof(url), "%s", r.final);
        free(s);
        s = NULL;
        if (o) {
            n = (size_t)snprintf(o, 4400, "<html><head><title>%s</title></head><body><center>"
                                 "<img src=\"%s\"></center></body></html>", url, url);
            show_src(t, o, n, r.final);
            if (px && t->doc.nimg > 0) {
                uint32_t *im = malloc((size_t)w * (size_t)h * 4);
                size_t k;
                if (im) {
                    for (k = 0; k < (size_t)w * (size_t)h; k++)
                        im[k] = 0xff000000u | (uint32_t)px[k * 4] << 16 |
                                (uint32_t)px[k * 4 + 1] << 8 | px[k * 4 + 2];
                    html_image_set(&t->doc, 0, im, w, h);
                    layout(t);
                }
            }
        }
        stbi_image_free(px);
        unlink(t->page.out);
        return;
    }
    unlink(t->page.out);
    if (r.type[0] && strncasecmp(r.type, "text/", 5) == 0 && !strstr(r.type, "html")) {
        size_t en = 0;
        char *e = escape_html(s, n, &en), *o = e ? malloc(en + 64) : NULL;
        free(s);
        s = NULL;
        if (o) {
            n = (size_t)sprintf(o, "<pre>%s</pre>", e);
            s = o;
        }
        free(e);
        if (!s)
            return;
    } else {
        /* the character set: from the header or a <meta> near the top */
        char head[4096];
        size_t hn = n < sizeof(head) - 1 ? n : sizeof(head) - 1, k;
        int latin = 0;
        memcpy(head, s, hn);
        head[hn] = '\0';
        for (k = 0; k < hn; k++)
            head[k] = (char)tolower((unsigned char)head[k]);
        if (strstr(r.type, "8859") || strcasestr(r.type, "1252") || strcasestr(r.type, "latin"))
            latin = 1;
        else if (!strcasestr(r.type, "utf-8") &&
                 (strstr(head, "charset=iso-8859") || strstr(head, "charset=\"iso-8859") ||
                  strstr(head, "windows-1252")))
            latin = 1;
        if (!latin && !valid_utf8((unsigned char *)s, n))
            latin = 1;
        if (latin) {
            size_t on = 0;
            char *o = from_cp1252(s, n, &on);
            if (o) {
                free(s);
                s = o;
                n = on;
            }
        }
    }
    show_src(t, s, n, r.final);
}

static void go_input(struct tab *t, const char *in)
{
    char url[2048], enc[1800];
    while (*in == ' ')
        in++;
    if (!*in)
        return;
    if (strstr(in, "://") || strncmp(in, "about:", 6) == 0 || in[0] == '/')
        snprintf(url, sizeof(url), "%s", in);
    else if (!strchr(in, ' ') && (strchr(in, '.') || strncmp(in, "localhost", 9) == 0))
        snprintf(url, sizeof(url), "https://%s", in);
    else {
        html_urlencode(in, enc, sizeof(enc));
        snprintf(url, sizeof(url), "%s%s", SEARCH, enc);
    }
    open_url(t, url, NULL, 1);
}

static void submit(struct tab *t, int button)
{
    struct html_doc *d = &t->doc;
    int fi = button >= 0 ? d->in[button].form : -1, i;
    char q[3000] = "", enc[1200], url[4200];
    size_t ql = 0;
    if (fi < 0) {                                   /* no form: the first one */
        if (d->nform == 0)
            return;
        fi = 0;
    }
    for (i = 0; i < d->nin; i++) {
        struct html_input *in = &d->in[i];
        const char *v = in->value;
        if (in->form != fi || !in->name[0])
            continue;
        if ((in->type == IN_SUBMIT || in->type == IN_BUTTON) && i != button)
            continue;
        if ((in->type == IN_CHECK || in->type == IN_RADIO) && !in->checked)
            continue;
        if ((in->type == IN_CHECK || in->type == IN_RADIO) && !v[0])
            v = "on";
        html_urlencode(v, enc, sizeof(enc));
        ql += (size_t)snprintf(q + ql, sizeof(q) - ql, "%s%s=%s", ql ? "&" : "", in->name, enc);
        if (ql >= sizeof(q))
            break;
    }
    if (d->form[fi].post) {
        open_url(t, d->form[fi].action, q, 1);
        return;
    }
    snprintf(url, sizeof(url), "%s", d->form[fi].action);
    {
        char *qm = strchr(url, '?'), *hash = strchr(url, '#');
        if (hash)
            *hash = '\0';
        if (qm)
            *qm = '\0';
    }
    snprintf(url + strlen(url), sizeof(url) - strlen(url), "?%s", q);
    open_url(t, url, NULL, 1);
}

static void new_tab(const char *url)
{
    struct tab *t;
    if (g_ntab >= MAX_TABS)
        return;
    t = &g_tab[g_ntab];
    memset(t, 0, sizeof(*t));
    t->focus = -1;
    t->hpos = -1;
    g_cur = g_ntab++;
    g_addr_focus = 0;
    open_url(t, url && *url ? url : "about:home", NULL, 1);
    if (!url || !*url || strcmp(url, "about:home") == 0) {
        g_addr_focus = 1;                             /* type at once */
        g_addr[0] = '\0';
    }
    set_title();
}

static void close_tab(int i)
{
    struct tab *t = &g_tab[i];
    int k;
    fetch_stop(&t->page);
    fetch_stop(&t->img);
    html_free(&t->doc);
    free(t->src);
    for (k = 0; k < t->hn; k++)
        free(t->hist[k]);
    memmove(&g_tab[i], &g_tab[i + 1], sizeof(g_tab[0]) * (size_t)(g_ntab - i - 1));
    g_ntab--;
    if (g_ntab == 0) {
        new_tab("about:home");
        return;
    }
    if (g_cur >= g_ntab)
        g_cur = g_ntab - 1;
    snprintf(g_addr, sizeof(g_addr), "%s", strcmp(T()->url, "about:home") ? T()->url : "");
    set_title();
}

static void go_hist(struct tab *t, int dir)
{
    int p = t->hpos + dir;
    if (p < 0 || p >= t->hn)
        return;
    t->hpos = p;
    open_url(t, t->hist[p], NULL, 0);
}

/* ---- drawing -------------------------------------------------------------------------------- */
static void fillr(int x, int y, int w, int h, uint32_t c)
{
    raster_fill(&g_r, x, y, w, h, 0xff000000u | c);
}

/* anti-aliased line, width wd */
static void aline(float x0, float y0, float x1, float y1, float wd, uint32_t c)
{
    int xa = (int)(fminf(x0, x1) - wd - 1), xb = (int)(fmaxf(x0, x1) + wd + 1);
    int ya = (int)(fminf(y0, y1) - wd - 1), yb = (int)(fmaxf(y0, y1) + wd + 1);
    float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
    int x, y;
    for (y = ya; y <= yb; y++)
        for (x = xa; x <= xb; x++) {
            float px = (float)x + 0.5f - x0, py = (float)y + 0.5f - y0;
            float t = l2 > 0 ? (px * dx + py * dy) / l2 : 0;
            float ex, ey, dist, cov;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            ex = px - t * dx;
            ey = py - t * dy;
            dist = sqrtf(ex * ex + ey * ey);
            cov = wd / 2 + 0.5f - dist;
            if (cov > 0)
                th_px(&g_r, x, y, c, (unsigned)(cov > 1 ? 255 : cov * 255));
        }
}

static void icon_btn(int i, int x, int y, int enabled)
{
    uint32_t c = enabled ? 0x3b4048 : 0xb4bac2;
    float cx = (float)x + 17, cy = (float)y + 17;
    if (g_hover_btn == i && enabled)
        th_round_rect(&g_r, x, y, 34, 34, 6, 0xe8ebef, 255);
    switch (i) {
    case 0: case 1: {                               /* back / forward */
        float s = i == 0 ? -1.0f : 1.0f;
        aline(cx - 6 * s, cy, cx + 6 * s, cy, 1.8f, c);
        aline(cx + 6 * s, cy, cx + 1 * s, cy - 5, 1.8f, c);
        aline(cx + 6 * s, cy, cx + 1 * s, cy + 5, 1.8f, c);
        break;
    }
    case 2:                                         /* reload / stop */
        if (T()->page.pid > 0) {
            aline(cx - 5, cy - 5, cx + 5, cy + 5, 1.8f, c);
            aline(cx + 5, cy - 5, cx - 5, cy + 5, 1.8f, c);
        } else {
            int k;
            for (k = 0; k < 14; k++) {              /* an open circle */
                float a0 = 0.6f + k * 0.37f, a1 = a0 + 0.37f;
                aline(cx + 6 * cosf(a0), cy + 6 * sinf(a0), cx + 6 * cosf(a1), cy + 6 * sinf(a1), 1.7f, c);
            }
            aline(cx + 6 * cosf(0.6f), cy + 6 * sinf(0.6f), cx + 6 * cosf(0.6f) + 4, cy + 6 * sinf(0.6f) - 1, 1.7f, c);
            aline(cx + 6 * cosf(0.6f), cy + 6 * sinf(0.6f), cx + 6 * cosf(0.6f) + 1, cy + 6 * sinf(0.6f) - 4, 1.7f, c);
        }
        break;
    case 3:                                         /* home */
        aline(cx - 7, cy, cx, cy - 7, 1.7f, c);
        aline(cx, cy - 7, cx + 7, cy, 1.7f, c);
        aline(cx - 5, cy - 2, cx - 5, cy + 7, 1.7f, c);
        aline(cx + 5, cy - 2, cx + 5, cy + 7, 1.7f, c);
        aline(cx - 5, cy + 7, cx + 5, cy + 7, 1.7f, c);
        break;
    }
}

static void star(float cx, float cy, float r, uint32_t c, int filled)
{
    float px[10], py[10];
    int k;
    for (k = 0; k < 10; k++) {
        float a = -1.5707963f + k * 0.6283185f, rr = (k & 1) ? r * 0.45f : r;
        px[k] = cx + rr * cosf(a);
        py[k] = cy + rr * sinf(a);
    }
    if (filled) {
        int x, y;
        for (y = (int)(cy - r); y <= (int)(cy + r); y++)
            for (x = (int)(cx - r); x <= (int)(cx + r); x++) {
                int in = 0, i, j;
                for (i = 0, j = 9; i < 10; j = i++)
                    if (((py[i] > y + 0.5f) != (py[j] > y + 0.5f)) &&
                        (x + 0.5f < (px[j] - px[i]) * (y + 0.5f - py[i]) / (py[j] - py[i]) + px[i]))
                        in = !in;
                if (in)
                    th_px(&g_r, x, y, c, 255);
            }
    }
    for (k = 0; k < 10; k++)
        aline(px[k], py[k], px[(k + 1) % 10], py[(k + 1) % 10], 1.4f, c);
}

static int tab_w(void)
{
    int w = (W - 60) / (g_ntab > 0 ? g_ntab : 1);
    return w > 240 ? 240 : w < 90 ? 90 : w;
}

static void draw_chrome(void)
{
    int i, tw = tab_w(), ax, aw;
    struct tab *t = T();
    fillr(0, 0, W, TAB_H, C_STRIP);
    for (i = 0; i < g_ntab; i++) {
        int x = 8 + i * tw, act = i == g_cur;
        const char *title = g_tab[i].title[0] ? g_tab[i].title : "New tab";
        if (act) {
            th_round_rect(&g_r, x, 6, tw - 2, TAB_H, 8, 0xffffff, 255);
        } else if (g_hover_btn == 100 + i) {
            th_round_rect(&g_r, x, 6, tw - 2, TAB_H - 10, 8, 0xd2d8df, 255);
        }
        /* the page's "icon": a circle with its first letter */
        th_round_rect(&g_r, x + 10, 14, 16, 16, 8, g_tab[i].page.pid > 0 ? C_ACCENT : 0x9aa5b1, 255);
        {
            char l[2] = { (char)toupper((unsigned char)title[0]), 0 };
            if (!isalnum((unsigned char)l[0])) l[0] = 'W';
            font_draw(&g_r, l, x + 18 - font_width(l, 10, FONT_BOLD) / 2, 15, 10, FONT_BOLD, 0xffffff);
        }
        font_draw_clip(&g_r, title, x + 34, 13, 13, FONT_REGULAR, act ? C_INK : 0x48505a, tw - 34 - 30);
        /* close (x) */
        if (g_hover_btn == 200 + i)
            th_round_rect(&g_r, x + tw - 28, 12, 20, 20, 4, act ? 0xe8ebef : 0xc8cfd7, 255);
        aline((float)(x + tw - 22), 18, (float)(x + tw - 14), 26, 1.3f, 0x59636e);
        aline((float)(x + tw - 14), 18, (float)(x + tw - 22), 26, 1.3f, 0x59636e);
        if (!act && i + 1 != g_cur && i + 1 < g_ntab)
            fillr(x + tw - 1, 14, 1, 16, 0xb8c0c9);
    }
    if (g_ntab < MAX_TABS) {                          /* + new tab */
        int x = 8 + g_ntab * tw + 4;
        if (g_hover_btn == 300)
            th_round_rect(&g_r, x, 8, 28, 28, 6, 0xd2d8df, 255);
        aline((float)x + 14, 15, (float)x + 14, 29, 1.6f, 0x3b4048);
        aline((float)x + 7, 22, (float)x + 21, 22, 1.6f, 0x3b4048);
    }
    /* toolbar */
    fillr(0, TAB_H, W, BAR_H, 0xffffff);
    fillr(0, TOP - 1, W, 1, 0xe1e4e8);
    icon_btn(0, 8, TAB_H + 7, t->hpos > 0);
    icon_btn(1, 44, TAB_H + 7, t->hpos + 1 < t->hn);
    icon_btn(2, 80, TAB_H + 7, 1);
    icon_btn(3, 116, TAB_H + 7, 1);
    ax = 158;
    aw = W - ax - 12;
    th_round_rect(&g_r, ax, TAB_H + 7, aw, 34, 17, g_addr_focus ? 0xffffff : 0xf0f2f5, 255);
    if (g_addr_focus) {
        th_round_rect(&g_r, ax, TAB_H + 7, aw, 34, 17, C_ACCENT, 255);
        th_round_rect(&g_r, ax + 2, TAB_H + 9, aw - 4, 30, 15, 0xffffff, 255);
    }
    {
        struct raster clip = g_r;
        int tx = ax + 38, base = TAB_H + 29, maxw = aw - 38 - 40;
        const char *shown = g_addr;
        /* the lock: https (the certificate isn't checked: see About) */
        if (t->secure && !g_addr_focus) {
            th_round_rect(&g_r, ax + 15, TAB_H + 21, 10, 8, 2, 0x59636e, 255);
            aline((float)ax + 17, TAB_H + 21, (float)ax + 17, TAB_H + 17, 1.4f, 0x59636e);
            aline((float)ax + 23, TAB_H + 21, (float)ax + 23, TAB_H + 17, 1.4f, 0x59636e);
            aline((float)ax + 17, TAB_H + 17, (float)ax + 23, TAB_H + 17, 1.4f, 0x59636e);
        } else {
            int k;
            for (k = 0; k < 12; k++) {              /* a magnifier / globe */
                float a0 = k * 0.5236f, a1 = a0 + 0.5236f;
                aline(ax + 20 + 5 * cosf(a0), TAB_H + 23 + 5 * sinf(a0),
                      ax + 20 + 5 * cosf(a1), TAB_H + 23 + 5 * sinf(a1), 1.4f, 0x59636e);
            }
            aline((float)ax + 24, TAB_H + 27, (float)ax + 28, TAB_H + 31, 1.6f, 0x59636e);
        }
        clip.w = tx + maxw;
        if (!g_addr[0]) {
            font_draw_base(&clip, "Search or type a web address", tx, base, 14, FONT_REGULAR, 0x7b8591);
            if (g_addr_focus)
                fillr(tx, TAB_H + 15, 1, 18, C_INK);
        }
        if (g_addr[0]) {
            int w = font_width(shown, 14, FONT_REGULAR), off = 0;
            if (g_addr_focus && w > maxw)
                off = w - maxw;
            if (g_addr_focus && g_addr_sel && g_addr[0])
                th_round_rect(&g_r, tx - 2, TAB_H + 14, (w < maxw ? w : maxw) + 4, 20, 3, 0xbfdbfe, 255);
            font_draw_base(&clip, shown, tx - off, base, 14, FONT_REGULAR, C_INK);
            if (g_addr_focus)
                fillr(tx - off + w + 1, TAB_H + 15, 1, 18, C_INK);
        }
        if (strcmp(t->url, "about:home") != 0 && !g_addr_focus)
            star((float)(ax + aw - 22), TAB_H + 24, 8, fav_has(t->url) ? 0xeab308 : 0x59636e,
                 fav_has(t->url));
    }
    if (t->page.pid > 0) {                            /* progress */
        struct stat sb;
        long el = now_ms() - t->page.t0;
        int p = (int)(el / 40) % 200;
        if (stat(t->page.out, &sb) == 0 && sb.st_size > 0)
            p = 60 + (int)(sb.st_size / 8192) % 140;
        fillr(0, TOP - 2, W * (p < 200 ? p : 199) / 200, 2, C_ACCENT);
    }
}

static void draw_page(void)
{
    struct tab *t = T();
    int vh = view_h(), y0 = TOP, maxs;
    fillr(0, y0, W, H - y0, 0xffffff);
    if (t->notice[0]) {
        fillr(0, y0, W, 34, t->notice_bg ? t->notice_bg : 0xe0f2fe);
        font_draw_clip(&g_r, t->notice, 16, y0 + 9, 13, FONT_REGULAR, C_INK, W - 80);
        aline((float)W - 26, (float)y0 + 12, (float)W - 18, (float)y0 + 20, 1.3f, 0x59636e);
        aline((float)W - 18, (float)y0 + 12, (float)W - 26, (float)y0 + 20, 1.3f, 0x59636e);
        y0 += 34;
    }
    maxs = t->doc.height - vh;
    if (maxs < 0) maxs = 0;
    if (t->scroll > maxs) t->scroll = maxs;
    if (t->scroll < 0) t->scroll = 0;
    if (t->src)
        html_draw(&t->doc, &g_r, page_x(), y0 + 12, t->scroll, vh - 12, t->focus, g_hover_link);
    if (t->doc.height > vh) {                         /* scroll indicator */
        int bh = vh * vh / t->doc.height, by;
        if (bh < 30) bh = 30;
        by = y0 + (int)((long)(vh - bh) * t->scroll / (maxs ? maxs : 1));
        th_round_rect(&g_r, W - 9, by + 2, 5, bh - 4, 2, 0xb4bac2, 255);
    }
    if (g_hover_link >= 0 && g_hover_link < t->doc.nlink) {   /* link preview */
        const char *u = t->doc.link[g_hover_link];
        int w = font_width(u, 12, FONT_REGULAR) + 16;
        if (w > W / 2) w = W / 2;
        th_round_rect(&g_r, 4, H - 26, w, 22, 5, 0xf3f4f6, 255);
        font_draw_clip(&g_r, u, 12, H - 23, 12, FONT_REGULAR, 0x3b4048, w - 16);
    }
}

static void redraw(void)
{
    if (T()->laid_w != page_w() && T()->src)
        layout(T());
    draw_chrome();
    draw_page();
    omni_client_blit(&g_c, 0, 0, W, H, g_fb, W);
    g_dirty = 0;
}

/* ---- input ------------------------------------------------------------------------------------ */
static void scroll_by(int dy)
{
    T()->scroll += dy;
    g_dirty = 1;
}

static int page_y0(void)
{
    return TOP + (T()->notice[0] ? 34 : 0) + 12;
}

/* doc box under window point, or -1 */
static int box_at(int x, int y)
{
    struct tab *t = T();
    if (y < page_y0() || !t->src)
        return -1;
    return html_hit(&t->doc, x - page_x(), y - page_y0() + t->scroll);
}

static int chrome_btn(int x, int y)
{
    int tw = tab_w(), i;
    if (y < TAB_H) {
        for (i = 0; i < g_ntab; i++) {
            int tx = 8 + i * tw;
            if (x >= tx + tw - 28 && x < tx + tw - 8 && y >= 12 && y < 32)
                return 200 + i;
            if (x >= tx && x < tx + tw - 2)
                return 100 + i;
        }
        if (g_ntab < MAX_TABS && x >= 8 + g_ntab * tw + 4 && x < 8 + g_ntab * tw + 32)
            return 300;
        return -1;
    }
    if (y < TOP) {
        int k;
        for (k = 0; k < 4; k++)
            if (x >= 8 + k * 36 && x < 8 + k * 36 + 34)
                return k;
        if (x >= W - 12 - 40 && x < W - 12)
            return 4;                                 /* star */
        if (x >= 158)
            return 5;                                 /* address bar */
    }
    if (T()->notice[0] && y >= TOP && y < TOP + 34 && x >= W - 40)
        return 6;                                     /* close the notice */
    return -1;
}

static void click(int x, int y, int btn)
{
    struct tab *t = T();
    int b = chrome_btn(x, y), k;
    if (b >= 0 || y < TOP) {
        g_addr_focus = b == 5;
        if (b == 5) {
            g_addr_sel = 1;
            snprintf(g_addr, sizeof(g_addr), "%s", strcmp(t->url, "about:home") ? t->url : "");
        } else {
            snprintf(g_addr, sizeof(g_addr), "%s", strcmp(t->url, "about:home") ? t->url : "");
        }
        if (b == 0) go_hist(t, -1);
        else if (b == 1) go_hist(t, 1);
        else if (b == 2) {
            if (t->page.pid > 0)
                fetch_stop(&t->page);
            else if (t->hpos >= 0 && t->hpos < t->hn)
                open_url(t, t->hist[t->hpos], NULL, 0);
        } else if (b == 3) open_url(t, "about:home", NULL, 1);
        else if (b == 4 && strcmp(t->url, "about:home")) fav_toggle(t->url, t->title);
        else if (b == 6) t->notice[0] = '\0';
        else if (b >= 200 && b < 300) close_tab(b - 200);
        else if (b >= 100 && b < 200) {
            g_cur = b - 100;
            snprintf(g_addr, sizeof(g_addr), "%s", strcmp(T()->url, "about:home") ? T()->url : "");
            set_title();
        } else if (b == 300) new_tab(NULL);
        g_dirty = 1;
        return;
    }
    g_addr_focus = 0;
    k = box_at(x, y);
    t->focus = -1;
    if (k >= 0) {
        struct html_box *bx = &t->doc.box[k];
        if (bx->type == BOX_INPUT) {
            struct html_input *in = &t->doc.in[bx->ref];
            if (in->type == IN_SUBMIT)
                submit(t, bx->ref);
            else if (in->type == IN_CHECK)
                in->checked = !in->checked;
            else if (in->type == IN_RADIO) {
                int i;
                for (i = 0; i < t->doc.nin; i++)
                    if (t->doc.in[i].type == IN_RADIO && !strcmp(t->doc.in[i].name, in->name))
                        t->doc.in[i].checked = 0;
                in->checked = 1;
            } else if (in->type != IN_BUTTON)
                t->focus = bx->ref;
        } else if (bx->link >= 0 && bx->link < t->doc.nlink) {
            char url[2048];
            snprintf(url, sizeof(url), "%s", t->doc.link[bx->link]);
            if (btn == 2) {
                int keep = g_cur;
                new_tab(url);
                g_cur = keep;
                g_addr_focus = 0;
                snprintf(g_addr, sizeof(g_addr), "%s", T()->url);
                set_title();
            } else {
                open_url(t, url, NULL, 1);
            }
        }
    }
    g_dirty = 1;
}

static void hover(int x, int y)
{
    int hl = -1, hb = chrome_btn(x, y), k;
    g_px = x;
    g_py = y;
    if (hb < 0 && (k = box_at(x, y)) >= 0)
        hl = T()->doc.box[k].link;
    if (hl != g_hover_link || hb != g_hover_btn) {
        g_hover_link = hl;
        g_hover_btn = hb;
        g_dirty = 1;
    }
}

static void type_into(char *buf, size_t cap, int key, char ch, int *sel)
{
    size_t l = strlen(buf);
    if (key == 14) {                                  /* Backspace */
        if (sel && *sel)
            buf[0] = '\0';
        else if (l > 0) {
            l--;
            while (l > 0 && ((unsigned char)buf[l] & 0xc0) == 0x80)
                l--;
            buf[l] = '\0';
        }
    } else if ((unsigned char)ch >= 32 && ch != 127) {
        if (sel && *sel)
            buf[0] = '\0', l = 0;
        if (l + 1 < cap) {
            buf[l] = ch;
            buf[l + 1] = '\0';
        }
    }
    if (sel)
        *sel = 0;
}

static void key(int code, char ch)
{
    struct tab *t = T();
    int vh = view_h();
    /* Ctrl+letter arrives as its control character */
    if (ch == 0x0c) { g_addr_focus = 1; g_addr_sel = 1; snprintf(g_addr, sizeof(g_addr), "%s", strcmp(t->url, "about:home") ? t->url : ""); g_dirty = 1; return; }
    if (ch == 0x14) { new_tab(NULL); g_dirty = 1; return; }
    if (ch == 0x17) { close_tab(g_cur); g_dirty = 1; return; }
    if (ch == 0x12 || code == 63) { if (t->hpos >= 0 && t->hpos < t->hn) open_url(t, t->hist[t->hpos], NULL, 0); return; }
    if (ch == 0x04) { if (strcmp(t->url, "about:home")) fav_toggle(t->url, t->title); g_dirty = 1; return; }
    if (g_addr_focus) {
        if (code == 28) {
            g_addr_focus = 0;
            go_input(t, g_addr);
        } else if (code == 1) {
            g_addr_focus = 0;
            snprintf(g_addr, sizeof(g_addr), "%s", strcmp(t->url, "about:home") ? t->url : "");
        } else {
            type_into(g_addr, sizeof(g_addr), code, ch, &g_addr_sel);
        }
        g_dirty = 1;
        return;
    }
    if (t->focus >= 0 && t->focus < t->doc.nin) {
        struct html_input *in = &t->doc.in[t->focus];
        if (code == 28 && in->type != IN_AREA) {
            int f = t->focus, i, btn = -1;
            for (i = 0; i < t->doc.nin; i++)           /* the form's submit button */
                if (t->doc.in[i].form == in->form && t->doc.in[i].type == IN_SUBMIT) {
                    btn = i;
                    break;
                }
            t->focus = -1;
            submit(t, btn >= 0 ? btn : f);
        } else if (code == 1) {
            t->focus = -1;
        } else if (code == 15) {                       /* Tab: the next field */
            int i;
            for (i = t->focus + 1; i < t->doc.nin; i++)
                if (t->doc.in[i].type == IN_TEXT || t->doc.in[i].type == IN_PASSWORD ||
                    t->doc.in[i].type == IN_AREA)
                    break;
            t->focus = i < t->doc.nin ? i : -1;
        } else if (in->type == IN_TEXT || in->type == IN_PASSWORD || in->type == IN_AREA) {
            type_into(in->value, sizeof(in->value), code, code == 28 ? ' ' : ch, NULL);
        }
        g_dirty = 1;
        return;
    }
    switch (code) {
    case 103: scroll_by(-48); break;                  /* Up    */
    case 108: scroll_by(48); break;                   /* Down  */
    case 104: scroll_by(-(vh - 60)); break;           /* PgUp  */
    case 109: case 57: scroll_by(vh - 60); break;     /* PgDn, Space */
    case 102: t->scroll = 0; g_dirty = 1; break;      /* Home  */
    case 107: t->scroll = t->doc.height; g_dirty = 1; break;
    case 14: go_hist(t, -1); break;                   /* Backspace: back */
    case 1: if (t->page.pid > 0) { fetch_stop(&t->page); g_dirty = 1; } break;
    default: break;
    }
}

/* ---- --dump: the page as text, for tests and the console ------------------------------------- */
static int dump(const char *url)
{
    struct tab *t = &g_tab[0];
    int i, last_y = -1, k;
    memset(t, 0, sizeof(*t));
    t->hpos = -1;
    W = 980 + 48;
    H = 2000;
    g_ntab = 1;
    open_url(t, url, NULL, 1);
    while (t->page.pid > 0) {
        int st = 0;
        if (waitpid(t->page.pid, &st, 0) == t->page.pid)
            page_done(t, st);
    }
    printf("title: %s\nurl: %s\nlinks: %d, images: %d, forms: %d\n", t->title, t->url,
           t->doc.nlink, t->doc.nimg, t->doc.nform);
    if (t->notice[0])
        printf("notice: %s\n", t->notice);
    for (i = 0, k = 0; i < t->doc.nbox && k < 4000; i++) {
        const struct html_box *b = &t->doc.box[i];
        if (b->type != BOX_TEXT && b->type != BOX_MONO)
            continue;
        if (last_y >= 0 && b->y > last_y + 4) {
            putchar('\n');
            k++;
        } else if (last_y >= 0) {
            putchar(' ');
        }
        fwrite(t->doc.text + b->text, 1, (size_t)b->len, stdout);
        last_y = b->y;
    }
    putchar('\n');
    return t->doc.nbox > 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *start = argc > 1 ? argv[1] : NULL;
    const char *home = getenv("HOME");
    int quit = 0;

    snprintf(g_home, sizeof(g_home), "%s", home && *home ? home : "/root");
    signal(SIGPIPE, SIG_IGN);
    font_init();
    if (argc > 2 && strcmp(argv[1], "--dump") == 0)
        return dump(argv[2]);

    if (omni_client_open(&g_c, "Browser") < 0)
        return 127;
    W = g_c.screen_w > 0 ? g_c.screen_w - 70 : 1180;
    H = g_c.screen_h > 0 ? g_c.screen_h - OMNI_TASKBAR_H - 40 - OMNI_WM_TITLE_H : 720;
    if (W > 1240) W = 1240;
    if (H > 860) H = 860;
    if (W < 640) W = 640;
    if (H < 400) H = 400;
    if (omni_client_window(&g_c, "Browser", W, H + OMNI_WM_TITLE_H) < 0) {
        omni_client_close(&g_c);
        return 127;
    }
    omni_client_want_pointer(&g_c);
    g_fb = malloc((size_t)W * (size_t)H * sizeof(uint32_t));
    if (!g_fb)
        return 1;
    raster_init(&g_r, g_fb, W, H, W);
    new_tab(start && *start ? start : "about:home");
    if (start && *start)
        g_addr_focus = 0;

    while (!quit) {
        struct omni_client_event e;
        struct pollfd pf = { g_c.fd, POLLIN, 0 };
        int r, i, busy = 0;
        for (i = 0; i < g_ntab; i++)
            busy |= g_tab[i].page.pid > 0 || g_tab[i].img.pid > 0;
        if (g_dirty)
            redraw();
        if (poll(&pf, 1, busy ? 60 : -1) < 0 && errno != EINTR)
            break;
        while ((r = omni_client_poll(&g_c, &e)) > 0) {
            if (e.type == 3) { quit = 1; break; }
            if (e.type == 1 && e.pressed) key(e.key, e.text);
            else if (e.type == 2 && e.pressed) click(e.x, e.y, e.key);
            else if (e.type == 4) scroll_by(e.key == 4 ? -60 : e.key == 5 ? 60 : 0);
            else if (e.type == 5) hover(e.x, e.y);
        }
        if (r < 0)
            break;
        for (i = 0; i < g_ntab; i++) {                 /* finished downloads */
            struct tab *t = &g_tab[i];
            int st = 0;
            if (t->page.pid > 0) {
                if (waitpid(t->page.pid, &st, WNOHANG) == t->page.pid)
                    page_done(t, st);
                if (i == g_cur)
                    g_dirty = 1;                       /* the progress bar */
            }
            if (t->img.pid > 0 && waitpid(t->img.pid, &st, WNOHANG) == t->img.pid)
                image_done(t, st);
            if (t->page.pid == 0 && t->img.pid == 0)
                next_image(t);
        }
    }
    for (quit = 0; quit < g_ntab; quit++) {
        fetch_stop(&g_tab[quit].page);
        fetch_stop(&g_tab[quit].img);
    }
    omni_client_close(&g_c);
    return 0;
}
