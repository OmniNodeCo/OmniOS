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

void omni_mount_all(void)
{
    /* early mount points */
    ensure_dir("/proc", 0555);
    ensure_dir("/sys", 0555);
    ensure_dir("/dev", 0755);
    ensure_dir("/dev/pts", 0755);
    ensure_dir("/dev/shm", 0755);
    ensure_dir("/dev/input", 0755);
    ensure_dir("/tmp", 01777);
    ensure_dir("/run", 0755);
    ensure_dir("/var/log", 0755);

    try_mount("proc", "/proc", "proc", 0, NULL);
    try_mount("sysfs", "/sys", "sysfs", 0, NULL);

    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) != 0)
        try_mount("tmpfs", "/dev", "tmpfs", 0, "mode=0755");

    try_mount("devpts", "/dev/pts", "devpts", 0, "mode=0620,gid=5");
    try_mount("tmpfs", "/tmp", "tmpfs", 0, "mode=01777");
    try_mount("none", "/run", "tmpfs", 0, NULL);

    /* legacy nodes (no-op under devtmpfs) */
    mknod_safe("/dev/console", S_IFCHR | 0600, 5, 1);
    mknod_safe("/dev/null",    S_IFCHR | 0666, 1, 3);
    mknod_safe("/dev/zero",    S_IFCHR | 0666, 1, 5);
    mknod_safe("/dev/tty",     S_IFCHR | 0666, 5, 0);
    mknod_safe("/dev/tty0",    S_IFCHR | 0600, 4, 0);
    mknod_safe("/dev/tty1",    S_IFCHR | 0600, 4, 1);
    mknod_safe("/dev/tty2",    S_IFCHR | 0600, 4, 2);
    mknod_safe("/dev/ttyS0",   S_IFCHR | 0600, 4, 64);
    mknod_safe("/dev/fb0",     S_IFCHR | 0666, 29, 0);
    mknod_safe("/dev/input/mice", S_IFCHR | 0666, 13, 63);

    /* seed /dev from sysfs via mdev */
    if (write_ctl("/proc/sys/kernel/hotplug", "/sbin/mdev") == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            execl("/sbin/mdev", "mdev", "-s", (char *)NULL);
            _exit(0);
        }
    }
}
