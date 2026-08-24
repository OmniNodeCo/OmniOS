SUMMARY = "OmniOS base desktop, hardware, and administration packages"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
inherit packagegroup features_check
REQUIRED_DISTRO_FEATURES = "systemd wayland opengl pam polkit"
PACKAGE_ARCH = "${MACHINE_ARCH}"
RDEPENDS:${PN} = " \
    packagegroup-core-full-cmdline \
    kernel-modules linux-firmware \
    networkmanager networkmanager-nmcli firewalld nftables wpa-supplicant \
    bluez5 pipewire wireplumber alsa-utils \
    mesa vulkan-loader vulkan-tools polkit dbus sudo bash coreutils util-linux procps \
    dnf rpm grub-editenv curl wget ca-certificates \
    pciutils usbutils ethtool iw \
    e2fsprogs e2fsprogs-resize2fs e2fsprogs-tune2fs dosfstools \
    btrfs-tools xfsprogs lvm2 cryptsetup mdadm nvme-cli smartmontools fwupd \
"
