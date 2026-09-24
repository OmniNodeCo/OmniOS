/*
 * OmniOS — os/apps/terminal.c
 *
 * The OmniOS Terminal: runs a real shell (/bin/sh -l) on a pseudo-terminal
 * and shows it through the VT100-subset emulator in vt.c. Keys become the
 * byte sequences a terminal sends (Enter = CR, Backspace = DEL, Delete =
 * ESC [ 3 ~, arrows, Ctrl+letter, ...); the shell's output is parsed into
 * a cell grid and only the rows that changed are redrawn, as runs of
 * same-coloured text.
 *
 * Closing the window hangs up the shell; typing `exit` closes the window.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "../gui/client.h"
#include "../gui/wm.h"          /* OMNI_WM_TITLE_H */
#include "vt.h"

#define CELL_W   8
#define CELL_H   10             /* 8x8 glyph + 2 px of leading          */
#define PAD      4
#define FRAME_MS 16             /* redraw at most ~60x/s while busy     */

/* 16-colour palette; 0 doubles as the terminal background */
static const uint32_t g_pal[16] = {
    0x101418, 0xcd3131, 0x0dbc79, 0xe5e510, 0x2472c8, 0xbc3fbc, 0x11a8cd, 0xd0d0d0,
    0x6e7681, 0xf14c4c, 0x23d18b, 0xf5f543, 0x3b8eea, 0xd670d6, 0x29b8db, 0xffffff,
};

static struct vt g_vt;          /* ~80 KB: keep it off the stack        */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Start argv on a new pseudo-terminal of cols x rows. Returns the child
 * pid and the (non-blocking) master fd in *master, or -1 with a reason in
 * err. The child becomes a session leader with the pty as its controlling
 * terminal, so job control and Ctrl+C work as usual. */
pid_t term_spawn(int *master, int cols, int rows, char *const argv[],
                 char *err, size_t errsz)
{
    struct winsize ws;
    const char *slave;
    pid_t pid;
    int m;

    m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0) {
        snprintf(err, errsz, "no pseudo-terminal (/dev/ptmx): %s", strerror(errno));
        return -1;
    }
    if (grantpt(m) < 0 || unlockpt(m) < 0 || !(slave = ptsname(m))) {
        snprintf(err, errsz, "pseudo-terminal setup failed: %s", strerror(errno));
        close(m);
        return -1;
    }
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    ws.ws_xpixel = (unsigned short)(cols * CELL_W);
    ws.ws_ypixel = (unsigned short)(rows * CELL_H);
    ioctl(m, TIOCSWINSZ, &ws);

    pid = fork();
    if (pid < 0) {
        snprintf(err, errsz, "fork failed: %s", strerror(errno));
        close(m);
        return -1;
    }
    if (pid == 0) {
        struct termios tio;
        int s, fd;

        setsid();
        s = open(slave, O_RDWR);
        if (s < 0)
            _exit(126);
        ioctl(s, TIOCSCTTY, 0);
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        for (fd = 3; fd < 256; fd++)       /* no desktop sockets leak in */
            close(fd);
        if (tcgetattr(0, &tio) == 0) {
            tio.c_cc[VERASE] = 0x7f;       /* Backspace sends DEL        */
            tio.c_iflag |= IUTF8;
            tcsetattr(0, TCSANOW, &tio);
        }
        signal(SIGPIPE, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        setenv("TERM", "linux", 1);
        {   /* the kernel hands init HOME=/; use the account's own home */
            struct passwd *pw = getpwuid(getuid());
            if (pw && pw->pw_dir && *pw->pw_dir) {
                setenv("HOME", pw->pw_dir, 1);
                setenv("USER", pw->pw_name, 1);
                setenv("LOGNAME", pw->pw_name, 1);
            } else if (!getenv("HOME")) {
                setenv("HOME", "/root", 1);
            }
        }
        if (!getenv("PATH"))
            setenv("PATH", "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", 1);
        setenv("SHELL", argv[0], 1);
        if (chdir(getenv("HOME")) != 0)
            (void)chdir("/");
        execv(argv[0], argv);
        fprintf(stderr, "\r\ncannot run %s: %s\r\n", argv[0], strerror(errno));
        _exit(127);
    }
    fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK);
    *master = m;
    return pid;
}

