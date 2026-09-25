# OmniOS — a lightweight OS built from scratch

OmniOS is now a **from-scratch, lightweight desktop operating system**. It is
not a Debian/KDE remaster and not based on any prebuilt distribution: the
kernel, C library, userspace, and graphical desktop are all compiled from
source inside this repository.

> This rewrite replaces the earlier KDE/live-build Debian project. The old
> `auto/`, `config/`, `ci/`, `packaging/`, and `scripts/` trees and their CI
> workflows are removed. `version.txt` remains the single version source.

## What it is

| Layer | From source | Notes |
| --- | --- | --- |
| Kernel | Linux `v6.12` (git, `torvalds/linux`) | x86_64, monolithic, bootable via EFI stub |
| C library | musl `1.2.6` | static userspace, no glibc |
| Core tools | BusyBox `1.36.1` | static, ash shell, ~75 applets |
| GUI | OmniOS desktop (written from scratch, `os/gui`) | Windows 11-style desktop on the Linux framebuffer: compositing window manager, centred taskbar, Start menu with search, Quick Settings, desktop icons, App Store and bundled apps. |
| Build tools | `bc` (gavinhoward), `musl-gcc`, `pycdlib`, flex+bison | only what the build itself needs |

Everything boots from an **embedded initramfs** built into the kernel, so a
single static kernel image carries the entire OS. The root filesystem is
assembled by `tools/make-rootfs.py` and wired into the kernel via
`CONFIG_INITRAMFS_SOURCE`.

## Why this shape

The build host for this project is intentionally minimal and network-filtered:

- reachable: `github.com` (git protocol), PyPI
- blocked: Debian/apt mirrors, `kernel.org` tarballs, Docker registries,
  `raw.githubusercontent.com`/`codeload` release assets, and most other hosts

So every dependency is fetched with `git clone` from GitHub, and the kernel is
configured with its own `conf`/`olddefconfig` (flex+bison are required build
deps; Linux 6.12 Kconfig uses the `modules` keyword, which the Python
`kconfiglib` library cannot parse).

## Layout

```
version.txt                        canonical version (single source of truth)
Makefile                           `make help` — thin wrapper over scripts/build.sh
scripts/build.sh                   build orchestrator, one stage per arg
os/                                from-scratch OmniOS source (init, libs, GUI)
  kernel/                          PID 1 init (ominit) + mount/device helpers
  lib/                             framebuffer, rasterizer, text canvas, input
  gui/                             desktop shell, window manager, protocol, client
  apps/                            terminal, files, calc, editor, sysinfo, about, App Store, clock, snake
  vendor/                          vendored public-domain stb_image + font8x8
tools/config/override.config       project kernel options (merged over defconfig)
tools/make-rootfs.py               assembles build/rootfs (the OS itself)
tools/make-iso.py                  assembles the hybrid BIOS+UEFI ISO + .vmx
tools/make-fat.py                  pure-Python FAT image builder for the EFI System Partition
tools/boot-test.py                 boots the ISO in QEMU/KVM, times it, screenshots (CI)
tools/config/busybox.config        BusyBox build config
patches/kernel-omnios.patch        the 3 kernel-tree edits, for reproducibility
.github/workflows/build.yml        CI: builds kernel + ISO (ccache + prebuilt-userspace caches), boot test
.github/workflows/release.yml      publishes the ISO as a GitHub release
.github/workflows/ci.yml           static checks (py_compile, shellcheck, C syntax)
src/                               upstream source checkouts (git-cloned; not committed)
build/                             build artifacts (not committed)
```

## The OS files

The runtime system is assembled by `tools/make-rootfs.py` into an initramfs
(`build/rootfs`); the kernel's `CONFIG_INITRAMFS_SOURCE` points at that tree
so the whole OS is compiled into the single static `bzImage`:

```
/init, /sbin/init        -> /usr/bin/ominit, the from-scratch PID 1: mounts
                         /proc, /sys, /dev, starts the desktop, the network
                         and OmniOS Update, reaps orphans
/usr/bin/omnios-desktop  the desktop shell (owns the framebuffer)
/usr/bin/omnios-*        the apps and OmniOS Update
/etc/init.d/network      DHCP on every wired adapter (BusyBox udhcpc)
/bin/*                   BusyBox: ash (the Terminal's shell), wget, tools
/etc/profile             the Terminal's shell environment
```

## The OS source tree (`os/`)

The actual OmniOS operating-system code is plain C, built statically against
musl, and lives in `os/`:

