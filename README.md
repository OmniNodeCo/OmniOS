# OmniOS Ubuntu Image

This repository turns an Ubuntu 24.04 LTS Desktop image into **OmniOS 1.0** using [Cubic](https://github.com/PJ-Singh-001/Cubic). Cubic handles the live filesystem, bootloader, installer, and final ISO. The scripts in this repository make package installation and branding repeatable.

## What is included

- A Cubic-safe chroot customization script
- OmniOS operating-system identity, MOTD, and GNOME wallpaper
- An editable Ubuntu package list
- A verified local Ubuntu ISO downloader that reuses its cached copy
- A GitHub Actions workflow that caches the large base ISO by SHA-256

## Host requirements

Use an Ubuntu desktop host with approximately 20 GB of free working space. Install Cubic on the host:

```bash
sudo add-apt-repository universe
sudo add-apt-repository ppa:cubic-wizard/release
sudo apt update
sudo apt install --no-install-recommends cubic
```

Do not install Cubic inside the customized image.

## Build OmniOS with Cubic

### 1. Prepare the source ISO and bundle

From this repository, run:

```bash
bash scripts/prepare-cubic.sh
```

This command:

1. resolves the latest published Ubuntu 24.04 LTS Desktop point release;
2. downloads it into `iso/` only when no valid local copy exists;
3. verifies the official SHA-256 checksum; and
4. creates `out/omnios-cubic-bundle.tar.gz`.

A later run verifies and reuses the ISO instead of downloading it again. To request a specific available point release:

```bash
UBUNTU_VERSION=24.04.4 bash scripts/prepare-cubic.sh
```

### 2. Create a Cubic project

Open Cubic:

```bash
cubic
```

Choose a new project directory and select the ISO from this repository's `iso/` directory. Continue until Cubic opens the **Terminal** page.

### 3. Run the customization inside Cubic

Cubic's Terminal is already running as root; do not use `sudo` there.

In Cubic's Terminal, change to `/tmp`:

```bash
cd /tmp
```

Use Cubic's copy button, context menu, or drag-and-drop support to copy `out/omnios-cubic-bundle.tar.gz` from the host into `/tmp`. Then run:

```bash
tar -xzf omnios-cubic-bundle.tar.gz
bash scripts/customize.sh
```

The customization script refuses to run on the host by default. Inside Cubic it will:

- install packages from `assets/packages.txt`;
- prevent package hooks from trying to start services in the chroot;
- install OmniOS release identity files;
- install the OmniOS wallpaper and GNOME defaults;
- run `assets/customize-extra.sh`; and
- clean APT data to reduce the final image size.

When it finishes, click **Next** in Cubic. Review Cubic's package and kernel selections, generate the ISO, and test both its live environment and installer in a virtual machine before using it on hardware.

## Customize OmniOS

### Package selection

Edit `assets/packages.txt`. Blank lines and `#` comments are supported. Use packages available from Ubuntu's enabled APT repositories. Ubuntu Desktop already provides Firefox as a snap, so it does not need to be installed from Cubic.

Validate the package list and assets without modifying the host:

```bash
bash scripts/customize.sh --dry-run
```

### Branding

Edit these files:

- `assets/os-release`
- `assets/lsb-release`
- `assets/omnios-release`
- `assets/motd`
- `assets/omnios-default.svg`
- `assets/90-omnios.gschema.override`

Place additional repeatable commands in `assets/customize-extra.sh`.

### Customization options

```text
--packages FILE    Use a different package list
--skip-packages    Install branding without extra packages
--skip-branding    Install packages without OmniOS branding
--keep-apt-cache   Retain APT indexes and downloaded packages
--dry-run          Validate and print the plan without changing files
```

## ISO caching

### Local cache

`scripts/download-ubuntu.sh` stores the verified ISO in `iso/`. It checks the cached file before downloading, and a `.gitignore` rule prevents the multi-gigabyte image from entering Git.

Metadata can be resolved without downloading the image:

```bash
bash scripts/download-ubuntu.sh --metadata-only
```

### GitHub Actions cache

Add `build.yml` at `.github/workflows/build.yml`. It resolves the official image checksum and uses it as the `actions/cache` key. The workflow:

- restores the exact ISO when it is already cached;
- downloads only on a cache miss;
- verifies restored and downloaded images;
- packages and uploads `omnios-cubic-bundle.tar.gz`; and
- runs periodically to keep the ISO cache active.

Run **Build OmniOS Cubic Assets** from the repository's Actions page. Scheduled workflows run from the default branch after the workflow is added there.

GitHub's Actions cache is for reuse by GitHub-hosted workflows; it does not place the ISO onto your Cubic desktop host. Use `scripts/prepare-cubic.sh` for the local copy.

## Important limitation

Cubic is an interactive desktop application and does not provide an unattended ISO-build command. GitHub Actions can cache and verify the base ISO, but the final OmniOS ISO must be generated and tested through Cubic.

## License

The repository scripts and original OmniOS assets are available under the [MIT License](LICENSE). Ubuntu and included packages retain their respective licenses and trademarks.
