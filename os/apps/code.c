/*
 * OmniOS — os/apps/code.c
 *
 * OmniOS Code: write, run and build OmniOS programs (OmniScript, see
 * oms.h and omx.c). An editor with syntax colours, line numbers and
 * auto-indent; Run (F5) starts the program with omx and shows what it
 * prints below (type there when it asks for input); an error points at
 * its line. Build makes an OmniOS executable (.omx) that runs on any
 * OmniOS computer: double-click it in File Manager, or ./name.omx in the
 * Terminal.
 *
 * Keys: Ctrl+N new, Ctrl+O open, Ctrl+S save, F5 run, Ctrl+B build, Esc
 * stop / close a dialog. Programs are kept in ~/Documents; the examples
 * are copied to ~/Documents/Examples the first time.
 *
 *   omnios-code [FILE.oms]
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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

#include "../gui/client.h"
#include "../gui/font.h"
#include "../gui/theme.h"
#include "../gui/wm.h"

#define BAR_H    46
#define OUT_H    170
#define GUTTER   52
#define LINE_H   18
#define CW       8
#define MAXL     20000
#define EXAMPLES "/usr/share/omnios/examples"
#define C_BG     0xffffff
#define C_INK    0x1f2328
#define C_DIM    0x6e7781
#define C_KW     0x8250df
#define C_STR    0x0a7d36
#define C_NUM    0xb35900
#define C_COM    0x8b949e
#define C_FN     0x0550ae
#define C_ACC    0x2563eb

static struct omni_client_conn g_c;
static int W, H;
static uint32_t *g_fb;
static struct raster g_r;

/* the text */
static char *g_line[MAXL];
static int g_nline = 1, g_cy, g_cx, g_top, g_dirty_doc, g_err_line = -1;
static char g_path[512], g_home[512];

/* output panel */
#define OUT_MAX 300
static char *g_out[OUT_MAX];
static int g_nout;
static char g_partial[1024], g_in[512];
static int g_focus_out;
static pid_t g_child;
static int g_child_out = -1, g_child_in = -1;

/* dialogs: 0 none, 1 open, 2 save as */
static int g_dialog;
static char g_files[200][512];
static int g_nfiles, g_dsel;
static char g_name[128];
static int g_hover = -1, g_redraw = 1;

/* ---- text buffer ------------------------------------------------------------------ */
static void set_line(int i, const char *s, size_t n)
{
    char *l = malloc(n + 1);
    if (!l)
        return;
    memcpy(l, s, n);
    l[n] = '\0';
    free(g_line[i]);
    g_line[i] = l;
}

static void doc_clear(void)
{
    int i;
    for (i = 0; i < g_nline; i++) {
        free(g_line[i]);
        g_line[i] = NULL;
    }
    g_nline = 1;
    set_line(0, "", 0);
    g_cx = g_cy = g_top = 0;
    g_err_line = -1;
}

static void doc_set(const char *text)
{
    const char *p = text;
    doc_clear();
    g_nline = 0;
    while (g_nline < MAXL) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (!e && n == 0 && g_nline > 0)
            break;                                /* the file's final newline */
        if (n && p[n - 1] == '\r')
            n--;
        set_line(g_nline++, p, n);
        if (!e)
            break;
        p = e + 1;
    }
    if (g_nline == 0)
        set_line(g_nline++, "", 0);
}

static char *doc_text(size_t *outn)
{
    size_t n = 0;
    int i;
    char *b, *q;
    for (i = 0; i < g_nline; i++)
        n += strlen(g_line[i]) + 1;
    b = malloc(n + 1);
    if (!b)
        return NULL;
    q = b;
    for (i = 0; i < g_nline; i++) {
        size_t l = strlen(g_line[i]);
        memcpy(q, g_line[i], l);
        q += l;
        *q++ = '\n';
    }
    *q = '\0';
    if (outn)
        *outn = (size_t)(q - b);
    return b;
}

static void insert_text(const char *s, size_t n)
{
    char *l = g_line[g_cy], *nl;
    size_t len = strlen(l);
    if ((size_t)g_cx > len)
        g_cx = (int)len;
    nl = malloc(len + n + 1);
    if (!nl)
        return;
    memcpy(nl, l, (size_t)g_cx);
    memcpy(nl + g_cx, s, n);
    memcpy(nl + g_cx + n, l + g_cx, len - (size_t)g_cx + 1);
    free(l);
    g_line[g_cy] = nl;
    g_cx += (int)n;
    g_dirty_doc = 1;
}

