/*
 * OmniOS — os/apps/omx.c
 *
 * omx: OmniOS's program runner. OmniOS programs are written in OmniScript
 * (.oms, see oms.h) and built into OmniOS executables (.omx): one file
 * with a manifest (name, icon, the permissions it wants), the program as
 * OmniScript bytecode, and a SHA-256 seal over both. They are not ELF,
 * .exe, .dmg or .rpm files: the kernel hands them to this runner, which
 * runs them in the OmniScript virtual machine, so they work the same on
 * every OmniOS computer and can only do what the user allowed.
 *
 *   omx run FILE [ARGS]        run a program (.omx) or source (.oms)
 *   omx build SRC.oms [-o OUT] build an executable (default: SRC.omx)
 *   omx check SRC.oms          compile only, report errors
 *   omx info FILE.omx          show the manifest and check the seal
 *   omx FILE.omx [ARGS]        the same as run (how the kernel calls it:
 *                              ominit registers .omx with binfmt_misc, so
 *                              ./game.omx just works)
 *
 * A program's header comments are its manifest:
 *
 *   #! name: Bouncing ball
 *   #! icon: B
 *   #! color: #22c55e
 *   #! permissions: files, network
 *
 * Permissions: files (read and write files in your folders), network
 * (download from the internet) and system (run commands). A program has to
 * list what it needs, and you are asked once before it gets it.
 * Everything else - windows, drawing, maths, text - needs no permission.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "oms.h"
#include "../gui/client.h"
#include "../gui/font.h"
#include "../gui/theme.h"
#include "../gui/wm.h"
#include "../lib/sha256.h"

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

static const unsigned char OMX_MAGIC[8] = { 0x89, 'O', 'M', 'X', '\r', '\n', 0x1a, '\n' };
#define OMX_VERSION 1

enum { P_FILES = 1, P_NETWORK = 2, P_SYSTEM = 4 };

struct manifest {
    char name[96], icon[8], color[16], version[32], author[96], source[128];
    int  perms;
};

static struct manifest g_man;
static char g_hash[65];                 /* the program's SHA-256 (permissions) */
static int  g_granted, g_denied;        /* asked this run                      */
static char g_home[512];
static int  g_gui_console;              /* print/input go to a window          */
static struct oms_vm *g_vm;

/* ---- small helpers ----------------------------------------------------------------- */
static char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    char *b = NULL;
    size_t cap = 0, len = 0, r;
    if (!f)
        return NULL;
    for (;;) {
        if (len + 65536 + 1 > cap) {
            char *nb = realloc(b, cap = cap ? cap * 2 : 131072);
            if (!nb) { free(b); fclose(f); return NULL; }
            b = nb;
        }
        r = fread(b + len, 1, cap - len - 1, f);
        if (r == 0)
            break;
        len += r;
        if (len > (size_t)64 << 20)
            break;
    }
    fclose(f);
    b[len] = '\0';
    if (n)
        *n = len;
    return b;
}

static void mkdir_p(const char *dir)
{
    char tmp[PATH_MAX], *p;
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (p = tmp + 1; *p; p++)
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

static const char *perm_name(int p)
{
    return p == P_FILES ? "files" : p == P_NETWORK ? "network" : "system";
}

static const char *perm_words(int p)
{
    return p == P_FILES ? "read and change files in your folders"
         : p == P_NETWORK ? "download from the internet"
         : "run commands on this computer";
}

/* the manifest from a source's "#! key: value" lines */
static void manifest_from_source(const char *src, const char *file, struct manifest *m)
{
    const char *p = src, *base = strrchr(file, '/');
    int line;
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", base ? base + 1 : file);
    {
        char *dot = strrchr(m->name, '.');
        if (dot && dot != m->name)
            *dot = '\0';
    }
    snprintf(m->source, sizeof(m->source), "%s", base ? base + 1 : file);
    for (line = 0; *p && line < 40; line++) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n > 2 && p[0] == '#' && p[1] == '!') {
            char key[32], val[160];
            const char *c = memchr(p, ':', n);
            if (c) {
                size_t kn = (size_t)(c - p - 2), vn = n - (size_t)(c - p) - 1;
                const char *k = p + 2, *v = c + 1;
                while (kn && isspace((unsigned char)*k)) { k++; kn--; }
                while (kn && isspace((unsigned char)k[kn - 1])) kn--;
                while (vn && isspace((unsigned char)*v)) { v++; vn--; }
                while (vn && isspace((unsigned char)v[vn - 1])) vn--;
                snprintf(key, sizeof(key), "%.*s", (int)kn, k);
                snprintf(val, sizeof(val), "%.*s", (int)vn, v);
                if (!strcasecmp(key, "name")) snprintf(m->name, sizeof(m->name), "%s", val);
                else if (!strcasecmp(key, "icon")) snprintf(m->icon, sizeof(m->icon), "%s", val);
                else if (!strcasecmp(key, "color")) snprintf(m->color, sizeof(m->color), "%s", val);
                else if (!strcasecmp(key, "version")) snprintf(m->version, sizeof(m->version), "%s", val);
                else if (!strcasecmp(key, "author")) snprintf(m->author, sizeof(m->author), "%s", val);
                else if (!strcasecmp(key, "permissions")) {
                    if (strcasestr(val, "file")) m->perms |= P_FILES;
                    if (strcasestr(val, "net")) m->perms |= P_NETWORK;
                    if (strcasestr(val, "system")) m->perms |= P_SYSTEM;
                }
            }
        }
        p = e ? e + 1 : p + n;
    }
}

static void manifest_text(const struct manifest *m, char *out, size_t n)
{
    char perms[64] = "";
    int k;
    for (k = 1; k <= P_SYSTEM; k <<= 1)
        if (m->perms & k)
            snprintf(perms + strlen(perms), sizeof(perms) - strlen(perms), "%s%s",
                     perms[0] ? "," : "", perm_name(k));
    snprintf(out, n, "name=%s\nicon=%s\ncolor=%s\nversion=%s\nauthor=%s\npermissions=%s\n"
             "source=%s\n", m->name, m->icon, m->color, m->version, m->author, perms, m->source);
}

