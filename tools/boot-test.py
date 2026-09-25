#!/usr/bin/env python3
"""
OmniOS boot test: boots OmniOS in QEMU with hardware close to the VMware
virtual machine the release ships (.vmx), and measures how long it takes
until the desktop is on the screen.

    tools/boot-test.py --iso build/out/OmniOS-<v>-amd64.iso \\
                       --kernel build/out/omnios-bzImage-<v> [options]

The machine: UEFI firmware (OVMF), the ISO on the secondary IDE channel
(VMware's ide1:0), an e1000 network adapter (NAT), the PS/2 keyboard and
mouse with the VMware mouse backdoor, 1 GB of memory, 2 CPUs. KVM is used
when /dev/kvm is usable (GitHub's Linux runners have it), so the timings
are close to a real hypervisor's; without KVM the boot still works, only
slower.

Each --variant is one way to boot, and every variant boots --repeat times:
    iso-efi              the ISO on UEFI firmware (the .vmx's default)
    iso-bios             the ISO on BIOS firmware (ISOLINUX)
    kernel-efi[:ARGS]    the kernel started by the firmware directly, with
                         ARGS added to its built-in command line, e.g.
                         kernel-efi:quiet or kernel-efi:loglevel=7
    update               the ISO on UEFI firmware, then OmniOS Update: the
                         test signs in, waits --update-wait seconds for
                         OmniOS Update to find, download and prepare the
                         latest release (from GitHub, through QEMU's NAT),
                         restarts from the Win+X menu and checks that the
                         newer version comes up (boot an older release)
with options after the mode, joined with "+": sata (the CD-ROM on SATA
instead of IDE), vmxnet3 or e1000e (the network adapter; e1000 otherwise).
iso-efi+sata+vmxnet3 is the machine the current .vmx describes.

How "the desktop is on the screen" is told: the screen is captured every
0.2 s, and the desktop (lock screen, wallpaper) is the first picture with
almost no pure-black pixels. The firmware logo and the kernel's text
console are mostly pure black.

With --ref-iso (and --ref-kernel), every ISO boot is repeated with that
build too, alternating, so both are timed on the same machine: runners
differ too much from one run to the next to compare separate runs.

The first boot of the first variant is also driven like a user would:
a key on the lock screen, Enter to sign in (no password), the Windows key
for Start, with a screenshot of each. If the kernel command line has
omnios.serialshell (kernel-efi variants), OmniOS opens a shell on the
serial port and the test reads the kernel log (dmesg) through it.

Output, in --out (default build/boot-test): result.json, summary.md, and
per boot a serial log and screenshots (PNG). In GitHub Actions the summary
also goes to the job summary and to annotations.
"""
import argparse
import base64
import json
import os
import re
import shutil
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

OVMF_PAIRS = [
    ("/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd"),
    ("/usr/share/OVMF/OVMF_CODE.fd", "/usr/share/OVMF/OVMF_VARS.fd"),
    ("/usr/share/edk2/ovmf/OVMF_CODE.fd", "/usr/share/edk2/ovmf/OVMF_VARS.fd"),
    ("/usr/share/edk2/x64/OVMF_CODE.4m.fd", "/usr/share/edk2/x64/OVMF_VARS.4m.fd"),
]

POLL = 0.1              # seconds between screen captures
DESKTOP_BLACK = 0.20    # at most this share of pure-black pixels: desktop


# ---- QEMU monitor (QMP) ------------------------------------------------------

class QMP:
    def __init__(self, path, timeout=15.0):
        self.sock = connect_unix(path, timeout)
        self.rd = self.sock.makefile("rb")
        self.events = []
        self._reply()                                   # greeting
        self.cmd("qmp_capabilities")

    def _reply(self):
        while True:
            line = self.rd.readline()
            if not line:
                raise EOFError("QEMU closed the monitor")
            msg = json.loads(line)
            if "event" in msg:
                self.events.append(msg)
                continue
            return msg

    def cmd(self, name, **arguments):
        req = {"execute": name}
        if arguments:
            req["arguments"] = arguments
        self.sock.sendall(json.dumps(req).encode() + b"\n")
        rep = self._reply()
        if "error" in rep:
            raise RuntimeError("QMP %s: %s" % (name, rep["error"].get("desc")))
        return rep.get("return")

    def key(self, *qcodes, hold_ms=80):
        self.cmd("send-key", keys=[{"type": "qcode", "data": q} for q in qcodes],
                 **{"hold-time": hold_ms})

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def connect_unix(path, timeout):
    deadline = time.time() + timeout
    while True:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.connect(path)
            return s
        except OSError:
            s.close()
            if time.time() > deadline:
                raise
            time.sleep(0.02)


