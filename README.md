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
| Build tools | NASM, `bc` (gavinhoward), `pycdlib`, `kconfiglib` | only what the build itself needs |

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
.github/workflows/ci.yml           static checks (py_compile, shellcheck)
src/                               upstream source checkouts (git-cloned; not committed)
build/                             build artifacts (not committed)
```

## The OS files

`tools/make-rootfs.py` authors the entire runtime system as an initramfs,
written to `build/rootfs`:

```
/init                    first process: mount proc/sysfs/devtmpfs, seed /dev, exec init
/etc/inittab             BusyBox init (getty on tty1/tty2/ttyS0, /bin/login)
/etc/init.d/rcS          sysinit: mdev over devtmpfs, hostname, motd, launch the desktop
/etc/passwd, /etc/group  root + omnios accounts
/etc/profile             ash login environment
/usr/bin/omnios-desktop  boots Nano-X + nanowm + nxterm/nxclock/nxcalc on /dev/fb0
/usr/bin/nano-X, ...     the Nano-X GUI binaries (static)
/etc/fonts/*.bdf         GUI fonts
/bin/*                   BusyBox plus applet symlinks (ash, mount, mdev, ...)
```

The kernel's `CONFIG_INITRAMFS_SOURCE` points at `build/rootfs`, so the whole
OS is compiled into the single static `bzImage`.

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