static void newline(void)
{
    char *l = g_line[g_cy];
    int indent = 0, i;
    size_t len = strlen(l);
    char pad[128];
    if (g_nline >= MAXL)
        return;
    if ((size_t)g_cx > len)
        g_cx = (int)len;
    while (l[indent] == ' ' && indent < 100)
        indent++;
    if (g_cx > 0 && l[g_cx - 1] == '{')
        indent += 4;                              /* a block opens: indent */
    memmove(g_line + g_cy + 2, g_line + g_cy + 1, sizeof(char *) * (size_t)(g_nline - g_cy - 1));
    g_line[g_cy + 1] = NULL;
    g_nline++;
    for (i = 0; i < indent && i < 127; i++)
        pad[i] = ' ';
    pad[i] = '\0';
    {
        char *rest = strdup(l + g_cx);
        char *nl = malloc((size_t)i + strlen(rest) + 1);
        sprintf(nl, "%s%s", pad, rest);
        g_line[g_cy + 1] = nl;
        l[g_cx] = '\0';
        free(rest);
    }
    g_cy++;
    g_cx = i;
    g_dirty_doc = 1;
}

static void backspace(void)
{
    if (g_cx > 0) {
        char *l = g_line[g_cy];
        int k = 1;
        /* a run of indent spaces goes 4 at a time */
        if (g_cx % 4 == 0 && g_cx >= 4) {
            int j, all = 1;
            for (j = 0; j < g_cx; j++)
                if (l[j] != ' ') all = 0;
            if (all) k = 4;
        }
        memmove(l + g_cx - k, l + g_cx, strlen(l + g_cx) + 1);
        g_cx -= k;
    } else if (g_cy > 0) {
        char *a = g_line[g_cy - 1], *b = g_line[g_cy], *nl = malloc(strlen(a) + strlen(b) + 1);
        g_cx = (int)strlen(a);
        sprintf(nl, "%s%s", a, b);
        free(a);
        free(b);
        g_line[g_cy - 1] = nl;
        memmove(g_line + g_cy, g_line + g_cy + 1, sizeof(char *) * (size_t)(g_nline - g_cy - 1));
        g_nline--;
        g_cy--;
    }
    g_dirty_doc = 1;
}

static void delete_char(void)
{
    char *l = g_line[g_cy];
    if ((size_t)g_cx < strlen(l)) {
        memmove(l + g_cx, l + g_cx + 1, strlen(l + g_cx));
    } else if (g_cy + 1 < g_nline) {
        int cx = g_cx;
        g_cy++;
        g_cx = 0;
        backspace();
        g_cx = cx;
    }
    g_dirty_doc = 1;
}

/* ---- files ------------------------------------------------------------------------------ */
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *b;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 4 << 20) { fclose(f); return NULL; }
    b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    if (b) b[n] = '\0';
    fclose(f);
    return b;
}

static void out_add(const char *s)
{
    free(g_out[g_nout % OUT_MAX]);
    g_out[g_nout % OUT_MAX] = strdup(s);
    g_nout++;
    g_redraw = 1;
}

static void set_title(void)
{
    char t[64];
    const char *b = strrchr(g_path, '/');
    snprintf(t, sizeof(t), "%.32s%s - OmniOS Code", g_path[0] ? (b ? b + 1 : g_path) : "Untitled",
             g_dirty_doc ? " *" : "");
    omni_client_title(&g_c, t);
}

static int save_to(const char *path_in)
{
    char path[512];
    size_t n;
    char *t = doc_text(&n);
    FILE *f;
    snprintf(path, sizeof(path), "%s", path_in);    /* it may be g_path itself */
    f = fopen(path, "wb");
    int ok = f && t && fwrite(t, 1, n, f) == n;
    if (f && fclose(f) != 0)
        ok = 0;
    free(t);
    if (!ok) {
        char m[600];
        snprintf(m, sizeof(m), "Couldn't save %s: %s", path, strerror(errno));
        out_add(m);
        return -1;
    }
    snprintf(g_path, sizeof(g_path), "%s", path);
    g_dirty_doc = 0;
    set_title();
    return 0;
}

static void open_file(const char *path)
{
    char *t = slurp(path);
    if (!t) {
        char m[600];
        snprintf(m, sizeof(m), "Couldn't open %s", path);
        out_add(m);
        return;
    }
    doc_set(t);
    free(t);
    snprintf(g_path, sizeof(g_path), "%s", path);
    g_dirty_doc = 0;
    set_title();
}