static void manifest_parse(const char *t, size_t n, struct manifest *m)
{
    char buf[2048], *line, *save = NULL;
    memset(m, 0, sizeof(*m));
    snprintf(buf, sizeof(buf), "%.*s", (int)(n < sizeof(buf) - 1 ? n : sizeof(buf) - 1), t);
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq++ = '\0';
        if (!strcmp(line, "name")) snprintf(m->name, sizeof(m->name), "%s", eq);
        else if (!strcmp(line, "icon")) snprintf(m->icon, sizeof(m->icon), "%s", eq);
        else if (!strcmp(line, "color")) snprintf(m->color, sizeof(m->color), "%s", eq);
        else if (!strcmp(line, "version")) snprintf(m->version, sizeof(m->version), "%s", eq);
        else if (!strcmp(line, "author")) snprintf(m->author, sizeof(m->author), "%s", eq);
        else if (!strcmp(line, "source")) snprintf(m->source, sizeof(m->source), "%s", eq);
        else if (!strcmp(line, "permissions")) {
            if (strstr(eq, "files")) m->perms |= P_FILES;
            if (strstr(eq, "network")) m->perms |= P_NETWORK;
            if (strstr(eq, "system")) m->perms |= P_SYSTEM;
        }
    }
}

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* an .omx file in memory: checks magic, sizes and the seal */
static int omx_parse(const unsigned char *b, size_t n, struct manifest *m,
                     const unsigned char **code, size_t *ncode, char *why, size_t wn)
{
    uint32_t mlen, clen;
    struct sha256 s;
    unsigned char digest[32];
    if (n < 8 + 4 + 4 + 4 + 32 || memcmp(b, OMX_MAGIC, 8) != 0) {
        snprintf(why, wn, "this isn't an OmniOS executable");
        return -1;
    }
    if (get32(b + 8) != OMX_VERSION) {
        snprintf(why, wn, "it was made for a newer OmniOS (format %u)", get32(b + 8));
        return -1;
    }
    mlen = get32(b + 12);
    if (mlen > n - 52) {
        snprintf(why, wn, "it is damaged (cut short)");
        return -1;
    }
    clen = get32(b + 16 + mlen);
    if ((size_t)clen != n - 20 - mlen - 32) {
        snprintf(why, wn, "it is damaged (sizes don't match)");
        return -1;
    }
    sha_init(&s);
    sha_update(&s, b, n - 32);
    sha_final_bin(&s, digest);
    if (memcmp(digest, b + n - 32, 32) != 0) {
        snprintf(why, wn, "it was changed or damaged after it was built (its seal doesn't match)");
        return -1;
    }
    manifest_parse((const char *)b + 16, mlen, m);
    *code = b + 20 + mlen;
    *ncode = clen;
    return 0;
}

/* ---- the console window (programs started from the desktop) ----------------------- */
#define CON_W     720
#define CON_H     440
#define CON_LINES 400

static struct {
    struct omni_client_conn c;
    int open, failed;
    char *line[CON_LINES];
    int nline;
    char partial[1024];
    char input[512];
    int reading;
    uint32_t *px;
    struct raster r;
} g_con;

static void con_draw(void)
{
    int rows = (CON_H - 20) / 18, first, i, y;
    if (!g_con.open)
        return;
    raster_fill(&g_con.r, 0, 0, CON_W, CON_H, 0xff0c0f14);
    first = g_con.nline + 1 > rows ? g_con.nline + 1 - rows : 0;
    for (i = first, y = 10; i <= g_con.nline; i++, y += 18) {
        char tmp[1600];
        const char *s;
        if (i < g_con.nline) {
            s = g_con.line[i % CON_LINES] ? g_con.line[i % CON_LINES] : "";
        } else {
            snprintf(tmp, sizeof(tmp), "%s%s%s", g_con.partial, g_con.reading ? g_con.input : "",
                     g_con.reading ? "_" : "");
            s = tmp;
        }
        {
            const char *p = s;
            int x = 12;
            while (*p && x < CON_W - 8) {
                uint32_t cp = omni_utf8_next(&p);
                const unsigned char *g = omni_glyph8(cp < 128 ? cp : '?');
                int gx, gy;
                for (gy = 0; gy < 16; gy++)
                    for (gx = 0; gx < 8; gx++)
                        if ((g[gy / 2] >> gx) & 1)
                            raster_px(&g_con.r, x + gx, y + gy, 0xffd7dde5);
                x += 8;
            }
        }
    }
    omni_client_blit(&g_con.c, 0, 0, CON_W, CON_H, g_con.px, CON_W);
}

static int con_open(void)
{
    char title[128];
    if (g_con.open)
        return 1;
    if (g_con.failed)
        return 0;
    snprintf(title, sizeof(title), "%s - Console", g_man.name[0] ? g_man.name : "Program");
    g_con.px = malloc((size_t)CON_W * CON_H * 4);
    if (!g_con.px || omni_client_open(&g_con.c, title) < 0 ||
        omni_client_window(&g_con.c, title, CON_W, CON_H + OMNI_WM_TITLE_H) < 0) {
        g_con.failed = 1;
        return 0;
    }
    raster_init(&g_con.r, g_con.px, CON_W, CON_H, CON_W);
    g_con.open = 1;
    return 1;
}

static void con_line(const char *s, size_t n)
{
    char *l = malloc(n + 1);
    if (!l)
        return;
    memcpy(l, s, n);
    l[n] = '\0';
    free(g_con.line[g_con.nline % CON_LINES]);
    g_con.line[g_con.nline % CON_LINES] = l;
    g_con.nline++;
}

static void con_write(void *ctx, const char *s, size_t n)
{
    size_t i, start = 0;
    (void)ctx;
    if (!g_gui_console || !con_open()) {
        fwrite(s, 1, n, stdout);
        fflush(stdout);
        return;
    }
    for (i = 0; i < n; i++)
        if (s[i] == '\n') {
            char tmp[2048];
            snprintf(tmp, sizeof(tmp), "%s%.*s", g_con.partial, (int)(i - start), s + start);
            con_line(tmp, strlen(tmp));
            g_con.partial[0] = '\0';
            start = i + 1;
        }
    if (start < n) {
        size_t l = strlen(g_con.partial);
        snprintf(g_con.partial + l, sizeof(g_con.partial) - l, "%.*s", (int)(n - start), s + start);
    }
    con_draw();
}

static void con_err(void *ctx, const char *s, size_t n)
{
    if (!g_gui_console) {
        (void)ctx;
        fwrite(s, 1, n, stderr);
        return;
    }
    con_write(ctx, s, n);
}

/* wait for the console's window to be closed (or Esc) */
static void con_wait_close(void)
{
    if (!g_con.open)
        return;
    con_write(NULL, "\n[The program has finished. Close this window when you are done.]\n", 69);
    for (;;) {
        struct omni_client_event e;
        struct pollfd pf = { g_con.c.fd, POLLIN, 0 };
        int r;
        if (poll(&pf, 1, -1) < 0 && errno != EINTR)
            break;
        while ((r = omni_client_poll(&g_con.c, &e)) > 0)
            if (e.type == 3 || (e.type == 1 && e.pressed && e.key == 1))
                return;
        if (r < 0)
            return;
    }
}

