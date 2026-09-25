#!/usr/bin/env bash
# OmniOS build orchestrator — builds the whole OS from source, stage by stage.
#
#   scripts/build.sh            full build (fetch, toolchain, kernel, iso)
#   scripts/build.sh kernel     only the kernel + embedded initramfs
#   scripts/build.sh rootfs     only the root filesystem
#   scripts/build.sh userspace  musl + busybox + the OmniOS core
#   scripts/build.sh fetch      only download upstream sources (musl,
#                               BusyBox, bc; the Linux kernel's source is
#                               part of this repository: os/kernel/src)
#   scripts/build.sh iso        assemble the bootable ISO (UEFI)
#
# Environment:
#   OMNIOS_JOBS   parallel make jobs (default: nproc)
#   OMNIOS_KEEP   do not remove build/work directories on failure
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
SRC="$ROOT/src"
BLD="$ROOT/build"
SIM="$BLD/sysroot"
# The Linux kernel: its source is in this repository (Linux v6.12 with
# OmniOS's edits), built out of tree so the source stays clean
KSRC="$ROOT/os/kernel/src"
KOUT="$BLD/linux"
JOBS="${OMNIOS_JOBS:-$(nproc 2>/dev/null || echo 2)}"
VERSION="$(tr -d '[:space:]' < "$ROOT/version.txt")"

log()  { printf '\033[1;32m[omnios]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[omnios]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[omnios]\033[0m %s\n' "$*" >&2; exit 1; }

had() { command -v "$1" >/dev/null 2>&1; }

require() {
    for t in "$@"; do
        had "$t" || die "missing required command: $t"
    done
}

stage_fetch() {
    log "fetching upstream sources (git)…"
    mkdir -p "$SRC"
    clone() { # name url [ref]
        local dst="$SRC/$1" ref="${3:-}" cur
        if [ -d "$dst/.git" ]; then
            cur="$(git -C "$dst" describe --tags --exact-match 2>/dev/null || true)"
            if [ -n "$ref" ] && [ "$cur" != "$ref" ]; then
                log "  $1 (re-cloning: want $ref, have ${cur:-branch HEAD})"
                rm -rf "$dst"
            else
                log "  $1 (present)"
            fi
        fi
        [ -d "$dst/.git" ] || {
            log "  $1 (cloning)"
            git clone --depth 1 ${ref:+--branch "$ref"} "$2" "$dst"
        }
    }
    clone musl         https://github.com/ifduyue/musl.git           v1.2.6
    clone busybox      https://github.com/mirror/busybox.git         1_36_1
    clone bc           https://github.com/gavinhoward/bc.git          7.1.0
    # (no kernel download: its source is os/kernel/src, in this repository)
    # BusyBox's TLS P-256 fix: without it, wget cannot talk to GitHub
    # (OmniOS Update); see the patch header
    (
        cd "$SRC/busybox"
        if git apply --check "$ROOT/patches/busybox-tls-p256.patch" 2>/dev/null; then
            git apply "$ROOT/patches/busybox-tls-p256.patch"
            log "  applied patches/busybox-tls-p256.patch"
        else
            warn "  busybox TLS patch not applied (already applied?)"
        fi
    )
}

stage_toolchain() {
    log "building host tools (bc, kernel headers)…"
    require gcc make git python3
    [ -x "$SRC/bc/bin/bc" ] || {
        cd "$SRC/bc"
        # bc 7.1.0, bc-only, optimised, no NLS. Long-form flags only:
        # newer bc reused `-s` for `-s SETTING`, which broke short forms.
        ./configure.sh --bc-only --disable-nls --opt=2
        make -j"$JOBS"
    }
    log "  bc ok: $SRC/bc/bin/bc"

    # kernel UAPI headers into sysroot for the musl userspace build
    [ -f "$KSRC/Makefile" ] || die "kernel source missing: $KSRC"
    PATH="$SRC/bc/bin:$PATH" make -C "$KSRC" O="$KOUT" ARCH=x86_64 \
        headers_install INSTALL_HDR_PATH="$SIM/usr" >/dev/null
    log "  kernel headers installed to $SIM/usr/include"
}

stage_musl() {
    log "building musl libc…"
    require gcc make
    if [ -f "$SRC/musl/lib/libc.a" ]; then
        log "  musl (already built)"
    else
        ( cd "$SRC/musl" && make distclean >/dev/null 2>&1 || true )
        ( cd "$SRC/musl" && ./configure \
                --prefix="$SIM/usr" --syslibdir="$SIM/lib" --disable-shared )
        make -C "$SRC/musl" -j"$JOBS"
        make -C "$SRC/musl" install
    fi
    log "  musl installed to $SIM"
}

