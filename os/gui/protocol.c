/*
 * OmniOS — os/gui/protocol.c
 *
 * Codec for the line-based window protocol plus framing helpers.
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "protocol.h"

/* How many leading numeric fields each verb carries (both directions,
 * see protocol.h). Once they are read, everything after the single
 * separating space is the text field, taken verbatim — so text that
 * happens to look numeric ("42", "-5", a colour such as "101418") or that
 * starts with spaces arrives intact. Unknown verbs (-1) keep the old
 * heuristic: numbers until the first non-numeric token. */
static int verb_numc(const char *verb)
{
    static const struct { const char *verb; int n; } t[] = {
        { "HELLO", 2 }, { "OPEN",  2 }, { "CLOSE", 1 }, { "TITLE", 1 },
        { "RAISE", 1 }, { "CLEAR", 1 }, { "FILL",  5 }, { "RECT",  5 },
        { "TEXT",  3 }, { "TEXTC", 5 }, { "QUIT",  0 }, { "OK",    1 },
        { "ERR",   0 }, { "KEY",   4 }, { "BTN",   5 }, { "BYE",   0 },
    };
    size_t i;
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (strcmp(verb, t[i].verb) == 0)
            return t[i].n;
    return -1;
}

int proto_parse(const char *line, struct proto_msg *m)
{
    const char *p = line;
    const char *tok;
    int maxn;

    memset(m, 0, sizeof(*m));

    /* verb */
    while (*p == ' ') p++;
    {
        int i = 0;
        while (*p && *p != ' ' && *p != '\n' && i < (int)sizeof(m->verb) - 1)
            m->verb[i++] = *p++;
        m->verb[i] = '\0';
    }

    /* numeric fields: only fully-numeric tokens count, at most maxn of
     * them; the first non-numeric token (or anything after the maxn-th
     * number) starts the text field. */
    maxn = verb_numc(m->verb);
    for (;;) {
        if (maxn >= 0 && m->n >= maxn) {
            size_t i = 0;
            if (*p == ' ')
                p++;                     /* exactly one separator */
            while (*p && *p != '\n' && i < sizeof(m->text) - 1)
                m->text[i++] = *p++;
            m->text[i] = '\0';
            break;
        }
        while (*p == ' ') p++;
        if (*p == '\0' || *p == '\n')
            break;

        tok = p;
        while (*p && *p != ' ' && *p != '\n')
            p++;

        /* test the token for pure integer */
        {
            size_t len = (size_t)(p - tok);
            size_t j;
            int numeric = (len > 0);
            for (j = 0; j < len; j++) {
                if (!isdigit((unsigned char)tok[j]) && tok[j] != '-') {
                    numeric = 0;
                    break;
                }
            }
            if (numeric && m->n < (int)(sizeof(m->num) / sizeof(m->num[0]))) {
                char numbuf[32];
                size_t cp = len < sizeof(numbuf) - 1 ? len : sizeof(numbuf) - 1;
                memcpy(numbuf, tok, cp);
                numbuf[cp] = '\0';
                m->num[m->n] = strtol(numbuf, NULL, 10);
                m->n++;
                continue;
            }
            /* non-numeric: from the token start to end-of-line is text */
            {
                size_t i = 0;
                p = tok;
                while (*p && *p != '\n' && i < sizeof(m->text) - 1)
                    m->text[i++] = *p++;
                m->text[i] = '\0';
            }
            break;
        }
    }

    return (m->verb[0] == '\0') ? -1 : 0;
}

static int write_long(char *out, size_t outsz, size_t *pos, const char *p)
{
    while (*p) {
        if (*pos + 1 >= outsz)
            return -1;
        out[(*pos)++] = *p++;
    }
    return 0;
}

int proto_build(char *out, size_t outsz, const char *verb, int numc,
                const long *num)
{
    size_t pos = 0;
    int i;
    char tmp[32];

    if (write_long(out, outsz, &pos, verb) < 0)
        return -1;
    for (i = 0; i < numc; i++) {
        if (pos + 1 >= outsz)
            return -1;
        out[pos++] = ' ';
        snprintf(tmp, sizeof(tmp), "%ld", num[i]);
        if (write_long(out, outsz, &pos, tmp) < 0)
            return -1;
    }
    if (pos + 1 >= outsz)
        return -1;
    out[pos++] = '\n';
    out[pos] = '\0';
    return (int)pos;
}

int proto_build_text(char *out, size_t outsz, const char *verb,
                     int numc, const long *num, const char *text)
{
    size_t pos = 0;
    int i;
    char tmp[32];

    if (write_long(out, outsz, &pos, verb) < 0)
        return -1;
    for (i = 0; i < numc; i++) {
        if (pos + 1 >= outsz)
            return -1;
        out[pos++] = ' ';
        snprintf(tmp, sizeof(tmp), "%ld", num[i]);
        if (write_long(out, outsz, &pos, tmp) < 0)
            return -1;
    }
    if (pos + 1 >= outsz)
        return -1;
    out[pos++] = ' ';
    if (write_long(out, outsz, &pos, text) < 0)
        return -1;
    if (pos + 1 >= outsz)
        return -1;
    out[pos++] = '\n';
    out[pos] = '\0';
    return (int)pos;
}

int proto_send(int fd, const char *line)
{
    size_t len = strlen(line);
    while (len > 0) {
        ssize_t w = write(fd, line, len);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (w == 0)
            return -1;
        line += w;
        len -= (size_t)w;
    }
    return 0;
}

int proto_recv(int fd, char *buf, size_t bufsz)
{
    size_t pos = 0;

    while (pos + 1 < bufsz) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            return (pos == 0) ? -1 : 0;   /* EOF */
        if (c == '\n') {
            buf[pos] = '\0';
            return 0;
        }
        buf[pos++] = c;
    }
    buf[pos] = '\0';
    return 0;
}
