/*
 * OmniOS — tools/shot/shot.c
 *
 * Screenshots of the real desktop without a screen: this builds the desktop
 * shell (os/gui/shell.c, included whole so its internals can be driven)
 * against a frame buffer in memory, runs the display server on the usual
 * socket so real app binaries can open windows in it, plays a list of
 * steps and writes PPM images. For working on the look of OmniOS on a
 * machine without a VM; tools/shot/run.sh builds it and turns the images
 * into PNG.
 *
 *   shot [-W width] [-H height] [-b bindir] step...
 *
 *   unlock              go past the lock screen to the desktop
 *   lock                show the lock screen
 *   menu | qs | cal     open Start, Quick Settings, the calendar
 *   run:PATH[:ARG]      start an app (/usr/bin/... is looked up in bindir)
 *   wait:MS             run the display server for MS milliseconds
 *   key:CODE            a key press + release (Linux key codes)
 *   type:TEXT           type TEXT into the focused window
 *   click:X,Y           a left click
 *   rclick:X,Y          a right click
 *   move:X,Y            move the pointer there (hover effects)
 *   shot:FILE.ppm       draw the frame and save it
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

static const char *g_bindir = "build/install/os/bin";

/* /usr/bin/omnios-x -> <bindir>/omnios-x, if it is there */
static const char *shot_map(const char *path, char *buf, size_t n)
{
    if (path && strncmp(path, "/usr/bin/", 9) == 0) {
        snprintf(buf, n, "%s/%s", g_bindir, path + 9);
        if (faccessat(AT_FDCWD, buf, X_OK, 0) == 0)
            return buf;
    }
    return path;
}

static int shot_access(const char *path, int mode)
{
    char buf[512];
    return faccessat(AT_FDCWD, shot_map(path, buf, sizeof(buf)), mode, 0);
}

static int shot_execl(const char *path, const char *a0, ...)
{
    char buf[512];
    char *argv[16];
    va_list ap;
    int n = 0;
    argv[n++] = (char *)a0;
    va_start(ap, a0);
    while (n < 15 && (argv[n] = va_arg(ap, char *)) != NULL)
        n++;
    va_end(ap);
    argv[n] = NULL;
    return execv(shot_map(path, buf, sizeof(buf)), argv);
}

#define access shot_access
#define execl  shot_execl
#include "../../os/gui/shell.c"
#undef access
#undef execl

/* run the display server (app sockets) for ms milliseconds, then draw */
static void pump(int ms)
{
    long long end = now_ms() + ms;
    for (;;) {
        struct pollfd pfds[1 + OMNI_WM_MAX_CLI];
        int map[OMNI_WM_MAX_CLI], n = 1, ci, left = (int)(end - now_ms());
        if (left < 0)
            break;
        pfds[0].fd = omni_wm_fd(&g_wm);
        pfds[0].events = POLLIN;
        for (ci = 0; ci < OMNI_WM_MAX_CLI; ci++) {
            map[ci] = -1;
            if (g_wm.clients[ci].fd >= 0) {
                pfds[n].fd = g_wm.clients[ci].fd;
                pfds[n].events = POLLIN;
                map[ci] = n++;
            }
        }
        if (poll(pfds, (nfds_t)n, left > 50 ? 50 : left) < 0)
            continue;
        if (pfds[0].revents & POLLIN)
            omni_wm_accept(&g_wm);
        for (ci = 0; ci < OMNI_WM_MAX_CLI; ci++) {
            int k = map[ci];
            if (k >= 0 && g_wm.clients[ci].fd == pfds[k].fd &&
                (pfds[k].revents & (POLLIN | POLLHUP | POLLERR)))
                omni_wm_handle_client(&g_wm, ci);
        }
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
    }
    update_hover();
    omni_wm_paint(&g_wm);
}