static void copy_examples(void)
{
    char dir[600], dst[1100], src[1100];
    DIR *d;
    struct dirent *de;
    snprintf(dir, sizeof(dir), "%s/Documents/Examples", g_home);
    if (access(dir, F_OK) == 0)
        return;
    snprintf(src, sizeof(src), "%s/Documents", g_home);
    mkdir(src, 0755);
    mkdir(dir, 0755);
    if (!(d = opendir(EXAMPLES)))
        return;
    while ((de = readdir(d)) != NULL) {
        char *t;
        FILE *f;
        if (!strstr(de->d_name, ".oms"))
            continue;
        snprintf(src, sizeof(src), "%s/%s", EXAMPLES, de->d_name);
        snprintf(dst, sizeof(dst), "%s/%s", dir, de->d_name);
        if ((t = slurp(src)) && (f = fopen(dst, "wb"))) {
            fputs(t, f);
            fclose(f);
        }
        free(t);
    }
    closedir(d);
}

static void list_files(void)
{
    const char *dirs[3];
    char d1[600], d2[600];
    int k;
    snprintf(d1, sizeof(d1), "%s/Documents", g_home);
    snprintf(d2, sizeof(d2), "%s/Documents/Examples", g_home);
    dirs[0] = d1;
    dirs[1] = d2;
    dirs[2] = EXAMPLES;
    g_nfiles = 0;
    for (k = 0; k < 3; k++) {
        DIR *d = opendir(dirs[k]);
        struct dirent *de;
        if (!d)
            continue;
        while ((de = readdir(d)) != NULL && g_nfiles < 200) {
            size_t l = strlen(de->d_name);
            if (l > 4 && !strcmp(de->d_name + l - 4, ".oms") && (k < 2 || access(d2, F_OK) != 0))
                snprintf(g_files[g_nfiles++], sizeof(g_files[0]), "%s/%s", dirs[k], de->d_name);
        }
        closedir(d);
    }
    g_dsel = 0;
}

/* ---- running ------------------------------------------------------------------------------ */
static void stop_child(void)
{
    if (g_child > 0) {
        kill(g_child, SIGTERM);
        waitpid(g_child, NULL, 0);
        g_child = 0;
        out_add("[stopped]");
    }
    if (g_child_out >= 0) close(g_child_out);
    if (g_child_in >= 0) close(g_child_in);
    g_child_out = g_child_in = -1;
}

/* "name.oms:12: ..." or "... line 12" -> 12 */
static void find_error_line(const char *s)
{
    const char *p = strstr(s, ".oms:");
    int n = 0;
    if (p)
        n = atoi(p + 5);
    else if ((p = strstr(s, " line ")) != NULL)
        n = atoi(p + 6);
    if (n > 0 && n <= g_nline) {
        g_err_line = n - 1;
        g_cy = n - 1;
        g_cx = 0;
        if (g_cy < g_top || g_cy >= g_top + (H - BAR_H - OUT_H) / LINE_H - 1)
            g_top = g_cy > 3 ? g_cy - 3 : 0;
    }
}

static int spawn_omx(const char *cmd, const char *path)
{
    int out[2], in[2];
    pid_t pid;
    if (pipe(out) < 0 || pipe(in) < 0)
        return -1;
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        close(in[1]);
        close(out[0]);
        setenv("OMX_CONSOLE", "0", 1);           /* print comes here, not a window */
        execl("/usr/bin/omx", "omx", cmd, path, (char *)NULL);
        execlp("omx", "omx", cmd, path, (char *)NULL);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    fcntl(out[0], F_SETFL, O_NONBLOCK);
    g_child = pid;
    g_child_out = out[0];
    g_child_in = in[1];
    return 0;
}

static const char *run_path(void)
{
    static char tmp[64];
    if (g_path[0]) {
        save_to(g_path);
        return g_path;
    }
    snprintf(tmp, sizeof(tmp), "/tmp/omnios-code-%d.oms", (int)getpid());
    {
        size_t n;
        char *t = doc_text(&n);
        FILE *f = fopen(tmp, "wb");
        if (f && t) fwrite(t, 1, n, f);
        if (f) fclose(f);
        free(t);
    }
    return tmp;
}

static void run_program(void)
{
    const char *p;
    stop_child();
    g_err_line = -1;
    p = run_path();
    out_add("");
    {
        char m[600];
        const char *b = strrchr(p, '/');
        snprintf(m, sizeof(m), "> Running %s", b ? b + 1 : p);
        out_add(m);
    }
    if (spawn_omx("run", p) < 0)
        out_add("Couldn't start omx");
    g_focus_out = 1;
}

