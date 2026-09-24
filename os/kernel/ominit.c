/*
 * OmniOS — os/kernel/ominit.c
 *
 * PID 1 for OmniOS. Written from scratch (no systemd/sysvinit/BusyBox init):
 *
 *   1. mount the virtual filesystems and seed /dev          (ommount.c)
 *   2. set the hostname, write /etc/motd to the console
 *   3. start the graphical desktop (/usr/bin/omnios-desktop), the network
 *      (/etc/init.d/network: DHCP) and OmniOS Update (/usr/bin/omnios-update
 *      daemon); with omnios.serialshell on the kernel command line, also a
 *      root shell on the first serial port (for tests and rescue)
 *   4. reap orphans continuously — the classic PID-1 duty — and honour
 *      Ctrl-Alt-Del (reboot) / shutdown requests and SIGINT/SIGTERM.
 *      A restart with a downloaded update loaded (kexec) boots straight
 *      into the new version.
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
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#include "ominit.h"

static volatile sig_atomic_t g_shutdown = 0;
static volatile sig_atomic_t g_reboot = 0;
static sigset_t g_mask0;             /* the signal mask children start with */

static void on_sig(int sig)
{
    if (sig == SIGINT || sig == SIGTERM)
        g_shutdown = 1;
    else if (sig == SIGQUIT)
        g_reboot = 1;
    /* SIGCHLD: nothing to do here, waking the main loop is the point */
}

/* fork + exec; returns the child pid, or -1. Blocks only the fork. */
pid_t omni_spawn(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        sigprocmask(SIG_SETMASK, &g_mask0, NULL);
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

/* OmniOS Update has loaded a new version with kexec_file_load() */
static int kexec_loaded(void)
{
    char c = '0';
    int fd = open("/sys/kernel/kexec_loaded", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (read(fd, &c, 1) != 1)
            c = '0';
        close(fd);
    }
    return c == '1';
}

/* Restart into the update: let omnios-update reload it with the current
 * settings (at most 15 s), then jump to the new kernel. Returns only if
 * the kernel refused, and the caller restarts the ordinary way. */
static void restart_into_update(void)
{
    char *args[] = { "/usr/bin/omnios-update", "restart", (char *)NULL };
    pid_t pid = omni_spawn(args);
    int i;

    omni_log("OmniOS init: restarting into the downloaded update\n");
    for (i = 0; pid > 0 && i < 150; i++) {
        pid_t w = waitpid(pid, NULL, WNOHANG);
        if (w == pid || w < 0)          /* done (or already reaped) */
            break;
        usleep(100000);
    }
    sync();
    reboot(RB_KEXEC);
    omni_log("OmniOS init: the update could not be started; restarting\n");
}

/* a word on the kernel command line (/proc/cmdline) */
static int cmdline_has(const char *word)
{
    char buf[1024], *p;
    size_t n = strlen(word);
    ssize_t got;
    int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return 0;
    got = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (got <= 0)
        return 0;
    buf[got] = '\0';
    for (p = buf; (p = strstr(p, word)) != NULL; p += n)
        if ((p == buf || p[-1] == ' ') &&
            (p[n] == '\0' || p[n] == ' ' || p[n] == '\n' || p[n] == '='))
            return 1;
    return 0;
}

/* omnios.serialshell: a root shell on /dev/ttyS0, restarted when it exits.
 * Nothing reads the serial port otherwise (the desktop owns the screen and
 * keyboard); the boot test reads the kernel log through this shell. */
static pid_t serial_shell(void)
{
    pid_t pid = fork();
    int fd;

    if (pid != 0)
        return pid;
    sigprocmask(SIG_SETMASK, &g_mask0, NULL);
    setsid();
    fd = open("/dev/ttyS0", O_RDWR | O_NOCTTY);
    if (fd < 0)
        _exit(127);
    ioctl(fd, TIOCSCTTY, 0);
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2)
        close(fd);
    setenv("PATH", "/usr/bin:/bin:/sbin:/usr/sbin", 1);
    setenv("HOME", "/root", 1);
    setenv("TERM", "vt100", 1);
    setenv("PS1", "omnios-serial# ", 1);
    execl("/bin/sh", "sh", "-i", (char *)NULL);
    _exit(127);
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
    char *fbset_args[]   = { "/usr/bin/omnios-desktop", (char *)NULL };
    char *net_args[]     = { "/bin/sh", "/etc/init.d/network", (char *)NULL };
    char *update_args[]  = { "/usr/bin/omnios-update", "daemon", (char *)NULL };
    pid_t shell = -1;
    time_t shell_started = 0;
    int want_shell;
    struct sigaction sa;
    sigset_t loop_mask;
    (void)argc; (void)argv;

    /* 1 is the PID; /init was the kernel's entry. The signals that matter
     * are blocked except while the main loop waits in sigsuspend(), so one
     * arriving between a check and the wait cannot be missed. SIGCHLD has
     * a handler so that a child's exit wakes the loop to reap it (the
     * default action would let zombies pile up). */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sig;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sa.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    sigemptyset(&loop_mask);
    sigaddset(&loop_mask, SIGINT);
    sigaddset(&loop_mask, SIGTERM);
    sigaddset(&loop_mask, SIGQUIT);
    sigaddset(&loop_mask, SIGCHLD);

    omni_mount_all();
    setup_stdio();

    /* The kernel booted "quiet" (errors only), which keeps its hundreds of
     * boot messages from slowing the boot down. From here on messages reach
     * the consoles again, so the serial log still shows OmniOS's own and
     * everything after them. (The desktop puts the screen in graphics mode,
     * so none of this draws over it.) */
    {
        int fd = open("/proc/sys/kernel/printk", O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            (void)!write(fd, "7", 1);
            close(fd);
        }
    }
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

    /* from here on those signals arrive only in sigsuspend() below;
     * g_mask0 is the mask to give children back */
    sigprocmask(SIG_BLOCK, &loop_mask, &g_mask0);

    /* the graphical desktop, first: it is what the user waits for */
    omni_spawn(fbset_args);

    /* DHCP on every wired adapter, then OmniOS Update (it waits for the
     * network by itself) */
    if (access("/etc/init.d/network", R_OK) == 0)
        omni_spawn(net_args);
    if (access("/usr/bin/omnios-update", X_OK) == 0)
        omni_spawn(update_args);

    want_shell = cmdline_has("omnios.serialshell");
    if (want_shell) {
        shell = serial_shell();
        shell_started = time(NULL);
    }

    omni_log("OmniOS init: services started, entering maintainer loop\n");

    for (;;) {
        if (g_shutdown || g_reboot) {
            omni_log("OmniOS init: mandated by signal\n");
            sync();
            if (g_reboot && kexec_loaded())
                restart_into_update();
            reboot_now(g_reboot ? RB_AUTOBOOT : RB_POWER_OFF);
        }

        /* reap children (fast, non-blocking): orphans are ours too */
        for (;;) {
            pid_t p = waitpid(-1, NULL, WNOHANG);
            if (p <= 0)
                break;
            if (p == shell)
                shell = -1;
        }

        /* the serial shell exited: start another, at most one a second */
        if (want_shell && shell <= 0) {
            if (time(NULL) - shell_started >= 1) {
                shell = serial_shell();
                shell_started = time(NULL);
            } else {
                struct timespec ts = { 1, 0 };
                nanosleep(&ts, NULL);
                continue;
            }
        }

        /* wait for a signal: a child exited, or shutdown/restart */
        sigsuspend(&g_mask0);
    }

    return 0;
}
