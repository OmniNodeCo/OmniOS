/*
 * OmniOS — os/gui/account.c
 *
 * User account name + password (see account.h).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <crypt.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "account.h"

static char g_login[64], g_display[64];

static const char *shadow_path(void)
{
    const char *p = getenv("OMNI_SHADOW");
    return (p && *p) ? p : OMNI_SHADOW_FILE;
}

static void load_names(void)
{
    struct passwd *pw;
    if (g_login[0])
        return;
    pw = getpwuid(getuid());
    snprintf(g_login, sizeof(g_login), "%s", pw ? pw->pw_name : "root");
    if (pw && pw->pw_gecos && pw->pw_gecos[0] && pw->pw_gecos[0] != ',')
        snprintf(g_display, sizeof(g_display), "%.*s",
                 (int)strcspn(pw->pw_gecos, ","), pw->pw_gecos);
    else
        snprintf(g_display, sizeof(g_display), "%s", g_login);
}

const char *omni_account_login(void)   { load_names(); return g_login; }
const char *omni_account_display(void) { load_names(); return g_display; }

/* the hash field of our shadow entry into `out` ("" if none) */
static void read_hash(char *out, size_t outsz)
{
    char line[512];
    size_t n;
    FILE *f;

    out[0] = '\0';
    load_names();
    n = strlen(g_login);
    f = fopen(shadow_path(), "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, g_login, n) == 0 && line[n] == ':') {
            char *h = line + n + 1;
            h[strcspn(h, ":\r\n")] = '\0';
            snprintf(out, outsz, "%s", h);
            break;
        }
    }
    fclose(f);
}

int omni_account_has_password(void)
{
    char h[256];
    read_hash(h, sizeof(h));
    return h[0] == '$';                 /* a real crypt hash; "", "!", "*" = none */
}

int omni_account_check(const char *pw)
{
    char h[256];
    const char *c;
    read_hash(h, sizeof(h));
    if (h[0] != '$')
        return 1;
    c = crypt(pw ? pw : "", h);
    return c && strcmp(c, h) == 0;
}

static void make_salt(char *salt, size_t n)
{
    static const char abc[] =
        "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned char rnd[16];
    size_t i;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0 || read(fd, rnd, sizeof(rnd)) != (ssize_t)sizeof(rnd)) {
        unsigned seed = (unsigned)time(NULL) ^ (unsigned)getpid();
        for (i = 0; i < sizeof(rnd); i++)
            rnd[i] = (unsigned char)(rand_r(&seed) & 0xff);
    }
    if (fd >= 0)
        close(fd);
    for (i = 0; i + 1 < n && i < sizeof(rnd); i++)
        salt[i] = abc[rnd[i] % 64];
    salt[i] = '\0';
}

int omni_account_set_password(const char *pw)
{
    char hash[256] = "", setting[40], salt[17], tmp[512], line[512];
    const char *path = shadow_path();
    long days = (long)(time(NULL) / 86400);
    int found = 0;
    size_t n;
    FILE *in, *out;

    load_names();
    if (pw && pw[0]) {
        const char *c;
        make_salt(salt, sizeof(salt));
        snprintf(setting, sizeof(setting), "$6$%s$", salt);
        c = crypt(pw, setting);
        if (!c || c[0] != '$')
            return -1;
        snprintf(hash, sizeof(hash), "%s", c);
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    out = fopen(tmp, "w");
    if (!out)
        return -1;
    fchmod(fileno(out), 0600);
    n = strlen(g_login);
    in = fopen(path, "r");
    if (in) {                           /* keep every other entry as it is */
        while (fgets(line, sizeof(line), in)) {
            if (strncmp(line, g_login, n) == 0 && line[n] == ':') {
                char *rest = strchr(line + n + 1, ':');
                fprintf(out, "%s:%s%s", g_login, hash, rest ? rest : ":::::::\n");
                found = 1;
            } else {
                fputs(line, out);
            }
        }
        fclose(in);
    }
    if (!found)
        fprintf(out, "%s:%s:%ld:0:99999:7:::\n", g_login, hash, days);
    if (fclose(out) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}