static void build_program(void)
{
    if (!g_path[0]) {
        g_dialog = 2;                             /* name it first */
        snprintf(g_name, sizeof(g_name), "my-program");
        return;
    }
    stop_child();
    save_to(g_path);
    out_add("");
    out_add("> Building an OmniOS executable (.omx)");
    if (spawn_omx("build", g_path) < 0)
        out_add("Couldn't start omx");
}

static void child_poll(void)
{
    char buf[2048];
    ssize_t r;
    int st;
    if (g_child_out >= 0) {
        while ((r = read(g_child_out, buf, sizeof(buf) - 1)) > 0) {
            ssize_t i, s = 0;
            buf[r] = '\0';
            for (i = 0; i < r; i++)
                if (buf[i] == '\n') {
                    char line[1600];
                    snprintf(line, sizeof(line), "%s%.*s", g_partial, (int)(i - s), buf + s);
                    out_add(line);
                    if (strstr(line, "rror") || strstr(line, ".oms:") || strstr(line, " line "))
                        find_error_line(line);
                    g_partial[0] = '\0';
                    s = i + 1;
                }
            if (s < r) {
                size_t l = strlen(g_partial);
                snprintf(g_partial + l, sizeof(g_partial) - l, "%.*s", (int)(r - s), buf + s);
            }
            g_redraw = 1;
        }
    }
    if (g_child > 0 && waitpid(g_child, &st, WNOHANG) == g_child) {
        char m[96];
        while (g_child_out >= 0 && (r = read(g_child_out, buf, sizeof(buf) - 1)) > 0) {
            buf[r] = '\0';
            strncat(g_partial, buf, sizeof(g_partial) - strlen(g_partial) - 1);
        }
        if (g_partial[0]) {
            out_add(g_partial);
            find_error_line(g_partial);
            g_partial[0] = '\0';
        }
        g_child = 0;
        snprintf(m, sizeof(m), "[finished%s]", WIFEXITED(st) && WEXITSTATUS(st) ? " with an error" : "");
        out_add(m);
        if (g_child_out >= 0) close(g_child_out);
        if (g_child_in >= 0) close(g_child_in);
        g_child_out = g_child_in = -1;
    }
}

/* ---- drawing ----------------------------------------------------------------------------- */
static void fillr(int x, int y, int w, int h, uint32_t c)
{
    raster_fill(&g_r, x, y, w, h, 0xff000000u | c);
}

static void mono(const char *s, int n, int x, int y, uint32_t c, int clip_x)
{
    int i;
    for (i = 0; i < n && s[i]; i++, x += CW) {
        const unsigned char *g;
        int gx, gy;
        unsigned char ch = (unsigned char)s[i];
        if (x + CW > clip_x)
            break;
        if (ch == ' ')
            continue;
        g = omni_glyph8(ch < 128 ? ch : '?');
        for (gy = 0; gy < 16; gy++) {
            unsigned bits = g[gy / 2];
            for (gx = 0; gx < 8; gx++)
                if ((bits >> gx) & 1)
                    raster_px(&g_r, x + gx, y + gy, 0xff000000u | c);
        }
    }
}

static int is_kw(const char *s, int n)
{
    static const char *const kw[] = { "let", "fn", "if", "elif", "else", "while", "for", "in",
        "return", "break", "continue", "true", "false", "nil", "and", "or", "not", NULL };
    int i;
    for (i = 0; kw[i]; i++)
        if ((int)strlen(kw[i]) == n && !memcmp(kw[i], s, (size_t)n))
            return 1;
    return 0;
}

/* one line, coloured: comments, strings, numbers, keywords, calls */
static void draw_code_line(const char *s, int x, int y, int clip)
{
    int i = 0, n = (int)strlen(s);
    while (i < n) {
        int j = i;
        uint32_t c = C_INK;
        if (s[i] == '#' || (s[i] == '/' && s[i + 1] == '/')) {
            mono(s + i, n - i, x + i * CW, y, C_COM, clip);
            return;
        }
        if (s[i] == '"' || s[i] == '\'') {
            char q = s[i];
            for (j = i + 1; j < n && s[j] != q; j++)
                if (s[j] == '\\') j++;
            if (j < n) j++;
            c = C_STR;
        } else if (isdigit((unsigned char)s[i])) {
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '.')) j++;
            c = C_NUM;
        } else if (isalpha((unsigned char)s[i]) || s[i] == '_') {
            int k;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
            for (k = j; k < n && s[k] == ' '; k++) ;
            c = is_kw(s + i, j - i) ? C_KW : (k < n && s[k] == '(') ? C_FN : C_INK;
        } else {
            j = i + 1;
        }
        mono(s + i, j - i, x + i * CW, y, c, clip);
        i = j;
    }
}

