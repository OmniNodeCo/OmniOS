/*
 * OmniOS — os/kernel/ommount.c
 *
 * Filesystem / device setup for the boot sequence:
 *   - mount proc, sysfs, devtmpfs (or tmpfs), devpts, tmpfs
 *   - create the essential device nodes when devtmpfs is unavailable
 *   - seed /dev via mdev (the BusyBox hotplug helper)
 *
 * Compiled into the standalone PID-1 init (/usr/bin/ominit).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#include "ominit.h"

void omni_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void try_mount(const char *src, const char *tgt, const char *type,
                      unsigned long flags, const char *data)
{
    if (mount(src, tgt, type, flags, data) != 0) {
        /* noisy on devtmpfs fallback paths; keep it quiet in production */
        (void)0;
    }
}

static void ensure_dir(const char *p, mode_t mode)
{
    mkdir(p, mode);
}

static int write_ctl(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    (void)!write(fd, val, strlen(val));
    close(fd);
    return 0;
}

static void mknod_safe(const char *path, mode_t mode, unsigned maj, unsigned min)
{
    if (access(path, F_OK) == 0)
        return;
    mknod(path, mode, makedev(maj, min));
}

static int is_chr(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISCHR(st.st_mode);
}

/* `mdev -s` creates a node for every device sysfs knows about. Wait for
 * it (up to ~5 s) so /dev is complete before the desktop looks for its
 * input devices; a straggler is reaped later by the main loop. */
static void run_mdev_scan(void)
{
    struct timespec ts = { 0, 50 * 1000 * 1000 };
    pid_t pid = fork();
    int i;

    if (pid < 0)
        return;
    if (pid == 0) {
        execl("/sbin/mdev", "mdev", "-s", (char *)NULL);
        execl("/bin/mdev", "mdev", "-s", (char *)NULL);
        _exit(127);
    }
    for (i = 0; i < 100; i++) {
        if (waitpid(pid, NULL, WNOHANG) != 0)
            return;
        nanosleep(&ts, NULL);
    }
}

void omni_mount_all(void)
{
    int have_devtmpfs, need_mdev;

    /* early mount points */
    ensure_dir("/proc", 0555);
    ensure_dir("/sys", 0555);
    ensure_dir("/dev", 0755);
    ensure_dir("/tmp", 01777);
    ensure_dir("/run", 0755);
    ensure_dir("/var/log", 0755);

    try_mount("proc", "/proc", "proc", 0, NULL);
    try_mount("sysfs", "/sys", "sysfs", 0, NULL);

    /* /dev. /init has normally mounted devtmpfs here already, and the
     * kernel refuses the same filesystem on the same mount point with
     * EBUSY. The old code took that as "no devtmpfs" and mounted an EMPTY
     * tmpfs over the working /dev, hiding every device node: only the
     * static list below came back (/dev/fb0, so the desktop could draw),
     * never the /dev/input nodes or /dev/kmsg. mdev, which could have recreated
     * them, only ran when /proc/sys/kernel/hotplug exists, and that needs
     * CONFIG_UEVENT_HELPER, which this kernel does not have. Result: no
     * mouse, no keyboard, and no desktop log lines in the serial log. */
    have_devtmpfs = mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0 ||
                    errno == EBUSY;
    if (!have_devtmpfs)
        try_mount("tmpfs", "/dev", "tmpfs", 0, "mode=0755");
    /* devtmpfs always holds /dev/null; if it is missing, /dev is not
     * kernel-maintained and mdev must populate it */
    need_mdev = !have_devtmpfs || !is_chr("/dev/null");

    /* directories inside /dev only once /dev itself is final */
    ensure_dir("/dev/pts", 0755);
    ensure_dir("/dev/shm", 0755);
    ensure_dir("/dev/input", 0755);

    try_mount("devpts", "/dev/pts", "devpts", 0, "mode=0620,gid=5");
    try_mount("tmpfs", "/tmp", "tmpfs", 0, "mode=01777");
    try_mount("none", "/run", "tmpfs", 0, NULL);

    /* legacy nodes (no-op under devtmpfs) */
    mknod_safe("/dev/console", S_IFCHR | 0600, 5, 1);
    mknod_safe("/dev/null",    S_IFCHR | 0666, 1, 3);
    mknod_safe("/dev/zero",    S_IFCHR | 0666, 1, 5);
    mknod_safe("/dev/kmsg",    S_IFCHR | 0644, 1, 11);
    mknod_safe("/dev/tty",     S_IFCHR | 0666, 5, 0);
    mknod_safe("/dev/tty0",    S_IFCHR | 0600, 4, 0);
    mknod_safe("/dev/tty1",    S_IFCHR | 0600, 4, 1);
    mknod_safe("/dev/tty2",    S_IFCHR | 0600, 4, 2);
    mknod_safe("/dev/ttyS0",   S_IFCHR | 0600, 4, 64);
    mknod_safe("/dev/fb0",     S_IFCHR | 0666, 29, 0);
    mknod_safe("/dev/input/mice", S_IFCHR | 0666, 13, 63);

    /* A plain tmpfs /dev: create everything else from sysfs now, and
     * register mdev for hotplug where the kernel supports that. A
     * devtmpfs /dev needs neither: the kernel maintains it, including
     * devices that appear later (USB). */
    if (need_mdev) {
        write_ctl("/proc/sys/kernel/hotplug", "/sbin/mdev");
        run_mdev_scan();
    }
}
