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

How "the desktop is on the screen" is told: the screen is captured every
0.2 s, and the desktop (lock screen, wallpaper) is the first picture with
almost no pure-black pixels. The firmware logo and the kernel's text
console are mostly pure black.

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

POLL = 0.2              # seconds between screen captures
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


def screen_stats(w, h, px, step=7):
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
    return {"milestones": marks, "gaps": gaps[:12], "initcalls": calls[:25],
            "last": last, "lines": len(stamped)}


# ---- one boot --------------------------------------------------------------------

class Boot:
    def __init__(self, args, variant, index, accel, outdir):
        self.args = args
        self.variant = variant
        self.index = index
        self.accel = accel
        self.name = "%s-%d" % (re.sub(r"[^A-Za-z0-9]+", "-", variant).strip("-"), index)
        self.dir = os.path.join(outdir, self.name)
        os.makedirs(self.dir, exist_ok=True)
        self.tmp = tempfile.mkdtemp(prefix="omni-qemu-")
        self.result = {"variant": variant, "run": index, "accel": accel,
                       "screens": [], "desktop": None}

    # the QEMU command line for this variant
    def command(self):
        a = self.args
        mode, _, extra = self.variant.partition(":")
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
               "-device", "e1000,netdev=n0,romfile="]
        if mode in ("iso-efi", "kernel-efi"):
            code, vars_ = find_ovmf()
            vcopy = os.path.join(self.tmp, "OVMF_VARS.fd")
            shutil.copy(vars_, vcopy)
            cmd += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=" + code,
                    "-drive", "if=pflash,format=raw,unit=1,file=" + vcopy]
        if mode in ("iso-efi", "iso-bios"):
            cmd += ["-drive", "file=%s,media=cdrom,if=none,id=cd0,readonly=on" % a.iso,
                    "-device", "ide-cd,drive=cd0,bus=ide.1,unit=0,bootindex=0"]
        elif mode == "kernel-efi":
            if not a.kernel:
                raise SystemExit("boot-test: %s needs --kernel" % self.variant)
            cmd += ["-kernel", a.kernel, "-append", extra]
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
                if interact:
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
        up = re.search(r"^(\d+\.\d+) \d+\.\d+$", text, re.M)
        if up:
            self.result["uptime_at_dump"] = float(up.group(1))
        body = text.split("__OMNI_DMESG__", 1)[-1].rsplit("__OMNI_END__", 1)[0]
        lines = [ln for ln in body.split("\n") if ln.strip()]
        with open(os.path.join(self.dir, "dmesg.txt"), "w") as f:
            f.write("\n".join(lines) + "\n")
        self.result["shell"] = "ok"
        self.result["kernel"] = kernel_timeline(lines)


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
        by.setdefault(r["variant"], []).append(r)
    rows, notes = [], []
    for variant, runs in by.items():
        ok = [r["desktop"] for r in runs if r.get("desktop") is not None]
        med = statistics.median(ok) if ok else None
        first = [r["first_serial"] for r in runs if r.get("first_serial") is not None]
        k = runs[0].get("kernel") or {}
        ms = k.get("milestones", {})
        rows.append("| `%s` | %d/%d | %s | %s | %s | %s |" % (
            variant, len(ok), len(runs), fmt(med),
            ", ".join(fmt(x, "") for x in ok) or "–",
            fmt(statistics.median(first)) if first else "–",
            fmt(ms.get("init_start"))))
        msg = "desktop on screen after %s (%s; runs: %s)" % (
            fmt(med), accel, ", ".join(fmt(x, "") for x in ok) or "none")
        if not ok:
            msg = "no desktop: %s" % runs[0].get("error", "?")
        notes.append((variant, msg))
    md = ["### Boot test (QEMU, %s)" % accel, ""]
    if sizes:
        md += [", ".join("%s: %.1f MB" % (k, v / 1e6) for k, v in sizes), ""]
    md += ["| boot | reached desktop | median | each run (s) | first serial output | kernel ran /init at |",
           "|---|---|---|---|---|---|"] + rows + [""]
    for r in results:
        k = r.get("kernel") or {}
        if r["run"] != 1 or not k.get("lines"):
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
        md.append("</details>")
        md.append("")
    return "\n".join(md) + "\n", notes


def gh_escape(s):
    return s.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--iso")
    ap.add_argument("--kernel")
    ap.add_argument("--variant", action="append", default=[])
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--timeout", type=int, default=0,
                    help="seconds to wait for the desktop (default 90, 600 without KVM)")
    ap.add_argument("--accel", default="auto", choices=["auto", "kvm", "tcg"])
    ap.add_argument("--qemu", default="qemu-system-x86_64")
    ap.add_argument("--out", default="build/boot-test")
    ap.add_argument("--no-interact", action="store_true")
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
            boot = Boot(args, variant, rep, accel, args.out)
            print("boot-test: %s (run %d, %s)..." % (variant, rep, accel), flush=True)
            r = boot.run(interact=(rep == 1 and vi == 0 and not args.no_interact))
            print("boot-test:   desktop at %s, first serial output at %s%s" % (
                fmt(r.get("desktop")), fmt(r.get("first_serial")),
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
        # the kernel timeline of each variant's first boot, for the record
        for r in results:
            k = r.get("kernel") or {}
            if r["run"] == 1 and k.get("lines"):
                lines = ["milestones %s" % json.dumps(k.get("milestones", {}))]
                if r.get("shell"):
                    lines.append("serial shell: %s" % r["shell"])
                lines += ["pause %.3f s after [%.3f] %s" % (g[0], g[1], g[2])
                          for g in k.get("gaps", [])[:8]]
                lines += ["initcall %.1f ms %s" % (us / 1000.0, n)
                          for us, n in k.get("initcalls", [])[:12]]
                if r.get("uptime_at_dump") is not None:
                    lines.append("uptime at dump %.2f" % r["uptime_at_dump"])
                print("::notice title=Kernel %s::%s" % (r["variant"], gh_escape("\n".join(lines))))
            if r.get("error"):
                print("::warning title=Boot %s run %d::%s%%0A%s" % (
                    r["variant"], r["run"], gh_escape(r["error"]),
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
                print("===THUMB %s/%s %d===" % (r["variant"], s["label"], len(b64)))
                for i in range(0, len(b64), 120):
                    print(b64[i:i + 120])
                print("===END===")

    first = [r for r in results if r["variant"] == variants[0]]
    return 0 if first and all(r.get("desktop") is not None for r in first) else 1


if __name__ == "__main__":
    sys.exit(main())
