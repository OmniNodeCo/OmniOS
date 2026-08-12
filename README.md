# OmniOS

**OmniOS 1.0** is a separately branded desktop operating-system image built on the Ubuntu 24.04 LTS foundation. It keeps Ubuntu's kernel, drivers, package repositories, signed boot chain, and desktop installer while supplying its own identity, package selection, wallpaper, defaults, boot labels, and installable filesystem.

The final output is a bootable hybrid AMD64 ISO that supports a live session and installation.

## What the automated builder changes

The remaster process:

1. verifies an official Ubuntu Desktop ISO;
2. combines Ubuntu's layered install filesystem;
3. installs the OmniOS package selection in an isolated chroot;
4. applies OmniOS release identity and desktop branding;
5. creates a complete `omnios.squashfs` install image;
6. creates an `omnios.live.squashfs` live-session layer;
7. replaces Ubuntu's installer source list with **OmniOS Desktop**;
8. changes the ISO and GRUB labels to OmniOS;
9. regenerates manifests and checksums; and
10. replays Ubuntu's original BIOS/UEFI boot configuration into the new ISO.

The builder removes the original `minimal.*` filesystem variants from the output. This ensures the installer deploys the customized OmniOS filesystem instead of offering an unmodified Ubuntu installation.

## OmniOS customizations

- Operating-system ID: `omnios`
- Product name: `OmniOS 1.0`
- Ubuntu compatibility declaration: `ID_LIKE="ubuntu debian"`
- Custom MOTD, console identity, GRUB distributor, and release metadata
- Custom GNOME wallpaper and background defaults
- Additional packages from `assets/packages.txt`
- Optional project commands in `assets/customize-extra.sh`

Ubuntu remains the upstream package and hardware-enablement foundation. Do not replace the Ubuntu APT sources unless you operate and sign your own compatible package repositories.

## Automated GitHub Actions build

Add the prepared workflow as:

```text
.github/workflows/build.yml
```

Run **Build OmniOS ISO** from the Actions page. The workflow:

- resolves the current Ubuntu 24.04 LTS point release;
- caches the verified Ubuntu ISO using its official SHA-256 digest;
- downloads the base ISO only on a cache miss;
- frees enough GitHub runner space for the remaster;
- builds and validates the bootable OmniOS ISO; and
- uploads `OmniOS-1.0-amd64.iso` with its SHA-256 file.

Scheduled runs refresh only the Ubuntu cache. They do not perform the expensive final ISO build.

The downloadable artifact is named:

```text
OmniOS-1.0-amd64
```

## Build locally without Cubic

### Requirements

Use an x86-64 Ubuntu host with at least 35 GB of free disk space. Install the build tools:

```bash
sudo apt update
sudo apt install squashfs-tools xorriso
```

### 1. Download and verify Ubuntu

```bash
bash scripts/download-ubuntu.sh
```

The downloader stores the ISO under `iso/`. Future runs verify and reuse that copy. To request a particular still-published point release:

```bash
UBUNTU_VERSION=24.04.4 bash scripts/download-ubuntu.sh
```

### 2. Build OmniOS

```bash
sudo bash scripts/build-iso.sh
```

Or specify all paths:

```bash
sudo bash scripts/build-iso.sh \
  --source iso/ubuntu-24.04.4-desktop-amd64.iso \
  --output out/OmniOS-1.0-amd64.iso
```

The output directory will contain:

```text
out/OmniOS-1.0-amd64.iso
out/OmniOS-1.0-amd64.iso.sha256
```

Verify the image:

```bash
cd out
sha256sum --check OmniOS-1.0-amd64.iso.sha256
```

Use `--keep-work` to preserve extracted layers for debugging. Control SquashFS parallelism with `SQUASHFS_PROCESSORS`; the default is capped at four workers to avoid exhausting memory.

## Optional Cubic build

Cubic remains available as an interactive alternative. It is not used by the automated ISO builder.

Install Cubic on an Ubuntu desktop host:

```bash
sudo add-apt-repository universe
sudo add-apt-repository ppa:cubic-wizard/release
sudo apt update
sudo apt install --no-install-recommends cubic
```

Prepare the verified Ubuntu source and Cubic bundle:

```bash
bash scripts/prepare-cubic.sh
```

Open Cubic, select the ISO under `iso/`, and continue to Cubic's Terminal page. In that terminal:

```bash
cd /tmp
# Copy out/omnios-cubic-bundle.tar.gz here using Cubic's copy button.
tar -xzf omnios-cubic-bundle.tar.gz
bash scripts/customize.sh
```

Then complete and test the image through Cubic's GUI.

## Customize the package set

Edit `assets/packages.txt`. Blank lines and `#` comments are accepted. Package names must exist in Ubuntu's enabled repositories.

Check the customization plan without changing the host:

```bash
bash scripts/customize.sh --dry-run
```

The script deliberately refuses to perform a real customization outside a chroot.

## Customize branding

The main identity and desktop files are:

- `assets/os-release`
- `assets/lsb-release`
- `assets/omnios-release`
- `assets/issue`
- `assets/motd`
- `assets/99-omnios-grub.cfg`
- `assets/omnios-default.svg`
- `assets/90-omnios.gschema.override`
- `assets/omnios-wallpapers.xml`

Add further repeatable commands to `assets/customize-extra.sh`.

## Testing before release

A successful build verifies the ISO tree and El Torito boot records, but it does not prove that every firmware or installer path works. Before distributing a release:

1. verify its SHA-256 checksum;
2. boot the ISO in both UEFI and legacy BIOS virtual machines;
3. test the live desktop;
4. perform a complete installation onto an empty virtual disk;
5. reboot into the installed system; and
6. confirm networking, graphics, audio, package updates, and the OmniOS identity.

Keep Secure Boot enabled for one UEFI test because the image reuses Ubuntu's signed boot components.

## License

The repository scripts and original OmniOS assets are available under the [MIT License](LICENSE). Ubuntu, its boot components, and included packages retain their own licenses and trademarks.
