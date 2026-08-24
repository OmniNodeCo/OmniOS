SUMMARY = "OmniOS RAUC platform configuration and verification keyring"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
SRC_URI = "file://system.conf"
S = "${UNPACKDIR}"
OMNIOS_RAUC_KEYRING ?= ""
RPROVIDES:${PN} += "virtual-rauc-conf"
INHIBIT_DEFAULT_DEPS = "1"
do_compile[noexec] = "1"
do_install() {
    if [ -z "${OMNIOS_RAUC_KEYRING}" ] || [ ! -s "${OMNIOS_RAUC_KEYRING}" ]; then
        bbfatal "OmniOS RAUC keyring missing. Run scripts/generate-rauc-keys.sh or configure production keys."
    fi
    install -d ${D}${nonarch_libdir}/rauc
    sed -e 's/@OMNIOS_BUNDLE_VERSION@/${OMNIOS_BUNDLE_VERSION}/g' ${S}/system.conf > ${D}${nonarch_libdir}/rauc/system.conf
    chmod 0644 ${D}${nonarch_libdir}/rauc/system.conf
    install -m 0644 ${OMNIOS_RAUC_KEYRING} ${D}${nonarch_libdir}/rauc/ca.cert.pem
}
do_install[vardeps] += "OMNIOS_RAUC_KEYRING OMNIOS_BUNDLE_VERSION"
do_install[file-checksums] += "${OMNIOS_RAUC_KEYRING}:False"
FILES:${PN} += "${nonarch_libdir}/rauc"