- **`os/kernel/`** — `ominit` the PID-1 init (mounts proc/sysfs/devtmpfs,
  seeds /dev with mdev, starts the login shell, the desktop, the network
  (DHCP) and OmniOS Update, reaps orphans, honours shutdown signals, and
  restarts straight into a downloaded update) and `ommount` its mount/device
  helpers.
- **`os/lib/`** — `osfb` framebuffer access, `raster` the 2-D software
  rasterizer, `canvas` the 8×8-font text layer, `input`/`devinput` keyboard
  and mouse decoding (evdev, PS/2 and tty), `gfx` the stb_image wrapper.
- **`os/gui/`** — `wm` the window manager/display server with Z-ordering,
  drag/move/resize and close-button chrome; `protocol` a line-based window
  protocol over a UNIX socket; `client` the app-side library that connects
  to the shell; `shell` the Windows 11-like desktop and its compositor
  (below), which builds each frame in RAM and copies only
  the changed 16-pixel blocks to the framebuffer, with the mouse cursor as
  a separate sprite; `theme` the drawing toolkit behind the look
  (generated wallpaper, rounded corners, shadows, frosted glass, gradients
  and app icons); `icons` the vector pictograms for every app and Settings
  page; `catalog` the app catalog behind the Start menu and the App Store;
  `settings` the user's settings file (wallpaper, accent colour, clock,
  time zone, taskbar, desktop icons, updates, sign-in); `account` the
  sign-in account and its password (`/etc/shadow`); `updstat` the OmniOS
  Update status shared by the updater, Settings and the desktop; `netinfo`
  the network status (address, gateway, DNS, traffic) for the taskbar and
  Settings; `desktop` the `omnios-desktop` executable.
- **`os/apps/`** — bundled applications each running as its own client
  process: `omnios-term` (a real shell on a pseudo-terminal, shown through
  the VT100-subset emulator in `vt.c`), `omnios-files`, `omnios-calc`,
  `omnios-edit`, `omnios-sysinfo`, `omnios-about`, `omnios-settings` (the
  Windows 11-style Settings app: System, Network & internet,
  Personalization, Apps, Accounts, Time & language, OmniOS Update), and
  `omnios-store`, the App Store, which
  installs and removes apps from the Start menu — including the ones that
  only come from the store: `omnios-clock`, `omnios-snake`,
  `omnios-taskmgr` (Task Manager), `omnios-calendar`, `omnios-mines`
  (Minesweeper), `omnios-2048` and `omnios-tictactoe`. The installed set
  lives in `/var/lib/omnios/installed-apps`. `omnios-update` is OmniOS
  Update (below); it has no window of its own.

The GUI model mirrors a real desktop: one display server owns /dev/fb0, and
every application is a separate process that draws into its window over the
socket, receiving keyboard/mouse focus from the server.

The desktop follows Windows 11:

- **Taskbar**: Start, Search and pinned apps (File Manager, Terminal, App
  Store, Settings) centred, or on the left (Settings > Personalization);
  other open apps join them. A dash marks open apps, a longer accent dash
  the focused one; an app's windows share one icon (click cycles through
  them), and a right-click offers a new window or **Close window**. The
  tray has OmniOS Update, the network icon (Quick Settings) and the clock
  (calendar).
- **Start**: type to search apps, Settings pages and App Store apps; a
  pinned grid of the installed apps; **Recommended** (update status,
  network, more apps, personalize); the account (settings, lock) and power
  (Lock, Restart or **Update and restart**, Shut down).
- **Quick Settings**: Ethernet, automatic updates, 24-hour clock, desktop
  icons, centred taskbar, next wallpaper; the IP address and update
  status; lock, Settings and power.
- **Desktop icons** (This PC, Terminal, App Store, Settings): double-click
  or Enter opens them; right-click the desktop for Personalize, Display
  settings and Open in Terminal.
- **Sign-in and lock screens**: the account at the bottom left, network and
  power (Restart, Shut down) at the bottom right.
- **Keys**: Windows opens Start; Windows + L locks, + A Quick Settings,
  + N calendar, + I Settings, + E File Manager, + D shows the desktop,
  + S search, + X quick links.

## Build (in order)

