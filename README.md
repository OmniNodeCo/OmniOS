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
| Build tools | `bc` (gavinhoward), `musl-gcc`, `pycdlib`, `kconfiglib` | only what the build itself needs |

Everything boots from an **embedded initramfs** built into the kernel, so a
single static kernel image carries the entire OS. The root filesystem is
assembled by `tools/make-rootfs.py` and wired into the kernel via
`CONFIG_INITRAMFS_SOURCE`.

## Why this shape

The build host for this project is intentionally minimal and network-filtered:

- reachable: `github.com` (git protocol), PyPI
- blocked: Debian/apt mirrors, `kernel.org` tarballs, Docker registries,
  `raw.githubusercontent.com`/`codeload` release assets, and most other hosts

So every dependency is fetched with `git clone` from GitHub, and the kernel's
normally flex+bison-built `conf` tool is replaced by `kconfiglib` (pure Python)
in `tools/make-config.py`.

## Layout

```
version.txt                        canonical version (single source of truth)
Makefile                           `make help` — thin wrapper over scripts/build.sh
scripts/build.sh                   build orchestrator, one stage per arg
os/                                from-scratch OmniOS source (init, libs, GUI)
  kernel/                          PID 1 init (ominit) + mount/device helpers
  lib/                             framebuffer, rasterizer, text canvas, input
  gui/                             desktop shell, window manager, protocol, client
  apps/                            terminal, files, calc, editor, sysinfo, about
  vendor/                          vendored public-domain stb_image + font8x8
tools/make-config.py               kernel Kconfig resolution (replaces `conf`)
tools/config/override.config       project kernel options
tools/make-rootfs.py               assembles build/rootfs (the OS itself)
tools/make-iso.py                  assembles the bootable UEFI ISO from the kernel
tools/make-fat.py                  pure-Python FAT image builder for the EFI System Partition
tools/config/busybox.config        BusyBox build config
tools/config/microwindows.config   Nano-X framebuffer build config
patches/kernel-omnios.patch        the 3 kernel-tree edits, for reproducibility
.github/workflows/build.yml        CI: builds kernel + ISO, uploads artifacts
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
  clock, wallpaper); `desktop` the `omnios-desktop` executable.
- **`os/apps/`** — bundled applications each running as its own client
  process: `omnios-term`, `omnios-files`, `omnios-calc`, `omnios-edit`,
  `omnios-sysinfo`, `omnios-about`.

The GUI model mirrors a real desktop: one display server owns /dev/fb0, and
every application is a separate process that draws into its window over the
socket, receiving keyboard/mouse focus from the server.

## Build (in order)

```bash
# 0. Python build deps (PyPI is reachable)
python3 -m pip install --user --break-system-packages kconfiglib pycdlib

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
scripts/build.sh kernel      # tools/make-config.py + bzImage (embeds the initramfs)
scripts/build.sh iso         # build/out/OmniOS-<version>-amd64.iso (UEFI)
scripts/build.sh clean       # remove build outputs
```

The ready-to-boot artifacts land in `build/out/`:

- `omnios-bzImage-<version>` — the static kernel, itself a UEFI bootloader
- `OmniOS-<version>-amd64.iso` + `.sha256` — the bootable UEFI ISO

The ISO is produced two ways, chosen automatically:

- **`xorriso` + `mtools`** when available (the CI path) — builds a proper
  El Torito "*-eltorito-alt-boot -e efi.img*" image.
- **pure Python** otherwise (`pycdlib` + `tools/make-fat.py`) — so the ISO can
  still be produced on a minimal host with no ISO tooling beyond Python.

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

- kernel: configures and compiles to a final linking stage (in progress)
- musl, BusyBox (static), Nano-X (static) all build successfully
- root filesystem assembles completely
- the ISO/EFI tooling is written and verified to produce a valid UEFI El Torito image

The next milestone is finishing the kernel `bzImage` link and booting the
resulting `OmniOS-<version>-amd64.iso`. See the build notes above for the
exact steps.
