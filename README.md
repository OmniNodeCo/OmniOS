# OmniOS 2026.1

OmniOS 2026.1 is a normal, installable KDE Plasma desktop operating system based on **Debian 13 Stable (trixie)**. It is built as a hybrid live ISO for 64-bit PCs and is not an Ubuntu remaster, Yocto appliance image, or development-only environment.

The live session and installed system use the same package set. Users can try the desktop from USB and install it with the Calamares graphical installer.

See [CHANGELOG.md](CHANGELOG.md) for the complete 2026.1 release changelog.

## Included system

- KDE Plasma desktop with Wayland and X11 support
- Calamares graphical installer
- Legacy BIOS and UEFI boot
- Debian's stable kernel, microcode, and broad PC firmware collection
- NetworkManager, Wi-Fi, VPN, WireGuard, Bluetooth, firewalld, and Plasma Firewall
- PipeWire and WirePlumber audio with broad GStreamer/FFmpeg media support
- Firefox ESR, LibreOffice, VLC, KDE Connect, and Flatpak with Flathub
- Complete KDE utilities for documents, archives, screenshots, storage, and file sharing
- Kup backup, Skanpage, AirScan, printing, Samba sharing, and storage diagnostics
- Laptop power profiles and Intel thermal management
- Automatic signed Debian security updates
- Optional Btrfs root filesystem with Snapper snapshots and Btrfs Assistant
- AppArmor policies, Linux auditing, hardened sysctl defaults, Lynis, and package verification
- ClamAV daemon, automatic signature updates, ClamTK, Dolphin integration, and weekly scans
- KeePassXC with browser integration, Kleopatra, and FIDO2/U2F tools
- OmniOS Security Center for antivirus, firewall, passwords, snapshots, audits, and updates

The first release architecture is **amd64/x86-64**. ARM64 and other architectures require separate future images.

## Update and rollback policy

OmniOS uses Debian's APT repositories and their cryptographic signatures. `unattended-upgrades` installs Debian 13 security updates automatically and never reboots without the user's permission.

When the system is installed on Btrfs—the Calamares default—OmniOS initializes Snapper and creates paired snapshots around APT/DPKG transactions. Btrfs Assistant provides a graphical view of snapshots. A damaged installation can be recovered from a prior snapshot using the OmniOS live ISO and standard Snapper/Btrfs tools.

This preserves normal Debian package management while providing practical recovery. It deliberately avoids unsigned package-download scripts.

## Security and antivirus

OmniOS 2026.1 enables AppArmor, auditd, firewalld, safer kernel/network defaults, Firefox tracking protection and HTTPS-only mode, automatic ClamAV definition updates, and a weekly low-priority antivirus scan while connected to AC power. Scans report suspicious files in `/var/log/omnios-antivirus.log` and never delete or quarantine user data automatically.

The **OmniOS Security Center** launches the graphical antivirus, firewall, password manager, Btrfs snapshot manager, Lynis audit, and software update interfaces. Advanced controls such as USB device blocking are not enabled by default because they could unexpectedly lock out ordinary desktop hardware.

## Versioning

`version.txt` is the single source of truth for the OmniOS version. It contains one simple value such as:

```text
2026.1
```

Changing that value updates the generated OS identity, Calamares branding, ISO volume label, ISO filename, GitHub artifact ZIP name, QEMU defaults, release tag, and nightly download link. Add a matching `CHANGELOG.md` section before publishing a new version.

Releases are tagged with a `v` prefix, so `version.txt` containing `2026.1` publishes as tag `v2026.1`.

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

With `version.txt` set to `2026.1`, the output is:

```text
out/OmniOS-2026.1-amd64.iso
out/OmniOS-2026.1-amd64.iso.sha256
out/OmniOS-2026.1-amd64.packages
out/OmniOS-2026.1-amd64.iso-info.txt
```

The Debian package download cache is retained under `build/downloads/`. Do not use `scripts/build.sh clean --purge-cache` unless the cache itself is damaged.

## Flash and boot

**Flashing destroys the selected device. Verify its path carefully.**

```bash
sudo dd if=out/OmniOS-2026.1-amd64.iso of=/dev/sdX \
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
version.txt                     canonical OmniOS version and artifact name
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

The build workflow reads `version.txt`, uses it for the ISO and artifact ZIP names, caches downloaded Debian packages, builds inside `debian:13-slim`, verifies the ISO checksum, optionally tests BIOS and UEFI boot, and publishes the nightly.link URL in the job summary and artifact metadata. Build artifacts are retained for 90 days.

### Publishing a release

Run **Release OmniOS ISO** from the Actions tab. It takes three optional inputs:

| Input | Default | Meaning |
| --- | --- | --- |
| `version` | blank | Version number to publish, for example `2026.1`. A leading `v` is accepted and stripped. Leave blank to release whatever the latest successful build produced. |
| `mark_latest` | true | Mark the release as the repository's *Latest* release. |
| `prerelease` | false | Publish as a pre-release instead. |

The version number you type selects the release, and the tag is that number prefixed with `v`:

```text
version input: 2026.1   ->  release tag: v2026.1   ->  release title: OmniOS v2026.1
```

Publishing a release requires the workflow token to have `contents: write`. The workflow declares that permission, but a repository whose **Settings → Actions → General → Workflow permissions** is set to *Read repository contents and packages permissions* caps the request, and the release step then fails with `HTTP 403: Resource not accessible by integration`. Fix it in one of two ways:

- set **Workflow permissions** to **Read and write permissions**; or
- publish with a personal access token instead. Create one that can write to this repository — fine-grained with **Contents: Read and write**, or classic with the **`repo`** scope — then save it under **Settings → Secrets and variables → Actions → New repository secret**. The workflow picks up `RELEASE_TOKEN`, `GH_TOKEN` or `PAT_TOKEN` automatically, in that order, and falls back to the built-in `GITHUB_TOKEN` when none is set.

A token secret is the better choice when you would rather not grant write access to every workflow in the repository, since only this release step uses it.

After changing either setting, start a **new** run from the Actions tab. *Re-run jobs* reuses the permission context of the original run, so it keeps failing.

The workflow then finds the newest successful `build.yml` run on the default branch **whose `version.txt` equals the requested version**, reads that build commit's changelog section, and links the release directly to its version-matched GitHub Actions artifact. If no successful build produced that version, the run fails and lists the versions that are actually available, so a release can never point at an ISO built from a different version. Releasing a version that already has a tag refreshes the existing release's title and notes in place.

## Release engineering

Before a public release:

1. update `version.txt` and add its matching `CHANGELOG.md` section;
2. build from the default branch and pass BIOS and UEFI smoke tests;
3. install on representative Intel and AMD hardware;
4. test Wi-Fi, Bluetooth, sound, suspend/resume, graphics, and disk encryption;
5. test a Btrfs snapshot and recovery procedure;
6. confirm the checksum, package manifest, and direct build-artifact download are available;
7. run **Release OmniOS ISO** and enter the version number, for example `2026.1`, to publish tag `v2026.1`.

## Licensing

Project scripts and configuration are MIT licensed. The OmniOS generated artwork in this repository is released under CC0-1.0. Debian packages retain their individual upstream licenses; the generated package manifest records the ISO composition.
