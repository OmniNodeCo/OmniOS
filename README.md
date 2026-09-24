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
| GUI | Microwindows / Nano-X | Windows-style windowing on the Linux framebuffer, tiny window manager (nanowm), terminal, clock, calculator, demos |
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
tools/config/busybox.config        BusyBox build config
tools/config/microwindows.config   Nano-X framebuffer build config
patches/kernel-omnios.patch        the 3 kernel-tree edits, for reproducibility
.github/workflows/build.yml        CI: builds kernel + ISO (ccache + prebuilt-userspace caches)
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
/init                    first process: mount, seed /dev, exec /sbin/init
/usr/bin/ominit          (as /sbin/init) the from-scratch PID 1
/usr/bin/omnios-desktop  the desktop shell (owns the framebuffer)
/sbin/getty, /bin/login  console login (BusyBox)
/etc/inittab, /etc/init.d/rcS, /etc/profile
```

## The OS source tree (`os/`)

The actual OmniOS operating-system code is plain C, built statically against
musl, and lives in `os/`:

- **`os/kernel/`** — `ominit` the PID-1 init (mounts proc/sysfs/devtmpfs,
  seeds /dev with mdev, starts the login shell + desktop, reaps orphans,
  honours shutdown signals) and `ommount` its mount/device helpers.
- **`os/lib/`** — `osfb` framebuffer access, `raster` the 2-D software
  rasterizer, `canvas` the 8×8-font text layer, `input`/`devinput` keyboard
  and mouse decoding (evdev, PS/2 and tty), `gfx` the stb_image wrapper.
- **`os/gui/`** — `wm` the window manager/display server with Z-ordering,
  drag/move/resize and close-button chrome; `protocol` a line-based window
  protocol over a UNIX socket; `client` the app-side library that connects
  to the shell; `shell` the Windows-like desktop (taskbar, Start menu,
  clock, wallpaper); `catalog` the app catalog behind the Start menu and
  the App Store; `desktop` the `omnios-desktop` executable.
- **`os/apps/`** — bundled applications each running as its own client
  process: `omnios-term` (a real shell on a pseudo-terminal, shown through
  the VT100-subset emulator in `vt.c`), `omnios-files`, `omnios-calc`,
  `omnios-edit`, `omnios-sysinfo`, `omnios-about`, and `omnios-store`, the
  App Store, which installs and removes apps from the Start menu —
  including `omnios-clock` and `omnios-snake`, which only come from the
  store. The installed set lives in `/var/lib/omnios/installed-apps`.

The GUI model mirrors a real desktop: one display server owns /dev/fb0, and
every application is a separate process that draws into its window over the
socket, receiving keyboard/mouse focus from the server.

## Build (in order)

```bash
# 0. Build deps: distro packages (flex, bison, bc, …) + Python pycdlib (PyPI)
python3 -m pip install --user --break-system-packages pycdlib

# 1. everything, in order:
#    fetch -> toolchain -> musl -> busybox -> microwindows -> rootfs -> kernel -> iso
scripts/build.sh full        # or: make full

# individual stages:
scripts/build.sh fetch       # git clone linux v6.12, musl, busybox, microwindows, bc
scripts/build.sh toolchain   # bc + kernel UAPI headers into build/sysroot
scripts/build.sh musl        # musl libc into build/sysroot
scripts/build.sh userspace   # musl + busybox + microwindows
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
  `/boot/omnios-bzImage` per the Linux boot protocol. The
  `"Operating system not found"` message appears only when the ISO lacks the
  ISOLINUX catalogue — i.e. when it was built without `xorriso`/`isolinux`.
- **VMware**: drop `OmniOS-<version>-amd64.iso` and the `.vmx` in the same
  folder and open the `.vmx`. It boots with UEFI firmware and logs the serial
  console to `omnios-serial.log`; set `firmware = "bios"` to boot legacy.
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
- musl, BusyBox (static), Nano-X (static) and the `os/` core all build
- root filesystem assembles completely
- ISO tooling produces a hybrid BIOS+UEFI image (CI) plus a VMware `.vmx`;
  a UEFI-only fallback exists on hosts without xorriso/isolinux

Next: boot `OmniOS-<version>-amd64.iso` in VMware (UEFI, via the bundled
`.vmx`) and verify the desktop shell starts on the framebuffer.
