# Changelog

All notable changes to OmniOS Desktop are documented in this file.

## [2026.1] - 2026-08-24

OmniOS Desktop 2026.1 is the first consumer desktop release in the year-based release series. It replaces the former appliance-oriented image with a conventional live and installable PC operating system.

### Highlights

- Based on Debian 13 Stable (`trixie`) for amd64/x86-64 PCs.
- KDE Plasma desktop with Wayland and X11 sessions.
- Hybrid live ISO supporting legacy BIOS and UEFI boot.
- Calamares graphical installer with Btrfs as the recommended root filesystem.
- Standard APT and Discover software management with automatic signed security updates.
- Desktop-safe hardening, graphical security tools, and non-destructive antivirus protection.

### Desktop and applications

- Added Firefox ESR, LibreOffice, VLC, KDE Connect, Kate, Okular, Gwenview, Ark, Filelight, Calculator, Spectacle, and KDE Partition Manager.
- Added Flatpak and automatic one-time Flathub configuration on installed systems.
- Added PipeWire, WirePlumber, FFmpeg, and broad GStreamer multimedia support.
- Added printing, Skanpage document scanning, AirScan network-scanner support, and common printer drivers.
- Added Dolphin Samba sharing, SMB client tools, Bluetooth, VPN, WireGuard, and KDE network integration.
- Added Kup backup, Btrfs Assistant, storage diagnostics, firmware updates, and laptop power-management tools.
- Added broad PC firmware, AMD and Intel microcode, Wi-Fi firmware, and storage/filesystem utilities.

### Security and privacy

- Enabled AppArmor policy enforcement and Linux audit services.
- Added firewalld with Plasma Firewall and a consumer-safe default public zone.
- Added conservative kernel, filesystem, and network hardening defaults.
- Added Firefox policies that disable telemetry and studies, enable tracking protection, and prefer HTTPS-only browsing.
- Added KeePassXC with browser integration, Kleopatra, FIDO2/U2F tools, Lynis, and package-integrity verification.
- Added the OmniOS Security Center for convenient access to antivirus, firewall, passwords, snapshots, audits, and software updates.
- Avoided disruptive server-style defaults such as automatic USB device blocking.

### Antivirus

- Added ClamAV daemon scanning and automatic signed definition updates through FreshClam.
- Added ClamTk graphical scanning with Dolphin file-manager integration.
- Added a weekly low-priority scan of user home directories while connected to AC power.
- Made scheduled scans non-destructive: detections are logged but files are never automatically deleted or quarantined.
- Added private persistent scan logs and a detection marker for administrator review.
- Disabled scheduled antivirus scans while running from live media.

### Updates and recovery

- Added unattended installation of signed Debian security updates without forced automatic restarts.
- Added Snapper initialization when OmniOS is installed on Btrfs.
- Added snapshots around APT and DPKG transactions for practical package-update recovery.
- Added graphical snapshot management through Btrfs Assistant.
- Retained conventional Debian package management and recovery from the OmniOS live ISO.

### Installation and identity

- Added a live KDE session with automatic login as the temporary `omnios` user.
- Added Calamares installation flows for user accounts, locale, keyboard, timezone, storage, and encryption choices.
- Added OmniOS wallpaper, boot artwork, installer branding, operating-system identity, and login text.
- Added first-boot setup for firewall defaults, Btrfs snapshots, Flathub, and cleanup of live-session launchers.
- Standardized release identity and artifact names on `2026.1`.

### Build and release engineering

- Replaced the former Yocto appliance build with Debian `live-build`.
- Added a reproducible privileged Debian 13 container builder.
- Added `version.txt` as the single source for OS identity, ISO labels, filenames, artifact ZIPs, release tags, and nightly links.
- Added package-list, shell, JSON, XML, desktop-file, duplicate, and private-key validation.
- Added APT metadata preflight validation for all explicitly selected Debian packages.
- Added automatic ISO boot-menu timeouts for unattended BIOS and UEFI testing.
- Added optional QEMU BIOS and UEFI smoke tests in the build workflow template.
- Added SHA-256 checksums, package manifests, ISO metadata, release metadata, and versioned artifacts.
- Added workflow templates for cached ISO builds, artifact upload, and GitHub releases sourced from the latest successful build.
- Added nightly links to build summaries and metadata, while releases resolve the newest successful `build.yml` artifact directly through GitHub Actions.
- Added a release workflow version input: entering `2026.1` publishes the release as tag `v2026.1`, titled `OmniOS v2026.1`.
- Added release selection by version number, so a release only ever links to an artifact built from that exact `version.txt`.
- Added `mark_latest` and `prerelease` release options and a release job summary.

### Fixed

- Corrected the Debian package selection for KDE Spectacle from `spectacle` to `kde-spectacle`.
- Fixed live-build cache linking for clean and repeatable container builds.
- Fixed live media waiting indefinitely at the boot menu during automated smoke tests.

### Compatibility and scope

- The 2026.1 image supports amd64/x86-64 PCs only.
- ARM64 and other processor architectures require separate future images.
- OmniOS Desktop is a normal consumer operating system; there is no separate developer edition.
