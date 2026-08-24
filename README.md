# OmniOS

OmniOS is a normal, installable KDE Plasma desktop operating system based on **Debian 13 Stable (trixie)**. It is built as a hybrid live ISO for 64-bit PCs and is not an Ubuntu remaster, Yocto appliance image, or development-only environment.

The live session and installed system use the same package set. Users can try the desktop from USB and install it with the Calamares graphical installer.

## Included system

- KDE Plasma desktop with Wayland and X11 support
- Calamares graphical installer
- Legacy BIOS and UEFI boot
- Debian's stable kernel, microcode, and broad PC firmware collection
- NetworkManager, Wi-Fi, VPN, WireGuard, Bluetooth, and firewalld
- PipeWire and WirePlumber audio
- Firefox ESR, LibreOffice, VLC, KDE Connect, and Flatpak
- Discover software management with Flatpak integration
- Printing and common storage/filesystem tools
- Automatic signed Debian security updates
- Optional Btrfs root filesystem with Snapper snapshots and Btrfs Assistant

The first release architecture is **amd64/x86-64**. ARM64 and other architectures require separate future images.

## Update and rollback policy

OmniOS uses Debian's APT repositories and their cryptographic signatures. `unattended-upgrades` installs Debian 13 security updates automatically and never reboots without the user's permission.

When the system is installed on Btrfs—the Calamares default—OmniOS initializes Snapper and creates paired snapshots around APT/DPKG transactions. Btrfs Assistant provides a graphical view of snapshots. A damaged installation can be recovered from a prior snapshot using the OmniOS live ISO and standard Snapper/Btrfs tools.

This preserves normal Debian package management while providing practical recovery. It deliberately avoids unsigned package-download scripts.

## Build requirements

The recommended build runs inside a privileged Debian 13 container so the host distribution does not affect live-build:

- Linux host with Docker
- 4 or more CPU cores
- 8 GiB RAM or more
- Approximately 30 GiB free disk

Build the ISO:

```bash
scripts/check-project.sh
scripts/build-container.sh
```

On a Debian 13 build host, it can also be built directly:

```bash
scripts/bootstrap-host.sh
scripts/build.sh build
```

Output:

```text
out/OmniOS-1.0-amd64.iso
out/OmniOS-1.0-amd64.iso.sha256
out/OmniOS-1.0-amd64.packages
out/OmniOS-1.0-amd64.iso-info.txt
```

The Debian package download cache is retained under `build/downloads/`. Do not use `scripts/build.sh clean --purge-cache` unless the cache itself is damaged.

## Flash and boot

**Flashing destroys the selected device. Verify its path carefully.**

```bash
sudo dd if=out/OmniOS-1.0-amd64.iso of=/dev/sdX \
  bs=16M oflag=direct status=progress conv=fsync
```

The image can also be written using KDE ISO Image Writer, GNOME Disks, Rufus, or Etcher.

The live session automatically logs in as `omnios`. During installation, Calamares asks for the permanent username, password, timezone, keyboard layout, disk layout, and encryption choices. There is no preconfigured account in the installed system.

## QEMU testing

Interactive legacy BIOS boot:

```bash
scripts/run-qemu.sh
```

Interactive UEFI boot:

```bash
QEMU_UEFI=1 scripts/run-qemu.sh
```

Automated smoke tests:

```bash
scripts/smoke-test.sh
QEMU_UEFI=1 scripts/smoke-test.sh
```

## Customization layout

```text
auto/                           reproducible live-build configuration
config/package-lists/           Debian desktop, firmware, and utility packages
config/includes.chroot/         files placed in the live and installed system
config/hooks/normal/            post-package configuration and cleanup
scripts/build.sh                direct Debian live-build wrapper
scripts/build-container.sh      recommended Debian 13 container build
scripts/check-project.sh        static validation
scripts/run-qemu.sh             interactive BIOS/UEFI test
scripts/smoke-test.sh           automated boot validation
ci/build.yml                    owner-installable build workflow
ci/release.yml                  owner-installable release workflow
```

## CI workflows

Workflow templates remain under `ci/` so repository owners can review and install them:

```bash
cp ci/build.yml .github/workflows/build.yml
cp ci/release.yml .github/workflows/release.yml
git add .github/workflows
git commit -m "ci: install Debian ISO workflows"
git push
```

The build workflow caches downloaded Debian packages, builds inside `debian:13-slim`, verifies the ISO checksum, tests BIOS and UEFI boot, and uploads the artifact. The release workflow verifies the source run and checksums, creates GitHub provenance attestations, and publishes the ISO.

## Release engineering

Before a public release:

1. build from the default branch with a monotonically increasing revision;
2. pass BIOS and UEFI smoke tests;
3. install on representative Intel and AMD hardware;
4. test Wi-Fi, Bluetooth, sound, suspend/resume, graphics, and disk encryption;
5. test a Btrfs snapshot and recovery procedure;
6. publish the ISO checksum, package manifest, and provenance attestation.

## Licensing

Project scripts and configuration are MIT licensed. The OmniOS generated artwork in this repository is released under CC0-1.0. Debian packages retain their individual upstream licenses; the generated package manifest records the ISO composition.
