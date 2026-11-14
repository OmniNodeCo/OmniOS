/*
 * OmniOS — os/apps/defender.c
 *
 * OmniOS Defender: antivirus for OmniOS. It looks for known threats by
 * their SHA-256 and by byte patterns (destructive scripts, fork bombs,
 * reverse shells, crypto miners, the EICAR test file), checks the seal of
 * OmniOS programs (.omx), and moves what it finds into quarantine, from
 * where it can be restored or deleted. Real-time protection watches
 * Downloads, Desktop, Documents, the home folder and /tmp, and checks
 * every file written there. Definitions update from this project's
 * GitHub releases (omnios-defs.txt), like OmniOS itself.
 *
 *   omnios-defender                 the app (status, scans, quarantine)
 *   omnios-defender --scan PATH...  scan; exit 0 clean, 1 threats found
 *                                   (and quarantined), 2 error
 *   omnios-defender --quick | --full   the same scans as the app
 *   omnios-defender --daemon        real-time protection (started by init)
 *   omnios-defender --update        fetch the latest definitions
 *   omnios-defender --list          what is in quarantine
 *   omnios-defender --restore ID | --delete ID
 *
 * Files: /var/lib/omnios/defender/{defs.txt,status,settings,quarantine/}
 * (OMNI_DEFENDER_DIR overrides it, for tests).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "../gui/client.h"
#include "../gui/font.h"
#include "../gui/theme.h"
#include "../gui/wm.h"
#include "../lib/sha256.h"

#define DEF_DIR   "/var/lib/omnios/defender"
#define MAX_SIG   512
#define MAX_FILE  (64L << 20)       /* bigger files are skipped           */
#define DEFS_VERSION_BUILTIN "2026.11.1"

enum { SIG_HASH, SIG_BYTES };

struct sig {
    int  kind;
    char name[64];
    unsigned char *bytes;           /* SIG_BYTES: the pattern             */
    size_t n;
    char hash[65];                  /* SIG_HASH                           */
};

static struct sig g_sig[MAX_SIG];
static int g_nsig;
static char g_defs_version[32] = DEFS_VERSION_BUILTIN;
static char g_dir[512], g_home[512];

/* Built-in definitions. Patterns use \xNN for any byte. */
static const char *const g_builtin[] = {
    "text EICAR-Test-File X5O!P%@AP[4\\x5cPZX54(P^)7CC)7}$EICAR-STANDARD-ANTIVIRUS-TEST-FILE!$H+H*",
    "text Linux.ForkBomb :(){ :|:& };:",
    "text Linux.ForkBomb :(){:|:&};:",
    "text Script.Destructive.RmRoot rm -rf --no-preserve-root /",
    "text Script.Destructive.RmRoot rm -rf /*",
    "text Script.Destructive.RmRoot rm -rf / ",
    "text Script.Destructive.DiskWipe of=/dev/sda",
    "text Script.Destructive.DiskWipe of=/dev/vda",
    "text Script.Destructive.DiskWipe of=/dev/nvme0n1",
    "text Shell.Reverse.Bash bash -i >& /dev/tcp/",
    "text Shell.Reverse.Bash sh -i >& /dev/tcp/",
    "text Shell.Reverse.Netcat nc -e /bin/sh",
    "text Shell.Reverse.Netcat nc -e /bin/bash",
    "text Miner.Stratum stratum+tcp://",
    "text Miner.XMRig xmrig --donate-level",
    "text Script.Persistence.Cron * * * * * curl ",
    NULL,
};

/* ---- definitions ------------------------------------------------------------------- */
static int unescape(const char *s, unsigned char *out, size_t cap)
{
    size_t n = 0;
    while (*s && n < cap) {
        if (s[0] == '\\' && s[1] == 'x' && isxdigit((unsigned char)s[2]) && isxdigit((unsigned char)s[3])) {
            unsigned v;
            sscanf(s + 2, "%2x", &v);
            out[n++] = (unsigned char)v;
            s += 4;
        } else {
            out[n++] = (unsigned char)*s++;
        }
    }
    return (int)n;
}

static void add_def_line(const char *line)
{
    char kind[16], name[64];
    const char *rest;
    struct sig *g;
    int k = 0;
    if (g_nsig >= MAX_SIG || line[0] == '#' || !line[0])
        return;
    if (!strncmp(line, "version=", 8)) {
        snprintf(g_defs_version, sizeof(g_defs_version), "%.31s", line + 8);
        g_defs_version[strcspn(g_defs_version, "\r\n")] = '\0';
        return;
    }
    if (sscanf(line, "%15s %63s %n", kind, name, &k) < 2 || !k)
        return;
    rest = line + k;
    g = &g_sig[g_nsig];
    memset(g, 0, sizeof(*g));
    if (!strcmp(kind, "sha256") || !strcmp(kind, "hash")) {
        /* "sha256 NAME HEX" */
        if (strlen(rest) < 64)
            return;
        g->kind = SIG_HASH;
        snprintf(g->hash, sizeof(g->hash), "%.64s", rest);
    } else if (!strcmp(kind, "text") || !strcmp(kind, "hex")) {
        unsigned char buf[512];
        int n;
        if (!strcmp(kind, "hex")) {
            n = 0;
            while (rest[0] && rest[1] && n < (int)sizeof(buf)) {
                unsigned v;
                if (sscanf(rest, "%2x", &v) != 1)
                    break;
                buf[n++] = (unsigned char)v;
                rest += 2;
                while (*rest == ' ')
                    rest++;
            }
        } else {
            char tmp[600];
            snprintf(tmp, sizeof(tmp), "%s", rest);
            tmp[strcspn(tmp, "\r\n")] = '\0';
            n = unescape(tmp, buf, sizeof(buf));
        }
        if (n < 4)
            return;                                   /* too short to be safe */
        g->kind = SIG_BYTES;
        g->bytes = malloc((size_t)n);
        if (!g->bytes)
            return;
        memcpy(g->bytes, buf, (size_t)n);
        g->n = (size_t)n;
    } else {
        return;
    }
    snprintf(g->name, sizeof(g->name), "%s", name);
    g_nsig++;
}