static char *con_read(void *ctx, const char *prompt)
{
    (void)ctx;
    if (!g_gui_console || !con_open()) {
        char buf[4096];
        fputs(prompt, stdout);
        fflush(stdout);
        if (!fgets(buf, sizeof(buf), stdin))
            return NULL;
        buf[strcspn(buf, "\r\n")] = '\0';
        return strdup(buf);
    }
    con_write(NULL, prompt, strlen(prompt));
    g_con.input[0] = '\0';
    g_con.reading = 1;
    con_draw();
    for (;;) {
        struct omni_client_event e;
        struct pollfd pf = { g_con.c.fd, POLLIN, 0 };
        int r;
        if (poll(&pf, 1, -1) < 0 && errno != EINTR)
            return NULL;
        while ((r = omni_client_poll(&g_con.c, &e)) > 0) {
            size_t l = strlen(g_con.input);
            if (e.type == 3) {
                g_con.reading = 0;
                g_con.open = 0;
                return NULL;
            }
            if (e.type != 1 || !e.pressed)
                continue;
            if (e.key == 28) {                         /* Enter */
                char *line = strdup(g_con.input);
                g_con.reading = 0;
                con_write(NULL, g_con.input, strlen(g_con.input));
                con_write(NULL, "\n", 1);
                return line;
            }
            if (e.key == 14) {                         /* Backspace */
                if (l > 0)
                    g_con.input[l - 1] = '\0';
            } else if ((unsigned char)e.text >= 32 && l + 1 < sizeof(g_con.input)) {
                g_con.input[l] = e.text;
                g_con.input[l + 1] = '\0';
            }
            con_draw();
        }
        if (r < 0)
            return NULL;
    }
}

/* ---- permissions ---------------------------------------------------------------------- */
static void perm_store_path(char *out, size_t n)
{
    snprintf(out, n, "%s/.omnios/permissions", g_home);
}

/* stored answer for (program, permission): 1 allow, 0 deny, -1 not asked */
static int perm_stored(int p)
{
    char path[600], line[256];
    FILE *f;
    int ans = -1;
    perm_store_path(path, sizeof(path));
    if (!(f = fopen(path, "r")))
        return -1;
    while (fgets(line, sizeof(line), f)) {
        char h[80], name[24], a[16];
        if (sscanf(line, "%79s %23s %15s", h, name, a) == 3 && !strcmp(h, g_hash) &&
            !strcmp(name, perm_name(p)))
            ans = !strcmp(a, "allow");
    }
    fclose(f);
    return ans;
}

static void perm_save(int p, int allow)
{
    char path[600], dir[560];
    FILE *f;
    snprintf(dir, sizeof(dir), "%s/.omnios", g_home);
    mkdir_p(dir);
    perm_store_path(path, sizeof(path));
    if ((f = fopen(path, "a")) != NULL) {
        fprintf(f, "%s %s %s %s\n", g_hash, perm_name(p), allow ? "allow" : "deny", g_man.name);
        fclose(f);
    }
}

/* a small window: "<name> wants to ..." [Don't allow] [Allow] */
static int ask_window(int p)
{
    struct omni_client_conn c;
    char title[160], line[200];
    int W = 460, H = 190, answer = 0;
    memset(&c, 0, sizeof(c));
    if (omni_client_open(&c, "Permission") < 0)
        return -1;
    snprintf(title, sizeof(title), "Allow %s?", g_man.name);
    if (omni_client_window(&c, title, W, H + OMNI_WM_TITLE_H) < 0) {
        omni_client_close(&c);
        return -1;
    }
    omni_client_clear(&c, 0xffffff);
    omni_client_icon(&c, 24, 24, 48, 0x2563eb, 'V');
    snprintf(line, sizeof(line), "%s wants to %s.", g_man.name, perm_words(p));
    omni_client_ftext(&c, 90, 24, W - 110, 0, 16, 1, 0x1f2328, "Allow this program?");
    omni_client_ftext(&c, 90, 52, W - 110, 0, 13, 0, 0x1f2328, line);
    omni_client_ftext(&c, 90, 74, W - 110, 0, 13, 0, 0x5f6b7a,
                      "OmniOS remembers your answer for this program.");
    omni_client_rfill(&c, W - 250, H - 54, 110, 34, 6, 0xd0d5dc);
    omni_client_rfill(&c, W - 249, H - 53, 108, 32, 5, 0xf6f7f9);
    omni_client_ftext(&c, W - 250, H - 46, 110, 1, 14, 0, 0x1f2328, "Don't allow");
    omni_client_rfill(&c, W - 130, H - 54, 110, 34, 6, 0x2563eb);
    omni_client_ftext(&c, W - 130, H - 46, 110, 1, 14, 1, 0xffffff, "Allow");
    for (;;) {
        struct omni_client_event e;
        struct pollfd pf = { c.fd, POLLIN, 0 };
        int r;
        if (poll(&pf, 1, -1) < 0 && errno != EINTR)
            break;
        while ((r = omni_client_poll(&c, &e)) > 0) {
            if (e.type == 3) { omni_client_close(&c); return 0; }
            if (e.type == 2 && e.pressed && e.y >= H - 54 && e.y < H - 20) {
                if (e.x >= W - 130 && e.x < W - 20) answer = 1;
                else if (e.x >= W - 250 && e.x < W - 140) answer = 0;
                else continue;
                omni_client_close(&c);
                return answer;
            }
            if (e.type == 1 && e.pressed && e.key == 1) { omni_client_close(&c); return 0; }
        }
        if (r < 0)
            break;
    }
    omni_client_close(&c);
    return 0;
}

/* may the program use p? asks once (window from the desktop, else here) */
static int allowed(struct oms_vm *vm, int p)
{
    int ans;
    if (g_granted & p)
        return 1;
    if (g_denied & p)
        return oms_error(vm, "you didn't allow this program to %s", perm_words(p)) == 0;
    if (!(g_man.perms & p)) {
        oms_error(vm, "this program didn't ask to %s (add '#! permissions: %s' at the top)",
                  perm_words(p), perm_name(p));
        return 0;
    }
    if (getenv("OMX_ALLOW") && strstr(getenv("OMX_ALLOW"), perm_name(p)))
        ans = 1;                                       /* tests, the IDE's own runs */
    else if ((ans = perm_stored(p)) < 0) {
        if (getenv("OMNI_DESKTOP") || !isatty(0))
            ans = ask_window(p);
        if (ans < 0) {                                 /* no desktop: ask here */
            char buf[16];
            fprintf(stderr, "%s wants to %s. Allow? [y/N] ", g_man.name, perm_words(p));
            ans = fgets(buf, sizeof(buf), stdin) && (buf[0] == 'y' || buf[0] == 'Y');
        }
        perm_save(p, ans);
    }
    if (ans)
        g_granted |= p;
    else {
        g_denied |= p;
        oms_error(vm, "you didn't allow this program to %s", perm_words(p));
    }
    return ans;
}

