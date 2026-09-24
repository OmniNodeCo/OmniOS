#!/usr/bin/env python3
"""
OmniOS root filesystem assembler.

Assembles the complete runtime filesystem of the OmniOS system from the
artifacts produced by the other build stages, into ROOTFS_OUT
(default build/rootfs). This tree is the OS itself: it is embedded into the
kernel as the initramfs via CONFIG_INITRAMFS_SOURCE, so everything below `/`
is authored here.

Layout produced:
    /init                    first process: mount, seed /dev, exec init
    /etc/inittab             BusyBox init (getty on tty1..2, ttyS0)
    /etc/init.d/rcS          sysinit hook (mdev, hostname, motd, DM)
    /etc/passwd, /etc/group
    /etc/profile             interactive ash environment
    /etc/omnios-release      identity
    /usr/bin/omnios-desktop  launches the graphical desktop (nano-X + nanowm)
    /bin/*                   BusyBox + applet symlinks
    /usr/bin/nano-X ...      the Nano-X GUI binaries
    /etc/fonts/*             built-in BDF fonts for the GUI

Everything is deterministic and never requires superuser privileges.
"""
import os
import shutil
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT = os.path.abspath(os.environ.get("ROOTFS_OUT", os.path.join(REPO, "build", "rootfs")))
VERSION = open(os.path.join(REPO, "version.txt")).read().strip()

# Where the per-stage builders put their install trees.
MW_BIN = os.path.join(REPO, "build", "install", "microwindows", "bin")
MW_FONTS = os.path.join(REPO, "build", "install", "microwindows", "fonts")
BB = os.path.join(REPO, "build", "install", "busybox")
OS_BIN = os.path.join(REPO, "build", "install", "os", "bin")


# ---- static file contents ------------------------------------------------
_INIT = """\
#!/bin/sh
# OmniOS /init — the very first userspace process (ash, BusyBox).
# Mounts the virtual filesystems, seeds /dev, then hands control to the
# from-scratch PID 1 (/sbin/init -> /usr/bin/ominit). ominit itself also
# performs these mounts, so if it is missing we fall back to the BusyBox
# init/inittab path below.
export PATH=/usr/bin:/bin:/sbin:/usr/sbin

/bin/mount -t proc     proc     /proc
/bin/mount -t sysfs    sysfs    /sys
/bin/busybox mount -t devtmpfs devtmpfs /dev 2>/dev/null \\
    || /bin/mount -t tmpfs tmpfs /dev
mkdir -p /dev/pts /dev/shm /dev/input
/bin/mount -t devpts devpts /dev/pts 2>/dev/null
/bin/mount -t tmpfs tmpfs /tmp
/bin/mkdir -p /run /var/log

# Device nodes that mdev may not create before init runs.
/bin/mknod /dev/console c 5 1 2>/dev/null
/bin/mknod /dev/null    c 1 3 2>/dev/null
/bin/mknod /dev/zero    c 1 5 2>/dev/null
/bin/mknod /dev/tty     c 5 0 2>/dev/null
/bin/mknod /dev/tty0    c 4 0 2>/dev/null
/bin/mknod /dev/tty1    c 4 1 2>/dev/null
/bin/mknod /dev/tty2    c 4 2 2>/dev/null
/bin/mknod /dev/ttyS0   c 4 64 2>/dev/null

if [ -e /proc/sys/kernel/hotplug ]; then
    echo /sbin/mdev > /proc/sys/kernel/hotplug 2>/dev/null
fi
/bin/mdev -s 2>/dev/null || true

echo "OmniOS ${VERSION} — booting."

# Prefer the from-scratch PID 1.
if [ -x /sbin/init ]; then
    exec /sbin/init
fi
# Fallback: BusyBox init + /etc/inittab.
exec /bin/busybox init
""".replace("${VERSION}", VERSION)

_INITTAB = """\
# OmniOS /etc/inittab — BusyBox init
::sysinit:/etc/init.d/rcS
::respawn:-/bin/login
tty1::respawn:/sbin/getty -L tty1 0 vt100
tty2::respawn:/sbin/getty -L tty2 0 vt100
ttyS0::respawn:/sbin/getty -L ttyS0 115200 vt100
::restart:/sbin/init
::ctrlaltdel:/sbin/reboot
::shutdown:/bin/umount -a -r
::shutdown:/bin/swapoff -a
"""

_RCS = """\
#!/bin/sh
# OmniOS /etc/init.d/rcS — sysinit for BusyBox init
export PATH=/usr/bin:/bin:/sbin:/usr/sbin

/bin/hostname omnios
/bin/mount -o remount,rw / 2>/dev/null || true
/bin/mount -a 2>/dev/null || true

if [ -x /sbin/mdev ]; then
    /sbin/mdev -s
fi

/bin/cat /etc/motd > /dev/console 2>/dev/null

# A serial console gives a rescue shell even when no display is attached.
if [ -c /dev/ttyS0 ]; then
    echo "OmniOS: serial console available." > /dev/ttyS0 2>/dev/null
fi

# Bring up the graphical desktop if we are on the active console.
# (Only reached under the BusyBox-init fallback; ominit spawns the desktop
# binary itself and never runs rcS.)
if [ -x /etc/init.d/desktop ]; then
    /etc/init.d/desktop &
fi
exit 0
"""

