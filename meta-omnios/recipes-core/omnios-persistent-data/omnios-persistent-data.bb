SUMMARY = "Initialize OmniOS persistent user and system data"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
SRC_URI = "file://omnios-persistent-data file://omnios-persistent-data.service"
S = "${UNPACKDIR}"
inherit allarch systemd
RDEPENDS:${PN} = "bash coreutils"
SYSTEMD_SERVICE:${PN} = "omnios-persistent-data.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"
do_install() {
    install -d ${D}${libexecdir} ${D}${systemd_system_unitdir}
    install -m 0755 ${S}/omnios-persistent-data ${D}${libexecdir}/omnios-persistent-data
    install -m 0644 ${S}/omnios-persistent-data.service ${D}${systemd_system_unitdir}/omnios-persistent-data.service
}
FILES:${PN} += "${libexecdir}/omnios-persistent-data"