static void save_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    int x, y;
    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", g_dev.w, g_dev.h);
    for (y = 0; y < g_dev.h; y++) {
        const uint32_t *row = g_dev.bits + (size_t)y * (size_t)g_dev.stride;
        for (x = 0; x < g_dev.w; x++) {
            unsigned char px[3] = { (unsigned char)(row[x] >> 16),
                                    (unsigned char)(row[x] >> 8),
                                    (unsigned char)row[x] };
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
    fprintf(stderr, "shot: %s\n", path);
}

static void click(int x, int y, int btn)
{
    g_px = g_wm.pointer_x = x;
    g_py = g_wm.pointer_y = y;
    omni_wm_motion(&g_wm, x, y);
    update_hover();
    if (g_locked) {
        lock_button(x, y);
        return;
    }
    if (!shell_button(x, y, btn)) {
        omni_wm_button(&g_wm, x, y, btn, 1);
        omni_wm_button(&g_wm, x, y, btn, 0);
    }
}

static void key(int code, char text)
{
    if (g_locked) {
        lock_key(code, 1, text);
        lock_key(code, 0, text);
        return;
    }
    if (!shell_key(code, 1, text) && g_wm.active)
        omni_wm_key(&g_wm, code, 1, text);
    if (!shell_key(code, 0, text) && g_wm.active)
        omni_wm_key(&g_wm, code, 0, text);
}

int main(int argc, char **argv)
{
    int W = 1280, H = 800, i = 1;
    struct raster dev;
    uint32_t *bits;

    signal(SIGPIPE, SIG_IGN);
    for (; i + 1 < argc && argv[i][0] == '-'; i += 2) {
        if (strcmp(argv[i], "-W") == 0) W = atoi(argv[i + 1]);
        else if (strcmp(argv[i], "-H") == 0) H = atoi(argv[i + 1]);
        else if (strcmp(argv[i], "-b") == 0) g_bindir = argv[i + 1];
    }
    bits = calloc((size_t)W * (size_t)H, sizeof(uint32_t));
    if (!bits)
        return 1;
    raster_init(&dev, bits, W, H, W);
    unlink(OMNI_WM_SOCKET);
    if (shell_display_init(dev) != 0)
        return 1;
    if (omni_wm_start(&g_wm) != 0)
        fprintf(stderr, "shot: no display socket (%s)\n", OMNI_WM_SOCKET);
    g_px = g_wm.pointer_x = W / 2;
    g_py = g_wm.pointer_y = H / 2;
    g_input_ok_ptr = g_input_ok_kbd = 1;        /* no devices here */
    pump(0);

    for (; i < argc; i++) {
        const char *s = argv[i];
        int x, y;
        if (strcmp(s, "unlock") == 0) {
            shell_unlock();
            pump(0);
        } else if (strcmp(s, "lock") == 0) {
            shell_lock();
            pump(0);
        } else if (strcmp(s, "menu") == 0) {
            open_menu();
            pump(0);
        } else if (strcmp(s, "qs") == 0) {
            open_qs();
            pump(0);
        } else if (strcmp(s, "cal") == 0) {
            open_cal();
            pump(0);
        } else if (strncmp(s, "run:", 4) == 0) {
            char path[256], *arg;
            snprintf(path, sizeof(path), "%s", s + 4);
            arg = strchr(path, ':');
            if (arg)
                *arg++ = '\0';
            run_app(path, arg);
        } else if (strncmp(s, "wait:", 5) == 0) {
            pump(atoi(s + 5));
        } else if (strncmp(s, "key:", 4) == 0) {
            key(atoi(s + 4), 0);
            pump(30);
        } else if (strncmp(s, "type:", 5) == 0) {
            const char *t;
            for (t = s + 5; *t; t++)
                key(0, *t);
            pump(30);
        } else if (sscanf(s, "click:%d,%d", &x, &y) == 2) {
            click(x, y, 1);
            pump(30);
        } else if (sscanf(s, "rclick:%d,%d", &x, &y) == 2) {
            click(x, y, 3);
            pump(30);
        } else if (sscanf(s, "move:%d,%d", &x, &y) == 2) {
            g_px = g_wm.pointer_x = x;
            g_py = g_wm.pointer_y = y;
            omni_wm_motion(&g_wm, x, y);
            pump(0);
        } else if (strncmp(s, "shot:", 5) == 0) {
            pump(0);
            cursor_move(g_px, g_py);
            save_ppm(s + 5);
        } else {
            fprintf(stderr, "shot: unknown step %s\n", s);
        }
    }
    unlink(OMNI_WM_SOCKET);
    return 0;
}
