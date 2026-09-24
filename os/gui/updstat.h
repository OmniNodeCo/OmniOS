/*
 * OmniOS — os/gui/updstat.h
 *
 * OmniOS Update status, shared by the updater (omnios-update, which writes
 * it) and the desktop + Settings (which show it). One "key=value" per line
 * in <dir>/status, where <dir> is /var/lib/omnios/update (OMNI_UPDATE_DIR
 * overrides it, for tests); <dir>/history keeps one line per event.
 */
#ifndef OMNI_OS_GUI_UPDSTAT_H
#define OMNI_OS_GUI_UPDSTAT_H

#define OMNI_UPDATE_DIR "/var/lib/omnios/update"

enum { UPD_IDLE, UPD_CHECKING, UPD_UPTODATE, UPD_AVAILABLE, UPD_DOWNLOADING,
       UPD_READY, UPD_ERROR };

struct omni_update_status {
    int  state;             /* UPD_*                                   */
    char current[32];       /* running version                         */
    char latest[32];        /* newest version found ("" = unknown)     */
    int  progress;          /* download progress, 0..100               */
    char message[160];      /* error or detail text                    */
    long checked;           /* time of the last successful check       */
};

const char *omni_update_dir(void);
/* read the status (missing file = UPD_IDLE); returns 0 */
int  omni_update_read(struct omni_update_status *st);
/* write it atomically; 0 on success */
int  omni_update_write(const struct omni_update_status *st);
/* append one line to the history */
void omni_update_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* the running version (from /etc/omnios-release), "" if unknown */
const char *omni_update_running_version(void);

#endif /* OMNI_OS_GUI_UPDSTAT_H */
