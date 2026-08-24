SUMMARY = "OmniOS identity and KDE Plasma branding"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
SRC_URI = " \
 file://omnios-wallpaper.svg file://wallpaper-metadata.json \
 file://99-omnios-system-partitions.rules \
 file://lookandfeel-metadata.json file://lookandfeel-defaults file://kdeglobals \
 file://sddm-omnios.conf file://sudoers-omnios file://omnios-release \
"
S = "${UNPACKDIR}"
inherit allarch
RDEPENDS:${PN} += "plasma-workspace sddm breeze sudo"
do_install() {
    install -d ${D}${datadir}/wallpapers/OmniOS/contents/images
    install -m 0644 ${S}/omnios-wallpaper.svg ${D}${datadir}/wallpapers/OmniOS/contents/images/3840x2160.svg
    install -m 0644 ${S}/wallpaper-metadata.json ${D}${datadir}/wallpapers/OmniOS/metadata.json
    install -d ${D}${datadir}/plasma/look-and-feel/org.omnios.desktop/contents
    install -m 0644 ${S}/lookandfeel-metadata.json ${D}${datadir}/plasma/look-and-feel/org.omnios.desktop/metadata.json
    install -m 0644 ${S}/lookandfeel-defaults ${D}${datadir}/plasma/look-and-feel/org.omnios.desktop/contents/defaults
    install -d ${D}${sysconfdir}/xdg
    install -m 0644 ${S}/kdeglobals ${D}${sysconfdir}/xdg/kdeglobals
    install -d ${D}${nonarch_base_libdir}/udev/rules.d
    install -m 0644 ${S}/99-omnios-system-partitions.rules ${D}${nonarch_base_libdir}/udev/rules.d/99-omnios-system-partitions.rules
    install -d ${D}${sysconfdir}/sddm.conf.d
    install -m 0644 ${S}/sddm-omnios.conf ${D}${sysconfdir}/sddm.conf.d/10-omnios.conf
    install -d ${D}${sysconfdir}/sudoers.d
    install -m 0440 ${S}/sudoers-omnios ${D}${sysconfdir}/sudoers.d/90-omnios
    install -d ${D}${sysconfdir}
    install -m 0644 ${S}/omnios-release ${D}${sysconfdir}/omnios-release
}
FILES:${PN} += "${datadir}/wallpapers/OmniOS ${datadir}/plasma/look-and-feel/org.omnios.desktop ${nonarch_base_libdir}/udev/rules.d/99-omnios-system-partitions.rules ${sysconfdir}/xdg/kdeglobals ${sysconfdir}/sddm.conf.d/10-omnios.conf ${sysconfdir}/sudoers.d/90-omnios"
CONFFILES:${PN} += "${sysconfdir}/omnios-release ${sysconfdir}/xdg/kdeglobals ${sysconfdir}/sddm.conf.d/10-omnios.conf"