static void load_defs(void)
{
    char path[600], line[1024];
    FILE *f;
    int i;
    for (i = 0; g_builtin[i]; i++)
        add_def_line(g_builtin[i]);
    snprintf(path, sizeof(path), "%s/defs.txt", g_dir);
    if ((f = fopen(path, "r")) != NULL) {
        while (fgets(line, sizeof(line), f))
            add_def_line(line);
        fclose(f);
    }
}

/* ---- scanning ------------------------------------------------------------------------ */
static const unsigned char OMX_MAGIC[8] = { 0x89, 'O', 'M', 'X', '\r', '\n', 0x1a, '\n' };

struct scan {
    long files, threats, skipped;
    int  quarantine;               /* move threats away               */
    int  report;                   /* print F/T lines (the app reads) */
    volatile int stop;
};

static void status_write(const struct scan *s, const char *kind)
{
    char path[600], tmp[620];
    FILE *f;
    snprintf(path, sizeof(path), "%s/status", g_dir);
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if ((f = fopen(tmp, "w")) != NULL) {
        fprintf(f, "last_scan=%ld\nscan_kind=%s\nfiles=%ld\nthreats=%ld\ndefs=%s\n",
                (long)time(NULL), kind, s->files, s->threats, g_defs_version);
        fclose(f);
        rename(tmp, path);
    }
}

static int quarantine_file(const char *path, const char *threat, char *id_out, size_t idn)
{
    char qdir[600], dst[700], info[720];
    FILE *f;
    struct stat st;
    long id = (long)time(NULL) * 1000 + (long)(getpid() % 1000);
    snprintf(qdir, sizeof(qdir), "%s/quarantine", g_dir);
    mkdir(g_dir, 0700);
    mkdir(qdir, 0700);
    for (;; id++) {
        snprintf(dst, sizeof(dst), "%s/%ld", qdir, id);
        if (access(dst, F_OK) != 0)
            break;
    }
    if (lstat(path, &st) != 0)
        return -1;
    {                                                  /* where it was, in full */
        char abs[PATH_MAX];
        if (realpath(path, abs))
            path = strdupa(abs);
    }
    if (rename(path, dst) != 0) {                      /* another filesystem: copy */
        FILE *in = fopen(path, "rb"), *out = in ? fopen(dst, "wb") : NULL;
        char buf[65536];
        size_t n;
        int ok = in && out;
        while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0)
            ok = fwrite(buf, 1, n, out) == n;
        if (in) fclose(in);
        if (out && fclose(out) != 0) ok = 0;
        if (!ok || unlink(path) != 0) {
            unlink(dst);
            return -1;
        }
    }
    chmod(dst, 0);
    snprintf(info, sizeof(info), "%s.info", dst);
    if ((f = fopen(info, "w")) != NULL) {
        fprintf(f, "path=%s\nthreat=%s\ndate=%ld\nsize=%ld\nmode=%o\n", path, threat,
                (long)time(NULL), (long)st.st_size, (unsigned)(st.st_mode & 07777));
        fclose(f);
    }
    if (id_out)
        snprintf(id_out, idn, "%ld", id);
    return 0;
}

/* what threat, if any, the file holds; returns its name or NULL */
static const char *check_file(const char *path, char *why, size_t wn)
{
    struct stat st;
    unsigned char *b;
    FILE *f;
    size_t n;
    char hex[65];
    int i;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0)
        return NULL;
    if (st.st_size > MAX_FILE)
        return NULL;
    if (!(f = fopen(path, "rb")))
        return NULL;
    b = malloc((size_t)st.st_size);
    n = b ? fread(b, 1, (size_t)st.st_size, f) : 0;
    fclose(f);
    if (!b)
        return NULL;
    sha256_hex(b, n, hex);
    for (i = 0; i < g_nsig; i++) {
        const struct sig *g = &g_sig[i];
        if ((g->kind == SIG_HASH && !strcasecmp(g->hash, hex)) ||
            (g->kind == SIG_BYTES && n >= g->n && memmem(b, n, g->bytes, g->n))) {
            snprintf(why, wn, "%s", g->name);
            free(b);
            return why;
        }
    }
    /* OmniOS programs: the seal must match */
    if (n >= 52 && !memcmp(b, OMX_MAGIC, 8)) {
        struct sha256 s;
        unsigned char d[32];
        sha_init(&s);
        sha_update(&s, b, n - 32);
        sha_final_bin(&s, d);
        if (memcmp(d, b + n - 32, 32) != 0) {
            snprintf(why, wn, "OMX.Tampered");
            free(b);
            return why;
        }
    }
    free(b);
    return NULL;
}

