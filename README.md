# OmniOS Ubuntu Custom ISO

Build an Ubuntu-based OmniOS image with [Cubic](https://github.com/PJ-Singh-001/Cubic). Cubic safely handles the modern Ubuntu live filesystem, boot metadata, and ISO repackaging; this repository supplies a repeatable customization script for Cubic's Terminal environment.

## Requirements

Use an Ubuntu desktop host with enough free space for Cubic's project and the generated ISO (approximately 20 GB is recommended).

Install Cubic on the **host**, not inside the customized image:

```bash
sudo add-apt-repository universe
sudo add-apt-repository ppa:cubic-wizard/release
sudo apt update
sudo apt install --no-install-recommends cubic
```

## Build with Cubic

### 1. Get an Ubuntu ISO

Download an Ubuntu Desktop ISO yourself, or download and SHA-256 verify the latest published Ubuntu 24.04 LTS point release:

```bash
bash scripts/download-ubuntu.sh
```

The image is saved under `iso/`. Set `UBUNTU_VERSION` only when a specific point release is required, for example:

```bash
UBUNTU_VERSION=24.04.4 bash scripts/download-ubuntu.sh
```

### 2. Create the Cubic project

Run `cubic` from the applications menu or terminal. Choose a new project directory, select the Ubuntu Desktop ISO, confirm the image details, and continue until Cubic displays its **Terminal** page.

Cubic's Terminal is already logged in as root. Do not use `sudo` there.

### 3. Copy and run the customization

In Cubic's Terminal, prepare a temporary destination:

```bash
mkdir -p /tmp/omnios
cd /tmp/omnios
```

From the host file manager, copy or drag both repository directories below onto Cubic's Terminal window, then approve the copy:

- `scripts/`
- `assets/`

The resulting paths must be `/tmp/omnios/scripts/customize.sh` and `/tmp/omnios/assets/packages.txt`. Run:

```bash
chmod +x scripts/customize.sh assets/customize-extra.sh
bash scripts/customize.sh
```

The script:

- refuses to modify the host OS accidentally;
- installs every Debian package in `assets/packages.txt`;
- prevents package post-install hooks from trying to start services in Cubic;
- runs `assets/customize-extra.sh` to install files such as the MOTD; and
- removes downloaded APT caches to keep the ISO smaller.

After it reports success, click **Next** in Cubic. Review the package and kernel pages, generate the ISO, and use Cubic's test page (or a separate virtual machine) to test both the live session and installation.

You can also launch the Cubic GUI through the repository helper:

```bash
bash scripts/build-iso.sh
```

## Customize the image

### Packages

Edit `assets/packages.txt`. Blank lines and `#` comments are accepted. Add Ubuntu repository package names only. Firefox is not listed because Ubuntu Desktop already ships its snap, while snaps cannot be installed from Cubic's Terminal environment.

### Files and settings

Add repeatable file or configuration changes to `assets/customize-extra.sh`. Resolve bundled files from `OMNIOS_ASSETS_DIR`, as the MOTD example does, so the script works regardless of Cubic's current directory.

### Script options

```text
--packages FILE       Use another package list
--skip-packages       Apply only file/configuration changes
--skip-extra          Install packages without running the extra script
--keep-apt-cache      Keep APT indexes and archives in the image
--dry-run             Validate and display the plan without changing anything
```

Validate changes safely on the host:

```bash
bash scripts/customize.sh --dry-run
bash -n scripts/*.sh assets/*.sh
```

## Automation note

Cubic is a GUI and does not expose an unattended ISO build interface. GitHub Actions therefore validates the scripts and dry-run plan but does not pretend to generate an ISO. ISO generation and boot/install testing are completed through Cubic.