static const char *const g_btn[] = { "New", "Open", "Save", "Run", "Stop", "Build .omx" };

static int btn_x(int i, int *w)
{
    static const int ww[] = { 54, 60, 56, 70, 60, 96 };
    int x = 10, k;
    for (k = 0; k < i; k++)
        x += ww[k] + (k == 2 || k == 4 ? 18 : 6);
    *w = ww[i];
    return x;
}

static void draw(void)
{
    int rows = (H - BAR_H - OUT_H) / LINE_H, i, y, w;
    char s[160];
    /* toolbar */
    fillr(0, 0, W, BAR_H, 0xf6f8fa);
    fillr(0, BAR_H - 1, W, 1, 0xd8dee4);
    for (i = 0; i < 6; i++) {
        int x = btn_x(i, &w), prim = i == 3, dis = (i == 4 && g_child <= 0) || (i == 3 && g_child > 0);
        uint32_t bg = prim ? (g_hover == i ? 0x1d4ed8 : C_ACC) : (g_hover == i ? 0xeaeef2 : 0xffffff);
        th_round_rect(&g_r, x, 8, w, 30, 6, prim ? bg : 0xd0d7de, 255);
        if (!prim)
            th_round_rect(&g_r, x + 1, 9, w - 2, 28, 5, bg, 255);
        if (i == 3) {                              /* a play triangle */
            int k;
            for (k = 0; k < 9; k++) {
                int half = 6 - k * 6 / 9;
                fillr(x + 12 + k, 23 - half, 1, 2 * half + 1, 0xffffff);
            }
        }
        font_draw(&g_r, g_btn[i], x + (i == 3 ? 26 : (w - font_width(g_btn[i], 13, FONT_REGULAR)) / 2), 15, 13,
                  prim ? FONT_BOLD : FONT_REGULAR, prim ? 0xffffff : dis ? 0xa0a8b0 : C_INK);
    }
    {
        const char *b = strrchr(g_path, '/');
        snprintf(s, sizeof(s), "%s%s", g_path[0] ? (b ? b + 1 : g_path) : "Untitled", g_dirty_doc ? "  \xe2\x80\xa2" : "");
        font_draw_clip(&g_r, s, btn_x(5, &w) + 120, 14, 14, FONT_BOLD, C_INK, W - (btn_x(5, &w) + 120) - 120);
        font_draw(&g_r, "OmniScript", W - 92, 15, 13, FONT_REGULAR, C_DIM);
    }
    /* editor */
    fillr(0, BAR_H, W, H - BAR_H - OUT_H, C_BG);
    fillr(0, BAR_H, GUTTER, H - BAR_H - OUT_H, 0xf6f8fa);
    if (g_cy < g_top) g_top = g_cy;
    if (g_cy >= g_top + rows) g_top = g_cy - rows + 1;
    for (i = g_top, y = BAR_H + 6; i < g_nline && y + LINE_H <= H - OUT_H; i++, y += LINE_H) {
        snprintf(s, sizeof(s), "%d", i + 1);
        if (i == g_err_line)
            fillr(GUTTER, y - 1, W - GUTTER, LINE_H, 0xffebe9);
        else if (i == g_cy && !g_focus_out)
            fillr(GUTTER, y - 1, W - GUTTER, LINE_H, 0xf3f6fb);
        mono(s, (int)strlen(s), GUTTER - 10 - (int)strlen(s) * CW, y, i == g_cy ? C_INK : 0x9aa4ae, GUTTER);
        draw_code_line(g_line[i], GUTTER + 10, y, W - 6);
    }
    if (!g_focus_out && !g_dialog) {               /* caret */
        int cx = g_cx > (int)strlen(g_line[g_cy]) ? (int)strlen(g_line[g_cy]) : g_cx;
        fillr(GUTTER + 10 + cx * CW, BAR_H + 6 + (g_cy - g_top) * LINE_H - 1, 2, LINE_H, C_ACC);
    }
    /* output */
    y = H - OUT_H;
    fillr(0, y, W, OUT_H, 0x0d1117);
    fillr(0, y, W, 26, 0x161b22);
    font_draw(&g_r, g_child > 0 ? "Output  (running: type here for input)" : "Output", 12, y + 5, 13,
              FONT_BOLD, 0xc9d1d9);
    snprintf(s, sizeof(s), "Ln %d, Col %d", g_cy + 1, g_cx + 1);
    font_draw(&g_r, s, W - 12 - font_width(s, 12, FONT_REGULAR), y + 6, 12, FONT_REGULAR, 0x8b949e);
    {
        int orows = (OUT_H - 34) / LINE_H, first = g_nout + 1 > orows ? g_nout + 1 - orows : 0, k;
        for (k = first, i = 0; k <= g_nout; k++, i++) {
            char tmp[1200];
            const char *t;
            uint32_t c = 0xc9d1d9;
            if (k < g_nout) {
                t = g_out[k % OUT_MAX] ? g_out[k % OUT_MAX] : "";
                if (t[0] == '>' || t[0] == '[') c = 0x58a6ff;
                if (strstr(t, "Error") || strstr(t, ".oms:")) c = 0xff7b72;
            } else {
                snprintf(tmp, sizeof(tmp), "%s%s%s", g_partial, g_child > 0 && g_focus_out ? g_in : "",
                         g_child > 0 && g_focus_out ? "_" : "");
                t = tmp;
            }
            mono(t, (int)strlen(t), 12, y + 32 + i * LINE_H, c, W - 6);
        }
    }
    /* dialogs */
    if (g_dialog) {
        int dw = 520, dh = g_dialog == 1 ? 420 : 170, dx = (W - dw) / 2, dy = (H - dh) / 2;
        th_shadow(&g_r, dx, dy, dw, dh, 10, 16, 6, 90);
        th_round_rect(&g_r, dx, dy, dw, dh, 10, 0xffffff, 255);
        if (g_dialog == 1) {
            int k, rowsd = (dh - 100) / 26;
            font_draw(&g_r, "Open a program", dx + 20, dy + 16, 17, FONT_BOLD, C_INK);
            if (g_nfiles == 0)
                font_draw(&g_r, "No programs in Documents yet.", dx + 20, dy + 56, 13, FONT_REGULAR, C_DIM);
            for (k = 0; k < g_nfiles && k < rowsd; k++) {
                int ry = dy + 52 + k * 26;
                const char *n = g_files[k], *home_rel = strncmp(n, g_home, strlen(g_home)) == 0 ? n + strlen(g_home) + 1 : n;
                if (k == g_dsel)
                    th_round_rect(&g_r, dx + 12, ry - 3, dw - 24, 24, 4, 0xdbeafe, 255);
                font_draw_clip(&g_r, home_rel, dx + 22, ry, 13, FONT_REGULAR, C_INK, dw - 44);
            }
            font_draw(&g_r, "Enter: open   Esc: cancel   or click a file", dx + 20, dy + dh - 30, 12, FONT_REGULAR, C_DIM);
        } else {
            font_draw(&g_r, "Save as", dx + 20, dy + 16, 17, FONT_BOLD, C_INK);
            font_draw(&g_r, "Name (saved in Documents as name.oms):", dx + 20, dy + 52, 13, FONT_REGULAR, C_DIM);
            th_round_rect(&g_r, dx + 20, dy + 76, dw - 40, 34, 6, C_ACC, 255);
            th_round_rect(&g_r, dx + 22, dy + 78, dw - 44, 30, 5, 0xffffff, 255);
            font_draw(&g_r, g_name, dx + 32, dy + 84, 14, FONT_REGULAR, C_INK);
            fillr(dx + 33 + font_width(g_name, 14, FONT_REGULAR), dy + 84, 1, 18, C_INK);
            font_draw(&g_r, "Enter: save   Esc: cancel", dx + 20, dy + dh - 34, 12, FONT_REGULAR, C_DIM);
        }
    }
    omni_client_blit(&g_c, 0, 0, W, H, g_fb, W);
    g_redraw = 0;
}