static int skip_dir(const char *p)
{
    static const char *const no[] = { "/proc", "/sys", "/dev", "/run", NULL };
    int i;
    for (i = 0; no[i]; i++)
        if (!strcmp(p, no[i]))
            return 1;
    /* its own folder: the definitions and the quarantine hold the patterns */
    return !strcmp(p, g_dir) || strstr(p, "/defender/quarantine") != NULL;
}

/* Defender's own program holds its patterns too */
static int is_self(const struct stat *st)
{
    static struct stat me;
    static int known = -1;
    if (known < 0)
        known = stat("/proc/self/exe", &me) == 0;
    return known && st->st_ino == me.st_ino && st->st_dev == me.st_dev;
}

static void scan_path(struct scan *s, const char *path, int depth)
{
    struct stat st;
    if (s->stop || depth > 24 || lstat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d;
        struct dirent *de;
        if (skip_dir(path) || !(d = opendir(path)))
            return;
        while (!s->stop && (de = readdir(d)) != NULL) {
            char sub[PATH_MAX];
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            snprintf(sub, sizeof(sub), "%s%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/", de->d_name);
            scan_path(s, sub, depth + 1);
        }
        closedir(d);
        return;
    }
    if (!S_ISREG(st.st_mode) || is_self(&st))
        return;
    s->files++;
    if (s->report && s->files % 25 == 1) {
        printf("F %ld %s\n", s->files, path);
        fflush(stdout);
    }
    {
        char why[80];
        const char *t = check_file(path, why, sizeof(why));
        if (t) {
            char id[32] = "";
            s->threats++;
            if (s->quarantine && quarantine_file(path, t, id, sizeof(id)) == 0) {
                if (s->report) printf("T %s\t%s\t%s\n", t, path, id);
                else printf("Threat: %s in %s (quarantined)\n", t, path);
            } else {
                if (s->report) printf("T %s\t%s\t\n", t, path);
                else printf("Threat: %s in %s\n", t, path);
            }
            fflush(stdout);
        }
    }
}

static void quick_paths(const char **out, char bufs[][600], int *n)
{
    static const char *const sub[] = { "Downloads", "Desktop", "Documents", NULL };
    int i;
    *n = 0;
    for (i = 0; sub[i]; i++) {
        snprintf(bufs[i], 600, "%s/%s", g_home, sub[i]);
        out[(*n)++] = bufs[i];
    }
    out[(*n)++] = "/tmp";
}

/* ---- definitions update (GitHub releases) ------------------------------------------- */
static int defs_url(char *out, size_t n)
{
    FILE *f = fopen("/etc/omnios-update.conf", "r");
    char line[600];
    snprintf(out, n, "https://github.com/OmniNodeCo/OmniOS/releases/latest/download/omnios-defs.txt");
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        char *p;
        if (strncmp(line, "url=", 4))
            continue;
        line[strcspn(line, "\r\n")] = '\0';
        if ((p = strstr(line + 4, "omnios-update.txt")) != NULL) {
            *p = '\0';
            snprintf(out, n, "%somnios-defs.txt", line + 4);
        }
    }
    fclose(f);
    return 0;
}

static int update_defs(int verbose)
{
    char url[600], tmp[640], dst[620];
    const char *w = getenv("OMNI_WGET");
    pid_t pid;
    int st = 0;
    FILE *f;
    char line[256];
    int lines = 0, ok = 0;
    defs_url(url, sizeof(url));
    mkdir(g_dir, 0700);
    snprintf(tmp, sizeof(tmp), "%s/defs.txt.download", g_dir);
    snprintf(dst, sizeof(dst), "%s/defs.txt", g_dir);
    if (!w || !*w)
        w = access("/bin/busybox", X_OK) == 0 ? "/bin/busybox" : "wget";
    pid = fork();
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        if (strstr(w, "busybox"))
            execl(w, "busybox", "wget", "-q", "-T", "30", "-O", tmp, url, (char *)NULL);
        else
            execlp(w, "wget", "-q", "-T", "30", "-O", tmp, url, (char *)NULL);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        unlink(tmp);
        if (verbose)
            fprintf(stderr, "Couldn't download definitions from %s\n", url);
        return -1;
    }
    /* sanity: a definitions file starts with its header */
    if ((f = fopen(tmp, "r")) != NULL) {
        while (fgets(line, sizeof(line), f) && lines < 5000) {
            if (lines == 0 && strncmp(line, "# OmniOS Defender definitions", 29) == 0)
                ok = 1;
            lines++;
        }
        fclose(f);
    }
    if (!ok) {
        unlink(tmp);
        if (verbose)
            fprintf(stderr, "The downloaded definitions aren't valid\n");
        return -1;
    }
    rename(tmp, dst);
    if (verbose)
        printf("Definitions updated (%d lines) from %s\n", lines, url);
    return 0;
}

/* ---- settings ---------------------------------------------------------------------------- */
static int realtime_on(void)
{
    char path[600], line[64];
    FILE *f;
    int on = 1;
    snprintf(path, sizeof(path), "%s/settings", g_dir);
    if ((f = fopen(path, "r")) != NULL) {
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, "realtime=", 9))
                on = atoi(line + 9) != 0;
        fclose(f);
    }
    return on;
}

static void set_realtime(int on)
{
    char path[600];
    FILE *f;
    mkdir(g_dir, 0700);
    snprintf(path, sizeof(path), "%s/settings", g_dir);
    if ((f = fopen(path, "w")) != NULL) {
        fprintf(f, "realtime=%d\n", on);
        fclose(f);
    }
}

