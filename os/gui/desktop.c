/*
 * OmniOS — os/gui/desktop.c
 *
 * Entry point of the desktop shell binary (/usr/bin/omnios-desktop).
 */
#include <fcntl.h>
#include <unistd.h>

#include "shell.h"

/* Make sure fds 0-2 are open before anything else. If the desktop is
 * started with them closed, the framebuffer and input devices would be
 * handed out as fds 0-2, and any stray stdout/stderr write would land
 * in /dev/fb0 or an evdev device. */
static void stdio_sanitize(void)
{
    int fd;

    while ((fd = open("/dev/null", O_RDWR)) >= 0 && fd <= 2)
        ;
    if (fd > 2)
        close(fd);
}

int main(void)
{
    stdio_sanitize();
    omni_shell_run();
    return 0;
}
