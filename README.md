# OmniOS

OmniOS is an independent Linux operating system built with OpenEmbedded and the Yocto Project. It is **not** an Ubuntu remaster or ISO-customization wrapper.

The first target is a general-purpose **64-bit x86 PC** (`genericx86-64`) with KDE Plasma. Future ARM64, x86-32, and board targets will have separate machine configurations and release images rather than one unreliable “universal” image.

> **Development status:** the reproducible build metadata, desktop image, hybrid boot layout, branding, and signed A/B update path are implemented. Hardware qualification and production signing remain release-engineering responsibilities.

## Included system

- KDE Plasma 6 with Wayland and X11 compatibility, SDDM, and OmniOS branding
- NetworkManager, Wi-Fi, Ethernet, IPv6, Bluetooth, firewalld, and nftables
- PipeWire/WirePlumber audio
- Mesa OpenGL/Vulkan and common Intel, AMD, Nouveau, and virtual GPU drivers
- Common Ethernet/Wi-Fi, USB, input, webcam, SATA, NVMe, filesystems, encryption, TPM, and virtualization support
- The generic x86 BSP's complete `linux-firmware` recommendation
- RPM/DNF package management
- Hybrid GRUB boot for UEFI and legacy BIOS
- Signed RAUC A/B system updates with trial boot and rollback

The release artifact is a directly flashable compressed GPT disk image (`.wic.zst`) plus `.wic.bmap`, not a live ISO. The fixed A/B layout requires at least a 40 GiB target drive. A signed `.raucb` bundle is produced from the same root filesystem.

## Reproducible source baseline

[`kas/omnios-x86_64.yml`](kas/omnios-x86_64.yml) pins full commits for Yocto Project 6.0.2 LTS (Wrynose), BitBake, OE-Core, meta-yocto, meta-openembedded, Qt 6.10, KDE Frameworks/Plasma, and RAUC. Yocto produces package, license, build-history, and SPDX metadata.

## Build

Use a recent Linux host with at least 16 GiB RAM, 4 CPU cores, and approximately 150 GiB free disk:

```bash
scripts/bootstrap-host.sh
source .venv/bin/activate
scripts/generate-rauc-keys.sh   # local development key only
scripts/build.sh build
```

Useful commands:

```bash
scripts/build.sh checkout
scripts/build.sh parse
scripts/build.sh feed
scripts/build.sh shell
scripts/build.sh clean
scripts/check-layer.sh
```

Artifacts are written under `build/tmp/deploy/images/genericx86-64/`, including:

- `omnios-plasma-image-genericx86-64.wic.zst`
- `omnios-plasma-image-genericx86-64.wic.bmap`
- `omnios-update-bundle-genericx86-64.raucb`

Build downloads and state remain ignored under `build/`. OmniOS accepts OE-Core's `commercial` flag for FFmpeg; distributors remain responsible for codec patent and regional compliance.

## Flash and boot

**Flashing destroys the selected device. Verify its path carefully.**

```bash
sudo bmaptool copy build/tmp/deploy/images/genericx86-64/omnios-plasma-image-genericx86-64.wic.zst /dev/sdX
```

Or:

```bash
zstd -dc path/to/image.wic.zst | sudo dd of=/dev/sdX bs=16M oflag=direct status=progress conv=fsync
```

The development image automatically logs in as `omnios`; its initial password is `omnios`. Replace the password hash and disable SDDM autologin in a private production layer before distribution.

QEMU testing:

```bash
scripts/run-qemu.sh                    # legacy BIOS
QEMU_UEFI=1 scripts/run-qemu.sh        # UEFI with OVMF
```

## Atomic updates and rollback

The GPT disk has hybrid boot, a writable GRUB environment, root slots A/B, and persistent data. `/home`, a persistent `/etc` overlay, update state, and RAUC status survive slot changes.

RAUC `verity` bundles are signed. The disabled-by-default update timer downloads only over HTTPS, limits download size, verifies the complete bundle against the image trust anchor, rejects bundles below the embedded semantic-version floor, and writes only the inactive root slot. GRUB gives a new slot one trial; `rauc-mark-good` confirms a healthy userspace boot. An unconfirmed slot is skipped at the next restart.

To operate a feed:

1. protect production signing keys as described in [`keys/README.md`](keys/README.md);
2. increment `OMNIOS_RELEASE_REVISION` for every published build;
3. publish the `.raucb` over HTTPS;
4. set `BUNDLE_URL` and `ENABLED=1` in `/etc/omnios/update.conf`;
5. choose whether `AUTO_REBOOT=1` is suitable;
6. qualify installation, power-loss handling, trial confirmation, and rollback on real hardware.

Manual installation uses the same verification path:

```bash
sudo rauc info OmniOS-update.raucb
sudo rauc install OmniOS-update.raucb
sudo systemctl reboot
```

DNF feeds may supplement applications, but base OS releases use RAUC to avoid partial upgrades.

## CI template

[`ci/build.yml`](ci/build.yml) is deliberately outside `.github/workflows/` so repository owners can review and install it themselves:

```bash
cp ci/build.yml .github/workflows/build.yml
```

It validates metadata, builds the image and signed bundle, generates feed metadata, smoke-tests BIOS and UEFI boot, and uploads artifacts. Replace its ephemeral CI signer with protected infrastructure for public releases.

## Layout

```text
kas/                         pinned x86-64 manifest
meta-omnios/conf/            distribution and layer policy
meta-omnios/recipes-core/    image, branding, persistence, and updates
meta-omnios/recipes-bsp/     hybrid GRUB A/B boot
meta-omnios/recipes-kernel/  broad PC kernel policy
meta-omnios/files/wic/       GPT A/B layout
scripts/                     setup, validation, build, keys, and QEMU
ci/                          owner-installable workflow template
keys/                        production signing guidance
```

## License

Project-owned metadata and scripts are MIT licensed. Built images contain independently licensed software; consult generated license and SPDX manifests.
