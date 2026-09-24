#!/usr/bin/env python3
"""
OmniOS root filesystem assembler.

Assembles the complete runtime filesystem of the OmniOS system from the
artifacts produced by the other build stages, into ROOTFS_OUT
(default build/rootfs). This tree is the OS itself: it is embedded into the
kernel as the initramfs via CONFIG_INITRAMFS_SOURCE, so everything below `/`
is authored here.

Layout produced:
    /init                    -> /usr/bin/ominit, the from-scratch PID 1: mounts
                             the virtual filesystems and starts the desktop,
                             the network and OmniOS Update
    /sbin/init               -> /usr/bin/ominit as well
    /usr/bin/omnios-*        the desktop shell and the apps
    /etc/init.d/network      DHCP on every wired adapter (started by ominit)
    /usr/share/udhcpc/default.script   applies a DHCP lease
    /etc/omnios-update.conf  where OmniOS Update finds new releases
    /etc/passwd, /etc/group, /etc/shadow
    /etc/profile             interactive ash environment (Terminal)
    /etc/omnios-release      identity
    /bin/*                   BusyBox + applet symlinks (shell, wget, udhcpc)

Everything is deterministic and never requires superuser privileges.
"""
import os
import shutil
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT = os.path.abspath(os.environ.get("ROOTFS_OUT", os.path.join(REPO, "build", "rootfs")))
VERSION = open(os.path.join(REPO, "version.txt")).read().strip()

# Where the per-stage builders put their install trees.
BB = os.path.join(REPO, "build", "install", "busybox")
OS_BIN = os.path.join(REPO, "build", "install", "os", "bin")


# ---- static file contents ------------------------------------------------
_NETWORK = """\
#!/bin/sh
# OmniOS /etc/init.d/network: started by ominit at boot. Loopback, then DHCP
# on every wired adapter (VMware's e1000 / e1000e / vmxnet3 show up as eth0,
# ...). udhcpc keeps the lease renewed in the background;
# /usr/share/udhcpc/default.script applies it (address, route, DNS).
export PATH=/usr/bin:/bin:/sbin:/usr/sbin
ifconfig lo 127.0.0.1 netmask 255.0.0.0 up 2>/dev/null
for dev in /sys/class/net/*; do
    ifc=${dev##*/}
    [ "$ifc" = lo ] && continue
    [ -e "$dev/device" ] || continue            # hardware adapters only
    ifconfig "$ifc" up 2>/dev/null
    busybox udhcpc -i "$ifc" -b -t 5 -T 3 -A 10 -x hostname:"$(hostname)" \\
        -s /usr/share/udhcpc/default.script >/dev/null 2>&1 &
done
exit 0
"""

_UDHCPC_SCRIPT = """\
#!/bin/sh
# udhcpc lease handler: address, default route and DNS for $interface.
# shellcheck disable=SC2154  # udhcpc sets $interface, $ip, $router, $dns...
export PATH=/usr/bin:/bin:/sbin:/usr/sbin
case "$1" in
deconfig)
    ifconfig "$interface" 0.0.0.0 up
    ;;
bound|renew)
    ifconfig "$interface" "$ip" ${subnet:+netmask "$subnet"} \\
        ${broadcast:+broadcast "$broadcast"} up
    if [ -n "$router" ]; then
        while route del default dev "$interface" 2>/dev/null; do :; done
        for r in $router; do
            route add default gw "$r" dev "$interface" && break
        done
    fi
    if [ -n "$dns" ]; then
        : > /etc/resolv.conf.new
        [ -n "$domain" ] && echo "search $domain" >> /etc/resolv.conf.new
        for d in $dns; do echo "nameserver $d" >> /etc/resolv.conf.new; done
        mv /etc/resolv.conf.new /etc/resolv.conf
    fi
    ;;
esac
exit 0
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
              "etc", "etc/init.d", "dev", "proc", "sys", "tmp",
              "run", "var", "var/log", "var/run", "var/lib/omnios",
              "var/lib/omnios/update", "usr/share/udhcpc", "mnt",
              "root", "home/omnios"):
        os.makedirs(os.path.join(OUT, d), exist_ok=True)

    # ---- OmniOS core binaries (from-scratch kernel core + GUI) -------------
    # ominit            the PID-1 init (replaces BusyBox init)
    # omnios-desktop    the desktop shell (window manager + display server)
    # omnios-*          bundled desktop apps
    for b in ("ominit", "omnios-desktop", "omnios-term", "omnios-files",
              "omnios-calc", "omnios-edit", "omnios-sysinfo", "omnios-about",
              "omnios-store", "omnios-clock", "omnios-snake",
              "omnios-settings", "omnios-taskmgr", "omnios-calendar",
              "omnios-mines", "omnios-2048", "omnios-tictactoe",
              "omnios-update"):
        s = os.path.join(OS_BIN, b)
        if os.path.exists(s):
            dst = os.path.join(OUT, "usr", "bin", b)
            shutil.copy(s, dst)
            os.chmod(dst, 0o755)
    # ominit is PID 1: the kernel runs /init, and /sbin/init is where
    # programs look for it. Both are links, so there is one copy.
    if os.path.exists(os.path.join(OS_BIN, "ominit")):
        os.symlink("/usr/bin/ominit", os.path.join(OUT, "init"))
        os.symlink("/usr/bin/ominit", os.path.join(OUT, "sbin", "init"))
    else:
        print("make-rootfs: WARN: ominit not built: the OS cannot start.",
              file=sys.stderr)

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

    # ---- /etc ----------------------------------------------------------------
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
    w("etc/init.d/network", _NETWORK, 0o755)
    w("usr/share/udhcpc/default.script", _UDHCPC_SCRIPT, 0o755)
    # OmniOS Update's release feed: this repository's latest GitHub release
    # (a fork's CI builds point at the fork)
    repo = os.environ.get("GITHUB_REPOSITORY") or "OmniNodeCo/OmniOS"
    w("etc/omnios-update.conf",
      "# OmniOS Update: the feed published with each release\n"
      "url=https://github.com/%s/releases/latest/download/omnios-update.txt\n"
      % repo)
    w("etc/fstab",
      "proc  /proc proc  defaults 0 0\n"
      "sysfs /sys sysfs defaults 0 0\n"
      "tmpfs /tmp tmpfs defaults 0 0\n"
      "devpts /dev/pts devpts defaults 0 0\n")
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