/* ---- input ------------------------------------------------------------------------------ */
static void new_doc(void)
{
    stop_child();
    doc_set("#! name: My program\n"
            "# Write OmniScript here, then press Run (or F5).\n"
            "# Examples: Open > Documents/Examples. Help: OmniOS Code's README.\n"
            "\n"
            "print(\"Hello, OmniOS!\")\n");
    g_path[0] = '\0';
    g_cy = 4;
    g_cx = 0;
    g_dirty_doc = 0;
    set_title();
}

static void dialog_key(int code, char ch)
{
    if (code == 1) {
        g_dialog = 0;
    } else if (g_dialog == 1) {
        if (code == 103 && g_dsel > 0) g_dsel--;
        else if (code == 108 && g_dsel + 1 < g_nfiles) g_dsel++;
        else if (code == 28 && g_nfiles > 0) {
            stop_child();
            open_file(g_files[g_dsel]);
            g_dialog = 0;
        }
    } else if (g_dialog == 2) {
        size_t l = strlen(g_name);
        if (code == 28 && g_name[0]) {
            char dir[600], path[800];
            snprintf(dir, sizeof(dir), "%s/Documents", g_home);
            mkdir(dir, 0755);
            snprintf(path, sizeof(path), "%s/%s%s", dir, g_name, strstr(g_name, ".oms") ? "" : ".oms");
            g_dialog = 0;
            if (save_to(path) == 0) {
                char m[900];
                snprintf(m, sizeof(m), "Saved %s", path);
                out_add(m);
            }
        } else if (code == 14) {
            if (l) g_name[l - 1] = '\0';
        } else if ((isalnum((unsigned char)ch) || ch == '-' || ch == '_' || ch == ' ' || ch == '.') &&
                   l + 1 < sizeof(g_name)) {
            g_name[l] = ch;
            g_name[l + 1] = '\0';
        }
    }
    g_redraw = 1;
}

