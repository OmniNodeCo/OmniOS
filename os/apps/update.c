/*
 * OmniOS — os/apps/update.c
 *
 * omnios-update: OmniOS Update. Like Windows Update it looks for a newer
 * OmniOS on its own, downloads it in the background and installs it when
 * the computer restarts.
 *
 *   omnios-update daemon    started by init: waits for the network, checks
 *                           a minute after startup, then every 6 hours
 *   omnios-update check     check now (Settings: "Check for updates")
 *   omnios-update install   download and prepare the newest release
 *   omnios-update restart   init, just before restarting into an update:
 *                           refresh the state that travels with it
 *
 * With "Get updates automatically" on (Settings > OmniOS Update, the
 * default), a check that finds a newer release downloads it straight away.
 * "Pause updates" stops the automatic checks for a week at a time (up to
 * five), as on Windows; a check asked for by hand still runs.
 * Every release publishes omnios-update.txt next to its ISO:
 *
 *     version=2026.2.3
 *     kernel=omnios-bzImage-2026.2.3
 *     size=26214400
 *     sha256=<64 hex digits>
 *
 * and that kernel image, which carries the whole OS (the root file system
 * is built into it). The download must match the size and the SHA-256 and
 * be a bzImage; it is then loaded with kexec_file_load(), so init's next
 * restart boots straight into it (reboot(RB_KEXEC)) instead of going back
 * through the firmware to the old ISO. A small initramfs rides along: the
 * settings, the installed apps, the password and the update history, so the
 * new version starts where the old one left off.
 *
 * OmniOS runs from RAM: an installed update lasts until the computer is
 * switched off. After a cold start from the old ISO, OmniOS Update fetches
 * it again (or use the new ISO from the release page).
 *
 * HTTPS goes through BusyBox wget and its own TLS code (which needs
 * patches/busybox-tls-p256.patch to talk to GitHub). BusyBox does not check
 * certificates, so the SHA-256 protects against damaged downloads but not
 * against an attacker who controls the network path.
 *
 * The status the desktop and Settings show is in updstat.h. Tests use
 * OMNI_UPDATE_DIR, OMNI_UPDATE_URL, OMNI_RELEASE_FILE, OMNI_SETTINGS,
 * OMNI_WGET (a wget or busybox binary) and OMNI_UPDATE_NO_KEXEC=1.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "../gui/catalog.h"
#include "../gui/settings.h"
#include "../gui/updstat.h"

#define DEFAULT_FEED  "https://github.com/OmniNodeCo/OmniOS/releases/latest/download/omnios-update.txt"
#define FEED_CONF     "/etc/omnios-update.conf"
#define FIRST_DELAY   60                /* s after the network comes up   */
#define CHECK_EVERY   (6 * 3600)        /* s between automatic checks     */
#define RETRY_ERROR   (30 * 60)         /* s before retrying a failure    */
#define DOWNLOAD_MAX  (45 * 60)         /* s a download may take at most  */
/* the kernel's built-in command line (CONFIG_CMDLINE): the new kernel adds
 * its own copy, so it is not passed along twice */
#define BUILTIN_CMDLINE "console=ttyS0,115200n8 console=tty1 quiet " \
                        "driver_async_probe=e1000,e1000e,vmwgfx,bochs-drm"

#ifndef KEXEC_FILE_NO_INITRAMFS
#define KEXEC_FILE_NO_INITRAMFS 0x00000004
#endif

static struct omni_update_status g_st;
static volatile sig_atomic_t g_wake;
static int g_lock_fd = -1;

/* ------------------------------------------------------------------ */
/* SHA-256 (FIPS 180-4)                                               */
/* ------------------------------------------------------------------ */

struct sha256 {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t n;
};

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(struct sha256 *s, const unsigned char *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) +
             K256[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_init(struct sha256 *s)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372,
        0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, iv, sizeof(iv));
    s->len = 0;
    s->n = 0;
}

