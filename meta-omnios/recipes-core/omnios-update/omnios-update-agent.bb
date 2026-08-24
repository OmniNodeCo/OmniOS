SUMMARY = "OmniOS automatic atomic update agent"
DESCRIPTION = "Downloads signed RAUC bundles over HTTPS and installs the inactive A/B slot."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
SRC_URI = "file://omnios-update file://omnios-update.conf file://omnios-update.service file://omnios-update.timer"
S = "${UNPACKDIR}"
inherit allarch systemd
RDEPENDS:${PN} = "bash coreutils curl rauc util-linux-flock ca-certificates"
SYSTEMD_SERVICE:${PN} = "omnios-update.timer"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"
do_install() {
    install -d ${D}${libexecdir} ${D}${sysconfdir}/omnios ${D}${systemd_system_unitdir}
    install -m 0755 ${S}/omnios-update ${D}${libexecdir}/omnios-update
    install -m 0600 ${S}/omnios-update.conf ${D}${sysconfdir}/omnios/update.conf
    install -m 0644 ${S}/omnios-update.service ${D}${systemd_system_unitdir}/omnios-update.service
    install -m 0644 ${S}/omnios-update.timer ${D}${systemd_system_unitdir}/omnios-update.timer
}
CONFFILES:${PN} += "${sysconfdir}/omnios/update.conf"
FILES:${PN} += "${libexecdir}/omnios-update"