static void do_save(void)
{
    if (g_path[0]) {
        if (save_to(g_path) == 0) {
            char m[600];
            snprintf(m, sizeof(m), "Saved %s", g_path);
            out_add(m);
        }
    } else {
        g_dialog = 2;
        snprintf(g_name, sizeof(g_name), "my-program");
    }
}

static void button(int i)
{
    switch (i) {
    case 0: new_doc(); break;
    case 1: list_files(); g_dialog = 1; break;
    case 2: do_save(); break;
    case 3: run_program(); break;
    case 4: stop_child(); break;
    case 5: build_program(); break;
    }
    g_redraw = 1;
}

static void key(int code, char ch)
{
    int len;
    if (g_dialog) {
        dialog_key(code, ch);
        return;
    }
    if (ch == 0x13) { do_save(); return; }        /* Ctrl+S */
    if (ch == 0x0f) { button(1); return; }        /* Ctrl+O */
    if (ch == 0x0e) { button(0); return; }        /* Ctrl+N */
    if (ch == 0x02) { button(5); return; }        /* Ctrl+B */
    if (code == 63) { button(3); return; }        /* F5 */
    if (g_focus_out && g_child > 0) {             /* typing for the program */
        size_t l = strlen(g_in);
        if (code == 28) {
            char line[600];
            snprintf(line, sizeof(line), "%s\n", g_in);
            if (g_child_in >= 0 && write(g_child_in, line, strlen(line)) < 0) { /* ended */ }
            snprintf(line, sizeof(line), "%s%s", g_partial, g_in);
            out_add(line);
            g_partial[0] = '\0';
            g_in[0] = '\0';
        } else if (code == 14) {
            if (l) g_in[l - 1] = '\0';
        } else if (code == 1) {
            stop_child();
        } else if ((unsigned char)ch >= 32 && l + 1 < sizeof(g_in)) {
            g_in[l] = ch;
            g_in[l + 1] = '\0';
        }
        g_redraw = 1;
        return;
    }
    g_focus_out = 0;
    len = (int)strlen(g_line[g_cy]);
    if (g_cx > len) g_cx = len;
    switch (code) {
    case 103: if (g_cy > 0) g_cy--; break;
    case 108: if (g_cy + 1 < g_nline) g_cy++; break;
    case 105: if (g_cx > 0) g_cx--; else if (g_cy > 0) { g_cy--; g_cx = (int)strlen(g_line[g_cy]); } break;
    case 106: if (g_cx < len) g_cx++; else if (g_cy + 1 < g_nline) { g_cy++; g_cx = 0; } break;
    case 102: g_cx = 0; while (g_line[g_cy][g_cx] == ' ') g_cx++; break;
    case 107: g_cx = len; break;
    case 104: g_cy -= (H - BAR_H - OUT_H) / LINE_H - 2; if (g_cy < 0) g_cy = 0; break;
    case 109: g_cy += (H - BAR_H - OUT_H) / LINE_H - 2; if (g_cy >= g_nline) g_cy = g_nline - 1; break;
    case 28: newline(); break;
    case 14: backspace(); break;
    case 111: delete_char(); break;
    case 15: insert_text("    ", 4 - (size_t)(g_cx % 4)); break;
    case 1: stop_child(); break;
    default:
        if ((unsigned char)ch >= 32 && ch != 127) {
            char c = ch;
            insert_text(&c, 1);
            if (c == '}') {                           /* dedent a lone } */
                char *l = g_line[g_cy];
                int k, all = 1;
                for (k = 0; k < g_cx - 1; k++) if (l[k] != ' ') all = 0;
                if (all && g_cx - 1 >= 4) {
                    memmove(l, l + 4, strlen(l + 4) + 1);
                    g_cx -= 4;
                }
            }
        }
        break;
    }
    if (g_err_line >= 0 && g_dirty_doc)
        g_err_line = -1;
    set_title();
    g_redraw = 1;
}