static void sha_update(struct sha256 *s, const unsigned char *p, size_t len)
{
    s->len += len;
    while (len) {
        size_t k = 64 - s->n;
        if (k > len)
            k = len;
        memcpy(s->buf + s->n, p, k);
        s->n += k;
        p += k;
        len -= k;
        if (s->n == 64) {
            sha_block(s, s->buf);
            s->n = 0;
        }
    }
}

static void sha_final(struct sha256 *s, char hex[65])
{
    uint64_t bits = s->len * 8;
    unsigned char pad = 0x80, len8[8];
    int i;
    sha_update(s, &pad, 1);
    pad = 0;
    while (s->n != 56)
        sha_update(s, &pad, 1);
    for (i = 0; i < 8; i++)
        len8[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha_update(s, len8, 8);
    for (i = 0; i < 8; i++)
        snprintf(hex + 8 * i, 9, "%08x", s->h[i]);
}

static int sha256_file(const char *path, char hex[65])
{
    unsigned char buf[65536];
    struct sha256 s;
    ssize_t n;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    sha_init(&s);
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        sha_update(&s, buf, (size_t)n);
    close(fd);
    if (n < 0)
        return -1;
    sha_final(&s, hex);
    return 0;
}

/* ------------------------------------------------------------------ */
/* status                                                             */
/* ------------------------------------------------------------------ */

static void set_state(int state, int progress, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void set_state(int state, int progress, const char *fmt, ...)
{
    va_list ap;
    g_st.state = state;
    g_st.progress = progress;
    va_start(ap, fmt);
    vsnprintf(g_st.message, sizeof(g_st.message), fmt, ap);
    va_end(ap);
    snprintf(g_st.current, sizeof(g_st.current), "%s", omni_update_running_version());
    omni_update_write(&g_st);
}

static void dir_path(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", omni_update_dir(), name);
}

/* one instance works at a time (the daemon, or a check from Settings) */
static int lock_take(void)
{
    char p[512];
    mkdir(omni_update_dir(), 0755);
    dir_path(p, sizeof(p), "lock");
    g_lock_fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (g_lock_fd < 0)
        return 0;
    if (flock(g_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        close(g_lock_fd);
        g_lock_fd = -1;
        return 0;
    }
    return 1;
}

static void lock_drop(void)
{
    if (g_lock_fd >= 0) {
        flock(g_lock_fd, LOCK_UN);
        close(g_lock_fd);
        g_lock_fd = -1;
    }
}

/* ------------------------------------------------------------------ */
/* versions, network, the release feed                                */
/* ------------------------------------------------------------------ */

/* "2026.2.10" > "2026.2.9"; missing parts count as 0 */
static int vercmp(const char *a, const char *b)
{
    for (;;) {
        char *ea, *eb;
        long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);
        if (x != y)
            return x < y ? -1 : 1;
        if (*ea != '.' && *eb != '.')
            return 0;
        a = (*ea == '.') ? ea + 1 : ea;
        b = (*eb == '.') ? eb + 1 : eb;
    }
}

static int auto_updates(void)
{
    struct omni_settings s;
    omni_settings_load(&s);
    return s.autoupdate;
}

/* seconds left in a "Pause updates" (Settings), 0 when not paused */
static long paused_for(void)
{
    struct omni_settings s;
    omni_settings_load(&s);
    return omni_updates_paused(&s) ? s.pause_until - (long)time(NULL) : 0;
}

/* a default route means the network is up (DHCP finished) */
static int online(void)
{
    char line[256];
    int up = 0;
    FILE *f = fopen("/proc/net/route", "r");
    if (!f)
        return 0;
    while (!up && fgets(line, sizeof(line), f)) {
        char ifc[32];
        unsigned long dst, gw, flags;
        if (sscanf(line, "%31s %lx %lx %lx", ifc, &dst, &gw, &flags) == 4 &&
            dst == 0 && (flags & 1))                    /* RTF_UP */
            up = 1;
    }
    fclose(f);
    return up;
}

static void feed_url(char *out, size_t n)
{
    const char *e = getenv("OMNI_UPDATE_URL");
    char line[600];
    FILE *f;
    snprintf(out, n, "%s", (e && *e) ? e : DEFAULT_FEED);
    if (e && *e)
        return;
    f = fopen(FEED_CONF, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "url=", 4) == 0 && line[4])
            snprintf(out, n, "%s", line + 4);
    }
    fclose(f);
}

