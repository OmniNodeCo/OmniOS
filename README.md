# OmniOS 2026.1.1

OmniOS 2026.1.1 is a normal, installable KDE Plasma desktop operating system based on **Debian 13 Stable (trixie)**. It is built as a hybrid live ISO for 64-bit PCs and is not an Ubuntu remaster, Yocto appliance image, or development-only environment.

The live session and installed system use the same package set. Users can try the desktop from USB and install it with the Calamares graphical installer.

See [CHANGELOG.md](CHANGELOG.md) for the complete 2026.1.1 release changelog.

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

OmniOS 2026.1.1 enables AppArmor, auditd, firewalld, safer kernel/network defaults, Firefox tracking protection and HTTPS-only mode, automatic ClamAV definition updates, and a weekly low-priority antivirus scan while connected to AC power. Scans report suspicious files in `/var/log/omnios-antivirus.log` and never delete or quarantine user data automatically.

The **OmniOS Security Center** launches the graphical antivirus, firewall, password manager, Btrfs snapshot manager, Lynis audit, and software update interfaces. Advanced controls such as USB device blocking are not enabled by default because they could unexpectedly lock out ordinary desktop hardware.

## Versioning

`version.txt` is the single source of truth for the OmniOS version. It contains one simple value such as:

```text
2026.1.1
```

Changing that value updates the generated OS identity, Calamares branding, ISO volume label, ISO filename, GitHub artifact ZIP name, QEMU defaults, release tag, and nightly download link. Add a matching `CHANGELOG.md` section before publishing a new version.

Releases are tagged with a `v` prefix, so `version.txt` containing `2026.1.1` publishes as tag `v2026.1.1`.

Every version-derived name is produced by one script, so nothing in the build or CI has to repeat it:

```bash
$ scripts/build-vars.sh
OMNIOS_VERSION=2026.1.1
OMNIOS_ARCH=amd64
OMNIOS_ARTIFACT=OmniOS-2026.1.1-amd64
OMNIOS_ISO=OmniOS-2026.1.1-amd64.iso
OMNIOS_ISO_SHA256=OmniOS-2026.1.1-amd64.iso.sha256
OMNIOS_RELEASE_TAG=v2026.1.1
```

The build workflow calls `scripts/build-vars.sh --github-env` to export these, and `scripts/verify-iso.sh` to check the ISO against its checksum. Because both live in the repository rather than inside the workflow YAML, changing `version.txt` is genuinely the only edit needed to build a new version.

## Desktop identity and logo

OmniOS replaces the Debian identity everywhere the desktop shows it. `config/includes.chroot/usr/share/omnios/identity/os-release` is installed over `/usr/lib/os-release` through a `dpkg-divert`, so a future `base-files` upgrade cannot restore the Debian name, version, or artwork.

Two details make KDE's **System Settings → About this System** page show OmniOS rather than the Debian swirl and the Debian release number:

- `LOGO=omnios-logo` in `os-release`, resolved against the icons in `config/includes.chroot/usr/share/icons/hicolor/`. Without a `LOGO` key and a matching installed icon, KDE falls back to the Debian logo.
- `/etc/xdg/kcm-about-distrorc`, which states the OmniOS name, version, and logo path explicitly and sets `UseOSReleaseVersion=false`.

The logo is generated from source rather than committed as an opaque binary:

```bash
scripts/render-logo.py
```

That writes the OmniOS ring in 16–512 px PNGs plus a scalable SVG. Edit the colours or geometry at the top of the script and re-run it to restyle every icon at once. `scripts/check-project.sh` verifies that `os-release` still identifies OmniOS, that `LOGO` names an icon that actually exists, and that each rendered PNG is valid.

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

With `version.txt` set to `2026.1.1`, the output is:

```text
out/OmniOS-2026.1.1-amd64.iso
out/OmniOS-2026.1.1-amd64.iso.sha256
out/OmniOS-2026.1.1-amd64.packages
out/OmniOS-2026.1.1-amd64.iso-info.txt
```

The Debian package download cache is retained under `build/downloads/`. Do not use `scripts/build.sh clean --purge-cache` unless the cache itself is damaged.

## Flash and boot

**Flashing destroys the selected device. Verify its path carefully.**