```bash
# 0. Build deps: distro packages (flex, bison, bc, …) + Python pycdlib (PyPI)
python3 -m pip install --user --break-system-packages pycdlib

# 1. everything, in order:
#    fetch -> toolchain -> musl -> busybox -> os -> rootfs -> kernel -> iso
scripts/build.sh full        # or: make full

# individual stages:
scripts/build.sh fetch       # git clone linux v6.12, musl, busybox, bc
scripts/build.sh toolchain   # bc + kernel UAPI headers into build/sysroot
scripts/build.sh musl        # musl libc into build/sysroot
scripts/build.sh userspace   # musl + busybox + the OmniOS core
scripts/build.sh os          # build the OmniOS core (init + desktop + apps)
scripts/build.sh rootfs      # assemble build/rootfs via tools/make-rootfs.py
scripts/build.sh kernel      # x86_64_defconfig + overrides + bzImage (initramfs)
scripts/build.sh iso         # build/out/OmniOS-<version>-amd64.iso (BIOS+UEFI)
scripts/build.sh clean       # remove build outputs
```

The ready-to-boot artifacts land in `build/out/`:

- `omnios-bzImage-<version>` — the static kernel; it is its own bootloader
  (EFI stub for UEFI, real-mode setup for BIOS).
- `OmniOS-<version>-amd64.iso` + `.sha256` — the bootable **hybrid ISO**
  (BIOS via ISOLINUX, UEFI via the EFI stub).
- `OmniOS-<version>-amd64.vmx` — a ready-to-run VMware machine definition
  that boots the ISO (see below).

### Booting

- **UEFI firmware**: the kernel is executed directly from
  `/EFI/BOOT/BOOTX64.EFI` (no bootloader).
- **BIOS firmware** (e.g. VMware "BIOS" mode): ISOLINUX loads
  `/boot/omnios-bzImage` per the Linux boot protocol. It needs
  `ldlinux.c32` beside `isolinux.bin` (Debian/Ubuntu: `syslinux-common`);
  `make-iso.py` refuses to build an ISO without it, since BIOS boots cannot
  work then (releases up to 2026.2.3 had this problem). The
  `"Operating system not found"` message appears only when the ISO lacks the
  ISOLINUX catalogue — i.e. when it was built without `xorriso`/`isolinux`.
- **VMware**: drop `OmniOS-<version>-amd64.iso` and the `.vmx` in the same
  folder and open the `.vmx`. It boots with UEFI firmware from a SATA
  CD-ROM, with a vmxnet3 network adapter, and logs the serial console to
  `omnios-serial.log`; set `firmware = "bios"` to boot legacy.
- The kernel's command line is built in (`CONFIG_CMDLINE`, the same for
  both firmware types): the consoles, `quiet` (only errors while the
  kernel starts; OmniOS turns the log level back up once it runs) and
  `driver_async_probe` for the slowest drivers.

### Boot test

`tools/boot-test.py` boots the ISO in QEMU with KVM, on hardware close to
the VMware VM (UEFI firmware, CD-ROM, network adapter, PS/2 keyboard and
VMware mouse, 1 GB, 2 CPUs), and times how long it takes until the desktop
(the lock screen) is on the screen. Then it presses a key, signs in, opens
Start and Quick Settings, with a screenshot of each. With
`omnios.serialshell` on the kernel command line OmniOS opens a root shell
on the serial port, through which the test reads the kernel log (initcall
times), checks the DHCP lease and looks for zombie processes.

The "Boot test (QEMU)" job in `build.yml` runs it after every build and
boots the latest release alternately on the same runner, for comparison
(runners differ too much between runs to compare separate runs). The
screenshots and logs are the `omnios-boot-test` artifact. A
`workflow_dispatch` with a `release` tag boot-tests that release instead.

Power-on to lock screen, 2026.2.3 against the changes since, booted
alternately on one runner (QEMU/KVM):

| boot | 2026.2.3 | since |
|---|---|---|
| UEFI, IDE CD-ROM, e1000 (the older `.vmx`) | 4.72 s | 3.34 s |
| UEFI, SATA CD-ROM, e1000 | 3.50 s | 2.57 s |
| BIOS (ISOLINUX) | did not start | 1.42 s |

Where it came from: the kernel no longer prints its ~700 boot messages
(`quiet`: 0.66 s), the kernel image went from 13.2 MB to 8.1 MB (zstd, the
initramfs not compressed twice, no Nano-X, none of the defconfig's unused
drivers), so firmware loads it sooner, and the slowest drivers probe on
the second CPU: kernel start to `/init` went from 1.22 s to 0.47 s, and
`/init` to the desktop's first frame takes 0.07 s. What is left in the
kernel is mostly e1000 reading its EEPROM (0.37 s), which is why the
`.vmx` now uses vmxnet3.
- **USB stick**: the CI-built ISO is hybrid, so
  `dd if=OmniOS-<version>-amd64.iso of=/dev/sdX bs=16M oflag=direct
  status=progress` yields a directly bootable drive on both firmware types.