/* files: only in the user's folders, /tmp and removable drives */
static int path_ok(struct oms_vm *vm, const char *in, char *out, size_t n)
{
    char abs[PATH_MAX], dir[PATH_MAX], real[PATH_MAX], *slash;
    const char *roots[] = { g_home, "/tmp", "/media", "/mnt", NULL };
    int i;
    if (in[0] == '~' && (in[1] == '/' || !in[1]))
        snprintf(abs, sizeof(abs), "%s%s", g_home, in + 1);
    else if (in[0] == '/')
        snprintf(abs, sizeof(abs), "%s", in);
    else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd)))
            snprintf(cwd, sizeof(cwd), "%s", g_home);
        snprintf(abs, sizeof(abs), "%s/%s", cwd, in);
    }
    /* resolve the folder (it exists), keep the last part */
    snprintf(dir, sizeof(dir), "%s", abs);
    slash = strrchr(dir, '/');
    if (realpath(abs, real) == NULL) {
        if (!slash)
            return oms_error(vm, "bad path '%s'", in) == 0;
        *slash = '\0';
        if (realpath(dir[0] ? dir : "/", real) == NULL)
            return oms_error(vm, "the folder of '%s' doesn't exist", in) == 0;
        snprintf(real + strlen(real), sizeof(real) - strlen(real), "/%s", slash + 1);
    }
    for (i = 0; roots[i]; i++) {
        size_t l = strlen(roots[i]);
        if (l && strncmp(real, roots[i], l) == 0 && (real[l] == '/' || real[l] == '\0')) {
            snprintf(out, n, "%s", real);
            return 1;
        }
    }
    return oms_error(vm, "programs may only use files in your folders, /tmp and drives, "
                     "not '%s'", real) == 0;
}

static int file_arg(struct oms_vm *vm, int argc, oms_value *a, int i, char *out, size_t n)
{
    const char *s;
    if (i >= argc || !(s = oms_str_of(a[i], NULL)))
        return oms_error(vm, "expected a file name") == 0;
    if (!allowed(vm, P_FILES))
        return 0;
    return path_ok(vm, s, out, n);
}

/* ---- files, network, system ---------------------------------------------------------- */
static int n_read(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    char path[PATH_MAX];
    size_t n;
    char *b;
    if (!file_arg(vm, argc, a, 0, path, sizeof(path))) return -1;
    b = slurp(path, &n);
    *ret = b ? oms_str(vm, b, n) : oms_nil();
    free(b);
    return 0;
}

static int write_common(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret, const char *mode)
{
    char path[PATH_MAX], *t;
    FILE *f;
    int ok;
    if (argc != 2) return oms_error(vm, "use %s(file, text)", mode[0] == 'a' ? "append" : "write");
    if (!file_arg(vm, argc, a, 0, path, sizeof(path))) return -1;
    t = oms_to_text(a[1]);
    f = fopen(path, mode);
    ok = f && fputs(t, f) >= 0;
    if (f && fclose(f) != 0) ok = 0;
    free(t);
    *ret = oms_bool(ok);
    return 0;
}

static int n_write(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret) { return write_common(vm, argc, a, ret, "w"); }
static int n_append(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret) { return write_common(vm, argc, a, ret, "a"); }

static int n_exists(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    char path[PATH_MAX];
    if (!file_arg(vm, argc, a, 0, path, sizeof(path))) return -1;
    *ret = oms_bool(access(path, F_OK) == 0);
    return 0;
}

static int n_listdir(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    char path[PATH_MAX];
    DIR *d;
    struct dirent *de;
    oms_value l;
    if (!file_arg(vm, argc, a, 0, path, sizeof(path))) return -1;
    if (!(d = opendir(path))) { *ret = oms_nil(); return 0; }
    l = oms_list_new(vm);
    while ((de = readdir(d)) != NULL)
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
            oms_list_push(vm, l, oms_cstr(vm, de->d_name));
    closedir(d);
    *ret = l;
    return 0;
}

static int n_mkdir(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    char path[PATH_MAX];
    if (!file_arg(vm, argc, a, 0, path, sizeof(path))) return -1;
    *ret = oms_bool(mkdir(path, 0755) == 0 || errno == EEXIST);
    return 0;
}

static int n_remove(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    char path[PATH_MAX];
    if (!file_arg(vm, argc, a, 0, path, sizeof(path))) return -1;
    *ret = oms_bool(remove(path) == 0);
    return 0;
}

/* run argv, return its output (at most 16 MB) */
static char *capture(char *const argv[], size_t *n, int *status)
{
    int pfd[2];
    pid_t pid;
    char *b = NULL;
    size_t cap = 0, len = 0;
    ssize_t r;
    if (pipe(pfd) < 0)
        return NULL;
    pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return NULL;
    }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        dup2(pfd[1], 1);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 2); }
        close(pfd[0]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    for (;;) {
        if (len + 65536 + 1 > cap) {
            char *nb = realloc(b, cap = cap ? cap * 2 : 65536 * 2);
            if (!nb) break;
            b = nb;
        }
        r = read(pfd[0], b + len, cap - len - 1);
        if (r <= 0)
            break;
        len += (size_t)r;
        if (len > (size_t)16 << 20)
            break;
    }
    close(pfd[0]);
    waitpid(pid, status, 0);
    if (b)
        b[len] = '\0';
    *n = len;
    return b;
}

static int n_fetch(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    const char *url = argc > 0 ? oms_str_of(a[0], NULL) : NULL;
    const char *w = getenv("OMNI_WGET");
    char *argv[12], *b;
    size_t n = 0;
    int st = 0, k = 0;
    if (!url) return oms_error(vm, "use fetch(url)");
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))
        return oms_error(vm, "only http:// and https:// addresses");
    if (!allowed(vm, P_NETWORK)) return -1;
    if (!w || !*w)
        w = access("/bin/busybox", X_OK) == 0 ? "/bin/busybox" : "wget";
    argv[k++] = (char *)w;
    if (strstr(w, "busybox"))
        argv[k++] = "wget";
    argv[k++] = "-q";
    argv[k++] = "-T";
    argv[k++] = "25";
    argv[k++] = "-O";
    argv[k++] = "-";
    argv[k++] = (char *)url;
    argv[k] = NULL;
    b = capture(argv, &n, &st);
    *ret = (b && WIFEXITED(st) && WEXITSTATUS(st) == 0) ? oms_str(vm, b, n) : oms_nil();
    free(b);
    return 0;
}