/* ---- quarantine ---------------------------------------------------------------------------- */
struct qitem { char id[32], path[512], threat[64]; long date; };

static int q_list(struct qitem *it, int max)
{
    char qdir[600];
    DIR *d;
    struct dirent *de;
    int n = 0;
    snprintf(qdir, sizeof(qdir), "%s/quarantine", g_dir);
    if (!(d = opendir(qdir)))
        return 0;
    while ((de = readdir(d)) != NULL && n < max) {
        char info[900], line[700];
        FILE *f;
        size_t l = strlen(de->d_name);
        if (l < 6 || strcmp(de->d_name + l - 5, ".info"))
            continue;
        snprintf(info, sizeof(info), "%s/%s", qdir, de->d_name);
        if (!(f = fopen(info, "r")))
            continue;
        memset(&it[n], 0, sizeof(it[n]));
        snprintf(it[n].id, sizeof(it[n].id), "%.*s", (int)(l - 5), de->d_name);
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = '\0';
            if (!strncmp(line, "path=", 5)) snprintf(it[n].path, sizeof(it[n].path), "%s", line + 5);
            else if (!strncmp(line, "threat=", 7)) snprintf(it[n].threat, sizeof(it[n].threat), "%s", line + 7);
            else if (!strncmp(line, "date=", 5)) it[n].date = atol(line + 5);
        }
        fclose(f);
        n++;
    }
    closedir(d);
    return n;
}

static int q_restore(const char *id, int del)
{
    char qdir[600], file[700], info[720], line[700], orig[512] = "";
    FILE *f;
    unsigned mode = 0644;
    if (strchr(id, '/') || strstr(id, ".."))
        return -1;
    snprintf(qdir, sizeof(qdir), "%s/quarantine", g_dir);
    snprintf(file, sizeof(file), "%s/%s", qdir, id);
    snprintf(info, sizeof(info), "%s.info", file);
    if ((f = fopen(info, "r")) != NULL) {
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = '\0';
            if (!strncmp(line, "path=", 5)) snprintf(orig, sizeof(orig), "%s", line + 5);
            if (!strncmp(line, "mode=", 5)) sscanf(line + 5, "%o", &mode);
        }
        fclose(f);
    }
    if (del) {
        unlink(file);
        unlink(info);
        return 0;
    }
    if (!orig[0] || rename(file, orig) != 0)
        return -1;
    chmod(orig, mode & 0777);
    unlink(info);
    return 0;
}

/* ---- the alert window ------------------------------------------------------------------------ */
static int alert(const char *file, const char *threat)
{
    struct omni_client_conn c;
    int W = 440, H = 150;
    char line[600];
    const char *base = strrchr(file, '/');
    memset(&c, 0, sizeof(c));
    if (omni_client_open(&c, "OmniOS Defender") < 0)
        return 1;
    if (omni_client_window(&c, "OmniOS Defender", W, H + OMNI_WM_TITLE_H) < 0)
        return 1;
    omni_client_clear(&c, 0xffffff);
    omni_client_rfill(&c, 0, 0, 6, H, 0, 0xdc2626);
    omni_client_icon(&c, 24, 22, 48, 0x1d4ed8, 'V');
    omni_client_ftext(&c, 88, 20, W - 100, 0, 16, 1, 0x1f2328, "Threat found and blocked");
    snprintf(line, sizeof(line), "%s was moved to quarantine.", base ? base + 1 : file);
    omni_client_ftext(&c, 88, 48, W - 100, 0, 13, 0, 0x1f2328, line);
    snprintf(line, sizeof(line), "Threat: %s", threat);
    omni_client_ftext(&c, 88, 70, W - 100, 0, 13, 0, 0x5f6b7a, line);
    omni_client_rfill(&c, W - 120, H - 48, 100, 32, 6, 0x2563eb);
    omni_client_ftext(&c, W - 120, H - 41, 100, 1, 14, 1, 0xffffff, "OK");
    for (;;) {
        struct omni_client_event e;
        struct pollfd pf = { c.fd, POLLIN, 0 };
        int r;
        if (poll(&pf, 1, 30000) <= 0)
            break;                                     /* goes away by itself */
        while ((r = omni_client_poll(&c, &e)) > 0)
            if (e.type == 3 || (e.type == 2 && e.pressed) || (e.type == 1 && e.pressed))
                goto out;
        if (r < 0)
            break;
    }
out:
    omni_client_close(&c);
    return 0;
}