stage_busybox() {
    log "building BusyBox (static, musl)…"
    require gcc
    [ -x "$SRC/busybox/busybox" ] && { log "  busybox (already built)"; return; }
    local bbcfg="$SRC/busybox/.config" tmp="$SRC/busybox/.config.omnios"
    # Start from a full upstream defaults tree so dependencies are satisfied
    # and no NEW symbol can prompt; then overlay our curated options.
    make -C "$SRC/busybox" mrproper >/dev/null 2>&1 || true
    make -C "$SRC/busybox" defconfig >/dev/null 2>&1
    awk 'NR==FNR {
            raw=$0;
            if (raw ~ /^[ \t]*CONFIG_[A-Za-z0-9_]+=/) {           # on-line
                k=raw; sub(/^[ \t]*/,"",k); split(k,a,"=");
                val[a[1]]=a[2]; off[a[1]]=0;
            } else if (raw ~ /^[ \t]*# CONFIG_[A-Za-z0-9_]+ is not set/) {
                k=raw; sub(/^[ \t]*#[ \t]*/,"",k); sub(/[ \t]*is not set[ \t]*$/,"",k);
                val[k]=""; off[k]=1;                                # off-line
            }
            next;
        } {
            k=$1; if ($0 ~ /^# CONFIG_/) { k=$2 }
            sub(/^#/,"",k); sub(/=.*/,"",k);
            if (k in val) {
                if (off[k]) printf "# %s is not set\n", k;
                else        printf "%s=%s\n", k, val[k];
                delete val[k]; delete off[k];
            } else {
                print;
            }
        } END {
            for (k in val) {
                if (off[k]) printf "# %s is not set\n", k;
                else        printf "%s=%s\n", k, val[k];
            }
        }' \
        "$ROOT/tools/config/busybox.config" "$bbcfg" > "$tmp"
    mv "$tmp" "$bbcfg"
    # Everything NEW is already answered; this just normalises the config.
    make -C "$SRC/busybox" oldconfig </dev/null >/dev/null 2>&1 || true
    # Record the resolved config for inspection/reproducibility.
    mkdir -p "$BLD"
    cp "$bbcfg" "$BLD/busybox.config.resolved"
    # OmniOS Update downloads with wget over HTTPS: BusyBox's own TLS, and
    # not the openssl helper, which crashes wget when there is no openssl
    grep -q '^CONFIG_FEATURE_WGET_HTTPS=y' "$bbcfg" ||
        warn "  busybox: CONFIG_FEATURE_WGET_HTTPS is off: no HTTPS downloads"
    if grep -q '^CONFIG_FEATURE_WGET_OPENSSL=y' "$bbcfg"; then
        warn "  busybox: CONFIG_FEATURE_WGET_OPENSSL is on: wget crashes after HTTPS downloads"
    fi
    make -C "$SRC/busybox" -j"$JOBS" \
        CC="$SIM/usr/bin/musl-gcc" \
        CONFIG_STATIC=y
    log "  busybox: $SRC/busybox/busybox"
}

stage_binaries() {
    # normalise the install trees make-rootfs expects
    local bbin="$BLD/install/busybox"
    local osbin="$BLD/install/os/bin"
    mkdir -p "$bbin" "$osbin"
    cp -f "$SRC/busybox/busybox" "$bbin/busybox" 2>/dev/null || true
    log "  staged binaries to $BLD/install"
}

stage_os() {
    log "building the OmniOS core (PID 1 init, desktop shell, apps, libs)…"
    require gcc make
    make -C "$ROOT/os" all
    log "  os binaries: $BLD/install/os/bin"
}

stage_rootfs() {
    log "assembling root filesystem…"
    stage_binaries
    python3 "$ROOT/tools/make-rootfs.py"
    log "  rootfs: $BLD/rootfs"
}

stage_kernel() {
    log "configuring and building the Linux kernel…"
    require gcc make perl python3 flex bison zstd
    # bc comes from our pinned in-tree build (src/bc), not the PATH
    [ -x "$SRC/bc/bin/bc" ] || die "bc not built yet: run scripts/build.sh toolchain"
    stage_musl
    stage_busybox
    stage_os
    stage_rootfs
    (
        PATH="$SRC/bc/bin:$PATH"
        # the kernel from os/kernel/src, built in build/linux
        kmake() { make -C "$KSRC" O="$KOUT" ARCH=x86_64 "$@"; }
        mkdir -p "$KOUT"
        cd "$KOUT"
        # Native Kconfig (not kconfiglib): Linux v6.12 Kconfig uses the `modules`
        # keyword, which the last kconfiglib release cannot parse. The kernel's
        # own `conf` handles it; it needs flex+bison (installed as build deps).
        kmake x86_64_defconfig >/dev/null
        "$KSRC/scripts/kconfig/merge_config.sh" -m .config \
            "$ROOT/tools/config/override.config" >/dev/null
        # Embed the assembled rootfs as the initramfs, uncompressed: the
        # whole kernel image is compressed (zstd, override.config) anyway,
        # and compressing the initramfs inside it as well only adds a
        # second pass to unpack at boot. The list adds /dev/console (a
        # device node make-rootfs.py cannot create without root): without
        # it the kernel prints "unable to open an initial console" on the
        # screen. Files owned by the building user become root's.
        printf 'nod /dev/console 0600 0 0 c 5 1\n' > "$BLD/initramfs-extra.list"
        "$KSRC/scripts/config" --file .config \
            --set-str INITRAMFS_SOURCE "$BLD/rootfs $BLD/initramfs-extra.list" \
            --set-val INITRAMFS_ROOT_UID -1 \
            --set-val INITRAMFS_ROOT_GID -1 \
            --disable INITRAMFS_COMPRESSION_GZIP \
            --enable INITRAMFS_COMPRESSION_NONE
        kmake olddefconfig >/dev/null
        # OmniOS Update needs these (a warning: the OS boots without them)
        for opt in KEXEC_FILE E1000 VMXNET3; do
            grep -q "^CONFIG_$opt=y" .config || warn "  kernel: CONFIG_$opt is off"
        done
        # what makes the boot fast (a warning: the OS boots without them)
        for opt in KERNEL_ZSTD INITRAMFS_COMPRESSION_NONE; do
            grep -q "^CONFIG_$opt=y" .config || warn "  kernel: CONFIG_$opt is off"
        done
        grep -q '^CONFIG_CMDLINE=".* quiet[ "]' .config || warn "  kernel: the command line is not quiet"
        # OmniOS Update strips the built-in command line from /proc/cmdline
        # before handing the rest to an update's kernel: its copy must match
        want=$(sed -n 's/^CONFIG_CMDLINE="\(.*\)"$/\1/p' .config)
        have=$(sed -n '/^#define BUILTIN_CMDLINE/,/[^\\]$/p' "$ROOT/os/apps/update.c" |
               grep -o '"[^"]*"' | tr -d '"\n')
        [ "$want" = "$have" ] ||
            warn "  kernel: CONFIG_CMDLINE differs from BUILTIN_CMDLINE in os/apps/update.c"
        kmake -j"$JOBS" bzImage
    )
    # collect the monolithic EFI-stub kernel
    mkdir -p "$BLD/out"
    cp "$KOUT/arch/x86/boot/bzImage" "$BLD/out/omnios-bzImage-$VERSION"
    log "  kernel: $BLD/out/omnios-bzImage-$VERSION"
}

stage_iso() {
    log "assembling bootable hybrid ISO (BIOS + UEFI)…"
    require python3
    [ -f "$BLD/out/omnios-bzImage-$VERSION" ] || stage_kernel
    if ! had xorriso; then
        # pure-Python fallback: UEFI-only ISO (no BIOS boot catalogue)
        python3 -c 'import pycdlib' 2>/dev/null \
            || die "pycdlib not installed: python3 -m pip install pycdlib"
        warn "xorriso/isolinux not found — ISO will be UEFI-only; BIOS "
        warn "firmware will still report 'Operating system not found'."
    fi
    python3 "$ROOT/tools/make-iso.py"
    log "  ISO: $BLD/out/OmniOS-$VERSION-amd64.iso"
    ls -la "$BLD/out"
}

stage_clean() {
    rm -rf "$BLD/rootfs" "$BLD/install" "$BLD/out" "$KOUT"
    log "cleaned build outputs"
}

STAGE="${1:-full}"
case "$STAGE" in
    fetch)     stage_fetch ;;
    toolchain) stage_toolchain ;;
    musl)      stage_musl ;;
    userspace) stage_toolchain; stage_musl; stage_busybox; stage_os ;;
    os)        stage_os ;;
    rootfs)    stage_rootfs ;;
    kernel)    stage_kernel ;;
    iso)       stage_iso ;;
    clean)     stage_clean ;;
    full|all)  stage_fetch; stage_toolchain; stage_musl; stage_busybox; \
               stage_os; stage_rootfs; stage_kernel; stage_iso ;;
    *) die "unknown stage: $STAGE (try fetch|toolchain|userspace|os|rootfs|kernel|iso|clean)" ;;
esac
log "done."