static int n_run(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    const char *cmd = argc > 0 ? oms_str_of(a[0], NULL) : NULL;
    char *argv[4], *b;
    size_t n = 0;
    int st = 0;
    if (!cmd) return oms_error(vm, "use run(command)");
    if (!allowed(vm, P_SYSTEM)) return -1;
    argv[0] = "/bin/sh";
    argv[1] = "-c";
    argv[2] = (char *)cmd;
    argv[3] = NULL;
    b = capture(argv, &n, &st);
    *ret = b ? oms_str(vm, b, n) : oms_nil();
    free(b);
    return 0;
}

/* ---- windows and drawing --------------------------------------------------------------- */
static struct {
    struct omni_client_conn c;
    int open, closed, told, dirty, w, h;    /* told: the close was delivered */
    uint32_t *px;
    struct raster r;
} g_win;

struct img_cache { char path[PATH_MAX]; int w, h; uint32_t *px; };
static struct img_cache g_img[16];
static int g_nimg;

static int num_arg(struct oms_vm *vm, int argc, oms_value *a, int i, double *out, double dflt)
{
    if (i >= argc) {
        if (isnan(dflt))
            return oms_error(vm, "missing value %d", i + 1);
        *out = dflt;
        return 0;
    }
    if (a[i].t != OMS_NUM)
        return oms_error(vm, "value %d must be a number", i + 1);
    *out = a[i].u.n;
    return 0;
}

static int need_win(struct oms_vm *vm)
{
    if (!g_win.open)
        return oms_error(vm, "open a window first: window(\"title\", width, height)");
    return 0;
}

static int n_window(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    const char *title = argc > 0 ? oms_str_of(a[0], NULL) : NULL;
    double w, h;
    if (!title) return oms_error(vm, "use window(\"title\", width, height)");
    if (num_arg(vm, argc, a, 1, &w, 480) || num_arg(vm, argc, a, 2, &h, 360)) return -1;
    if (w < 80) w = 80;
    if (h < 60) h = 60;
    if (w > 1600) w = 1600;
    if (h > 1000) h = 1000;
    if (g_win.open) {                                   /* again: a new title */
        omni_client_title(&g_win.c, title);
        *ret = oms_bool(1);
        return 0;
    }
    if (omni_client_open(&g_win.c, title) < 0)
        return oms_error(vm, "there is no desktop to open a window on");
    if (omni_client_window(&g_win.c, title, (int)w, (int)h + OMNI_WM_TITLE_H) < 0)
        return oms_error(vm, "the desktop didn't open the window");
    omni_client_want_pointer(&g_win.c);
    g_win.w = (int)w;
    g_win.h = (int)h;
    g_win.px = malloc((size_t)g_win.w * (size_t)g_win.h * 4);
    if (!g_win.px) return oms_error(vm, "out of memory");
    raster_init(&g_win.r, g_win.px, g_win.w, g_win.h, g_win.w);
    raster_fill(&g_win.r, 0, 0, g_win.w, g_win.h, 0xffffffff);
    g_win.open = 1;
    g_win.dirty = 1;
    *ret = oms_bool(1);
    return 0;
}

static uint32_t col(double c) { return (uint32_t)(long)c & 0xffffff; }

static int n_clear(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    double c;
    if (need_win(vm) || num_arg(vm, argc, a, 0, &c, 0xffffff)) return -1;
    raster_fill(&g_win.r, 0, 0, g_win.w, g_win.h, 0xff000000u | col(c));
    g_win.dirty = 1;
    *ret = oms_nil();
    return 0;
}

static int n_rect(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    double x, y, w, h, c;
    if (need_win(vm) || num_arg(vm, argc, a, 0, &x, NAN) || num_arg(vm, argc, a, 1, &y, NAN) ||
        num_arg(vm, argc, a, 2, &w, NAN) || num_arg(vm, argc, a, 3, &h, NAN) ||
        num_arg(vm, argc, a, 4, &c, 0)) return -1;
    raster_fill(&g_win.r, (int)x, (int)y, (int)w, (int)h, 0xff000000u | col(c));
    g_win.dirty = 1;
    *ret = oms_nil();
    return 0;
}

static int n_box(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    double x, y, w, h, c;
    if (need_win(vm) || num_arg(vm, argc, a, 0, &x, NAN) || num_arg(vm, argc, a, 1, &y, NAN) ||
        num_arg(vm, argc, a, 2, &w, NAN) || num_arg(vm, argc, a, 3, &h, NAN) ||
        num_arg(vm, argc, a, 4, &c, 0)) return -1;
    raster_rect(&g_win.r, (int)x, (int)y, (int)w, (int)h, 0xff000000u | col(c));
    g_win.dirty = 1;
    *ret = oms_nil();
    return 0;
}

static int n_rrect(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    double x, y, w, h, r, c;
    if (need_win(vm) || num_arg(vm, argc, a, 0, &x, NAN) || num_arg(vm, argc, a, 1, &y, NAN) ||
        num_arg(vm, argc, a, 2, &w, NAN) || num_arg(vm, argc, a, 3, &h, NAN) ||
        num_arg(vm, argc, a, 4, &r, 8) || num_arg(vm, argc, a, 5, &c, 0)) return -1;
    if (w > 0 && h > 0) {
        if (r > w / 2) r = w / 2;
        if (r > h / 2) r = h / 2;
        th_round_rect(&g_win.r, (int)x, (int)y, (int)w, (int)h, r < 0 ? 0 : (int)r, col(c), 255);
    }
    g_win.dirty = 1;
    *ret = oms_nil();
    return 0;
}

