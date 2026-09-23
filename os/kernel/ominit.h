/*
 * OmniOS — os/kernel/ominit.h
 *
 * Declarations shared by the boot core (ominit + mount helper).
 */
#ifndef OMNI_OS_KERNEL_OMINIT_H
#define OMNI_OS_KERNEL_OMINIT_H

#include <sys/types.h>

/* boot logging (stderr -> /dev/console as appropriate) */
void omni_log(const char *fmt, ...);

/* mount everything + seed /dev (see ommount.c) */
void omni_mount_all(void);

/* run a program with argv, return child pid (or 0 in the child) */
pid_t omni_spawn(char *const argv[]);

/* reap exited children without blocking; return number reaped */
int  omni_reap(void);

#endif /* OMNI_OS_KERNEL_OMINIT_H */