static int hit_button(int x, int y)
{
    int i, w;
    if (y < 8 || y >= 38)
        return -1;
    for (i = 0; i < 6; i++) {
        int bx = btn_x(i, &w);
        if (x >= bx && x < bx + w)
            return i;
    }
    return -1;
}

static void click(int x, int y)
{
    int b;
    if (g_dialog) {
        int dw = 520, dh = g_dialog == 1 ? 420 : 170, dx = (W - dw) / 2, dy = (H - dh) / 2;
        if (x < dx || x > dx + dw || y < dy || y > dy + dh) {
            g_dialog = 0;
        } else if (g_dialog == 1) {
            int k = (y - dy - 49) / 26;
            if (k >= 0 && k < g_nfiles && y >= dy + 49) {
                stop_child();
                open_file(g_files[k]);
                g_dialog = 0;
            }
        }
        g_redraw = 1;
        return;
    }
    if ((b = hit_button(x, y)) >= 0) {
        button(b);
        return;
    }
    if (y >= H - OUT_H) {
        g_focus_out = 1;
    } else if (y > BAR_H) {
        int row = g_top + (y - BAR_H - 6) / LINE_H, col = (x - GUTTER - 10 + CW / 2) / CW;
        g_focus_out = 0;
        if (row >= g_nline) row = g_nline - 1;
        if (row < 0) row = 0;
        g_cy = row;
        g_cx = col < 0 ? 0 : col;
        if (g_cx > (int)strlen(g_line[g_cy])) g_cx = (int)strlen(g_line[g_cy]);
    }
    g_redraw = 1;
}

int main(int argc, char **argv)
{
    const char *home = getenv("HOME");
    snprintf(g_home, sizeof(g_home), "%s", home && *home && strcmp(home, "/") ? home : "/root");
    signal(SIGPIPE, SIG_IGN);
    font_init();
    copy_examples();
    if (omni_client_open(&g_c, "OmniOS Code") < 0)
        return 127;
    W = g_c.screen_w > 0 ? g_c.screen_w - 160 : 1000;
    H = g_c.screen_h > 0 ? g_c.screen_h - OMNI_TASKBAR_H - 70 - OMNI_WM_TITLE_H : 640;
    if (W > 1100) W = 1100;
    if (H > 760) H = 760;
    if (W < 700) W = 700;
    if (H < 440) H = 440;
    if (omni_client_window(&g_c, "OmniOS Code", W, H + OMNI_WM_TITLE_H) < 0)
        return 127;
    omni_client_want_pointer(&g_c);
    g_fb = malloc((size_t)W * (size_t)H * 4);
    if (!g_fb)
        return 1;
    raster_init(&g_r, g_fb, W, H, W);
    doc_clear();
    if (argc > 1)
        open_file(argv[1]);
    else
        new_doc();
    out_add("Welcome to OmniOS Code. Press Run (F5) to start your program.");
    for (;;) {
        struct omni_client_event e;
        struct pollfd pf[2];
        int n = 1, r;
        if (g_redraw)
            draw();
        pf[0].fd = g_c.fd;
        pf[0].events = POLLIN;
        if (g_child_out >= 0) {
            pf[1].fd = g_child_out;
            pf[1].events = POLLIN;
            n = 2;
        }
        if (poll(pf, (nfds_t)n, g_child > 0 ? 200 : -1) < 0 && errno != EINTR)
            break;
        while ((r = omni_client_poll(&g_c, &e)) > 0) {
            if (e.type == 3) {
                stop_child();
                return 0;
            }
            if (e.type == 1 && e.pressed) key(e.key, e.text);
            else if (e.type == 2 && e.pressed) click(e.x, e.y);
            else if (e.type == 4 && !g_dialog) {
                g_top += e.key == 4 ? -3 : 3;
                if (g_top > g_nline - 3) g_top = g_nline - 3;
                if (g_top < 0) g_top = 0;
                if (g_cy < g_top) g_cy = g_top;
                if (g_cy >= g_top + (H - BAR_H - OUT_H) / LINE_H) g_cy = g_top + (H - BAR_H - OUT_H) / LINE_H - 1;
                g_redraw = 1;
            } else if (e.type == 5) {
                int h = g_dialog ? -1 : hit_button(e.x, e.y);
                if (h != g_hover) { g_hover = h; g_redraw = 1; }
            }
        }
        if (r < 0)
            break;
        child_poll();
    }
    stop_child();
    return 0;
}
