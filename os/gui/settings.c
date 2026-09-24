/*
 * OmniOS — os/gui/settings.c
 *
 * User settings file (see settings.h).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "settings.h"

const struct omni_accent omni_accents[] = {
    { "Blue",    0x3b82f6 }, { "Violet", 0x8b5cf6 }, { "Teal",   0x14b8a6 },
    { "Green",   0x22c55e }, { "Orange", 0xf97316 }, { "Red",    0xef4444 },
    { "Pink",    0xec4899 }, { "Slate",  0x64748b },
};
const int omni_accents_n = (int)(sizeof(omni_accents) / sizeof(omni_accents[0]));

/* POSIX TZ rules (musl reads them directly; the image has no zoneinfo) */
const struct omni_zone omni_zones[] = {
    { "UTC",                          "UTC0" },
    { "New York, Miami (Eastern)",    "EST5EDT,M3.2.0,M11.1.0" },
    { "Chicago (Central)",            "CST6CDT,M3.2.0,M11.1.0" },
    { "Denver (Mountain)",            "MST7MDT,M3.2.0,M11.1.0" },
    { "Phoenix (Arizona)",            "MST7" },
    { "Los Angeles (Pacific)",        "PST8PDT,M3.2.0,M11.1.0" },
    { "Anchorage (Alaska)",           "AKST9AKDT,M3.2.0,M11.1.0" },
    { "Honolulu (Hawaii)",            "HST10" },
    { "Sao Paulo",                    "<-03>3" },
    { "London, Dublin, Lisbon",       "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Berlin, Paris, Rome, Madrid",  "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Athens, Helsinki, Kyiv",       "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Moscow, Istanbul",             "<+03>-3" },
    { "Dubai",                        "<+04>-4" },
    { "India",                        "IST-5:30" },
    { "Beijing, Singapore, Perth",    "<+08>-8" },
    { "Tokyo, Seoul",                 "JST-9" },
    { "Sydney, Melbourne",            "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Auckland",                     "NZST-12NZDT,M9.5.0,M4.1.0/3" },
};
const int omni_zones_n = (int)(sizeof(omni_zones) / sizeof(omni_zones[0]));

/* names + preview colours of the TH_WALL_* styles, in the same order */
const struct omni_wallinfo omni_walls[] = {
    { "Bloom",    0x0b1233, 0x4521a0, 0xc4b5fd },
    { "Aurora",   0x06261d, 0x138a6c, 0x6ee7b7 },
    { "Sunset",   0x2a1410, 0x9c4a3c, 0xfdba74 },
    { "Ocean",    0x061a33, 0x1d6fd0, 0x7dd3fc },
    { "Rose",     0x2a0f22, 0x8f2f73, 0xf9a8d4 },
    { "Graphite", 0x151619, 0x5a5d66, 0xe5e7eb },
};
const int omni_walls_n = (int)(sizeof(omni_walls) / sizeof(omni_walls[0]));

const char *omni_settings_path(void)
{
    const char *p = getenv("OMNI_SETTINGS");
    return (p && *p) ? p : OMNI_SETTINGS_FILE;
}

void omni_settings_defaults(struct omni_settings *s)
{
    memset(s, 0, sizeof(*s));
    s->autoupdate = 1;
    s->signin = 1;
}

static int find_name(const char *v, int n, const char *(*name_at)(int))
{
    int i;
    for (i = 0; i < n; i++)
        if (strcmp(v, name_at(i)) == 0)
            return i;
    return -1;
}
static const char *accent_name(int i) { return omni_accents[i].name; }
static const char *zone_name(int i)   { return omni_zones[i].name; }
static const char *wall_name(int i)   { return omni_walls[i].name; }

int omni_settings_load(struct omni_settings *s)
{
    char line[256];
    FILE *f;

    omni_settings_defaults(s);
    f = fopen(omni_settings_path(), "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '='), *v;
        int i;
        if (!eq || line[0] == '#')
            continue;
        *eq = '\0';
        v = eq + 1;
        v[strcspn(v, "\r\n")] = '\0';
        if (strcmp(line, "wallpaper") == 0) {
            if ((i = find_name(v, omni_walls_n, wall_name)) >= 0) s->wallpaper = i;
        } else if (strcmp(line, "accent") == 0) {
            if ((i = find_name(v, omni_accents_n, accent_name)) >= 0) s->accent = i;
        } else if (strcmp(line, "timezone") == 0) {
            if ((i = find_name(v, omni_zones_n, zone_name)) >= 0) s->zone = i;
        } else if (strcmp(line, "clock24") == 0) {
            s->clock24 = atoi(v) != 0;
        } else if (strcmp(line, "autoupdate") == 0) {
            s->autoupdate = atoi(v) != 0;
        } else if (strcmp(line, "signin") == 0) {
            s->signin = atoi(v) != 0;
        }
    }
    fclose(f);
    return 0;
}

int omni_settings_save(const struct omni_settings *s)
{
    char tmp[512], dir[512];
    const char *path = omni_settings_path();
    char *slash;
    FILE *f;

    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        mkdir(dir, 0755);                       /* /var/lib/omnios */
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f)
        return -1;
    fprintf(f, "# OmniOS settings (managed by the Settings app)\n");
    fprintf(f, "wallpaper=%s\n", omni_walls[s->wallpaper % omni_walls_n].name);
    fprintf(f, "accent=%s\n", omni_accents[s->accent % omni_accents_n].name);
    fprintf(f, "timezone=%s\n", omni_zones[s->zone % omni_zones_n].name);
    fprintf(f, "clock24=%d\n", s->clock24 ? 1 : 0);
    fprintf(f, "autoupdate=%d\n", s->autoupdate ? 1 : 0);
    fprintf(f, "signin=%d\n", s->signin ? 1 : 0);
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

void omni_settings_apply_tz(const struct omni_settings *s)
{
    setenv("TZ", omni_zones[(s->zone >= 0 && s->zone < omni_zones_n) ? s->zone : 0].tz, 1);
    tzset();
}
