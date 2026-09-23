#!/usr/bin/env python3
"""
OmniOS kernel configuration generator.

Replaces the kernel's `conf`/`scripts/kconfig` binary (which is compiled with
flex+bison, unavailable on the OmniOS build host) by driving the pure-Python
`kconfiglib` library. It produces exactly what `make syncconfig` would:

  .config                          kconfiglib.write_config()  (dotconfig style)
  include/generated/autoconf.h     kconfiglib.write_autoconf() (C header)
  include/config/auto.conf         same contents, make-compatible
  include/config/auto.conf.cmd     dependency placeholder the Makefile includes
  include/config/<SYM>             split-config marker files (for fixdep)

The kernel's own Makefile drives the rest of the build from these files, so no
flex/bison/bootstrap stage is needed.

Environment (defaults suit the project):
  LINUX_SRC         kernel tree (default <repo>/src/linux)
  CONFIG_OVERRIDES  override config (default tools/config/override.config)
"""
import os
import sys

import kconfiglib as K

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
LINUX = os.path.abspath(os.environ.get("LINUX_SRC", os.path.join(REPO, "src", "linux")))
OVERRIDES = os.environ.get("CONFIG_OVERRIDES", os.path.join(REPO, "tools", "config", "override.config"))


def fail(msg):
    print("make-config: ERROR: " + msg, file=sys.stderr)
    sys.exit(1)


def _CONFIG(name):
    # kconfiglib Symbol.name omits the CONFIG_ prefix; restore it.
    return name if name.startswith("CONFIG_") else "CONFIG_" + name


def setup_env():
    os.environ.setdefault("CC", "gcc")
    os.environ.setdefault("LD", "ld")
    os.environ.setdefault("srctree", LINUX)
    os.environ.setdefault("SRCARCH", "x86")
    os.environ.setdefault("CLANG_FLAGS", "")
    # Kconfig runs cc-version.sh/ld-version.sh and $(shell cc-option ...) while
    # parsing; provide what scripts/Kconfig.include expects.
    os.environ.setdefault("CC_VERSION_TEXT", "gcc 12.2.0")
    os.environ.setdefault("LD_VERSION_TEXT", "GNU ld (GNU Binutils for Debian) 2.40")


def main():
    setup_env()
    os.chdir(LINUX)

    kconf = K.Kconfig("Kconfig", warn=False, warn_to_stderr=False)
    print("make-config: parsed %d symbols" % len(kconf.unique_defined_syms))

    # Base config: the in-tree x86_64 defconfig.
    defconfig = os.path.join("arch", "x86", "configs", "x86_64_defconfig")
    if not os.path.exists(defconfig):
        fail("defconfig not found: %s" % defconfig)
    kconf.load_config(defconfig, replace=False)

    # Project overrides.
    if os.path.exists(OVERRIDES):
        kconf.load_config(OVERRIDES, replace=False)
    print("make-config: loaded %s + %s" % (defconfig, OVERRIDES))

    # --- embedded initramfs source (the assembled OmniOS rootfs) ------------
    rootfs = os.path.abspath(os.environ.get(
        "ROOTFS_SOURCE",
        os.path.join(REPO, "build", "rootfs")))
    if "INITRAMFS_SOURCE" in kconf.syms:
        kconf.syms["INITRAMFS_SOURCE"].set_value(rootfs)
        print("make-config: INITRAMFS_SOURCE=%s" % rootfs)
        if "INITRAMFS_COMPRESSION_GZIP" in kconf.syms:
            kconf.syms["INITRAMFS_COMPRESSION_GZIP"].set_value("y")
        if "INITRAMFS_COMPRESSION_NONE" in kconf.syms:
            kconf.syms["INITRAMFS_COMPRESSION_NONE"].set_value("n")

    # --- resolve (olddefconfig semantics) ----------------------------------
    # For anything still unset: default it like the C `olddefconfig` does.
    for sym in kconf.unique_defined_syms:
        if sym.choice is not None or sym.is_constant:
            continue
        if sym.user_value is not None:
            continue
        if sym.type in (K.BOOL, K.TRISTATE):
            sym.set_value(sym.tri_value)
        elif sym.type in (K.INT, K.HEX, K.STRING):
            dv = ""
            for default, cond in sym.defaults:
                if cond and K.expr_value(cond):
                    dv = K.expr_str(default)
                    break
            sym.set_value(dv)

    # --- .config -----------------------------------------------------------
    kconf.write_config(".config")
    print("make-config: wrote .config (%d bytes)" % os.path.getsize(".config"))

    # --- C header: include/generated/autoconf.h ----------------------------
    os.makedirs("include/generated", exist_ok=True)
    kconf.write_autoconf("include/generated/autoconf.h")
    print("make-config: wrote include/generated/autoconf.h")

    # --- make file: include/config/auto.conf -------------------------------
    # The kernel Makefile consumes this as Make variables: `$(CONFIG_X)`.
    # Format (byte-compatible with the C tool's print_symbol_for_autoconf):
    #   CONFIG_BOOL=y / CONFIG_TRI=m     (n -> line omitted)
    #   CONFIG_INT=16  CONFIG_HEX=0x...  CONFIG_STR=<raw, unquoted>
    os.makedirs("include/config", exist_ok=True)
    lines = []
    for sym in kconf.unique_defined_syms:
        n = sym.name
        if not n or sym.is_constant or sym.orig_type == K.UNKNOWN:
            continue
        if not sym._write_to_conf:  # C tool's SYMBOL_WRITE gate
            continue
        t = sym.orig_type
        if t in (K.BOOL, K.TRISTATE):
            if sym.tri_value == 0:
                continue
            lines.append("%s=%s" % (_CONFIG(n), "m" if sym.tri_value == 1 else "y"))
        else:
            val = sym.str_value or ""
            if t == K.HEX and val and not val.startswith(("0x", "0X")):
                val = "0x" + val
            lines.append("%s=%s" % (_CONFIG(n), val))
    with open("include/config/auto.conf", "w") as f:
        f.write("\n".join(lines) + "\n")
    print("make-config: wrote include/config/auto.conf (%d lines)" % len(lines))

    # --- include/config/auto.conf.cmd --------------------------------------
    with open("include/config/auto.conf.cmd", "w") as f:
        f.write("autoconfig := include/config/auto.conf\n")
    print("make-config: wrote include/config/auto.conf.cmd")

    # --- split-config marker files (fixdep) ----------------------------------
    count = 0
    for sym in kconf.unique_defined_syms:
        n = sym.name
        if not n or sym.is_constant:
            continue
        # A marker exists for every symbol that is y/m (in C header form) or
        # has a non-empty string/int/hex value — mirroring the C tool's split
        # config tree (which also honours the SYMBOL_WRITE gate).
        if not sym._write_to_conf:
            continue
        keep = sym.tri_value == 2 or (
            sym.orig_type in (K.STRING, K.INT, K.HEX)
            and sym.str_value not in ("", None))
        if keep:
            short = n[len("CONFIG_"):] if n.startswith("CONFIG_") else n
            marker = os.path.join("include/config", short)
            os.makedirs(os.path.dirname(marker), exist_ok=True)
            with open(marker, "a"):
                pass
            count += 1
    print("make-config: wrote %d split-config marker files" % count)

    # --- make the outputs newer than .config so make never runs syncconfig ----
    import time
    now = max(time.time(), os.path.getmtime(".config") + 10)
    for p in ("include/generated/autoconf.h", "include/config/auto.conf",
              "include/config/auto.conf.cmd"):
        os.utime(p, (now, now))

    print("make-config: done.")


if __name__ == "__main__":
    main()