The ISO is produced two ways, chosen automatically:

- **`xorriso` + `mtools` + `isolinux`** when available (the CI path) — builds
  a proper hybrid BIOS+UEFI El Torito image with a BIOS boot catalogue.
- **pure Python** otherwise (`pycdlib` + `tools/make-fat.py`) — a
  **UEFI-only** ISO on a minimal host with no ISO tooling beyond Python.

`tools/make-fat.py` builds a small FAT16 "superfloppy" EFI System Partition
(`/EFI/BOOT/BOOTX64.EFI`) without requiring `dosfstools`/`mtools`.

## OmniOS Update

Like Windows Update, OmniOS keeps itself up to date:

- At boot, `ominit` runs `/etc/init.d/network` (DHCP on every wired
  adapter; the generated `.vmx` has a NAT adapter) and `omnios-update
  daemon`, which checks a minute after the network comes up and then every
  6 hours.
- Each release publishes `omnios-update.txt` (version, file name, size,
  SHA-256) next to its ISO, together with the kernel image it names. That
  image is the whole OS, as the root file system is built into the kernel.
  The updater reads the feed from
  `https://github.com/<repo>/releases/latest/download/omnios-update.txt`
  (`/etc/omnios-update.conf`).
- With **Get updates automatically** on (Settings > OmniOS Update, the
  default), a newer release is downloaded in the background and checked
  against the size and SHA-256. Off, you get an "update available"
  notification and a **Download & install** button instead.
- **Pause updates** stops the automatic checks for a week at a time (up to
  five weeks); **Resume updates** checks straight away.
- The download is loaded with `kexec_file_load()` (`CONFIG_KEXEC_FILE`). A
  notification, an amber dot on the update icon in the taskbar and on the
  power button in Start (**Update and restart**) say it is ready;
  restarting boots straight into the
  new version (`reboot(RB_KEXEC)`) instead of going back through the
  firmware. The settings, installed apps, password and update history
  travel along in a small initramfs, so the new version starts where the old
  one left off.

Limits: OmniOS runs from RAM, so an installed update lasts until the
computer is switched off. After a cold start from the old ISO the updater
downloads it again (or use the new ISO). HTTPS goes through BusyBox `wget`,
whose TLS code does not validate certificates: the SHA-256 catches damaged
downloads, not an attacker who controls the network path.

BusyBox 1.36.1's TLS miscomputes P-256 keys on x86_64, and GitHub rejects
the handshake. `patches/busybox-tls-p256.patch` carries the two upstream
fixes (made after 1.36.1); `scripts/build.sh fetch` applies it.

## Kernel tree edits

Three minimal, justified edits (see `patches/kernel-omnios.patch`):

1. `arch/x86/Kconfig` — leave `HAVE_OBJTOOL` unselected: the build host has no
   libelf headers, so the in-kernel `objtool` cannot be compiled. Every
   objtool consumer is already guarded by `if HAVE_OBJTOOL` and falls back to
   its non-objtool path.
2. `include/linux/objtool.h` — define a no-op `VALIDATE_UNRET_BEGIN` for the
   `CONFIG_OBJTOOL=n` case (upstream only defines it under `CONFIG_OBJTOOL`,
   but `asm/unwind_hints.h` always references it).
3. `Makefile` — `headers_install` without `rsync` (unavailable), using
   `find | xargs cp --parents`.

## Status

- kernel: configures and compiles (native `olddefconfig` + flex/bison; the
  in-tree `bc` build is pinned at 7.1.0)
- musl, BusyBox (static) and the `os/` core all build
- root filesystem assembles completely
- ISO tooling produces a hybrid BIOS+UEFI image (CI) plus a VMware `.vmx`;
  a UEFI-only fallback exists on hosts without xorriso/isolinux

- boots to the desktop in QEMU/KVM on UEFI and BIOS firmware, signs in,
  opens Start and Quick Settings (the boot test, after every CI build)

Next: boot `OmniOS-<version>-amd64.iso` in VMware (UEFI, via the bundled
`.vmx`) and verify the desktop shell starts on the framebuffer, the network
comes up and OmniOS Update reaches GitHub.
