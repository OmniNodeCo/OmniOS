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

int proto_parse(const char *line, struct proto_msg *m)
{
    const char *p = line;
    const char *tok;

    memset(m, 0, sizeof(*m));

    /* verb */
    while (*p == ' ') p++;
    {
        int i = 0;
        while (*p && *p != ' ' && *p != '\n' && i < (int)sizeof(m->verb) - 1)
            m->verb[i++] = *p++;
        m->verb[i] = '\0';
    }

    /* numeric fields: only fully-numeric tokens count; the first
     * non-numeric token starts the text field. */
    for (;;) {
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