/* ---- real-time protection -------------------------------------------------------------------- */
static int daemon_main(void)
{
    static const char *const sub[] = { "", "/Downloads", "/Desktop", "/Documents", NULL };
    int fd = inotify_init1(IN_CLOEXEC), i;
    time_t last_update = 0, started = time(NULL);
    char buf[8192] __attribute__((aligned(8)));
    struct { int wd; char dir[600]; } w[8];
    int nw = 0;
    signal(SIGCHLD, SIG_IGN);
    if (fd < 0)
        return 2;
    for (i = 0; sub[i]; i++) {
        snprintf(w[nw].dir, sizeof(w[nw].dir), "%s%s", g_home, sub[i]);
        mkdir(w[nw].dir, 0755);
        w[nw].wd = inotify_add_watch(fd, w[nw].dir, IN_CLOSE_WRITE | IN_MOVED_TO);
        if (w[nw].wd >= 0)
            nw++;
    }
    snprintf(w[nw].dir, sizeof(w[nw].dir), "/tmp");
    if ((w[nw].wd = inotify_add_watch(fd, "/tmp", IN_CLOSE_WRITE | IN_MOVED_TO)) >= 0)
        nw++;
    for (;;) {
        struct pollfd pf = { fd, POLLIN, 0 };
        time_t now = time(NULL);
        /* definitions: two minutes after start, then once a day */
        if ((last_update == 0 && now - started > 120) || (last_update && now - last_update > 86400)) {
            last_update = now;
            if (update_defs(0) == 0) {
                for (i = 0; i < g_nsig; i++)
                    free(g_sig[i].bytes);
                g_nsig = 0;
                load_defs();
            }
        }
        if (poll(&pf, 1, 30000) <= 0)
            continue;
        {
            ssize_t n = read(fd, buf, sizeof(buf)), off = 0;
            while (n > 0 && off < n) {
                struct inotify_event *ev = (struct inotify_event *)(buf + off);
                off += (ssize_t)sizeof(*ev) + ev->len;
                if (!ev->len || !realtime_on() || !strncmp(ev->name, ".omni", 5))
                    continue;                          /* the desktop's own files */
                for (i = 0; i < nw; i++)
                    if (w[i].wd == ev->wd) {
                        char path[PATH_MAX], why[80];
                        const char *t;
                        snprintf(path, sizeof(path), "%s/%s", w[i].dir, ev->name);
                        if ((t = check_file(path, why, sizeof(why))) != NULL) {
                            char id[32];
                            if (quarantine_file(path, t, id, sizeof(id)) == 0) {
                                pid_t pid = fork();
                                if (pid == 0) {
                                    execl("/proc/self/exe", "omnios-defender", "--alert", path, t, (char *)NULL);
                                    _exit(0);
                                }
                            }
                        }
                    }
            }
        }
    }
    return 0;
}

/* ---- the app ------------------------------------------------------------------------------------ */
static struct omni_client_conn g_c;
static int W = 760, H = 560;
static uint32_t *g_fb;
static struct raster g_r;
static int g_page;                  /* 0 status, 1 quarantine */
static pid_t g_scan_pid;
static int g_scan_fd = -1;
static long g_scan_files, g_scan_threats;
static char g_scan_now[512], g_scan_kind[16], g_msg[256];
static struct qitem g_q[64];
static int g_nq, g_hover = -1;

static void fillr(int x, int y, int w, int h, uint32_t c)
{
    raster_fill(&g_r, x, y, w, h, 0xff000000u | c);
}

static void status_read(long *when, long *files, long *threats, char *kind, size_t kn)
{
    char path[600], line[128];
    FILE *f;
    *when = *files = *threats = 0;
    kind[0] = '\0';
    snprintf(path, sizeof(path), "%s/status", g_dir);
    if (!(f = fopen(path, "r")))
        return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        if (!strncmp(line, "last_scan=", 10)) *when = atol(line + 10);
        else if (!strncmp(line, "files=", 6)) *files = atol(line + 6);
        else if (!strncmp(line, "threats=", 8)) *threats = atol(line + 8);
        else if (!strncmp(line, "scan_kind=", 10)) snprintf(kind, kn, "%s", line + 10);
    }
    fclose(f);
}

struct btn { int x, y, w, h, id; const char *label; int primary; };
static struct btn g_btn[80];
static int g_nbtn;

static void button(int x, int y, int w, int h, int id, const char *label, int primary)
{
    struct btn *b;
    if (g_nbtn >= 80)
        return;
    b = &g_btn[g_nbtn++];
    b->x = x; b->y = y; b->w = w; b->h = h; b->id = id; b->label = label; b->primary = primary;
    if (primary) {
        th_round_rect(&g_r, x, y, w, h, 6, g_hover == id ? 0x1d4ed8 : 0x2563eb, 255);
    } else {
        th_round_rect(&g_r, x, y, w, h, 6, 0xd0d7de, 255);
        th_round_rect(&g_r, x + 1, y + 1, w - 2, h - 2, 5, g_hover == id ? 0xf0f3f6 : 0xffffff, 255);
    }
    font_draw(&g_r, label, x + (w - font_width(label, 13, primary ? FONT_BOLD : FONT_REGULAR)) / 2,
              y + (h - 16) / 2, 13, primary ? FONT_BOLD : FONT_REGULAR, primary ? 0xffffff : 0x1f2328);
}

static void card(int x, int y, int w, int h)
{
    th_round_rect(&g_r, x, y, w, h, 8, 0xe1e4e8, 255);
    th_round_rect(&g_r, x + 1, y + 1, w - 2, h - 2, 7, 0xffffff, 255);
}

static void shield(int cx, int cy, int s, uint32_t c, int ok)
{
    if (ok) {                                          /* the Defender shield */
        th_icon(&g_r, cx - s / 2, cy - s / 2, s, c, 'V');
        return;
    }
    th_round_rect(&g_r, cx - s / 2, cy - s / 2, s, s, s / 2, c, 255);   /* a warning */
    font_draw(&g_r, "!", cx - font_width("!", s * 2 / 3, FONT_BOLD) / 2, cy - s / 2 + s / 8,
              s * 2 / 3, FONT_BOLD, 0xffffff);
}