```bash
sudo dd if=out/OmniOS-2026.1.1-amd64.iso of=/dev/sdX \
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
scripts/build-vars.sh           version-derived names shared with CI
scripts/verify-iso.sh           checksum verification of the built ISO
scripts/render-logo.py          regenerates the OmniOS logo icons
scripts/check-project.sh        static validation
scripts/run-qemu.sh             interactive BIOS/UEFI test
scripts/smoke-test.sh           automated boot validation
packaging/omnios-desktop/       Debian packaging for the OmniOS files
ci/build.yml                    owner-installable build workflow
ci/release.yml                  owner-installable release workflow
ci/package.yml                  owner-installable APT repository workflow
ci/sign.yml                     owner-installable signing-key workflow
```

## CI workflows

Workflow templates remain under `ci/` so repository owners can review and install them:

```bash
cp ci/build.yml .github/workflows/build.yml
cp ci/release.yml .github/workflows/release.yml
cp ci/package.yml .github/workflows/package.yml
cp ci/sign.yml .github/workflows/sign.yml
git add .github/workflows
git commit -m "ci: install Debian ISO workflows"
git push
```

The build workflow reads `version.txt`, uses it for the ISO and artifact ZIP names, caches downloaded Debian packages, builds inside `debian:13-slim`, verifies the ISO checksum, optionally tests BIOS and UEFI boot, and publishes the nightly.link URL in the job summary and artifact metadata. Build artifacts are retained for 90 days.

### Publishing a release

Run **Release OmniOS ISO** from the Actions tab. It takes three optional inputs:

| Input | Default | Meaning |
| --- | --- | --- |
| `version` | blank | Version number to publish, for example `2026.1.1`. A leading `v` is accepted and stripped. Leave blank to release whatever the latest successful build produced. |
| `mark_latest` | true | Mark the release as the repository's *Latest* release. |
| `prerelease` | false | Publish as a pre-release instead. |

The version number you type selects the release, and the tag is that number prefixed with `v`:

```text
version input: 2026.1.1   ->  release tag: v2026.1.1   ->  release title: OmniOS v2026.1.1
```

Publishing a release requires the workflow token to have `contents: write`. The workflow declares that permission, but a repository whose **Settings → Actions → General → Workflow permissions** is set to *Read repository contents and packages permissions* caps the request, and the release step then fails with `HTTP 403: Resource not accessible by integration`. Fix it in one of two ways:

- set **Workflow permissions** to **Read and write permissions**; or
- publish with a personal access token instead. Create one that can write to this repository — fine-grained with **Contents: Read and write**, or classic with the **`repo`** scope — then save it under **Settings → Secrets and variables → Actions → New repository secret** as `RELEASE_TOKEN`, `GH_TOKEN` or `PAT_TOKEN`.

Either route works on its own; you do not need both. Setting **Workflow permissions** to read and write is the simpler of the two, because it needs no secret to create, store, or rotate.

The workflow does not blindly trust whichever token is configured. Before downloading anything it **tries every available token in turn** — `RELEASE_TOKEN`, `GH_TOKEN`, `PAT_TOKEN`, then the built-in `GITHUB_TOKEN` — by creating and immediately deleting a throwaway draft release, and uses the first one that genuinely works. A misconfigured PAT therefore no longer masks a perfectly good built-in token; it is reported and skipped. If every token is rejected, the run fails in seconds with a per-token reason.

A token with `Contents: Read and write` can publish releases, but creating a **tag on a commit that is not the current branch head** additionally requires permission to create git refs. When the token lacks that, the workflow logs a warning and creates the tag at the default branch head instead; the exact build commit is always recorded in the release notes, so the ISO's provenance is never lost. The release is verified before the multi-gigabyte download starts, so a permission problem fails the run in seconds rather than after the ISO transfer.

After changing either setting, start a **new** run from the Actions tab. *Re-run jobs* reuses the permission context of the original run, so it keeps failing.

The workflow then finds the newest successful `build.yml` run on the default branch **whose `version.txt` equals the requested version**, reads that build commit's changelog section, downloads that run's ISO artifact, and attaches the ISO to the release. If no successful build produced that version, the run fails and lists the versions that are actually available, so a release can never publish an ISO built from a different version. Releasing a version that already has a tag refreshes the existing release's title, notes, and assets in place.

### Split ISO assets

A single GitHub release asset must stay under **2 GiB**, and the OmniOS ISO is larger than that, so the workflow attaches it as split parts and publishes the checksums alongside them:

```text
OmniOS-2026.1.1-amd64.iso.part00      first 1900 MiB
OmniOS-2026.1.1-amd64.iso.part01      remainder
OmniOS-2026.1.1-amd64.iso.sha256      checksum of the rejoined ISO
OmniOS-2026.1.1-amd64.iso-parts.sha256  checksums of the individual parts
```

Download every part into one folder and rejoin them:

