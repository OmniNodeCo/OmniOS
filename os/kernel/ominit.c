/*
 * OmniOS — os/kernel/ominit.c
 *
 * PID 1 for OmniOS. Written from scratch (no systemd/sysvinit/BusyBox init):
 *
 *   1. mount the virtual filesystems and seed /dev          (ommount.c)
 *   2. set the hostname, write /etc/motd to the console
 *   3. start a login shell on the console (getty) and launch the graphical
 *      desktop (/usr/bin/omnios-desktop)
 *   4. reap orphans continuously — the classic PID-1 duty — and honour
 *      Ctrl-Alt-Del (reboot) / shutdown requests and SIGINT/SIGTERM.
 *
 * It keeps running as a static musl binary with no other dependencies.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "ominit.h"

static volatile sig_atomic_t g_shutdown = 0;
static volatile sig_atomic_t g_reboot = 0;

static void on_sig(int sig)
{
    if (sig == SIGINT || sig == SIGTERM)
        g_shutdown = 1;
    else if (sig == SIGQUIT)
        g_reboot = 1;
}

/* fork + exec; returns the child pid, or -1. Blocks only the fork. */
pid_t omni_spawn(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);          /* exec failed */
    }
    return pid;
}

int omni_reap(void)
{
    int n = 0;
    for (;;) {
        pid_t p = waitpid(-1, NULL, WNOHANG);
        if (p <= 0)
            break;
        n++;
    }
    return n;
}

/* The kernel starts /init with no stdin/stdout/stderr when the initramfs
 * has no /dev/console ("unable to open an initial console"), and every
 * child inherits that. Give PID 1 and all its children:
 *   stdin  = /dev/null   nothing may read the console: a shell reading
 *                        tty1 would also receive every key typed into
 *                        the desktop
 *   stdout = stderr = /dev/kmsg   output lands in the kernel log, so
 *                        printk carries it to every console, including
 *                        the serial log (plain /dev/console writes reach
 *                        only tty1, underneath the desktop).
 * Needs /dev mounted; call after omni_mount_all(). */
static void setup_stdio(void)
{
    int fd = open("/dev/null", O_RDONLY);

    if (fd >= 0 && fd != 0) {
        dup2(fd, 0);
        close(fd);
    }
    fd = open("/dev/kmsg", O_WRONLY);
    if (fd < 0)
        fd = open("/dev/null", O_WRONLY);
    if (fd >= 0) {
        if (fd != 1)
            dup2(fd, 1);
        if (fd != 2)
            dup2(fd, 2);
        if (fd > 2)
            close(fd);
    }
}

static void reboot_now(int cmd)
{
    sync();
    reboot(cmd);
    /* if we got here, the kernel refused; halt busy */
    for (;;)
        pause();
}

int main(int argc, char **argv)
{
    char *sh_args[]      = { "/bin/sh", "-l", (char *)NULL };
    char *fbset_args[]   = { "/usr/bin/omnios-desktop", (char *)NULL };
    (void)argc; (void)argv;

    /* 1 is the PID; /init was the kernel's entry */
    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGQUIT, on_sig);

    omni_mount_all();
    setup_stdio();
    omni_log("OmniOS init (PID 1): starting\n");

    /* hostname */
    {
        int fd = open("/etc/hostname", O_RDONLY);
        char name[64];
        ssize_t n;
        if (fd >= 0) {
            memset(name, 0, sizeof(name));
            n = read(fd, name, sizeof(name) - 1);
            close(fd);
            if (n > 0) {
                name[strcspn(name, "\n")] = '\0';
                if (sethostname(name, strlen(name)) != 0) {
                    /* non-fatal */
                }
            }
        }
    }

    /* motd banner */
    {
        int fd = open("/etc/motd", O_RDONLY);
        char buf[2048];
        ssize_t n;
        if (fd >= 0) {
            n = read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                write(STDOUT_FILENO, buf, (size_t)n);
            }
            close(fd);
        }
    }

    /* login on the console + the graphical desktop */
    omni_spawn(sh_args);
    omni_spawn(fbset_args);

    omni_log("OmniOS init: services started, entering maintainer loop\n");

    for (;;) {
        if (g_shutdown || g_reboot) {
            omni_log("OmniOS init: mandated by signal\n");
            sync();
            reboot_now(g_reboot ? RB_AUTOBOOT : RB_POWER_OFF);
        }

        /* reap children (fast, non-blocking) */
        omni_reap();

        /* brief sleep to avoid spinning; signals wake us */
        pause();
    }

    return 0;
}