static void draw(void)
{
    long when, files, threats;
    char kind[16], s[300];
    int rt = realtime_on(), x = 220, cw = W - 220 - 24, y;
    g_nbtn = 0;
    fillr(0, 0, W, H, 0xf6f7f9);
    /* side navigation */
    fillr(0, 0, 196, H, 0xeef0f3);
    th_icon(&g_r, 20, 20, 32, 0x1d4ed8, 'V');
    font_draw(&g_r, "OmniOS", 62, 18, 13, FONT_REGULAR, 0x5f6b7a);
    font_draw(&g_r, "Defender", 62, 33, 16, FONT_BOLD, 0x1f2328);
    {
        const char *nav[2] = { "Protection", "Quarantine" };
        int i;
        for (i = 0; i < 2; i++) {
            int ny = 84 + i * 40;
            if (g_page == i)
                th_round_rect(&g_r, 10, ny, 176, 34, 6, 0xdde3ea, 255);
            if (g_page == i)
                fillr(10, ny + 9, 3, 16, 0x2563eb);
            font_draw(&g_r, nav[i], 26, ny + 9, 14, FONT_REGULAR, 0x1f2328);
            g_btn[g_nbtn].x = 10; g_btn[g_nbtn].y = ny; g_btn[g_nbtn].w = 176; g_btn[g_nbtn].h = 34;
            g_btn[g_nbtn].id = 100 + i; g_btn[g_nbtn].label = ""; g_nbtn++;
        }
    }
    font_draw(&g_r, "Definitions", 20, H - 52, 12, FONT_REGULAR, 0x5f6b7a);
    font_draw(&g_r, g_defs_version, 20, H - 34, 13, FONT_BOLD, 0x1f2328);

    status_read(&when, &files, &threats, kind, sizeof(kind));
    g_nq = q_list(g_q, 64);
    if (g_page == 0) {
        int ok = rt && g_nq == 0, busy = g_scan_pid > 0;
        y = 24;
        font_draw(&g_r, "Protection", x, y, 22, FONT_REGULAR, 0x1f2328);
        y += 46;
        card(x, y, cw, 120);
        shield(x + 56, y + 60, 64, ok ? 0x16a34a : 0xd97706, ok);
        font_draw(&g_r, busy ? "Scanning..." : ok ? "Your device is protected" : g_nq ? "Threats in quarantine"
                  : "Real-time protection is off", x + 104, y + 26, 18, FONT_BOLD, 0x1f2328);
        if (busy) {
            snprintf(s, sizeof(s), "%ld files checked, %ld threats", g_scan_files, g_scan_threats);
            font_draw(&g_r, s, x + 104, y + 56, 13, FONT_REGULAR, 0x5f6b7a);
            font_draw_clip(&g_r, g_scan_now, x + 104, y + 78, 12, FONT_REGULAR, 0x8b949e, cw - 130);
        } else {
            if (when) {
                struct tm tmv;
                time_t t = (time_t)when;
                char ts[64];
                localtime_r(&t, &tmv);
                strftime(ts, sizeof(ts), "%b %d, %H:%M", &tmv);
                snprintf(s, sizeof(s), "Last scan: %s %s, %ld files, %ld threats", kind[0] ? kind : "", ts, files, threats);
            } else {
                snprintf(s, sizeof(s), "No scan yet. Run a quick scan to check your files.");
            }
            font_draw_clip(&g_r, s, x + 104, y + 56, 13, FONT_REGULAR, 0x5f6b7a, cw - 130);
            if (g_nq) {
                snprintf(s, sizeof(s), "%d item%s in quarantine: see Quarantine", g_nq, g_nq == 1 ? "" : "s");
                font_draw(&g_r, s, x + 104, y + 78, 13, FONT_REGULAR, 0xb45309);
            }
        }
        y += 136;
        card(x, y, cw, 96);
        font_draw(&g_r, "Scan", x + 18, y + 14, 15, FONT_BOLD, 0x1f2328);
        font_draw(&g_r, "Quick: Downloads, Desktop, Documents, /tmp. Full: every file.", x + 18, y + 36, 12,
                  FONT_REGULAR, 0x5f6b7a);
        if (busy)
            button(x + 18, y + 56, 110, 30, 3, "Stop scan", 0);
        else {
            button(x + 18, y + 56, 110, 30, 1, "Quick scan", 1);
            button(x + 136, y + 56, 100, 30, 2, "Full scan", 0);
        }
        y += 112;
        card(x, y, cw, 64);
        font_draw(&g_r, "Real-time protection", x + 18, y + 12, 15, FONT_BOLD, 0x1f2328);
        font_draw(&g_r, "Checks files as they are saved or downloaded", x + 18, y + 34, 12, FONT_REGULAR, 0x5f6b7a);
        {
            int tx = x + cw - 70, ty = y + 20;
            th_round_rect(&g_r, tx, ty, 44, 22, 11, rt ? 0x2563eb : 0x8c959f, 255);
            th_round_rect(&g_r, rt ? tx + 25 : tx + 3, ty + 3, 16, 16, 8, 0xffffff, 255);
            g_btn[g_nbtn].x = tx; g_btn[g_nbtn].y = ty; g_btn[g_nbtn].w = 44; g_btn[g_nbtn].h = 22;
            g_btn[g_nbtn].id = 4; g_btn[g_nbtn].label = ""; g_nbtn++;
        }
        y += 80;
        card(x, y, cw, 64);
        font_draw(&g_r, "Definitions", x + 18, y + 12, 15, FONT_BOLD, 0x1f2328);
        snprintf(s, sizeof(s), "Version %s, %d signatures. Updates come from GitHub every day.", g_defs_version, g_nsig);
        font_draw_clip(&g_r, s, x + 18, y + 34, 12, FONT_REGULAR, 0x5f6b7a, cw - 170);
        button(x + cw - 150, y + 16, 132, 30, 5, "Check for updates", 0);
        if (g_msg[0])
            font_draw_clip(&g_r, g_msg, x, H - 30, 13, FONT_REGULAR, 0x1f2328, cw);
    } else {
        int i;
        y = 24;
        font_draw(&g_r, "Quarantine", x, y, 22, FONT_REGULAR, 0x1f2328);
        font_draw(&g_r, "Threats are kept here, locked away, until you restore or delete them.", x, y + 34, 12,
                  FONT_REGULAR, 0x5f6b7a);
        y += 64;
        if (g_nq == 0) {
            card(x, y, cw, 70);
            font_draw(&g_r, "Nothing in quarantine.", x + 18, y + 26, 14, FONT_REGULAR, 0x5f6b7a);
        }
        for (i = 0; i < g_nq && y + 66 < H - 50; i++, y += 72) {
            card(x, y, cw, 64);
            font_draw_clip(&g_r, g_q[i].threat, x + 18, y + 10, 14, FONT_BOLD, 0xb91c1c, cw - 230);
            font_draw_clip(&g_r, g_q[i].path, x + 18, y + 34, 12, FONT_REGULAR, 0x5f6b7a, cw - 230);
            button(x + cw - 196, y + 17, 84, 30, 200 + i, "Restore", 0);
            button(x + cw - 104, y + 17, 86, 30, 300 + i, "Delete", 1);
        }
        if (g_nq > 1)
            button(x, H - 46, 130, 30, 6, "Delete all", 0);
    }
    omni_client_blit(&g_c, 0, 0, W, H, g_fb, W);
}

