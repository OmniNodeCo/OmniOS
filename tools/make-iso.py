#!/usr/bin/env python3
"""
OmniOS ISO assembler — hybrid BIOS + UEFI.

Builds a bootable ISO 9660 image that starts on BOTH kinds of firmware:

  * UEFI  — monolithic EFI-stub kernel as /EFI/BOOT/BOOTX64.EFI inside a FAT
            "EFI System Partition" (efi.img), exposed via an El Torito EFI
            entry. The Linux kernel is its own UEFI bootloader.
  * BIOS  — ISOLINUX. It reads /boot/omnios-bzImage per the Linux boot
            protocol and jumps into the kernel's real-mode setup code. (The
            "Operating system not found" message from VMware/BIOS firmware
            disappears because the ISO now carries a BIOS boot catalogue.)

The kernel contains the whole OS as an embedded initramfs, so there is no
separate initrd anywhere.

ISO layout:
    /EFI/BOOT/BOOTX64.EFI     the kernel for UEFI firmware
    /boot/omnios-bzImage      the same kernel for ISOLINUX/other loaders
    /isolinux/isolinux.bin    BIOS boot sector (El Torito default entry)
    /isolinux/isolinux.cfg    BIOS boot menu (serial + VGA console)
    /efi.img                  the EFI System Partition image (El Torito EFI)
    /README.txt               identity + dd-to-USB instructions

When xorriso + ISOLINUX + mtools are available (the CI host) the ISO is built
with -isohybrid-mbr too, so it can be `dd`-ed straight onto a USB stick and
booted by BIOS firmware. On a host with only Python, pycdlib builds a UEFI
image (no BIOS catalogue, since ISOLINUX is absent).

Usage: tools/make-iso.py   (needs a kernel at build/out/omnios-bzImage-<V>)
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

# Candidate paths for ISOLINUX binaries across common distros.
ISOLINUX_BIN_PATHS = [
    "/usr/lib/ISOLINUX/isolinux.bin",
    "/usr/lib/syslinux/isolinux.bin",
    "/usr/lib/syslinux/bios/isolinux.bin",
    "/usr/share/syslinux/isolinux.bin",
    "/usr/share/syslinux/bios/isolinux.bin",
]
LDLINUX_C32_PATHS = [
    "/usr/lib/syslinux/modules/bios/ldlinux.c32",    # Debian/Ubuntu syslinux-common
    "/usr/lib/ISOLINUX/ldlinux.c32",
    "/usr/lib/syslinux/ldlinux.c32",
    "/usr/lib/syslinux/bios/ldlinux.c32",
    "/usr/share/syslinux/ldlinux.c32",
    "/usr/share/syslinux/bios/ldlinux.c32",
]
ISOHYBRID_MBR_PATHS = [
    "/usr/lib/ISOLINUX/isohdpfx.bin",
    "/usr/lib/syslinux/mbr/isohdpfx.bin",
    "/usr/share/syslinux/isohdpfx.bin",
    "/usr/lib/syslinux/isohdpfx.bin",
    "/usr/share/syslinux/mbr/isohdpfx.bin",
]


def had(tool):
    return shutil.which(tool) is not None


def find_existing(paths):
    for p in paths:
        if os.path.isfile(p):
            return p
    return None


def main():
    ksrc = os.path.join(OUT, "omnios-bzImage-%s" % VERSION)
    if not os.path.exists(ksrc):
        print("make-iso: missing kernel: %s (run scripts/build.sh kernel)"
              % ksrc, file=sys.stderr)
        sys.exit(1)

    # ---- staging tree (the ISO content) ------------------------------------
    stage = os.path.join(OUT, ".iso-stage")
    if os.path.exists(stage):
        shutil.rmtree(stage)
    os.makedirs(os.path.join(stage, "EFI", "BOOT"))
    os.makedirs(os.path.join(stage, "boot"))
    os.makedirs(os.path.join(stage, "isolinux"))
    shutil.copy(ksrc, os.path.join(stage, "EFI", "BOOT", "BOOTX64.EFI"))
    shutil.copy(ksrc, os.path.join(stage, "boot", "omnios-bzImage"))

    # ---- ISOLINUX (BIOS) boot files -----------------------------------------
    isolinux_bin = find_existing(ISOLINUX_BIN_PATHS)
    have_bios = isolinux_bin is not None
    if have_bios:
        shutil.copy(isolinux_bin, os.path.join(stage, "isolinux", "isolinux.bin"))
        ldlinux = find_existing(LDLINUX_C32_PATHS)
        if not ldlinux:
            # ISOLINUX 6.x loads its core module from the isolinux directory
            # and stops with "Failed to load ldlinux.c32" without it: every
            # BIOS boot would fail. (On Debian/Ubuntu it is in syslinux-
            # common, which the isolinux package only recommends.)
            print("make-iso: isolinux.bin found (%s) but not ldlinux.c32: "
                  "install syslinux-common" % isolinux_bin, file=sys.stderr)
            sys.exit(1)
        shutil.copy(ldlinux, os.path.join(stage, "isolinux", "ldlinux.c32"))
        with open(os.path.join(stage, "isolinux", "isolinux.cfg"), "w") as f:
            f.write(
                "DEFAULT omnios\n"
                "PROMPT 0\n"
                "TIMEOUT 50\n"
                "LABEL omnios\n"
                "  MENU LABEL OmniOS %s\n"
                "  LINUX /boot/omnios-bzImage\n"
                # no APPEND: the kernel's built-in command line (consoles,
                # quiet) is the same for BIOS and UEFI boots
                % VERSION)

    with open(os.path.join(stage, "README.txt"), "w") as f:
        f.write(
            "OmniOS %s — lightweight OS built from source\n\n"
            "Hybrid image: boots on UEFI firmware (the kernel is its own\n"
            "bootloader) and on BIOS firmware (ISOLINUX). The whole OS is\n"
            "embedded in the kernel as an initramfs.\n\n"
            "Write to a USB stick with:\n\n"
            "  dd if=%s of=/dev/sdX bs=16M oflag=direct status=progress\n"
            % (VERSION, ARTIFACT + ".iso"))

    # ---- EFI System Partition FAT image --------------------------------------
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
    else:                                          # pure-python route
        subprocess.run([sys.executable,
                        os.path.join(REPO, "tools", "make-fat.py"),
                        os.path.join(stage, "EFI", "BOOT", "BOOTX64.EFI"),
                        esp],
                       check=True)

    # ---- build the ISO --------------------------------------------------------
    iso = os.path.join(OUT, ARTIFACT + ".iso")

    if had("xorriso"):
        cmd = ["xorriso", "-as", "mkisofs",
               "-iso-level", "3",
               "-J", "-R",
               "-V", "OMNIOS_%s" % VERSION.replace(".", "_"),
               "-o", iso]
        if have_bios:
            # Default (BIOS) El Torito entry -> ISOLINUX.
            cmd += ["-b", "isolinux/isolinux.bin",
                    "-c", "isolinux/boot.cat",
                    "-no-emul-boot", "-boot-load-size", "4",
                    "-boot-info-table"]
        # EFI El Torito entry -> the ESP image.
        cmd += ["-eltorito-alt-boot",
                "-e", "efi.img",
                "-no-emul-boot"]
        iso_mbr = find_existing(ISOHYBRID_MBR_PATHS)
        if have_bios and iso_mbr:
            # Make the ISO dd-able to USB (BIOS boots it via the hybrid MBR).
            cmd += ["-isohybrid-mbr", iso_mbr]
        cmd += [stage]
        subprocess.run(cmd, check=True)
        bootmsg = "BIOS + UEFI" if have_bios else "UEFI"
    else:
        if not have_bios:
            print("make-iso: no xorriso/isolinux — building UEFI-only ISO "
                  "(no BIOS boot catalogue)", file=sys.stderr)
        _iso_pycdlib(iso, esp, ksrc, os.path.join(stage, "README.txt"),
                     have_bios, stage)
        bootmsg = "UEFI only"

    size = os.path.getsize(iso)
    sha = os.path.join(OUT, ARTIFACT + ".iso.sha256")
    h = hashlib.sha256(open(iso, "rb").read()).hexdigest()
    with open(sha, "w") as f:
        f.write("%s  %s\n" % (h, ARTIFACT + ".iso"))

    # ---- VMware helper ---------------------------------------------------------
    # A .vmx next to the ISO that boots it (UEFI, serial console logged to a
    # file). Drop both into VMware Workstation/Player/Fusion and power on.
    vmx = os.path.join(OUT, ARTIFACT + ".vmx")
    with open(vmx, "w") as f:
        f.write(_vmx(ARTIFACT + ".iso", VERSION))

    print("make-iso: wrote %s (%.1f MiB, %s)"
          % (iso, size / 1048576.0, bootmsg))
    print("make-iso: wrote %s" % sha)
    print("make-iso: wrote %s" % vmx)


def _vmx(iso_name, version):
    """Minimal VMware VM that boots the OmniOS ISO.

    - UEFI firmware (the ISO's EFI stub path is the most portable); the ISO
      also boots under BIOS firmware: set firmware = "bios" to try that.
    - Serial console mirrored to omnios-serial.log so boot messages are
      visible even before the framebuffer/drm driver comes up.
    """
    return (
        '.encoding = "UTF-8"\n'
        'config.version = "8"\n'
        'virtualHW.version = "19"\n'
        'displayName = "OmniOS %s"\n'
        'guestOS = "other5xlinux-64"\n'
        'firmware = "efi"\n'
        'memsize = "1024"\n'
        'numvcpus = "2"\n'
        'svga.present = "TRUE"\n'
        'svga.vramSize = "16777216"\n'
        'svga.autodetect = "FALSE"\n'
        'svga.maxWidth = "1920"\n'
        'svga.maxHeight = "1080"\n'
        'ide1:0.present = "TRUE"\n'
        'ide1:0.deviceType = "cdrom-image"\n'
        'ide1:0.fileName = "%s"\n'
        'ide1:0.startConnected = "TRUE"\n'
        # USB controller present so a physical USB mouse/keyboard can be
        # passed through (VMware's built-in virtual mouse also arrives over
        # the emulated PS/2 port, which needs no vmx setting at all).
        'usb.present = "TRUE"\n'
        # network: NAT through the host, e1000 (built into the kernel);
        # DHCP at boot, used by OmniOS Update
        'ethernet0.present = "TRUE"\n'
        'ethernet0.connectionType = "nat"\n'
        'ethernet0.virtualDev = "e1000"\n'
        'ethernet0.addressType = "generated"\n'
        'ethernet0.startConnected = "TRUE"\n'
        'serial0.present = "TRUE"\n'
        'serial0.fileType = "file"\n'
        'serial0.fileName = "omnios-serial.log"\n'
        'serial0.yieldOnMsrRead = "TRUE"\n'
        'tools.syncTime = "FALSE"\n'
    ) % (version, iso_name)


def _iso_pycdlib(iso, esp, ksrc, readme, have_bios, stage):
    """Fallback UEFI ISO build with pycdlib (pure Python)."""
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
