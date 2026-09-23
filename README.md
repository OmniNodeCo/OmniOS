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
| C library | musl `1.36.1` | static userspace, no glibc |
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
tools/make-config.py               kernel Kconfig resolution (replaces `conf`)
tools/config/override.config       project kernel options
tools/make-rootfs.py               assembles build/rootfs (the OS)
tools/config/busybox.config        BusyBox build config
tools/config/microwindows.config   Nano-X framebuffer build config
patches/kernel-omnios.patch        the 3 kernel-tree edits, for reproducibility
src/                               upstream source checkouts (git-cloned; not committed)
build/                             build artifacts (not committed)
```

## Build (in order)

```bash
# 0. Python build deps (PyPI is reachable)
python3 -m pip install --user --break-system-packages kconfiglib pycdlib

# 1. fetch upstream sources (git protocol)
#    linux v6.12, musl v1.2.6, busybox 1_36_1, microwindows, gavinhoward/bc
# 2. build host tools
#    src/bc:  ./configure.sh && make   (provides `bc` for the kernel build)
# 3. kernel headers + config
python3 tools/make-config.py                      # writes .config + generated headers
# 4. userspace
#    musl:        ./configure --prefix=... && make && make install   -> build/sysroot
#    busybox:     make CC=<sysroot>/bin/musl-gcc CONFIG_STATIC=y
#    microwindows: make COMPILER=<sysroot>/bin/musl-gcc CFLAGS=-Os LDFLAGS=-static
# 5. root filesystem
python3 tools/make-rootfs.py                      # -> build/rootfs
# 6. kernel (embeds the initramfs)
( cd src/linux && PATH="$PWD/../../src/bc/bin:$PATH" make -j2 bzImage )
```

The ready-to-boot artifact is `src/linux/arch/x86/boot/bzImage` — a static
Linux kernel with the full OmniOS root filesystem and GUI embedded.

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

The next milestone is producing the bootable `bzImage` (and, for BIOS/ISO
testing, a boot sector route). See the build notes above for the exact steps.