static int n_circle(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    double cx, cy, r, c;
    int x, y;
    if (need_win(vm) || num_arg(vm, argc, a, 0, &cx, NAN) || num_arg(vm, argc, a, 1, &cy, NAN) ||
        num_arg(vm, argc, a, 2, &r, NAN) || num_arg(vm, argc, a, 3, &c, 0)) return -1;
    if (r > 2000) r = 2000;
    for (y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
        for (x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            double dx = x + 0.5 - cx, dy = y + 0.5 - cy, d = sqrt(dx * dx + dy * dy);
            double cov = r + 0.5 - d;
            if (cov > 0)
                th_px(&g_win.r, x, y, col(c), cov >= 1 ? 255 : (unsigned)(cov * 255));
        }
    g_win.dirty = 1;
    *ret = oms_nil();
    return 0;
}

static int n_line(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    double x0, y0, x1, y1, c, wd;
    int x, y, xa, xb, ya, yb;
    double dx, dy, l2;
    if (need_win(vm) || num_arg(vm, argc, a, 0, &x0, NAN) || num_arg(vm, argc, a, 1, &y0, NAN) ||
        num_arg(vm, argc, a, 2, &x1, NAN) || num_arg(vm, argc, a, 3, &y1, NAN) ||
        num_arg(vm, argc, a, 4, &c, 0) || num_arg(vm, argc, a, 5, &wd, 1.5)) return -1;
    if (wd < 0.5) wd = 0.5;
    if (wd > 100) wd = 100;
    xa = (int)(fmin(x0, x1) - wd - 1); xb = (int)(fmax(x0, x1) + wd + 1);
    ya = (int)(fmin(y0, y1) - wd - 1); yb = (int)(fmax(y0, y1) + wd + 1);
    if (xa < 0) xa = 0;
    if (ya < 0) ya = 0;
    if (xb > g_win.w) xb = g_win.w;
    if (yb > g_win.h) yb = g_win.h;
    dx = x1 - x0; dy = y1 - y0; l2 = dx * dx + dy * dy;
    for (y = ya; y < yb; y++)
        for (x = xa; x < xb; x++) {
            double px = x + 0.5 - x0, py = y + 0.5 - y0, t = l2 > 0 ? (px * dx + py * dy) / l2 : 0;
            double ex, ey, cov;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            ex = px - t * dx; ey = py - t * dy;
            cov = wd / 2 + 0.5 - sqrt(ex * ex + ey * ey);
            if (cov > 0)
                th_px(&g_win.r, x, y, col(c), cov >= 1 ? 255 : (unsigned)(cov * 255));
        }
    g_win.dirty = 1;
    *ret = oms_nil();
    return 0;
}

static int n_text(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    double x, y, c, size, bold;
    char *t;
    int w;
    if (need_win(vm) || num_arg(vm, argc, a, 0, &x, NAN) || num_arg(vm, argc, a, 1, &y, NAN)) return -1;
    if (argc < 3) return oms_error(vm, "use text(x, y, \"text\", color, size)");
    if (num_arg(vm, argc, a, 3, &c, 0x1f2328) || num_arg(vm, argc, a, 4, &size, 16) ||
        num_arg(vm, argc, a, 5, &bold, 0)) return -1;
    t = oms_to_text(a[2]);
    w = font_draw(&g_win.r, t, (int)x, (int)y, (int)size, bold != 0 ? FONT_BOLD : FONT_REGULAR, col(c));
    free(t);
    g_win.dirty = 1;
    *ret = oms_num(w);
    return 0;
}

static int n_textw(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    double size, bold;
    char *t;
    if (argc < 1) return oms_error(vm, "use textw(\"text\", size)");
    if (num_arg(vm, argc, a, 1, &size, 16) || num_arg(vm, argc, a, 2, &bold, 0)) return -1;
    t = oms_to_text(a[0]);
    *ret = oms_num(font_width(t, (int)size, bold != 0 ? FONT_BOLD : FONT_REGULAR));
    free(t);
    return 0;
}

static int n_image(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    const char *in = argc > 0 ? oms_str_of(a[0], NULL) : NULL;
    char path[PATH_MAX];
    double x, y, w, h;
    struct img_cache *im = NULL;
    int i, j;
    if (need_win(vm)) return -1;
    if (!in) return oms_error(vm, "use image(\"file.png\", x, y, width, height)");
    if (num_arg(vm, argc, a, 1, &x, 0) || num_arg(vm, argc, a, 2, &y, 0) ||
        num_arg(vm, argc, a, 3, &w, -1) || num_arg(vm, argc, a, 4, &h, -1)) return -1;
    /* pictures in your folders or OmniOS's own; reading them needs no permission */
    if (strncmp(in, "/usr/share/omnios/", 18) == 0)
        snprintf(path, sizeof(path), "%s", in);
    else if (!path_ok(vm, in, path, sizeof(path)))
        return -1;
    for (i = 0; i < g_nimg; i++)
        if (!strcmp(g_img[i].path, path))
            im = &g_img[i];
    if (!im) {
        int iw, ih, comp;
        unsigned char *px = stbi_load(path, &iw, &ih, &comp, 4);
        if (!px) return oms_error(vm, "can't read the picture '%s'", in);
        if (g_nimg == 16) { free(g_img[0].px); memmove(g_img, g_img + 1, sizeof(g_img[0]) * 15); g_nimg--; }
        im = &g_img[g_nimg++];
        snprintf(im->path, sizeof(im->path), "%s", path);
        im->w = iw;
        im->h = ih;
        im->px = malloc((size_t)iw * (size_t)ih * 4);
        for (j = 0; j < iw * ih && im->px; j++)
            im->px[j] = (uint32_t)px[j * 4 + 3] << 24 | (uint32_t)px[j * 4] << 16 |
                        (uint32_t)px[j * 4 + 1] << 8 | px[j * 4 + 2];
        stbi_image_free(px);
    }
    if (w <= 0) w = im->w;
    if (h <= 0) h = im->h;
    for (j = 0; j < (int)h; j++)
        for (i = 0; i < (int)w; i++) {
            int sx = (int)((long)i * im->w / (int)w), sy = (int)((long)j * im->h / (int)h);
            uint32_t p = im->px ? im->px[(size_t)sy * (size_t)im->w + (size_t)sx] : 0;
            th_px(&g_win.r, (int)x + i, (int)y + j, p & 0xffffff, p >> 24);
        }
    g_win.dirty = 1;
    *ret = oms_nil();
    return 0;
}

static int n_present(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    (void)argc; (void)a;
    if (need_win(vm)) return -1;
    if (!g_win.closed)
        omni_client_blit(&g_win.c, 0, 0, g_win.w, g_win.h, g_win.px, g_win.w);
    g_win.dirty = 0;
    *ret = oms_nil();
    return 0;
}

static const char *key_name(int k)
{
    switch (k) {
    case 103: return "up";      case 108: return "down";
    case 105: return "left";    case 106: return "right";
    case 28:  return "enter";   case 57:  return "space";
    case 1:   return "escape";  case 14:  return "backspace";
    case 15:  return "tab";     case 111: return "delete";
    default:  return "";
    }
}

/* the next event (waits up to ms; no value = until one comes), or nil */
static int n_event(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    double ms;
    struct omni_client_event e;
    oms_value m;
    long deadline;
    struct timespec ts;
    if (need_win(vm) || num_arg(vm, argc, a, 0, &ms, -1)) return -1;
    if (g_win.dirty && !g_win.closed) {                /* show what was drawn */
        omni_client_blit(&g_win.c, 0, 0, g_win.w, g_win.h, g_win.px, g_win.w);
        g_win.dirty = 0;
    }
    if (g_win.closed) {
        /* the user closed the window: say so once, then the program ends */
        if (g_win.told) {
            oms_exit(vm, 0);
            *ret = oms_nil();
            return 0;
        }
        g_win.told = 1;
        m = oms_map_new(vm);
        oms_map_set(vm, m, "type", oms_cstr(vm, "close"));
        *ret = m;
        return 0;
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    deadline = (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + (long)ms;
    for (;;) {
        int r = omni_client_poll(&g_win.c, &e), left;
        if (r < 0) { g_win.closed = 1; e.type = 3; r = 1; }
        if (r > 0) {
            const char *type = e.type == 1 ? (e.pressed ? "key" : NULL) : e.type == 2 ? (e.pressed ? "click" : NULL)
                             : e.type == 3 ? "close" : e.type == 4 ? "wheel" : e.type == 5 ? "move" : NULL;
            if (!type)
                continue;
            if (e.type == 3)
                g_win.closed = g_win.told = 1;
            m = oms_map_new(vm);
            oms_map_set(vm, m, "type", oms_cstr(vm, type));
            if (e.type == 1) {
                char ch[2] = { (unsigned char)e.text >= 32 ? e.text : 0, 0 };
                oms_map_set(vm, m, "key", oms_num(e.key));
                oms_map_set(vm, m, "name", oms_cstr(vm, ch[0] ? (e.key == 57 ? "space" : ch) : key_name(e.key)));
                oms_map_set(vm, m, "char", oms_cstr(vm, ch));
            } else if (e.type == 2 || e.type == 4 || e.type == 5) {
                oms_map_set(vm, m, "x", oms_num(e.x));
                oms_map_set(vm, m, "y", oms_num(e.y));
                if (e.type == 2) oms_map_set(vm, m, "button", oms_num(e.key));
                if (e.type == 4) oms_map_set(vm, m, "dir", oms_cstr(vm, e.key == 4 ? "up" : e.key == 5 ? "down" : e.key == 6 ? "left" : "right"));
            }
            *ret = m;
            return 0;
        }
        clock_gettime(CLOCK_MONOTONIC, &ts);
        left = ms < 0 ? -1 : (int)(deadline - ((long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000));
        if (ms >= 0 && left <= 0) {
            *ret = oms_nil();
            return 0;
        }
        {
            struct pollfd pf = { g_win.c.fd, POLLIN, 0 };
            if (poll(&pf, 1, left) < 0 && errno != EINTR) {
                *ret = oms_nil();
                return 0;
            }
        }
    }
}

static int n_rgb(struct oms_vm *vm, int argc, oms_value *a, oms_value *ret)
{
    double r, g, b;
    if (num_arg(vm, argc, a, 0, &r, NAN) || num_arg(vm, argc, a, 1, &g, NAN) || num_arg(vm, argc, a, 2, &b, NAN))
        return -1;
#define C8(v) ((v) < 0 ? 0 : (v) > 255 ? 255 : (int)(v))
    *ret = oms_num((double)(C8(r) << 16 | C8(g) << 8 | C8(b)));
#undef C8
    return 0;
}

static void define_host(struct oms_vm *vm, int argc, char **argv)
{
    static const struct { const char *n; oms_native f; } host[] = {
        { "read", n_read }, { "write", n_write }, { "append", n_append }, { "exists", n_exists },
        { "listdir", n_listdir }, { "mkdir", n_mkdir }, { "remove", n_remove },
        { "fetch", n_fetch }, { "run", n_run }, { "window", n_window }, { "clear", n_clear },
        { "rect", n_rect }, { "box", n_box }, { "rrect", n_rrect }, { "circle", n_circle },
        { "line", n_line }, { "text", n_text }, { "textw", n_textw }, { "image", n_image },
        { "present", n_present }, { "event", n_event }, { "rgb", n_rgb },
    };
    oms_value l = oms_list_new(vm);
    size_t i;
    int k;
    for (i = 0; i < sizeof(host) / sizeof(host[0]); i++)
        oms_define(vm, host[i].n, host[i].f);
    for (k = 0; k < argc; k++)
        oms_list_push(vm, l, oms_cstr(vm, argv[k]));
    oms_set_global(vm, "args", l);
    oms_set_global(vm, "HOME", oms_cstr(vm, g_home));
}

/* ---- commands -------------------------------------------------------------------------- */
static int defender_ok(const char *path)
{
    pid_t pid;
    int st = 0;
    if (getenv("OMX_NO_SCAN") || access("/usr/bin/omnios-defender", X_OK) != 0)
        return 1;
    pid = fork();
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        execl("/usr/bin/omnios-defender", "omnios-defender", "--scan", path, (char *)NULL);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &st, 0) < 0)
        return 1;
    return !(WIFEXITED(st) && WEXITSTATUS(st) == 1);
}

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (g_gui_console) {
        con_err(NULL, msg, strlen(msg));
        con_err(NULL, "\n", 1);
        con_wait_close();
    } else {
        fprintf(stderr, "omx: %s\n", msg);
    }
}

static int load_program(struct oms_vm *vm, const char *path)
{
    size_t n;
    char *b = slurp(path, &n), err[700];
    if (!b) {
        fail("can't open %s: %s", path, strerror(errno));
        return -1;
    }
    if (n >= 8 && memcmp(b, OMX_MAGIC, 8) == 0) {      /* an executable */
        const unsigned char *code;
        size_t ncode;
        if (omx_parse((unsigned char *)b, n, &g_man, &code, &ncode, err, sizeof(err)) < 0) {
            fail("OmniOS won't run %s: %s.", path, err);
            free(b);
            return -1;
        }
        sha256_hex(b, n, g_hash);
        oms_set_name(vm, g_man.source[0] ? g_man.source : path);
        if (oms_load(vm, code, ncode, err, sizeof(err)) < 0) {
            fail("can't run %s: %s.", path, err);
            free(b);
            return -1;
        }
    } else {                                            /* source */
        manifest_from_source(b, path, &g_man);
        sha256_hex(b, n, g_hash);
        if (oms_compile(vm, b, path, err, sizeof(err)) < 0) {
            fail("%s", err);
            free(b);
            return -1;
        }
    }
    free(b);
    return 0;
}

static int cmd_run(const char *path, int argc, char **argv)
{
    struct oms_io io = { con_write, con_err, con_read, NULL };
    int r;
    if (!defender_ok(path)) {
        fail("OmniOS Defender found a threat in %s and stopped it.", path);
        return 1;
    }
    g_vm = oms_new();
    if (!g_vm)
        return 1;
    oms_set_io(g_vm, &io);
    if (load_program(g_vm, path) < 0)
        return 1;
    define_host(g_vm, argc, argv);
    r = oms_run(g_vm);
    /* a window it drew in stays until the user closes it */
    if (g_win.open && !g_win.closed) {
        if (g_win.dirty)
            omni_client_blit(&g_win.c, 0, 0, g_win.w, g_win.h, g_win.px, g_win.w);
        for (;;) {
            struct omni_client_event e;
            struct pollfd pf = { g_win.c.fd, POLLIN, 0 };
            int k;
            if (poll(&pf, 1, -1) < 0 && errno != EINTR)
                break;
            while ((k = omni_client_poll(&g_win.c, &e)) > 0)
                if (e.type == 3)
                    goto done;
            if (k < 0)
                break;
        }
    }
done:
    if (g_gui_console && g_con.open)
        con_wait_close();
    return r;
}

static int cmd_build(const char *src, const char *out_path)
{
    char out[PATH_MAX], err[700], mtext[1024];
    struct oms_vm *vm = oms_new();
    unsigned char *code, *file, hdr[16];
    size_t ncode, n, mlen;
    char *b = slurp(src, &n);
    struct sha256 s;
    unsigned char digest[32];
    FILE *f;
    if (!b) {
        fprintf(stderr, "omx: can't open %s: %s\n", src, strerror(errno));
        return 1;
    }
    manifest_from_source(b, src, &g_man);
    if (oms_compile(vm, b, src, err, sizeof(err)) < 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    oms_save(vm, &code, &ncode);
    manifest_text(&g_man, mtext, sizeof(mtext));
    mlen = strlen(mtext);
    if (out_path)
        snprintf(out, sizeof(out), "%s", out_path);
    else {
        char *dot;
        snprintf(out, sizeof(out), "%s", src);
        dot = strrchr(out, '.');
        if (dot && !strchr(dot, '/'))
            *dot = '\0';
        snprintf(out + strlen(out), sizeof(out) - strlen(out), ".omx");
    }
    n = 20 + mlen + ncode + 32;
    file = malloc(n);
    memcpy(hdr, OMX_MAGIC, 8);
    put32(hdr + 8, OMX_VERSION);
    put32(hdr + 12, (uint32_t)mlen);
    memcpy(file, hdr, 16);
    memcpy(file + 16, mtext, mlen);
    put32(file + 16 + mlen, (uint32_t)ncode);
    memcpy(file + 20 + mlen, code, ncode);
    sha_init(&s);
    sha_update(&s, file, n - 32);
    sha_final_bin(&s, digest);
    memcpy(file + n - 32, digest, 32);
    f = fopen(out, "wb");
    if (!f || fwrite(file, 1, n, f) != n || fclose(f) != 0) {
        fprintf(stderr, "omx: can't write %s: %s\n", out, strerror(errno));
        return 1;
    }
    chmod(out, 0755);
    printf("Built %s (%zu bytes): %s\n", out, n, g_man.name);
    free(file);
    free(code);
    free(b);
    oms_free(vm);
    return 0;
}

static int cmd_check(const char *src)
{
    char err[700];
    size_t n;
    char *b = slurp(src, &n);
    struct oms_vm *vm = oms_new();
    int r;
    if (!b) {
        fprintf(stderr, "omx: can't open %s\n", src);
        return 1;
    }
    r = oms_compile(vm, b, src, err, sizeof(err));
    if (r < 0)
        fprintf(stderr, "%s\n", err);
    else
        printf("%s: no errors\n", src);
    free(b);
    oms_free(vm);
    return r < 0 ? 1 : 0;
}

static int cmd_info(const char *path)
{
    size_t n, ncode;
    char *b = slurp(path, &n), err[300], hex[65];
    const unsigned char *code;
    int k;
    if (!b) {
        fprintf(stderr, "omx: can't open %s\n", path);
        return 1;
    }
    if (omx_parse((unsigned char *)b, n, &g_man, &code, &ncode, err, sizeof(err)) < 0) {
        printf("%s: %s\n", path, err);
        return 1;
    }
    sha256_hex(b, n, hex);
    printf("OmniOS executable: %s\n  name: %s\n", path, g_man.name);
    if (g_man.version[0]) printf("  version: %s\n", g_man.version);
    if (g_man.author[0]) printf("  author: %s\n", g_man.author);
    printf("  permissions:");
    if (!g_man.perms) printf(" none");
    for (k = 1; k <= P_SYSTEM; k <<= 1)
        if (g_man.perms & k) printf(" %s", perm_name(k));
    printf("\n  program: %zu bytes of bytecode\n  seal: intact (SHA-256 %s)\n", ncode, hex);
    free(b);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "omx: OmniOS's program runner\n"
        "  omx run FILE [ARGS]        run a program (.omx) or OmniScript source (.oms)\n"
        "  omx build SRC.oms [-o OUT] build an OmniOS executable\n"
        "  omx check SRC.oms          look for mistakes without running\n"
        "  omx info FILE.omx          what a program is and what it may do\n");
}

int main(int argc, char **argv)
{
    const char *home = getenv("HOME");
    snprintf(g_home, sizeof(g_home), "%s", home && *home && strcmp(home, "/") ? home : "/root");
    signal(SIGPIPE, SIG_IGN);
    font_init();
    g_gui_console = getenv("OMNI_DESKTOP") && !(getenv("OMX_CONSOLE") && !strcmp(getenv("OMX_CONSOLE"), "0"));
    if (argc < 2) {
        usage();
        return 2;
    }
    if (!strcmp(argv[1], "run") && argc >= 3)
        return cmd_run(argv[2], argc - 2, argv + 2);
    if (!strcmp(argv[1], "build") && argc >= 3)
        return cmd_build(argv[2], argc >= 5 && !strcmp(argv[3], "-o") ? argv[4] : NULL);
    if (!strcmp(argv[1], "check") && argc >= 3)
        return cmd_check(argv[2]);
    if (!strcmp(argv[1], "info") && argc >= 3)
        return cmd_info(argv[2]);
    if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
        usage();
        return 0;
    }
    /* omx FILE [ARGS]: the kernel's way (binfmt_misc), or by hand */
    return cmd_run(argv[1], argc - 1, argv + 1);
}