# ---- serial port ---------------------------------------------------------------

class Serial(threading.Thread):
    """Everything the guest writes to its first serial port, with the time
    (seconds since QEMU started) at which each line arrived."""

    def __init__(self, path, t0):
        super().__init__(daemon=True)
        self.sock = connect_unix(path, 15.0)
        self.t0 = t0
        self.data = bytearray()
        self.lines = []                 # (seconds, text)
        self._part = b""
        self.lock = threading.Lock()

    def run(self):
        while True:
            try:
                chunk = self.sock.recv(65536)
            except OSError:
                break
            if not chunk:
                break
            now = time.time() - self.t0
            with self.lock:
                self.data += chunk
                self._part += chunk
                while b"\n" in self._part:
                    line, self._part = self._part.split(b"\n", 1)
                    self.lines.append((now, line.decode("utf-8", "replace").rstrip("\r")))

    def send(self, text):
        self.sock.sendall(text.encode())

    def size(self):
        with self.lock:
            return len(self.data)

    def wait_for(self, pattern, start, timeout):
        """bytes from `start` up to the end of the first match, or None"""
        rx = re.compile(pattern.encode() if isinstance(pattern, str) else pattern)
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                m = rx.search(self.data, start)
                if m:
                    return bytes(self.data[start:m.end()])
            time.sleep(0.05)
        return None

    def first(self, pattern):
        rx = re.compile(pattern)
        with self.lock:
            for t, line in self.lines:
                if rx.search(line):
                    return t
        return None


# ---- screen --------------------------------------------------------------------

def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m:
        raise ValueError("not a binary PPM: %s" % path)
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, data[m.end():m.end() + w * h * 3]


def screen_stats(w, h, px, step=9):
    """share of pure-black pixels and mean brightness (0-255), sampled"""
    n = black = total = 0
    for y in range(0, h, step):
        row = y * w * 3
        for x in range(0, w * 3, step * 3):
            s = px[row + x] + px[row + x + 1] + px[row + x + 2]
            n += 1
            total += s
            if s < 8:
                black += 1
    if not n:
        return 1.0, 0.0
    return black / n, total / (3.0 * n)


