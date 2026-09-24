/*
 * OmniOS — os/gui/catalog.h
 *
 * The app catalog: every app OmniOS ships, and which of them are
 * installed. Shared by the desktop shell (the Start menu lists the
 * installed apps) and the App Store (which installs and removes them).
 *
 * All apps are part of the image; "installing" one adds it to the Start
 * menu. The installed set is kept in OMNI_APPS_STATE, one app id per line;
 * without that file the preinstalled defaults apply. System apps are
 * always installed.
 */
#ifndef OMNI_OS_GUI_CATALOG_H
#define OMNI_OS_GUI_CATALOG_H

#define OMNI_APPS_STATE_DIR "/var/lib/omnios"
#define OMNI_APPS_STATE     OMNI_APPS_STATE_DIR "/installed-apps"
#define OMNI_CATALOG_MAX    32

struct omni_app_info {
    const char *id;             /* stable id, stored in the state file   */
    const char *name;           /* Start menu / store label              */
    const char *path;           /* the app binary                        */
    const char *category;
    const char *summary;        /* one line for the store                */
    int         system;         /* part of OmniOS: cannot be removed     */
    int         preinstalled;   /* installed on a fresh system           */
};

extern const struct omni_app_info omni_catalog[];
extern const int omni_catalog_n;

/* index of the app with this id, or -1 */
int  omni_app_find(const char *id);

/* state file in use (the OMNI_APPS_STATE environment variable overrides
 * the default, for tests) */
const char *omni_apps_state_path(void);

/* installed[i] = 1 if catalog entry i is installed */
void omni_apps_load(unsigned char installed[OMNI_CATALOG_MAX]);

/* store the installed set (atomically: temp file + rename);
 * returns 0, or -1 with errno set */
int  omni_apps_save(const unsigned char installed[OMNI_CATALOG_MAX]);

#endif /* OMNI_OS_GUI_CATALOG_H */