struct manifest {
    char version[32];
    char kernel[128];
    char sha256[65];
    char url[600];
    long size;
};

/* ------------------------------------------------------------------ */
/* downloads (BusyBox wget)                                           */
/* ------------------------------------------------------------------ */

static void exec_wget(const char *url, const char *out)
{
    const char *w = getenv("OMNI_WGET");
    if (!(w && *w) && access("/bin/busybox", X_OK) == 0)
        w = "/bin/busybox";
    if (w && *w) {
        const char *base = strrchr(w, '/');
        base = base ? base + 1 : w;
        if (strcmp(base, "busybox") == 0)
            execl(w, "busybox", "wget", "-q", "-T", "30", "-O", out, url, (char *)NULL);
        else
            execl(w, "wget", "-q", "-T", "30", "-O", out, url, (char *)NULL);
    }
    execlp("wget", "wget", "-q", "-T", "30", "-O", out, url, (char *)NULL);
}

/* turn wget's complaint into something a person can act on */
static void explain(const char *err, char *msg, size_t n)
{
    if (strstr(err, "bad address") || strstr(err, "resolve"))
        snprintf(msg, n, "Couldn't find the update server. Check the network connection.");
    else if (strstr(err, "TLS") || strstr(err, "SSL") || strstr(err, "ssl"))
        snprintf(msg, n, "Couldn't make a secure connection to the update server.");
    else if (strstr(err, "timed out") || strstr(err, "unreachable") ||
             strstr(err, "refused") || strstr(err, "reset"))
        snprintf(msg, n, "Couldn't reach the update server. We'll try again later.");
    else if (strstr(err, "server returned error"))
        snprintf(msg, n, "The update server returned an error (%.60s).",
                 strstr(err, "HTTP") ? strstr(err, "HTTP") : err);
    else if (err[0])
        snprintf(msg, n, "Download failed: %.100s", err);
    else
        snprintf(msg, n, "Download failed.");
}

/* fetch url into path; while it runs, report progress against expect
 * bytes (0 = don't). Returns 0 ok, 1 not found (404), -1 error (msg). */
static int fetch(const char *url, const char *path, long expect, char *msg, size_t n)
{
    char err[512] = "";
    size_t elen = 0;
    int pfd[2], status = 0, last = -1;
    time_t start = time(NULL), last_write = 0;
    pid_t pid;

    unlink(path);
    if (pipe(pfd) != 0) {
        snprintf(msg, n, "Download failed: %s", strerror(errno));
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        snprintf(msg, n, "Download failed: %s", strerror(errno));
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 1);
        }
        dup2(pfd[1], 2);
        close(pfd[0]);
        close(pfd[1]);
        exec_wget(url, path);
        _exit(127);
    }
    close(pfd[1]);
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);
    for (;;) {
        ssize_t r;
        pid_t w;
        while (elen < sizeof(err) - 1 &&
               (r = read(pfd[0], err + elen, sizeof(err) - 1 - elen)) > 0)
            elen += (size_t)r;
        err[elen] = '\0';
        w = waitpid(pid, &status, WNOHANG);
        if (w == pid)
            break;
        if (expect > 0) {
            struct stat st;
            int pc = (stat(path, &st) == 0) ? (int)(st.st_size * 100 / expect) : 0;
            if (pc > 99)
                pc = 99;
            if (pc != last && time(NULL) != last_write) {
                last = pc;
                last_write = time(NULL);
                g_st.progress = pc;
                omni_update_write(&g_st);
            }
        }
        if (time(NULL) - start > DOWNLOAD_MAX) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            close(pfd[0]);
            unlink(path);
            snprintf(msg, n, "The download took too long. We'll try again later.");
            return -1;
        }
        usleep(250000);
    }
    {
        ssize_t r;                      /* what wget said last */
        while (elen < sizeof(err) - 1 &&
               (r = read(pfd[0], err + elen, sizeof(err) - 1 - elen)) > 0)
            elen += (size_t)r;
        err[elen] = '\0';
    }
    close(pfd[0]);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
    unlink(path);
    if (strstr(err, " 404"))
        return 1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127 && !err[0])
        snprintf(err, sizeof(err), "wget is missing");
    err[strcspn(err, "\n")] = '\0';
    if (strncmp(err, "wget: ", 6) == 0)
        memmove(err, err + 6, strlen(err + 6) + 1);
    explain(err, msg, n);
    return -1;
}

