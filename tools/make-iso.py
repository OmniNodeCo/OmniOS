#!/usr/bin/env python3
"""
OmniOS ISO assembler (UEFI).

Builds a UEFI-bootable ISO 9660 image around a FAT "EFI System Partition"
image containing the monolithic EFI-stub kernel:

  ESP:  /EFI/BOOT/BOOTX64.EFI   (== the bzImage; Linux boots itself via EFI stub)
  ISO:  /boot/omnios-bzImage     same kernel for other loaders
        /efi.img                 the ESP image the El Torito entry points at
        /README.txt              identity + dd-to-USB instructions

Two code paths, picked automatically:
  * xorriso + mtools are used when available (CI; the battle-tested route).
  * otherwise a pure-Python FAT builder (tools/make-fat.py) and pycdlib
    build the image, so the ISO can be produced even on a minimal host
    with no ISO tooling beyond Python.

Usage: tools/make-iso.py   (requires a kernel at build/out/omnios-bzImage-<V>)
"""
import hashlib
import os
import shutil
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT = os.path.abspath(os.environ.get("ISO_OUT", os.path.join(REPO, "build", "out")))
VERSION = open(os.path.join(REPO, "version.txt")).read().strip()
ARTIFACT = "OmniOS-%s-amd64" % VERSION


def had(tool):
    return shutil.which(tool) is not None


def main():
    ksrc = os.path.join(OUT, "omnios-bzImage-%s" % VERSION)
    if not os.path.exists(ksrc):
        print("make-iso: missing kernel: %s (run scripts/build.sh kernel)"
              % ksrc, file=sys.stderr)
        sys.exit(1)

    # ---- staging directory (the ISO tree) ---------------------------------
    stage = os.path.join(OUT, ".iso-stage")
    if os.path.exists(stage):
        shutil.rmtree(stage)
    os.makedirs(os.path.join(stage, "EFI", "BOOT"))
    os.makedirs(os.path.join(stage, "boot"))
    shutil.copy(ksrc, os.path.join(stage, "EFI", "BOOT", "BOOTX64.EFI"))
    shutil.copy(ksrc, os.path.join(stage, "boot", "omnios-bzImage"))

    with open(os.path.join(stage, "README.txt"), "w") as f:
        f.write(
            "OmniOS %s — lightweight OS built from source\n\n"
            "Boots on UEFI firmware (the kernel is its own bootloader).\n"
            "Write to a USB stick with:\n\n"
            "  dd if=%s of=/dev/sdX bs=16M oflag=direct status=progress\n"
            % (VERSION, ARTIFACT + ".iso"))

    # ---- build the EFI System Partition FAT image --------------------------
    esp = os.path.join(stage, "efi.img")

    if had("mcopy") and had("mformat"):            # mtools route
        with open(esp, "wb") as f:
            f.truncate(48 * 1024 * 1024)
        subprocess.run(["mformat", "-i", esp, "-F", "::"], check=True)
        subprocess.run(["mmd", "-i", esp, "::EFI"], check=True)
        subprocess.run(["mmd", "-i", esp, "::EFI/BOOT"], check=True)
        subprocess.run(["mcopy", "-i", esp,
                        os.path.join(stage, "EFI", "BOOT", "BOOTX64.EFI"),
                        "::EFI/BOOT/BOOTX64.EFI"], check=True)
    else:                                         # pure-python route
        subprocess.run([sys.executable,
                        os.path.join(REPO, "tools", "make-fat.py"),
                        os.path.join(stage, "EFI", "BOOT", "BOOTX64.EFI"),
                        esp],
                       check=True)

    # ---- build the ISO ------------------------------------------------------
    iso = os.path.join(OUT, ARTIFACT + ".iso")

    if had("xorriso"):
        subprocess.run(["xorriso", "-as", "mkisofs",
                        "-iso-level", "3",
                        "-J", "-R",
                        "-V", "OMNIOS_%s" % VERSION.replace(".", "_"),
                        "-o", iso,
                        "-eltorito-alt-boot",
                        "-e", "efi.img",
                        "-no-emul-boot",
                        stage],
                       check=True)
    else:
        _iso_pycdlib(iso, esp, ksrc, os.path.join(stage, "README.txt"))

    size = os.path.getsize(iso)
    sha = os.path.join(OUT, ARTIFACT + ".iso.sha256")
    h = hashlib.sha256(open(iso, "rb").read()).hexdigest()
    with open(sha, "w") as f:
        f.write("%s  %s\n" % (h, ARTIFACT + ".iso"))

    print("make-iso: wrote %s (%.1f MiB)" % (iso, size / 1048576.0))
    print("make-iso: wrote %s" % sha)


def _iso_pycdlib(iso, esp, ksrc, readme):
    """Fallback ISO build with pycdlib (pure Python)."""
    import pycdlib
    c = pycdlib.PyCdlib()
    c.new(interchange_level=3, joliet=3)
    c.add_directory("/EFI", joliet_path="/efi")
    c.add_directory("/EFI/BOOT", joliet_path="/efi/boot")
    c.add_directory("/BOOT", joliet_path="/boot")
    c.add_file(esp, "/EFI/BOOT.IMG", joliet_path="/efi/boot.img")
    c.add_file(ksrc, "/EFI/BOOT/BOOTX64.EFI", joliet_path="/efi/boot/bootx64.efi")
    c.add_file(ksrc, "/BOOT/OMNIOS_BZIMAGE", joliet_path="/boot/omnios-bzImage")
    c.add_file(readme, "/README.TXT", joliet_path="/readme.txt")
    # The El Torito boot-info sector count is a 16-bit count of 512-byte
    # sectors; pass the ESP size explicitly so EFI firmware knows its extent.
    load_size = os.path.getsize(esp) // 512
    c.add_eltorito("/EFI/BOOT.IMG", platform_id=0xEF, efi=True,
                   boot_info_table=False, media_name="noemul",
                   boot_load_size=load_size)
    c.write(iso)
    c.close()


if __name__ == "__main__":
    main()