/* effective colours of one cell (bold brightens, reverse/cursor swap) */
static void cell_colors(const struct vt *t, int r, int x, int cursor,
                        uint32_t *fg, uint32_t *bg, int *ul)
{
    const struct vt_cell *c = &t->cell[r][x];
    int f = c->fg, b = c->bg;
    if ((c->attr & VT_BOLD) && f < 8)
        f += 8;
    if (c->attr & VT_REVERSE) { int tmp = f; f = b; b = tmp; }
    if (cursor)               { int tmp = f; f = b; b = tmp; }
    *fg = g_pal[f & 15];
    *bg = g_pal[b & 15];
    *ul = (c->attr & VT_UNDERLINE) != 0;
}

/* Draw every dirty row as runs of identically-coloured cells: one FILL
 * for the run's background (covering the leading) plus one TEXTC. */
static void render(struct omni_client_conn *c, struct vt *t)
{
    int r;
    for (r = 0; r < t->rows; r++) {
        int x0 = 0;
        if (!t->dirty[r])
            continue;
        t->dirty[r] = 0;
        while (x0 < t->cols) {
            uint32_t fg, bg, fg2, bg2;
            int ul, ul2, x1, n, blank = 1;
            char text[VT_MAX_COLS + 1];
            int cur0 = t->cursor_on && r == t->cy && x0 == t->cx;

            cell_colors(t, r, x0, cur0, &fg, &bg, &ul);
            for (x1 = x0 + 1; x1 < t->cols; x1++) {
                int cur = t->cursor_on && r == t->cy && x1 == t->cx;
                cell_colors(t, r, x1, cur, &fg2, &bg2, &ul2);
                if (fg2 != fg || bg2 != bg || ul2 != ul)
                    break;
            }
            for (n = 0; n < x1 - x0; n++) {
                text[n] = (char)t->cell[r][x0 + n].ch;
                if (text[n] != ' ')
                    blank = 0;
            }
            text[n] = '\0';
            omni_client_fill(c, PAD + x0 * CELL_W, PAD + r * CELL_H,
                             n * CELL_W, CELL_H, bg);
            if (!blank)
                omni_client_textc(c, PAD + x0 * CELL_W, PAD + r * CELL_H + 1,
                                  fg, bg, text);
            if (ul)
                omni_client_fill(c, PAD + x0 * CELL_W,
                                 PAD + r * CELL_H + CELL_H - 1,
                                 n * CELL_W, 1, fg);
            x0 = x1;
        }
    }
}

static void write_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                struct pollfd pf = { fd, POLLOUT, 0 };
                poll(&pf, 1, 100);
                continue;
            }
            return;
        }
        p += w;
        n -= (size_t)w;
    }
}

