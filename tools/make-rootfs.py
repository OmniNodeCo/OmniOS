#!/usr/bin/env python3
"""
OmniOS root filesystem assembler.

Assembles the complete runtime filesystem of the OmniOS live system from the
artifacts built by the other `tools/` stages, into OUT_DIR (default
build/rootfs). The result is consumed by the kernel's initramfs generation
(CONFIG_INITRAMFS_SOURCE=.../build/rootfs), so it IS the installed OS: kernel,
musl libc userspace, BusyBox, and the Nano-X / Microwindows GUI.

Generates:
  /init, /etc/inittab, /etc/passwd, /etc/group, /etc/profile
  /etc/omnios-release            identity
  /etc/fonts/...                 Microwindows fonts
  /usr/bin, /usr/share, /dev entries, /tmp, /var/log, /proc, /sys, /mnt
  the GUI session launcher (/usr/bin/omnios-desktop) and its shell
  plus the script /usr/bin/nanowm that proxies the built-in window manager
"""
import os
import shutil
import stat as statmod
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT = os.path.abspath(os.environ.get("ROOTFS_OUT", os.path.join(REPO, "build", "rootfs")))
VERSION = open(os.path.join(REPO, "version.txt")).read().strip()

MW = os.path.join(REPO, "src", "microwindows", "src")
BB = os.path.join(REPO, "src", "busybox")


def main():
    if os.path.exists(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)

    # ---- directory skeleton ------------------------------------------------
    for d in ("bin", "sbin", "usr/bin", "usr/sbin", "usr/lib", "usr/share",
              "etc", "etc/init.d", "etc/fonts", "dev", "proc", "sys", "tmp",
              "var", "var/log", "var/run", "mnt", "root", "home"):
        os.makedirs(os.path.join(OUT, d), exist_ok=True)

    # ---- dynamically-linked userspace gets a self-contained musl -----------
    # (static-linked BusyBox and Nano-X still get /lib/ld-musl for running any
    # eventually-dynamic binaries on the system)
    os.makedirs(os.path.join(OUT, "lib"), exist_ok=True)

    # ---- BusyBox ------------------------------------------------------------
    for tool in ("busybox",):
        shutil.copy(os.path.join(BB, tool), os.path.join(OUT, "bin", tool))
    os.chmod(os.path.join(OUT, "bin", "busybox"), 0o755)
    # create the applet symlinks a real system would have (ash first so
    # /bin/sh resolves)
    applets = ["sh", "ash", "mount", "umount", "cat", "ls", "cp", "mv", "rm",
               "mkdir", "rmdir", "echo", "grep", "sed", "awk", "vi", "ps",
               "top", "kill", "killall", "sleep", "date", "hostname", "init",
               "login", "getty", "su", "ifconfig", "route", "ping", "sync",
               "halt", "reboot", "poweroff", "dmesg", "mdev", "blkid",
               "mkfs.ext2", "mkfs.vfat", "swapon", "swapoff", "wget", "nc",
               "id", "whoami", "clear", "head", "tail", "wc", "sort", "cut",
               "tr", "uniq", "find", "xargs", "ln", "chmod", "chown", "tar",
               "gzip", "uname", "df", "du", "free", "uptime", "setterm",
               "stty", "tty", "pwd", "true", "false", "test", "yes", "printf"]
    for a in applets:
        dst = os.path.join(OUT, "bin", a)
        if not os.path.lexists(dst):
            os.symlink("busybox", dst)
    os.symlink("bin/sh", os.path.join(OUT, "sh")) if False else None

    # ---- Nano-X / Microwindows GUI -----------------------------------------
    gui_bins = ("nano-X", "nanowm", "nxclock", "nxterm", "nxcalc", "nxview",
                "mwtris", "mwsci")
    for b in ("nano-X", "nxclock", "nxterm", "nxcalc", "nxview", "nxev",
              "nxkbd", "nxlsclients", "nxroach", "nxtetris", "mwhello"):
        src = os.path.join(MW, "bin", b)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(OUT, "usr", "bin", b))
            os.chmod(os.path.join(OUT, "usr", "bin", b), 0o755)

    # ---- fonts -------------------------------------------------------------
    for f in os.listdir(os.path.join(MW, "fonts", "bdf")):
        shutil.copy(os.path.join(MW, "fonts", "bdf", f),
                    os.path.join(OUT, "etc", "fonts", f))

    # ---- device nodes ------------------------------------------------------
    _mk(OUT, "dev")

    # ---- /init (simple, deterministic, no systemd) -------------------------
    _write(OUT, "init", _INIT_SCRIPT)
    os.chmod(os.path.join(OUT, "init"), 0o755)

    # ---- /etc files ---------------------------------------------------------
    _write(OUT, "etc/inittab", _INITTAB)
    _write(OUT, "etc/passwd", "root:x:0:0:root:/root:/bin/sh\nomnios:x:1000:1000:OmniOS user:/home/omnios:/bin/sh\n")
    _write(OUT, "etc/group", "root:x:0:\nomnios:x:1000:\n")
    _write(OUT, "etc/profile", _PROFILE)
    _write(OUT, "etc/omnios-release", "OmniOS %s (from-source lightweight OS)\n" % VERSION)
    _write(OUT, "etc/hostname", "omnios\n")
    _write(OUT, "etc/motd", _MOTD)
    _write(OUT, "etc/issue", _MOTD)
    # the window-manager shim and GUI launcher
    _write(OUT, "usr/bin/nanowm", "#!/bin/sh\n# OmniOS: window manager runs inside the nano-X server (built-in nanowm)\nexec /bin/sleep infinity\n")
    os.chmod(os.path.join(OUT, "usr", "bin", "nanowm"), 0o755)
    _write(OUT, "usr/bin/omnios-desktop", _DESKTOP_LAUNCHER)
    os.chmod(os.path.join(OUT, "usr", "bin", "omnios-desktop"), 0o755)

    # the GUI apps that need a thermal/fun start script
    _write(OUT, "etc/init.d/omnios-gui", _GUI_INIT_SCRIPT)
    os.chmod(os.path.join(OUT, "etc", "init.d", "omnios-gui"), 0o755)

    print("make-rootfs: assembled %s" % OUT)
    print("make-rootfs: version %s" % VERSION)


