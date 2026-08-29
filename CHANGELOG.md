# Changelog

All notable changes to OmniOS Desktop are documented in this file.

## [2026.1.1] - 2026-08-29

OmniOS Desktop 2026.1.1 is a maintenance release. It corrects the operating-system identity shown on the desktop and improves how images are published, with no change to the package selection or desktop layout of 2026.1.

### Fixed

- Fixed KDE's About this System page showing the Debian swirl instead of the OmniOS logo. `os-release` now sets `LOGO=omnios-logo`, and the OmniOS ring is installed into the hicolor icon theme from 16 to 512 px plus a scalable SVG.
- Fixed the same page reporting the Debian base release, such as `OmniOS 13`, instead of the OmniOS version. An explicit `/etc/xdg/kcm-about-distrorc` now states the OmniOS name, version, and logo with `UseOSReleaseVersion=false`, so the heading survives a `base-files` upgrade.

### Build and release engineering

- Added the `omnios-desktop` package and a signed OmniOS APT repository, so OmniOS's own identity, branding and system integration changes reach installed systems through automatic updates instead of only through a new ISO.
- Added `origin=OmniOS` to the unattended-upgrades allowlist, and carried the OmniOS repository and signing key onto installed systems during Calamares installation.
- Added APT download retries and longer timeouts so a single dropped CDN connection no longer fails a build that fetches over two thousand packages.
- Added `scripts/build-vars.sh` and `scripts/verify-iso.sh`, so the ISO name, checksum filename, and release tag are derived from `version.txt` by the repository rather than repeated inside the CI workflow.
- Added a build-workflow check that fails immediately when the installed `.github/workflows/build.yml` differs from `ci/build.yml`, instead of failing later with a missing-file error for a stale version.
- Added `scripts/render-logo.py`, which generates every logo icon from source so the artwork can be restyled without editing binaries.
- Added `hicolor-icon-theme` to guarantee the icon theme directory exists.
- Extended validation to reject an `os-release` that loses the OmniOS identity, hardcodes a version, or names a logo with no installed icon.
- Added a release workflow version input, so entering `2026.1.1` publishes tag `v2026.1.1` titled `OmniOS v2026.1.1`, and releases only ever resolve an artifact built from that exact version.
- Added automatic download of the ISO from the matching build run and attachment of it to the release, split into sub-2-GiB assets, so downloads no longer require signing in to GitHub.
- Added token auto-selection, a permission preflight, and a tag fallback so releases publish with whichever configured token genuinely works.

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
- Added optional `RELEASE_TOKEN`, `GH_TOKEN` and `PAT_TOKEN` secrets so releases can be published with a personal access token when the default workflow token is read-only.
- Added actionable diagnostics when the release token is denied `contents: write`, instead of a bare HTTP 403.
- Added automatic download of the ISO from the matching build run and attachment of it to the GitHub release, so downloads no longer require signing in to GitHub.
- Added automatic splitting of the ISO into sub-2-GiB release assets, with checksums for both the rejoined image and the individual parts.
- Added a release permission preflight so a token problem fails the run before the multi-gigabyte ISO download.
- Added automatic selection of the first configured token that can genuinely publish releases, so a misconfigured personal access token no longer masks a working built-in token.
- Added a tag fallback for tokens that cannot create git refs on arbitrary commits, keeping the build commit recorded in the release notes.
- Added distinct diagnostics for personal access tokens, the built-in workflow token, and invalid credentials.

### Fixed

- Fixed the desktop showing the Debian swirl instead of the OmniOS logo in KDE's About this System page, by adding a `LOGO` key to `os-release` and installing OmniOS icons into the hicolor theme.
- Fixed the About this System page reporting the Debian base release, such as `OmniOS 13`, instead of the OmniOS version, by adding an explicit `kcm-about-distrorc` with `UseOSReleaseVersion=false`.
- Added `scripts/render-logo.py` so the logo icons are generated from source, and extended validation to reject an os-release that loses the OmniOS identity or names a missing logo.
- Corrected the Debian package selection for KDE Spectacle from `spectacle` to `kde-spectacle`.
- Fixed live-build cache linking for clean and repeatable container builds.
- Fixed live media waiting indefinitely at the boot menu during automated smoke tests.

### Compatibility and scope

- The 2026.1 image supports amd64/x86-64 PCs only.
- ARM64 and other processor architectures require separate future images.
- OmniOS Desktop is a normal consumer operating system; there is no separate developer edition.