_DESKTOP = """\
#!/bin/sh
# OmniOS graphical desktop session.
#
# Prefer the from-scratch OmniOS desktop shell (owns the framebuffer and runs
# a window-manager/display-server on /tmp/.omnios-wm). If it is not present,
# fall back to Nano-X (nano-X) with its window manager and demo apps.
export PATH=/usr/bin:/bin:/sbin:/usr/sbin
export DISPLAY=:0

# Wait briefly for the kernel to register /dev/fb0 (simpledrm / VESA / GOP).
i=0
while [ ! -e /dev/fb0 ] && [ "$i" -lt 50 ]; do
    sleep 0.1 2>/dev/null || sleep 1
    i=$((i + 1))
done

if [ ! -e /dev/fb0 ]; then
    echo "OmniOS: no framebuffer found — text console only."
    exec /bin/sh
fi

if [ -x /usr/bin/omnios-desktop ]; then
    echo "OmniOS: starting the desktop shell."
    exec /usr/bin/omnios-desktop
fi

# Nano-X fallback (kept for reference/demos)
echo "OmniOS: starting Nano-X desktop (fallback)."
/usr/bin/nano-X -p &
NANOX_PID=$!
trap 'kill $NANOX_PID 2>/dev/null' INT TERM EXIT
i=0
while [ ! -S /tmp/.nano-X ] && [ "$i" -lt 50 ]; do
    sleep 0.1 2>/dev/null || sleep 1
    i=$((i + 1))
done
/usr/bin/nxterm -T "OmniOS Terminal" /bin/sh &
/usr/bin/nxclock &
/usr/bin/nxcalc &
wait $NANOX_PID
"""

_PROFILE = """\
export PATH=/usr/bin:/bin:/sbin:/usr/sbin
export HOME=/root
export PS1='omnios:\\w\\$ '
export TERM=vt100
export DISPLAY=:0
export LANG=C.UTF-8
ulimit -c 0 2>/dev/null || true
alias ll='ls -la'
alias ls='ls --color=never'

echo "OmniOS ${VERSION} — lightweight OS (Linux + musl + OmniOS desktop)."
echo "Type 'omnios-desktop' to (re)start the graphical desktop."
""".replace("${VERSION}", VERSION)

_MOTD = """\
       OmniOS ${VERSION} — a lightweight OS built from source
         kernel: Linux (static, x86_64)   libc: musl
         shell: ash (BusyBox)             GUI: OmniOS desktop (from scratch)
""".replace("${VERSION}", VERSION)

_PASSWD = """\
root:x:0:0:Administrator:/root:/bin/sh
omnios:x:1000:1000:OmniOS user:/home/omnios:/bin/sh
"""

_GROUP = """\
root:x:0:
tty:x:5:
dialout:x:20:
audio:x:29:
video:x:44:
omnios:x:1000:
"""