def write_png(path, w, h, rgb):
    stride = w * 3
    raw = b"".join(b"\x00" + rgb[y * stride:(y + 1) * stride] for y in range(h))

    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body +
                struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def thumbnail(png_path, jpg_path, width=512):
    """a small JPEG of a screenshot (needs Pillow), or None"""
    try:
        from PIL import Image
    except ImportError:
        return None
    im = Image.open(png_path).convert("RGB")
    if im.width > width:
        im = im.resize((width, im.height * width // im.width), Image.LANCZOS)
    im.save(jpg_path, "JPEG", quality=62, optimize=True)
    return jpg_path


# ---- kernel log ------------------------------------------------------------------

KLINE = re.compile(r"^\s*(?:<\d+>)?\[\s*(\d+\.\d+)\]\s?(.*)$")

MILESTONES = [
    ("kernel_start", r"Linux version"),
    ("initramfs", r"Unpacking initramfs|Trying to unpack rootfs"),
    ("init_start", r"Run /init as init process|Run /sbin/init"),
    ("ominit", r"OmniOS init \(PID 1\): starting"),
    ("fb_ready", r"desktop: framebuffer ready"),
    ("desktop_input", r"desktop: input:"),
    ("desktop_ready", r"desktop: ready"),
]


def kernel_timeline(lines):
    """milestones (seconds since the kernel started) and the longest quiet
    stretches between two kernel log lines"""
    stamped = []
    for text in lines:
        m = KLINE.match(text)
        if m:
            stamped.append((float(m.group(1)), m.group(2)))
    marks = {}
    for key, rx in MILESTONES:
        r = re.compile(rx)
        for t, msg in stamped:
            if r.search(msg):
                marks[key] = t
                break
    gaps = []
    for (t1, m1), (t2, m2) in zip(stamped, stamped[1:]):
        if t2 - t1 >= 0.02:
            gaps.append((round(t2 - t1, 3), t1, m1[:110], m2[:110]))
    gaps.sort(reverse=True)
    calls = []
    rx = re.compile(r"initcall (\S+) returned -?\d+ after (\d+) usecs")
    for t, msg in stamped:
        m = rx.search(msg)
        if m:
            calls.append((int(m.group(2)), m.group(1)))
    calls.sort(reverse=True)
    last = stamped[-1][0] if stamped else None
    rx = re.compile(r"drm|fbcon|Console:|e1000|vmwgfx|simple-framebuffer|"
                    r"Freeing unused|Run /init|OmniOS init|desktop: (ready|framebuffer)")
    excerpt = ["[%9.6f] %s" % (t, msg[:120]) for t, msg in stamped
               if rx.search(msg) and not msg.startswith(("calling ", "initcall "))]
    return {"milestones": marks, "gaps": gaps[:12], "initcalls": calls[:25],
            "last": last, "lines": len(stamped), "excerpt": excerpt[:30]}


# ---- one boot --------------------------------------------------------------------

class Boot:
    def __init__(self, args, variant, index, accel, outdir, build="this",
                 iso=None, kernel=None):
        self.args = args
        self.variant = variant
        self.index = index
        self.accel = accel
        self.iso = iso or args.iso
        self.kernel = kernel or args.kernel
        self.name = "%s%s-%d" % ("" if build == "this" else "ref-",
                                 re.sub(r"[^A-Za-z0-9]+", "-", variant).strip("-"), index)
        self.dir = os.path.join(outdir, self.name)
        os.makedirs(self.dir, exist_ok=True)
        self.tmp = tempfile.mkdtemp(prefix="omni-qemu-")
        self.result = {"variant": variant, "build": build, "run": index, "accel": accel,
                       "screens": [], "desktop": None}

    # the QEMU command line for this variant
    def command(self):
        a = self.args
        mode, _, extra = self.variant.partition(":")
        mode, *opts = mode.split("+")
        nic = "vmxnet3" if "vmxnet3" in opts else "e1000e" if "e1000e" in opts else "e1000"
        qmp = os.path.join(self.tmp, "qmp.sock")
        ser = os.path.join(self.tmp, "serial.sock")
        cmd = [a.qemu,
               "-machine", "pc,accel=%s" % self.accel,
               "-cpu", "host" if self.accel == "kvm" else "max",
               "-m", "1024", "-smp", "2",
               "-display", "none", "-vga", "std",
               "-no-reboot",
               "-qmp", "unix:%s,server=on,wait=off" % qmp,
               "-chardev", "socket,id=ser0,path=%s,server=on,wait=off,logfile=%s"
               % (ser, os.path.join(self.dir, "serial.log")),
               "-serial", "chardev:ser0",
               "-netdev", "user,id=n0",
               "-device", "%s,netdev=n0,romfile=" % nic]
        if mode == "update":
            mode = "iso-efi"
        if mode in ("iso-efi", "kernel-efi"):
            code, vars_ = find_ovmf()
            vcopy = os.path.join(self.tmp, "OVMF_VARS.fd")
            shutil.copy(vars_, vcopy)
            cmd += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=" + code,
                    "-drive", "if=pflash,format=raw,unit=1,file=" + vcopy]
        if mode in ("iso-efi", "iso-bios"):
            cmd += ["-drive", "file=%s,media=cdrom,if=none,id=cd0,readonly=on" % self.iso]
            if "sata" in opts:
                cmd += ["-device", "ahci,id=sata0",
                        "-device", "ide-cd,drive=cd0,bus=sata0.0,bootindex=0"]
            else:
                cmd += ["-device", "ide-cd,drive=cd0,bus=ide.1,unit=0,bootindex=0"]
        elif mode == "kernel-efi":
            if not self.kernel:
                raise SystemExit("boot-test: %s needs --kernel" % self.variant)
            cmd += ["-kernel", self.kernel, "-append", extra]
        else:
            raise SystemExit("boot-test: unknown variant %r" % self.variant)
        return cmd, qmp, ser

    def shot(self, qmp, label):
        ppm = os.path.join(self.tmp, "shot.ppm")
        qmp.cmd("screendump", filename=ppm)
        w, h, px = read_ppm(ppm)
        png = os.path.join(self.dir, "%s.png" % label)
        write_png(png, w, h, px)
        black, mean = screen_stats(w, h, px)
        self.result["screens"].append({"label": label, "file": os.path.relpath(png, self.args.out),
                                       "size": [w, h], "black": round(black, 3),
                                       "mean": round(mean, 1)})
        return png

    def run(self, interact):
        cmd, qmp_path, ser_path = self.command()
        self.result["command"] = " ".join(cmd)
        t0 = time.time()
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        qmp = serial = None
        timeout = self.args.timeout
        try:
            qmp = QMP(qmp_path)
            serial = Serial(ser_path, t0)
            serial.start()
            ppm = os.path.join(self.tmp, "poll.ppm")
            seen_picture = None
            last = None
            while time.time() - t0 < timeout:
                if proc.poll() is not None:
                    self.result["error"] = "QEMU exited (%s)" % proc.returncode
                    break
                try:
                    qmp.cmd("screendump", filename=ppm)
                    w, h, px = read_ppm(ppm)
                except (RuntimeError, ValueError, OSError):
                    time.sleep(POLL)
                    continue
                now = time.time() - t0
                black, mean = screen_stats(w, h, px)
                last = (w, h, black, mean)
                if seen_picture is None and black < 0.995:
                    seen_picture = now
                if black <= DESKTOP_BLACK and mean > 12:
                    self.result["desktop"] = round(now, 2)
                    self.result["resolution"] = [w, h]
                    break
                time.sleep(POLL)
            self.result["first_picture"] = seen_picture and round(seen_picture, 2)
            self.result["first_serial"] = (round(serial.lines[0][0], 2)
                                           if serial.lines else None)
            if self.result["desktop"] is None:
                self.result.setdefault("error", "no desktop after %d s" % timeout)
                if last:
                    self.result["last_screen"] = {"size": [last[0], last[1]],
                                                  "black": round(last[2], 3),
                                                  "mean": round(last[3], 1)}
                self.shot(qmp, "timeout")
            else:
                time.sleep(1.5)                  # let the lock screen settle
                self.shot(qmp, "1-lock")
                if self.variant.startswith("update"):
                    self.update_flow(qmp, serial, proc, t0)
                elif interact:
                    self.interact(qmp)
            if serial is not None:
                self.serial_shell(serial)
        finally:
            if qmp is not None:
                try:
                    qmp.cmd("quit")
                except (RuntimeError, EOFError, OSError):
                    pass
                qmp.close()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            out = proc.stdout.read().decode("utf-8", "replace") if proc.stdout else ""
            if out.strip():
                self.result["qemu_output"] = out.strip()[-2000:]
            shutil.rmtree(self.tmp, ignore_errors=True)
        if serial is not None:
            texts = [t for _, t in serial.lines]
            self.result["serial_lines"] = len(texts)
            self.result["serial_tail"] = texts[-25:]
            marks = {}
            for key, rx in MILESTONES:
                t = serial.first(rx)
                if t is not None:
                    marks[key] = round(t, 2)
            self.result["serial_arrival"] = marks
            # when the kernel started, in seconds since QEMU started: each
            # time-stamped line arrives a little after its kernel time
            # stamp, so the smallest difference is the start
            offs = [t - float(m.group(1)) for t, m in
                    ((t, KLINE.match(text)) for t, text in serial.lines) if m]
            if offs:
                self.result["kernel_offset"] = round(min(offs), 3)
            if "kernel" not in self.result:
                self.result["kernel"] = kernel_timeline(texts)
        return self.result

    # a user at the keyboard: lock screen -> sign in -> desktop -> Start
    def interact(self, qmp):
        qmp.key("spc")
        time.sleep(1.2)
        self.shot(qmp, "2-signin")
        qmp.key("ret")
        time.sleep(2.0)
        self.shot(qmp, "3-desktop")
        qmp.key("meta_l")
        time.sleep(1.2)
        self.shot(qmp, "4-start")
        qmp.key("esc")
        time.sleep(0.6)
        qmp.key("meta_l", "a")                   # Quick Settings
        time.sleep(1.2)
        self.shot(qmp, "5-quicksettings")
        qmp.key("esc")
        time.sleep(0.4)

    # OmniOS Update, end to end: the running version finds the latest
    # release, downloads and prepares it, and a restart starts it (kexec)
    def update_flow(self, qmp, serial, proc, t0):
        up = {"result": "?"}
        self.result["update"] = up
        motd = re.compile(rb"OmniOS (\d[\w.]*) \S+ a lightweight OS")
        m = motd.search(bytes(serial.data))
        up["old"] = m.group(1).decode() if m else None
        qmp.key("spc")                           # lock screen -> sign in
        time.sleep(1.2)
        qmp.key("ret")                           # no password
        time.sleep(2.0)
        self.shot(qmp, "u1-desktop")
        # it checks 60 s after the network is up; downloading the kernel
        # image and loading it for the restart take a few seconds more
        time.sleep(self.args.update_wait)
        self.shot(qmp, "u2-waited")
        qmp.key("meta_l", "x")                   # quick links
        time.sleep(1.0)
        qmp.key("up")                            # ... Restart, Shut down:
        time.sleep(0.25)                         # two up from the start
        qmp.key("up")
        time.sleep(0.5)
        self.shot(qmp, "u3-restart")
        mark = serial.size()
        t_restart = time.time()
        qmp.key("ret")
        deadline = time.time() + 90
        new = None
        while time.time() < deadline:
            if proc.poll() is not None:          # -no-reboot: a firmware restart
                up["result"] = ("the machine restarted through the firmware: "
                                "no update was installed")
                return
            with serial.lock:
                m = motd.search(bytes(serial.data), mark)
            if m:
                new = m.group(1).decode()
                break
            time.sleep(0.2)
        if new is None:
            up["result"] = "no OmniOS came back within 90 s of the restart"
            return
        up["new"] = new
        up["restart_to_kernel"] = round(time.time() - t_restart, 2)
        ppm = os.path.join(self.tmp, "upd.ppm")
        while time.time() < deadline:
            try:
                qmp.cmd("screendump", filename=ppm)
                w, h, px = read_ppm(ppm)
                black, mean = screen_stats(w, h, px)
                if black <= DESKTOP_BLACK and mean > 12:
                    up["restart_to_desktop"] = round(time.time() - t_restart, 2)
                    break
            except (RuntimeError, ValueError, OSError):
                pass
            time.sleep(POLL)
        time.sleep(1.5)
        self.shot(qmp, "u4-after")
        if new == up["old"]:
            up["result"] = "the same version came back (%s)" % new
        elif "restart_to_desktop" not in up:
            up["result"] = "%s started, but no desktop appeared" % new
        else:
            up["result"] = "ok"

    # OmniOS started with omnios.serialshell: read the kernel log through it
    def serial_shell(self, serial):
        if "omnios.serialshell" not in self.variant:
            return
        start = serial.size()
        serial.send("\n")
        if serial.wait_for(r"omnios-serial# ", start, 8.0) is None:
            self.result["shell"] = "no prompt on the serial port"
            return
        start = serial.size()
        # the markers are split in the command so its echo does not match
        serial.send("dmesg -n 1; cat /proc/uptime; echo __OMNI_\"DMESG\"__; dmesg; "
                    "echo __OMNI_\"END\"__\n")
        out = serial.wait_for(r"__OMNI_END__", start, 60.0)
        if out is None:
            self.result["shell"] = "dmesg did not finish"
            return
        text = out.decode("utf-8", "replace").replace("\r", "")
        self.shell_checks(serial)
        up = re.search(r"^(\d+\.\d+) \d+\.\d+$", text, re.M)
        if up:
            self.result["uptime_at_dump"] = float(up.group(1))
        body = text.split("__OMNI_DMESG__", 1)[-1].rsplit("__OMNI_END__", 1)[0]
        lines = [ln for ln in body.split("\n") if ln.strip()]
        with open(os.path.join(self.dir, "dmesg.txt"), "w") as f:
            f.write("\n".join(lines) + "\n")
        self.result["shell"] = "ok"
        self.result["kernel"] = kernel_timeline(lines)

    def shell_checks(self, serial):
        """the network (DHCP lease) and PID 1's reaping, through the shell"""
        start = serial.size()
        serial.send("i=0; while [ $i -lt 15 ] && ! ifconfig eth0 2>/dev/null | grep -q 'inet addr'; "
                    "do sleep 1; i=$((i+1)); done; echo __OMNI_\"NET\"__; ifconfig eth0; route -n; "
                    "echo zombies=$(grep -l ') Z ' /proc/[0-9]*/stat 2>/dev/null | wc -l); "
                    "echo __OMNI_\"END2\"__\n")
        out = serial.wait_for(r"__OMNI_END2__", start, 30.0)
        if out is None:
            self.result["network"] = "no answer"
            return
        text = out.decode("utf-8", "replace").replace("\r", "").split("__OMNI_NET__", 1)[-1]
        ip = re.search(r"inet addr:(\S+)", text)
        gw = re.search(r"^0\.0\.0\.0\s+(\S+)", text, re.M)
        self.result["network"] = ("eth0 %s, gateway %s" % (ip.group(1), gw.group(1) if gw else "none")
                                  if ip else "no address")
        z = re.search(r"zombies=(\d+)", text)
        if z:
            self.result["zombies"] = int(z.group(1))
        # OmniOS Update's own check (and how long it took), then its
        # download by hand: every redirect hop and wget's own words, so a
        # failure says why and where. wget gets a timeout (its default is
        # 15 minutes), and whatever arrived is kept if the end never comes.
        # wget's words go to a file, not a pipe: its TLS helper process can
        # outlive it, and would keep a pipe (and the shell) waiting.
        start = serial.size()
        serial.send("echo __OMNI_\"UPD\"__; cat /etc/resolv.conf; t=$(date +%s); "
                    "omnios-update check; echo check-exit=$? after $(( $(date +%s) - t )) s; "
                    "omnios-update status; "
                    "u=$(sed -n 's/^url=//p' /etc/omnios-update.conf); echo url=$u; "
                    "busybox wget -S -T 20 -O /tmp/feed.txt \"$u\" 2>/tmp/wget.err; "
                    "echo wget-exit=$?; grep -E '^Connecting|HTTP/1|wget' /tmp/wget.err | cut -c1-150; "
                    "head -c 400 /tmp/feed.txt; echo; "
                    "busybox wget -T 20 -O /dev/null https://api.github.com/ 2>/tmp/wget2.err; "
                    "echo api.github.com-exit=$?; grep wget /tmp/wget2.err | cut -c1-150; "
                    "echo __OMNI_\"END3\"__\n")
        out = serial.wait_for(r"__OMNI_END3__", start, 150.0)
        ended = out is not None
        if not ended:
            with serial.lock:
                out = bytes(serial.data[start:])
        text = out.decode("utf-8", "replace").replace("\r", "")
        text = text.split("__OMNI_UPD__", 1)[-1].rsplit("__OMNI_END3__", 1)[0]
        lines = [ln.strip() for ln in text.split("\n")
                 if ln.strip() and "__OMNI_" not in ln and not ln.startswith("omnios-serial#")]
        if not ended:
            lines.append("(unfinished after 150 s)")
        self.result["update_check"] = "\n".join(lines[-30:])


def find_ovmf():
    for code, vars_ in OVMF_PAIRS:
        if os.path.isfile(code) and os.path.isfile(vars_):
            return code, vars_
    raise SystemExit("boot-test: OVMF (UEFI firmware for QEMU) not found; "
                     "install the ovmf package")


def pick_accel(want):
    if want != "auto":
        return want
    try:
        fd = os.open("/dev/kvm", os.O_RDWR)
        os.close(fd)
        return "kvm"
    except OSError:
        return "tcg"


# ---- report ----------------------------------------------------------------------

def fmt(v, unit=" s"):
    return "–" if v is None else ("%.2f%s" % (v, unit))


def summarize(results, accel, sizes):
    by = {}
    for r in results:
        by.setdefault((r["variant"], r.get("build", "this")), []).append(r)
    rows, notes = [], []

    def med_of(vals):
        vals = [v for v in vals if v is not None]
        return statistics.median(vals) if vals else None

    for (variant, build), runs in by.items():
        label = variant if build == "this" else "%s (%s)" % (variant, build)
        ok = [r["desktop"] for r in runs if r.get("desktop") is not None]
        med = statistics.median(ok) if ok else None
        # the phases: firmware + loading the kernel, the kernel until it
        # starts /init (ominit's first message if the kernel was quiet),
        # userspace until the desktop's first frame
        k_start = med_of([r.get("kernel_offset") for r in runs])
        user = med_of([((r.get("kernel") or {}).get("milestones", {}).get("init_start") or
                        (r.get("kernel") or {}).get("milestones", {}).get("ominit"))
                       for r in runs])
        ready = med_of([(r.get("kernel") or {}).get("milestones", {}).get("desktop_ready")
                        for r in runs])
        rows.append("| `%s` | %d/%d | %s | %s | %s | %s | %s |" % (
            label, len(ok), len(runs), fmt(med),
            ", ".join(fmt(x, "") for x in ok) or "–",
            fmt(k_start), fmt(user),
            fmt(ready - user) if ready is not None and user is not None else "–"))
        msg = "desktop on screen after %s (%s; runs: %s); kernel started at %s, " \
              "/init at %s, desktop ready %s later" % (
                  fmt(med), accel, ", ".join(fmt(x, "") for x in ok) or "none",
                  fmt(k_start), fmt(user),
                  fmt(ready - user) if ready is not None and user is not None else "–")
        if not ok:
            msg = "no desktop: %s" % runs[0].get("error", "?")
        notes.append((label, msg))
    md = ["### Boot test (QEMU, %s)" % accel, ""]
    if sizes:
        md += [", ".join("%s: %.1f MB" % (k, v / 1e6) for k, v in sizes), ""]
    md += ["| boot | reached desktop | desktop visible after (median) | each run (s) "
           "| power-on to kernel | kernel to /init | /init to desktop ready |",
           "|---|---|---|---|---|---|---|"] + rows + [""]
    for r in results:
        k = r.get("kernel") or {}
        if r["run"] != 1 or not k.get("lines") or r.get("build", "this") != "this":
            continue
        md.append("<details><summary><code>%s</code>: kernel timeline</summary>" % r["variant"])
        md.append("")
        md.append("milestones (s since the kernel started): `%s`" %
                  json.dumps(k.get("milestones", {})))
        if k.get("gaps"):
            md.append("")
            md.append("longest pauses between two kernel log lines:")
            md.append("")
            for g in k["gaps"][:8]:
                md.append("- %.3f s after `[%.3f] %s`" % (g[0], g[1], g[2]))
        if k.get("initcalls"):
            md.append("")
            md.append("slowest initcalls:")
            md.append("")
            for us, name in k["initcalls"][:15]:
                md.append("- %.1f ms `%s`" % (us / 1000.0, name))
        if k.get("excerpt"):
            md.append("")
            md.append("```")
            md += k["excerpt"]
            md.append("```")
        md.append("</details>")
        md.append("")
    return "\n".join(md) + "\n", notes


def gh_escape(s):
    return s.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--iso")
    ap.add_argument("--kernel")
    ap.add_argument("--ref-iso", help="another build's ISO, booted alternately")
    ap.add_argument("--ref-kernel")
    ap.add_argument("--ref-name", default="reference")
    ap.add_argument("--variant", action="append", default=[])
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--timeout", type=int, default=0,
                    help="seconds to wait for the desktop (default 90, 600 without KVM)")
    ap.add_argument("--accel", default="auto", choices=["auto", "kvm", "tcg"])
    ap.add_argument("--qemu", default="qemu-system-x86_64")
    ap.add_argument("--out", default="build/boot-test")
    ap.add_argument("--no-interact", action="store_true")
    ap.add_argument("--update-wait", type=int, default=100,
                    help="seconds the update variant waits after signing in")
    ap.add_argument("--print-thumbs", default="",
                    help="print small JPEGs of these screenshots (comma-separated "
                         "labels, e.g. 3-desktop,4-start) as base64, for logs")
    args = ap.parse_args()

    variants = args.variant or ["iso-efi"]
    if any(v.startswith("iso") for v in variants) and not args.iso:
        ap.error("--iso is needed for the iso-* variants")
    accel = pick_accel(args.accel)
    if not args.timeout:
        args.timeout = 90 if accel == "kvm" else 600
    if shutil.which(args.qemu) is None:
        raise SystemExit("boot-test: %s not found" % args.qemu)
    os.makedirs(args.out, exist_ok=True)

    results = []
    for rep in range(1, args.repeat + 1):
        for vi, variant in enumerate(variants):
            builds = [("this", args.iso, args.kernel)]
            # the reference build boots the same way, right after (UEFI
            # boots of the ISO, and plain kernel-efi: its kernel may lack
            # newer options)
            if args.ref_iso and (variant.startswith("iso-efi") or variant == "kernel-efi:"):
                builds.append((args.ref_name, args.ref_iso, args.ref_kernel))
            for build, iso, kernel in builds:
                boot = Boot(args, variant, rep, accel, args.out, build, iso, kernel)
                print("boot-test: %s [%s] (run %d, %s)..." % (variant, build, rep, accel),
                      flush=True)
                r = boot.run(interact=(rep == 1 and vi == 0 and build == "this"
                                       and not args.no_interact))
                print("boot-test:   desktop at %s, kernel started at %s%s" % (
                    fmt(r.get("desktop")), fmt(r.get("kernel_offset")),
                    ("  [%s]" % r["error"]) if r.get("error") else ""), flush=True)
                results.append(r)

    with open(os.path.join(args.out, "result.json"), "w") as f:
        json.dump({"accel": accel, "results": results}, f, indent=1)
    sizes = [(name, os.path.getsize(path)) for name, path in
             (("kernel image", args.kernel), ("ISO", args.iso)) if path and os.path.exists(path)]
    md, notes = summarize(results, accel, sizes)
    with open(os.path.join(args.out, "summary.md"), "w") as f:
        f.write(md)
    print(md)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
            f.write(md)
    if os.environ.get("GITHUB_ACTIONS") == "true":
        if sizes:
            print("::notice title=Sizes::%s" % ", ".join("%s %d bytes" % s_ for s_ in sizes))
        for variant, msg in notes:
            print("::notice title=Boot %s::%s" % (variant, gh_escape(msg)))
        for r in results:
            if r.get("update"):
                u = r["update"]
                print("::%s title=OmniOS Update run %d::%s" % (
                    "notice" if u.get("result") == "ok" else "warning", r["run"],
                    gh_escape("%s: %s -> %s, restart to kernel %s, to desktop %s" % (
                        u.get("result"), u.get("old"), u.get("new"),
                        fmt(u.get("restart_to_kernel")), fmt(u.get("restart_to_desktop"))))))
        # the kernel timeline of each variant's first boot, for the record
        for r in results:
            k = r.get("kernel") or {}
            if r["run"] == 1 and k.get("lines") and r.get("build", "this") == "this":
                lines = ["milestones %s" % json.dumps(k.get("milestones", {}))]
                if r.get("shell"):
                    lines.append("serial shell: %s" % r["shell"])
                if r.get("network"):
                    lines.append("network: %s; zombie processes: %s" % (
                        r["network"], r.get("zombies", "?")))
                if r.get("update_check"):
                    lines.append("update check:")
                    lines += ["  " + ln for ln in r["update_check"].split("\n")]
                lines += ["pause %.3f s after [%.3f] %s -> %s" % (g[0], g[1], g[2], g[3])
                          for g in k.get("gaps", [])[:6]]
                lines += ["initcall %.1f ms %s" % (us / 1000.0, n)
                          for us, n in k.get("initcalls", [])[:12]]
                if r.get("uptime_at_dump") is not None:
                    lines.append("uptime at dump %.2f" % r["uptime_at_dump"])
                lines += k.get("excerpt", [])
                print("::notice title=Kernel %s::%s" % (r["variant"], gh_escape("\n".join(lines))))
            if r.get("error"):
                print("::warning title=Boot %s [%s] run %d::%s%%0A%s" % (
                    r["variant"], r.get("build", "this"), r["run"], gh_escape(r["error"]),
                    gh_escape("\n".join(r.get("serial_tail", [])[-12:]))))

    if args.print_thumbs:
        wanted = set(args.print_thumbs.split(","))
        for r in results:
            for s in r.get("screens", []):
                if s["label"] not in wanted and "all" not in wanted:
                    continue
                png = os.path.join(args.out, s["file"])
                jpg = thumbnail(png, png[:-4] + ".jpg")
                if not jpg:
                    continue
                with open(jpg, "rb") as f:
                    b64 = base64.b64encode(f.read()).decode()
                print("===THUMB %s [%s]/%s %d===" % (r["variant"], r.get("build", "this"),
                                                    s["label"], len(b64)))
                for i in range(0, len(b64), 120):
                    print(b64[i:i + 120])
                print("===END===")

    first = [r for r in results if r["variant"] == variants[0] and r.get("build", "this") == "this"]
    ok = first and all(r.get("desktop") is not None for r in first)
    ok = ok and all(r["update"].get("result") == "ok" for r in results if r.get("update"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
