/*
 * OmniOS — os/gui/settings.h
 *
 * User settings shared by the desktop shell (which applies them), the
 * Settings app (which edits them) and the updater. One "key=value" per line
 * in /var/lib/omnios/settings (OMNI_SETTINGS overrides the path, for tests).
 * The file is replaced atomically, and the shell reloads it when its
 * modification time changes.
 */
#ifndef OMNI_OS_GUI_SETTINGS_H
#define OMNI_OS_GUI_SETTINGS_H

#include <stdint.h>

#define OMNI_SETTINGS_FILE "/var/lib/omnios/settings"

struct omni_settings {
    int wallpaper;          /* TH_WALL_* index                              */
    int accent;             /* index into omni_accents[]                   */
    int clock24;            /* taskbar clock: 1 = 24-hour                   */
    int zone;               /* index into omni_zones[]                     */
    int autoupdate;         /* install updates automatically               */
    int signin;             /* show the sign-in screen at startup          */
};

struct omni_accent  { const char *name; uint32_t rgb; };
struct omni_zone    { const char *name; const char *tz; };   /* POSIX TZ rule */
struct omni_wallinfo { const char *name; uint32_t dark, mid, ribbon; };   /* preview colours */

extern const struct omni_accent   omni_accents[];
extern const int                  omni_accents_n;
extern const struct omni_zone     omni_zones[];
extern const int                  omni_zones_n;
extern const struct omni_wallinfo omni_walls[];
extern const int                  omni_walls_n;

const char *omni_settings_path(void);
void omni_settings_defaults(struct omni_settings *s);
/* load (missing file or keys = defaults); returns 0 */
int  omni_settings_load(struct omni_settings *s);
/* write atomically; returns 0 on success, -1 on error */
int  omni_settings_save(const struct omni_settings *s);
/* apply the time zone to this process (and the children it starts) */
void omni_settings_apply_tz(const struct omni_settings *s);

#endif /* OMNI_OS_GUI_SETTINGS_H */