def main():
    # ---- collection of built binaries ------------------------------------
    bb = os.path.join(BB, "busybox")
    if not os.path.exists(bb):
        print("make-rootfs: WARN: %s not built yet (build stage)." % bb,
              file=sys.stderr)

    if os.path.exists(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)

    # ---- directory skeleton ------------------------------------------------
    for d in ("bin", "sbin", "usr/bin", "usr/sbin", "usr/lib", "usr/share",
              "etc", "etc/init.d", "etc/fonts", "dev", "proc", "sys", "tmp",
              "run", "var", "var/log", "var/run", "var/lib/omnios", "mnt",
              "root", "home/omnios"):
        os.makedirs(os.path.join(OUT, d), exist_ok=True)

    # ---- OmniOS core binaries (from-scratch kernel core + GUI) -------------
    # ominit            the PID-1 init (replaces BusyBox init)
    # omnios-desktop    the desktop shell (window manager + display server)
    # omnios-*          bundled desktop apps
    for b in ("ominit", "omnios-desktop", "omnios-term", "omnios-files",
              "omnios-calc", "omnios-edit", "omnios-sysinfo", "omnios-about",
              "omnios-store", "omnios-clock", "omnios-snake",
              "omnios-settings"):
        s = os.path.join(OS_BIN, b)
        if os.path.exists(s):
            dst = os.path.join(OUT, "usr", "bin", b)
            shutil.copy(s, dst)
            os.chmod(dst, 0o755)
    # ominit doubles as /sbin/init (PID 1) and /init's exec target.
    # Install it BEFORE the BusyBox /sbin aliases below: overlaying a plain
    # file onto an absolute symlink would otherwise follow the link and try
    # to write the host's /bin/busybox (PermissionError).
    ominit = os.path.join(OS_BIN, "ominit")
    if os.path.exists(ominit):
        shutil.copy(ominit, os.path.join(OUT, "sbin", "init"))
        os.chmod(os.path.join(OUT, "sbin", "init"), 0o755)

    # ---- BusyBox and applet symlinks --------------------------------------
    if os.path.exists(bb):
        shutil.copy(bb, os.path.join(OUT, "bin", "busybox"))
        os.chmod(os.path.join(OUT, "bin", "busybox"), 0o755)
        for a in _APPLETS:
            dst = os.path.join(OUT, "bin", a)
            if not os.path.lexists(dst):
                os.symlink("busybox", dst)
        # /sbin aliases for programs scripts and mdev expect (init is ominit)
        for a in ("getty", "login", "mdev", "halt", "reboot",
                  "poweroff", "swapoff", "swapon", "blkid"):
            s = os.path.join(OUT, "sbin", a)
            if not os.path.lexists(s):
                os.symlink("/bin/busybox", s)
    else:
        print("make-rootfs: skipping BusyBox (not built).", file=sys.stderr)

    # ---- Nano-X / Microwindows GUI -----------------------------------------
    for b in ("nano-X", "nanowm", "nxterm", "nxclock", "nxcalc", "nxview",
              "nxev", "nxkbd", "nxlsclients", "nxroach", "nxtetris",
              "mwhello", "mwin", "mwsci"):
        s = os.path.join(MW_BIN, b)
        if os.path.exists(s):
            shutil.copy(s, os.path.join(OUT, "usr", "bin", b))
            os.chmod(os.path.join(OUT, "usr", "bin", b), 0o755)
    # fonts
    if os.path.isdir(MW_FONTS):
        for f in sorted(os.listdir(MW_FONTS)):
            shutil.copy(os.path.join(MW_FONTS, f),
                        os.path.join(OUT, "etc", "fonts", f))

    # ---- /init and /etc ----------------------------------------------------
    w("init", _INIT, 0o755)
    w("etc/inittab", _INITTAB)
    w("etc/init.d/rcS", _RCS, 0o755)
    w("etc/profile", _PROFILE)
    w("etc/passwd", _PASSWD)
    # no password until the user sets one (Settings > Accounts)
    w("etc/shadow", "root::20000:0:99999:7:::\n")
    os.chmod(os.path.join(OUT, "etc", "shadow"), 0o600)
    w("etc/group", _GROUP)
    w("etc/motd", _MOTD)
    w("etc/issue", _MOTD)
    w("etc/omnios-release", "OmniOS %s (from-source lightweight OS)\n" % VERSION)
    w("etc/os-release", _OS_RELEASE)
    w("etc/hostname", "omnios\n")
    w("etc/hosts", "127.0.0.1 localhost omnios\n::1 localhost omnios\n")
    w("etc/shells", "/bin/sh\n/bin/ash\n")
    w("etc/resolv.conf", "nameserver 8.8.8.8\nnameserver 1.1.1.1\n")
    w("etc/fstab",
      "proc  /proc proc  defaults 0 0\n"
      "sysfs /sys sysfs defaults 0 0\n"
      "tmpfs /tmp tmpfs defaults 0 0\n"
      "devpts /dev/pts devpts defaults 0 0\n")
    # Desktop session launcher (shell). The from-scratch window manager is
    # the COMPILED binary /usr/bin/omnios-desktop, copied above; this script
    # waits for /dev/fb0 and starts it (Nano-X fallback for the BusyBox path).
    w("etc/init.d/desktop", _DESKTOP, 0o755)

    print("make-rootfs: assembled %s" % OUT)


def w(rel, content, mode=0o644):
    p = os.path.join(OUT, rel)
    with open(p, "w") as f:
        f.write(content)
    os.chmod(p, mode)


# Every applet symlink the system may reference (/bin/<name>).
_APPLETS = [
    "sh", "ash", "mount", "umount", "cat", "ls", "cp", "mv", "rm", "mkdir",
    "rmdir", "echo", "grep", "sed", "awk", "vi", "ps", "top", "kill",
    "killall", "sleep", "date", "hostname", "init", "login", "getty", "su",
    "ifconfig", "route", "ping", "sync", "halt", "reboot", "poweroff",
    "dmesg", "mdev", "blkid", "mkfs.ext2", "mkfs.vfat", "swapon", "swapoff",
    "wget", "nc", "id", "whoami", "clear", "head", "tail", "wc", "sort",
    "cut", "tr", "uniq", "find", "xargs", "ln", "chmod", "chown", "tar",
    "gzip", "gunzip", "uname", "df", "du", "free", "uptime", "setterm",
    "stty", "tty", "pwd", "true", "false", "test", "yes", "printf", "mknod",
    "expr", "basename", "dirname", "which", "passwd", "adduser", "chroot",
    "fsck", "mkfs", "losetup", "ip", "nameif", "telnet", "telnetd",
    "httpd", "ftpd", "getopt", "watch", "less", "more", "md5sum", "sha256sum",
]

_OS_RELEASE = """\
NAME="OmniOS"
ID=omnios
PRETTY_NAME="OmniOS %s"
VERSION="%s"
VERSION_ID=%s
HOME_URL="https://github.com/OmniNodeCo/OmniOS"
""" % (VERSION, VERSION, VERSION.split(".")[0])


if __name__ == "__main__":
    main()