/* read the feed; 0 ok, 1 no feed on the latest release, -1 error */
static int get_manifest(struct manifest *m, char *msg, size_t n)
{
    char url[600], path[512], line[700];
    FILE *f;
    int r;

    memset(m, 0, sizeof(*m));
    feed_url(url, sizeof(url));
    dir_path(path, sizeof(path), "feed.txt");
    r = fetch(url, path, 0, msg, n);
    if (r != 0)
        return r;
    f = fopen(path, "r");
    if (!f) {
        snprintf(msg, n, "Couldn't read the update information.");
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        char *v = strchr(line, '=');
        if (!v)
            continue;
        *v++ = '\0';
        v[strcspn(v, "\r\n")] = '\0';
        if (strcmp(line, "version") == 0)
            snprintf(m->version, sizeof(m->version), "%s", v);
        else if (strcmp(line, "kernel") == 0 && !strchr(v, '/'))
            snprintf(m->kernel, sizeof(m->kernel), "%s", v);
        else if (strcmp(line, "sha256") == 0)
            snprintf(m->sha256, sizeof(m->sha256), "%s", v);
        else if (strcmp(line, "size") == 0)
            m->size = atol(v);
        else if (strcmp(line, "url") == 0)
            snprintf(m->url, sizeof(m->url), "%s", v);
    }
    fclose(f);
    unlink(path);
    if (!m->version[0] || !m->kernel[0] || strlen(m->sha256) != 64 || m->size <= 0) {
        snprintf(msg, n, "The update information is incomplete.");
        return -1;
    }
    if (!m->url[0]) {           /* .../releases/latest/download/<feed> -> */
        char *p = strstr(url, "/releases/latest/download/");  /* .../download/v<ver>/<kernel> */
        if (p) {
            *p = '\0';
            snprintf(m->url, sizeof(m->url), "%s/releases/download/v%s/%s",
                     url, m->version, m->kernel);
        } else {                /* a plain directory: the kernel sits beside it */
            char *slash = strrchr(url, '/');
            if (slash)
                slash[1] = '\0';
            snprintf(m->url, sizeof(m->url), "%s%s", url, m->kernel);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* installing: kexec_file_load + the state that rides along           */
/* ------------------------------------------------------------------ */

static int is_bzimage(const char *path)
{
    unsigned char hdr[0x210];
    int fd = open(path, O_RDONLY | O_CLOEXEC), ok = 0;
    if (fd >= 0) {
        ok = read(fd, hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr) &&
             memcmp(hdr + 0x202, "HdrS", 4) == 0 &&        /* boot protocol */
             hdr[0x1fe] == 0x55 && hdr[0x1ff] == 0xaa;
        close(fd);
    }
    return ok;
}

static void cpio_entry(FILE *out, const char *name, unsigned mode,
                       const void *data, unsigned long size)
{
    static unsigned ino = 1;
    static const char zero[4] = { 0, 0, 0, 0 };
    unsigned long namesz = strlen(name) + 1;
    fprintf(out, "070701%08X%08X%08X%08X%08X%08lX%08lX%08X%08X%08X%08X%08lX%08X",
            ino++, mode, 0u, 0u, 1u, (unsigned long)time(NULL), size,
            0u, 0u, 0u, 0u, namesz, 0u);
    fwrite(name, 1, namesz, out);
    fwrite(zero, 1, (4 - ((110 + namesz) & 3)) & 3, out);
    if (size) {
        fwrite(data, 1, size, out);
        fwrite(zero, 1, (4 - (size & 3)) & 3, out);
    }
}

/* the file (if it exists) at the same path in the new system */
static void cpio_file(FILE *out, const char *path)
{
    struct stat st;
    char *buf;
    FILE *f;
    if (path[0] != '/' || stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size > (1 << 20))
        return;
    buf = malloc((size_t)st.st_size + 1);
    f = fopen(path, "rb");
    if (buf && f && fread(buf, 1, (size_t)st.st_size, f) == (size_t)st.st_size)
        cpio_entry(out, path + 1, 0100000u | (st.st_mode & 07777), buf,
                   (unsigned long)st.st_size);
    if (f)
        fclose(f);
    free(buf);
}

/* directories leading to path (the new system has most; mkdir is harmless) */
static void cpio_dirs(FILE *out, const char *path)
{
    char d[512];
    size_t i;
    snprintf(d, sizeof(d), "%s", path + 1);
    for (i = 0; d[i]; i++) {
        if (d[i] == '/') {
            d[i] = '\0';
            cpio_entry(out, d, 040755u, NULL, 0);
            d[i] = '/';
        }
    }
}

/* settings, installed apps, password, update status + history */
static int build_carry(const char *cpio)
{
    const char *files[6];
    char st[512], hist[512];
    FILE *out = fopen(cpio, "wb");
    int i;
    if (!out)
        return -1;
    dir_path(st, sizeof(st), "status");
    dir_path(hist, sizeof(hist), "history");
    files[0] = omni_settings_path();
    files[1] = omni_apps_state_path();
    files[2] = "/etc/shadow";
    files[3] = st;
    files[4] = hist;
    files[5] = NULL;
    for (i = 0; files[i]; i++) {
        if (access(files[i], R_OK) == 0) {
            cpio_dirs(out, files[i]);
            cpio_file(out, files[i]);
        }
    }
    cpio_entry(out, "TRAILER!!!", 0, NULL, 0);
    return fclose(out) == 0 ? 0 : -1;
}

/* load the kernel for the next restart; 0 ok, else msg */
static int stage(const char *kernel, char *msg, size_t n)
{
    char cmd[1024] = "", cpio[512];
    int kfd, ifd = -1, flags = 0, fd;
    long r;
    const char *c;

    if (getenv("OMNI_UPDATE_NO_KEXEC"))
        return 0;
    fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t k = read(fd, cmd, sizeof(cmd) - 1);
        cmd[k > 0 ? k : 0] = '\0';
        close(fd);
    }
    cmd[strcspn(cmd, "\n")] = '\0';
    c = cmd;
    if (strncmp(c, BUILTIN_CMDLINE, strlen(BUILTIN_CMDLINE)) == 0)
        c += strlen(BUILTIN_CMDLINE);
    while (*c == ' ')
        c++;

    kfd = open(kernel, O_RDONLY | O_CLOEXEC);
    if (kfd < 0) {
        snprintf(msg, n, "The downloaded update is missing.");
        return -1;
    }
    dir_path(cpio, sizeof(cpio), "carry.cpio");
    if (build_carry(cpio) == 0)
        ifd = open(cpio, O_RDONLY | O_CLOEXEC);
    if (ifd < 0)
        flags |= KEXEC_FILE_NO_INITRAMFS;
#ifdef SYS_kexec_file_load
    r = syscall(SYS_kexec_file_load, kfd, ifd, strlen(c) + 1, c, flags);
#else
    r = -1;
    errno = ENOSYS;
#endif
    if (r != 0) {
        int e = errno;
        if (e == ENOSYS)
            snprintf(msg, n, "This version of OmniOS can't install updates by itself. Use the new ISO.");
        else if (e == EPERM)
            snprintf(msg, n, "Installing updates needs administrator rights.");
        else if (e == ENOEXEC || e == EINVAL || e == EKEYREJECTED)
            snprintf(msg, n, "The update couldn't be prepared: the kernel was rejected (%s).", strerror(e));
        else
            snprintf(msg, n, "The update couldn't be prepared (%s).", strerror(e));
    }
    close(kfd);
    if (ifd >= 0)
        close(ifd);
    return r == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* the work                                                           */
/* ------------------------------------------------------------------ */

static int enough_room(long need)
{
    struct statvfs v;
    if (statvfs(omni_update_dir(), &v) != 0 || v.f_blocks == 0)
        return 1;                       /* ramfs reports no size: RAM is the limit */
    return (unsigned long long)v.f_bavail * v.f_frsize > (unsigned long long)need * 2;
}

static void remember_staged(const char *path)
{
    char p[512];
    FILE *f;
    dir_path(p, sizeof(p), "staged");
    f = fopen(p, "w");
    if (f) {
        fprintf(f, "%s\n", path);
        fclose(f);
    }
}

static int download_and_stage(const struct manifest *m)
{
    char part[640], final[640], hex[65], msg[200];
    struct stat st;
    int r;

    snprintf(g_st.latest, sizeof(g_st.latest), "%s", m->version);
    if (!enough_room(m->size)) {
        set_state(UPD_ERROR, 0, "Not enough memory to download OmniOS %s (%ld MB).",
                  m->version, m->size >> 20);
        return -1;
    }
    set_state(UPD_DOWNLOADING, 0, "Downloading OmniOS %s", m->version);
    snprintf(final, sizeof(final), "%s/%s", omni_update_dir(), m->kernel);
    snprintf(part, sizeof(part), "%s.part", final);
    r = fetch(m->url, part, m->size, msg, sizeof(msg));
    if (r != 0) {
        set_state(UPD_ERROR, 0, "%s", r == 1 ? "The update file is missing from the release." : msg);
        omni_update_log("OmniOS %s: download failed", m->version);
        return -1;
    }
    if (stat(part, &st) != 0 || st.st_size != m->size ||
        sha256_file(part, hex) != 0 || strcasecmp(hex, m->sha256) != 0) {
        unlink(part);
        set_state(UPD_ERROR, 0, "The download of OmniOS %s was damaged. We'll try again later.",
                  m->version);
        omni_update_log("OmniOS %s: bad checksum", m->version);
        return -1;
    }
    if (!is_bzimage(part)) {
        unlink(part);
        set_state(UPD_ERROR, 0, "The download of OmniOS %s isn't an OmniOS system.", m->version);
        return -1;
    }
    rename(part, final);
    g_st.progress = 100;
    if (stage(final, msg, sizeof(msg)) != 0) {
        set_state(UPD_ERROR, 0, "%s", msg);
        omni_update_log("OmniOS %s: couldn't be prepared", m->version);
        return -1;
    }
    remember_staged(final);
    set_state(UPD_READY, 100, "Restart to finish installing OmniOS %s.", m->version);
    omni_update_log("OmniOS %s downloaded", m->version);
    return 0;
}

/* check (and with install or automatic updates, download) */
static int run(int install)
{
    struct manifest m;
    char msg[200];
    int r;

    if (!lock_take())
        return 0;                       /* already busy: its status shows */
    omni_update_read(&g_st);
    if (!online()) {
        set_state(UPD_ERROR, 0, "You're offline. OmniOS Update will check when you're connected.");
        lock_drop();
        return -1;
    }
    set_state(UPD_CHECKING, 0, "Checking for updates");
    r = get_manifest(&m, msg, sizeof(msg));
    if (r < 0) {
        set_state(UPD_ERROR, 0, "%s", msg);
        lock_drop();
        return -1;
    }
    g_st.checked = (long)time(NULL);
    if (r == 1 || vercmp(m.version, omni_update_running_version()) <= 0) {
        g_st.latest[0] = '\0';
        set_state(UPD_UPTODATE, 0, "You're up to date");
    } else if (g_st.state == UPD_READY && strcmp(g_st.latest, m.version) == 0) {
        set_state(UPD_READY, 100, "Restart to finish installing OmniOS %s.", m.version);
    } else if (install || auto_updates()) {
        download_and_stage(&m);
    } else {
        snprintf(g_st.latest, sizeof(g_st.latest), "%s", m.version);
        set_state(UPD_AVAILABLE, 0, "OmniOS %s is available.", m.version);
    }
    lock_drop();
    return g_st.state == UPD_ERROR ? -1 : 0;
}

/* at startup: did we just restart into an update? */
static void reconcile(void)
{
    omni_update_read(&g_st);
    if (g_st.state == UPD_READY && g_st.latest[0] &&
        vercmp(omni_update_running_version(), g_st.latest) >= 0) {
        omni_update_log("OmniOS %s installed", g_st.latest);
        set_state(UPD_UPTODATE, 0, "OmniOS %s was installed.", g_st.latest);
    } else if (g_st.state != UPD_IDLE && g_st.state != UPD_UPTODATE) {
        g_st.latest[0] = '\0';          /* a download from before power-off */
        set_state(UPD_IDLE, 0, "%s", "");
    }
}

/* init, before reboot(RB_KEXEC): load again with today's settings */
static int refresh_for_restart(void)
{
    char p[512], kernel[512] = "", msg[200];
    FILE *f;
    dir_path(p, sizeof(p), "staged");
    f = fopen(p, "r");
    if (!f)
        return 1;
    if (fgets(kernel, sizeof(kernel), f))
        kernel[strcspn(kernel, "\n")] = '\0';
    fclose(f);
    if (!kernel[0] || access(kernel, R_OK) != 0)
        return 1;
    return stage(kernel, msg, sizeof(msg)) == 0 ? 0 : 1;   /* else the earlier load stands */
}

static void on_signal(int sig)
{
    (void)sig;
    g_wake = 1;
}

/* sleep up to secs; "check now" (SIGUSR1) cuts it short */
static void nap(int secs)
{
    time_t end = time(NULL) + secs;
    while (!g_wake && time(NULL) < end)
        sleep(1);
    g_wake = 0;
}

static void run_daemon(void)
{
    struct sigaction sa;
    int waited = 0;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGUSR1, &sa, NULL);
    signal(SIGCHLD, SIG_DFL);
    reconcile();
    {
        char p[512];
        FILE *f;
        dir_path(p, sizeof(p), "pid");
        f = fopen(p, "w");
        if (f) {
            fprintf(f, "%d\n", (int)getpid());
            fclose(f);
        }
    }
    while (!online() && !g_wake) {          /* DHCP may take a while */
        sleep(5);
        waited += 5;
    }
    nap(waited >= FIRST_DELAY ? 5 : FIRST_DELAY);
    for (;;) {
        long pause = paused_for();
        if (pause > 0) {
            /* paused (Settings > OmniOS Update): no checks, no downloads
             * until the pause ends; "Resume updates" checks right away */
            nap(pause < 600 ? (int)pause : 600);
            continue;
        }
        /* like Windows: always check; download on our own only with
         * automatic updates on, else just say an update is available */
        run(0);
        nap(g_st.state == UPD_ERROR ? RETRY_ERROR : CHECK_EVERY);
    }
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "";

    signal(SIGPIPE, SIG_IGN);
    umask(022);
    if (strcmp(cmd, "daemon") == 0) {
        run_daemon();
        return 0;
    }
    if (strcmp(cmd, "check") == 0)
        return run(0) == 0 ? 0 : 1;
    if (strcmp(cmd, "install") == 0)
        return run(1) == 0 ? 0 : 1;
    if (strcmp(cmd, "restart") == 0)
        return refresh_for_restart();
    if (strcmp(cmd, "carry") == 0 && argc > 2)    /* tests: the restart bundle */
        return build_carry(argv[2]) == 0 ? 0 : 1;
    if (strcmp(cmd, "status") == 0) {
        omni_update_read(&g_st);
        printf("running %s, state %d, latest %s, %d%%: %s\n",
               omni_update_running_version(), g_st.state, g_st.latest,
               g_st.progress, g_st.message);
        return 0;
    }
    fprintf(stderr, "usage: omnios-update daemon|check|install|restart|status\n");
    return 2;
}