#ifndef OMNI_TERMINAL_NO_MAIN
int main(void)
{
    struct omni_client_conn conn;
    char *sh_argv[] = { "/bin/sh", "-l", NULL };
    const char *sh = getenv("SHELL");
    struct passwd *pw;
    char err[160] = "";
    int cols = 80, rows = 25, master = -1, running = 1, child_gone = 0;
    long last_frame = 0;
    pid_t pid;

    signal(SIGPIPE, SIG_IGN);
    /* shell: $SHELL, else the account's login shell, else /bin/sh */
    if (!sh || access(sh, X_OK) != 0) {
        pw = getpwuid(getuid());
        sh = (pw && pw->pw_shell && access(pw->pw_shell, X_OK) == 0)
             ? pw->pw_shell : "/bin/sh";
    }
    sh_argv[0] = (char *)sh;
    memset(&conn, 0, sizeof(conn));
    if (omni_client_open(&conn, "OmniOS Terminal") < 0)
        return 127;

    /* 80x25, shrunk if the screen is small (leave room for the taskbar) */
    if (conn.screen_w > 0 && conn.screen_h > 0) {
        int maxc = (conn.screen_w - 48 - 2 * PAD) / CELL_W;
        int maxr = (conn.screen_h - OMNI_TASKBAR_H - 48 - OMNI_WM_TITLE_H - 2 * PAD) / CELL_H;
        if (cols > maxc) cols = maxc;
        if (rows > maxr) rows = maxr;
        if (cols < 20) cols = 20;
        if (rows < 5)  rows = 5;
    }
    if (omni_client_window(&conn, "Terminal", cols * CELL_W + 2 * PAD,
                           rows * CELL_H + 2 * PAD + OMNI_WM_TITLE_H) < 0) {
        omni_client_close(&conn);
        return 127;
    }
    omni_client_clear(&conn, g_pal[0]);

    vt_init(&g_vt, cols, rows);
    pid = term_spawn(&master, cols, rows, sh_argv, err, sizeof(err));
    if (pid < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "\033[1;31mOmniOS Terminal: cannot start %s\033[0m\r\n%s\r\n",
                 sh_argv[0], err);
        vt_feed(&g_vt, msg, strlen(msg));
        child_gone = 1;
    }
    render(&conn, &g_vt);

    while (running) {
        struct pollfd pf[2];
        int nfd = 1, pending = 0, r, x;
        long wait_ms = -1;

        for (x = 0; x < g_vt.rows; x++)
            pending |= g_vt.dirty[x];
        if (pending)
            wait_ms = FRAME_MS - (now_ms() - last_frame);
        if (wait_ms < -1 || (pending && wait_ms < 0))
            wait_ms = 0;

        pf[0].fd = conn.fd;  pf[0].events = POLLIN; pf[0].revents = 0;
        if (master >= 0 && !child_gone) {
            pf[1].fd = master; pf[1].events = POLLIN; pf[1].revents = 0;
            nfd = 2;
        }
        if (poll(pf, (nfds_t)nfd, (int)wait_ms) < 0 && errno != EINTR)
            break;

        /* shell output -> emulator (bounded per pass to stay responsive) */
        if (nfd == 2 && (pf[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buf[4096];
            int budget = 16;
            while (budget-- > 0) {
                ssize_t n = read(master, buf, sizeof(buf));
                if (n > 0) {
                    vt_feed(&g_vt, buf, (size_t)n);
                    if (g_vt.nreply) {
                        write_all(master, g_vt.reply, (size_t)g_vt.nreply);
                        g_vt.nreply = 0;
                    }
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EINTR))
                    break;
                child_gone = 1;             /* EOF / EIO: the shell ended */
                break;
            }
        }

        /* desktop events: keys -> pty, window close -> quit */
        if (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            struct omni_client_event e;
            while ((r = omni_client_poll(&conn, &e)) > 0) {
                if (e.type == 3) {
                    running = 0;
                    break;
                }
                if (e.type == 1 && e.pressed && master >= 0 && !child_gone) {
                    char seq[16];
                    int n = vt_key(&g_vt, e.key, e.text, seq, (int)sizeof(seq));
                    if (n > 0)
                        write_all(master, seq, (size_t)n);
                }
            }
            if (r < 0)
                running = 0;
        }

        if (now_ms() - last_frame >= FRAME_MS || child_gone) {
            render(&conn, &g_vt);
            last_frame = now_ms();
        }
        if (child_gone && pid > 0)
            running = 0;                    /* `exit` closes the window  */
    }

    if (master >= 0)
        close(master);                      /* hangs up the session      */
    if (pid > 0) {
        int i;
        kill(pid, SIGHUP);
        for (i = 0; i < 20 && waitpid(pid, NULL, WNOHANG) == 0; i++)
            usleep(10000);
    }
    omni_client_close(&conn);
    return 0;
}
#endif