static void start_scan(int full)
{
    int pfd[2];
    pid_t pid;
    if (g_scan_pid > 0 || pipe(pfd) < 0)
        return;
    pid = fork();
    if (pid == 0) {
        dup2(pfd[1], 1);
        close(pfd[0]);
        execl("/proc/self/exe", "omnios-defender", full ? "--full" : "--quick", "--report", (char *)NULL);
        _exit(2);
    }
    close(pfd[1]);
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);
    g_scan_pid = pid;
    g_scan_fd = pfd[0];
    g_scan_files = g_scan_threats = 0;
    g_scan_now[0] = '\0';
    snprintf(g_scan_kind, sizeof(g_scan_kind), "%s", full ? "full" : "quick");
    g_msg[0] = '\0';
}

static void scan_poll(void)
{
    static char part[2048];
    char buf[4096];
    ssize_t n;
    int st;
    if (g_scan_fd < 0)
        return;
    while ((n = read(g_scan_fd, buf, sizeof(buf) - 1)) > 0) {
        char *line, *nl;
        buf[n] = '\0';
        strncat(part, buf, sizeof(part) - strlen(part) - 1);
        line = part;
        while ((nl = strchr(line, '\n')) != NULL) {
            *nl = '\0';
            if (line[0] == 'F') {
                long k = atol(line + 2);
                char *p = strchr(line + 2, ' ');
                g_scan_files = k;
                snprintf(g_scan_now, sizeof(g_scan_now), "%s", p ? p + 1 : "");
            } else if (line[0] == 'T') {
                g_scan_threats++;
            } else if (line[0] == 'D') {
                g_scan_files = atol(line + 2);
            }
            line = nl + 1;
        }
        memmove(part, line, strlen(line) + 1);
    }
    if (g_scan_pid > 0 && waitpid(g_scan_pid, &st, WNOHANG) == g_scan_pid) {
        g_scan_pid = 0;
        close(g_scan_fd);
        g_scan_fd = -1;
        part[0] = '\0';
        snprintf(g_msg, sizeof(g_msg), g_scan_threats ? "Scan finished: %ld threat%s moved to quarantine."
                 : "Scan finished: no threats found.", g_scan_threats, g_scan_threats == 1 ? "" : "s");
    }
}

static void click(int x, int y)
{
    int i;
    for (i = 0; i < g_nbtn; i++) {
        struct btn *b = &g_btn[i];
        if (x < b->x || x >= b->x + b->w || y < b->y || y >= b->y + b->h)
            continue;
        if (b->id == 100 || b->id == 101) g_page = b->id - 100;
        else if (b->id == 1 || b->id == 2) start_scan(b->id == 2);
        else if (b->id == 3 && g_scan_pid > 0) {
            kill(g_scan_pid, SIGTERM);
            snprintf(g_msg, sizeof(g_msg), "Scan stopped.");
        } else if (b->id == 4) set_realtime(!realtime_on());
        else if (b->id == 5) {
            int r = update_defs(0);
            int k;
            for (k = 0; k < g_nsig; k++)
                free(g_sig[k].bytes);
            g_nsig = 0;
            load_defs();
            snprintf(g_msg, sizeof(g_msg), r == 0 ? "Definitions are up to date (%s)."
                     : "Couldn't reach GitHub for new definitions; the current ones (%s) stay.", g_defs_version);
        } else if (b->id == 6) {
            int k;
            for (k = 0; k < g_nq; k++)
                q_restore(g_q[k].id, 1);
        } else if (b->id >= 200 && b->id < 300 && b->id - 200 < g_nq) {
            if (q_restore(g_q[b->id - 200].id, 0) != 0)
                snprintf(g_msg, sizeof(g_msg), "Couldn't restore it to its folder.");
        } else if (b->id >= 300 && b->id < 400 && b->id - 300 < g_nq) {
            q_restore(g_q[b->id - 300].id, 1);
        }
        return;
    }
}