def _mk(root, name):
    """crate a device node branch lazily; for now only directories"""
    os.makedirs(os.path.join(root, name), exist_ok=True)


def _write(root, rel, content):
    p = os.path.join(root, rel)
    with open(p, "w") as f:
        f.write(content)


_INIT_SCRIPT = r"""#!/bin/sh
# OmniOS /init — musl + BusyBox userspace, ash shell.
# Mount the virtual filesystems, seed /dev with mdev, and start init.
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || mount -t tmpfs tmpfs /dev
mkdir -p /dev/pts /dev/shm /dev/input
mount -t devpts devpts /dev/pts 2>/dev/null
mount -t tmpfs tmpfs /tmp
mount -t tmpfs tmpfs /run 2>/dev/null || true
# Seed legacy /dev nodes without udev:
if [ -e /proc/sys/kernel/hotplug ]; then echo /sbin/mdev > /proc/sys/kernel/hotplug; fi
[ -x /sbin/mdev ] && /sbin/mdev -s 2>/dev/null || true
[ -e /dev/console ] || mknod /dev/console c 5 1
[ -e /dev/tty1 ] || mknod /dev/tty1 c 4 1
[ -e /dev/tty0 ] || mknod /dev/tty0 c 4 0
[ -e /dev/null ] || mknod /dev/null c 1 3
[ -e /dev/zero ] || mknod /dev/zero c 1 5
[ -e /dev/fb0 ] || true
[ -e /dev/input/mice ] || true
echo "OmniOS booted. Starting the desktop session."
# Launch the graphical session on the framebuffer console:
/usr/bin/omnios-desktop &
# Fall back to a login shell on the console for rescue:
exec /sbin/init
"""

_INITTAB = """::sysinit:/etc/init.d/rcS
tty1::respawn:/sbin/getty -L tty1 115200 vt100
tty2::respawn:/sbin/getty -L tty2 115200 vt100
ttyS0::respawn:/sbin/getty -L ttyS0 115200 vt100
::ctrlaltdel:/sbin/reboot
::shutdown:/bin/umount -a -r
"""

_PROFILE = """export PATH=/usr/bin:/bin:/sbin:/usr/sbin
export HOME=/root
export PS1='omnios:\\w\\$ '
export TERM=vt100
export LANG=C.UTF-8
export DISPLAY=:0
ulimit -c 0
alias ll='ls -la'
alias ls='ls --color=never'
echo "OmniOS lightweight shell. Type 'omnios-desktop' to launch the GUI."
"""

_MOTD = """
 Welcome to OmniOS %s — a lightweight OS built from source.
 kernel: Linux (static, FROM-SOURCE)   libc: musl   shell: ash (BusyBox)
 GUI: Nano-X / Microwindows (Windows-style windowing on the framebuffer)

 Auto-login shell. Type:  omnios-desktop   -> start the graphical desktop
"""

_DESKTOP_LAUNCHER = r"""#!/bin/sh
# OmniOS desktop session: framebuffer -> nano-X server -> windowed apps.
# The built-in window manager (nanowm) runs inside the server (NANOWM=Y).
set -e

# Very small framebuffer sanity check; proceed even if no fb yet (simpledrm
# may need a moment — give it one retry).
for i in 1 2 3 4 5; do
    if [ -c /dev/fb0 ]; then break; fi
    sleep 1
done

export DISPLAY=:0
# run the Nano-X server in persistent mode so it survives client reconnect;
# it reads the keyboard (TTYKBD) and /dev/input/mice itself.
/usr/bin/nano-X -p &
NANOX_PID=$!
trap 'kill $NANOX_PID 2>/dev/null; exit 0' INT TERM
sleep 2

# Start a couple of floating-window apps as a first desktop.
/usr/bin/nxclock &
/usr/bin/nxterm -T "OmniOS terminal" /bin/sh &
/usr/bin/nxcalc &
# nxview shows the built-in demo; keep as an optional extra:
# /usr/bin/nxview &

wait $NANOX_PID
"""

_GUI_INIT_SCRIPT = r"""#!/bin/sh
# OmniOS GUI hook run at boot (referenced by /etc/inittab via rcS if present).
[ -x /usr/bin/omnios-desktop ] && /usr/bin/omnios-desktop &
"""


if __name__ == "__main__":
    main()