```bash
cat OmniOS-2026.1.1-amd64.iso.part* > OmniOS-2026.1.1-amd64.iso
sha256sum --check OmniOS-2026.1.1-amd64.iso.sha256
```

The parts checksum file is deliberately named `.iso-parts.sha256` rather than `.iso.parts.sha256` so that it does **not** match the `*.iso.part*` glob used to rejoin the image. If the ISO ever drops below 2 GiB, the workflow attaches it as a single `.iso` file instead, with no further action required.

## Updating installed systems

Automatic updates cover two different things, and they arrive by different routes.

**Debian packages** — Firefox, KDE, the kernel, security fixes — are installed automatically by `unattended-upgrades` from Debian's own archives. Nothing extra is required.

**OmniOS's own changes** — the identity, logo, wallpaper, Plasma defaults, Security Center, and the antivirus and snapshot helpers — are not Debian packages. They are shipped in the `omnios-desktop` package, built from exactly the same `config/includes.chroot` tree the ISO is built from:

```bash
scripts/build-package.sh    # -> out/omnios-desktop_<version>_all.deb
scripts/build-apt-repo.sh   # -> out/repo, a signed APT repository
```

Installed systems carry `/etc/apt/sources.list.d/omnios.sources`, and `51omnios-unattended-upgrades` allows the `origin=OmniOS` archive, so a published `omnios-desktop` upgrade installs itself the same way a Debian security update does. Without this, an OmniOS change would only reach people who reinstall from a newer ISO.

### One-time signing key setup

APT rejects an unsigned repository, so OmniOS needs a signing key. The **Create OmniOS signing key** workflow generates one for you; you do not need GPG installed locally.

First save a transport passphrase, which is used to encrypt the key on its way out of the runner:

- **Settings → Secrets and variables → Actions → New repository secret**
- Name `OMNIOS_KEY_PASSPHRASE`, value a long random passphrase. Keep a copy in your password manager.

Then run **Create OmniOS signing key**. It generates the key, encrypts the private half with that passphrase, and uploads it as the `omnios-signing-key` artifact. It also tries to store the two release secrets for you automatically, in which case there is nothing further to do.

If it could not store them, the run summary prints exact instructions. In short: download and unzip the artifact, then

```bash
gpg --decrypt OMNIOS_GPG_PRIVATE_KEY.asc.gpg > private-key.asc
```

and add two repository secrets:

| Secret | Value |
| --- | --- |
| `OMNIOS_GPG_KEY_ID` | the fingerprint printed in the run summary |
| `OMNIOS_GPG_PRIVATE_KEY` | the entire contents of `private-key.asc` |

Then `shred -u private-key.asc`. Finally, commit the artifact's `omnios-archive-keyring.asc` to `config/includes.chroot/usr/share/keyrings/`, so installed systems can verify what they download. That file is a public key and is not secret.

The private key is never written to a log, and the artifact is encrypted, so a leaked artifact is useless without the passphrase.

### Publishing the repository

**Build OmniOS APT repository** builds the package, verifies with a real `apt-get update` that APT resolves the expected version, and deploys to GitHub Pages. It runs automatically whenever `version.txt`, `config/includes.chroot/`, or the packaging changes, and can also be run by hand.

Enable **Settings → Pages → Source: GitHub Actions** once. Until the signing secrets exist, the repository is still built and attached as an artifact, but publishing is skipped and the run warns that installed systems cannot use an unsigned repository.

### Shipping a change

1. edit the files under `config/includes.chroot/`;
2. bump `version.txt` and add its `CHANGELOG.md` section;
3. run **Build OmniOS ISO**, then **Release OmniOS ISO**.

New installs get the change from the ISO, and existing installs get it from the APT repository within a day, on the normal `unattended-upgrades` schedule. `scripts/check-project.sh` verifies that every path the package claims to ship exists in the ISO tree, so the two cannot drift apart.

## Release engineering

Before a public release:

1. update `version.txt` and add its matching `CHANGELOG.md` section;
2. build from the default branch and pass BIOS and UEFI smoke tests;
3. install on representative Intel and AMD hardware;
4. test Wi-Fi, Bluetooth, sound, suspend/resume, graphics, and disk encryption;
5. test a Btrfs snapshot and recovery procedure;
6. confirm the checksum, package manifest, and direct build-artifact download are available;
7. run **Release OmniOS ISO** and enter the version number, for example `2026.1.1`, to publish tag `v2026.1.1`.

## Licensing

Project scripts and configuration are MIT licensed. The OmniOS generated artwork in this repository is released under CC0-1.0. Debian packages retain their individual upstream licenses; the generated package manifest records the ISO composition.