static int app_main(void)
{
    if (omni_client_open(&g_c, "OmniOS Defender") < 0)
        return 127;
    if (omni_client_window(&g_c, "OmniOS Defender", W, H + OMNI_WM_TITLE_H) < 0)
        return 127;
    omni_client_want_pointer(&g_c);
    g_fb = malloc((size_t)W * (size_t)H * 4);
    if (!g_fb)
        return 1;
    raster_init(&g_r, g_fb, W, H, W);
    draw();
    for (;;) {
        struct omni_client_event e;
        struct pollfd pf[2];
        int n = 1, r, redraw = 0;
        pf[0].fd = g_c.fd;
        pf[0].events = POLLIN;
        if (g_scan_fd >= 0) {
            pf[1].fd = g_scan_fd;
            pf[1].events = POLLIN;
            n = 2;
        }
        if (poll(pf, (nfds_t)n, g_scan_pid > 0 ? 250 : 5000) < 0 && errno != EINTR)
            break;
        while ((r = omni_client_poll(&g_c, &e)) > 0) {
            if (e.type == 3) {
                if (g_scan_pid > 0) kill(g_scan_pid, SIGTERM);
                return 0;
            }
            if (e.type == 2 && e.pressed) { click(e.x, e.y); redraw = 1; }
            if (e.type == 1 && e.pressed && e.key == 1) return 0;
            if (e.type == 5) {
                int i, h = -1;
                for (i = 0; i < g_nbtn; i++)
                    if (e.x >= g_btn[i].x && e.x < g_btn[i].x + g_btn[i].w &&
                        e.y >= g_btn[i].y && e.y < g_btn[i].y + g_btn[i].h)
                        h = g_btn[i].id;
                if (h != g_hover) { g_hover = h; redraw = 1; }
            }
        }
        if (r < 0)
            break;
        if (g_scan_fd >= 0 || g_scan_pid > 0) {
            scan_poll();
            redraw = 1;
        }
        if (redraw || g_scan_pid > 0)
            draw();
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *home = getenv("HOME"), *dir = getenv("OMNI_DEFENDER_DIR");
    struct scan s;
    int i, report = 0;
    snprintf(g_home, sizeof(g_home), "%s", home && *home && strcmp(home, "/") ? home : "/root");
    snprintf(g_dir, sizeof(g_dir), "%s", dir && *dir ? dir : DEF_DIR);
    signal(SIGPIPE, SIG_IGN);
    load_defs();
    memset(&s, 0, sizeof(s));
    s.quarantine = 1;
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--report"))
            report = 1;
    s.report = report;
    if (argc >= 3 && !strcmp(argv[1], "--scan")) {
        for (i = 2; i < argc; i++)
            if (strcmp(argv[i], "--report"))
                scan_path(&s, argv[i], 0);
        if (!report)
            printf("%ld file%s checked, %ld threat%s\n", s.files, s.files == 1 ? "" : "s",
                   s.threats, s.threats == 1 ? "" : "s");
        return s.threats ? 1 : 0;
    }
    if (argc >= 2 && (!strcmp(argv[1], "--quick") || !strcmp(argv[1], "--full"))) {
        int full = !strcmp(argv[1], "--full");
        if (full) {
            scan_path(&s, "/", 0);
        } else {
            const char *paths[8];
            char bufs[4][600];
            int n, k;
            quick_paths(paths, bufs, &n);
            for (k = 0; k < n; k++)
                scan_path(&s, paths[k], 0);
        }
        status_write(&s, full ? "full" : "quick");
        if (report)
            printf("D %ld\n", s.files);
        else
            printf("%s scan: %ld files checked, %ld threats\n", full ? "Full" : "Quick", s.files, s.threats);
        return s.threats ? 1 : 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "--daemon"))
        return daemon_main();
    if (argc >= 2 && !strcmp(argv[1], "--update"))
        return update_defs(1) == 0 ? 0 : 2;
    if (argc >= 2 && !strcmp(argv[1], "--list")) {
        struct qitem q[64];
        int n = q_list(q, 64), k;
        if (!n)
            printf("Nothing in quarantine.\n");
        for (k = 0; k < n; k++)
            printf("%s  %s  %s\n", q[k].id, q[k].threat, q[k].path);
        return 0;
    }
    if (argc >= 3 && (!strcmp(argv[1], "--restore") || !strcmp(argv[1], "--delete")))
        return q_restore(argv[2], !strcmp(argv[1], "--delete")) == 0 ? 0 : 2;
    if (argc >= 4 && !strcmp(argv[1], "--alert"))
        return alert(argv[2], argv[3]);
    if (argc >= 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        printf("omnios-defender [--scan PATH... | --quick | --full | --daemon | --update | --list |"
               " --restore ID | --delete ID]\n");
        return 0;
    }
    font_init();
    return app_main();
}
