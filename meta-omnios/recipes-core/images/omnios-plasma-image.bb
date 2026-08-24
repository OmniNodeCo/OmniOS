SUMMARY = "OmniOS Plasma desktop image"
DESCRIPTION = "Complete x86-64 KDE Plasma desktop with firmware, drivers, diagnostics, and signed atomic updates."
LICENSE = "MIT"

inherit core-image extrausers
IMAGE_FEATURES += "splash package-management"

IMAGE_INSTALL = " \
    packagegroup-core-boot \
    packagegroup-omnios-base \
    packagegroup-plasma-desktop-workspace \
    sddm \
    sddm-config-plasma-desktop \
    dolphin \
    konsole \
    okular \
    gwenview \
    elisa \
    angelfish \
    kdeconnect-kde \
    ttf-noto \
    ttf-noto-emoji-color \
    hack-font \
    omnios-branding \
    omnios-persistent-data \
    omnios-rauc-conf \
    omnios-update-agent \
    rauc \
    rauc-mark-good \
"

IMAGE_LINGUAS = "en-us"
IMAGE_FSTYPES = "ext4 wic.zst wic.bmap"
IMAGE_ROOTFS_SIZE = "12582912"
IMAGE_ROOTFS_EXTRA_SPACE = "1048576"
EXTRA_IMAGECMD:ext4 = "-L omnios-root-a"
WKS_FILE = "omnios-x86_64.wks.in"
WKS_FILE_DEPENDS += "omnios-grubconf"

# Development login only. Replace in a private production layer.
OMNIOS_DEV_PASSWORD = "\$6\$omniosdev\$vnNfXECU.O9fbXm96tB0jQdXXWJJazH.1tT8EqlPZsMV10ruCUfUiy3L4/PDjY8PwSBh9uVeOr.4HW2VzyQ.1/"
EXTRA_USERS_PARAMS = "useradd -p '${OMNIOS_DEV_PASSWORD}' -m -s /bin/bash -G audio,video,input,render,wheel omnios;"
