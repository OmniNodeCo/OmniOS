SUMMARY = "OmniOS hybrid GRUB A/B boot configuration"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
include conf/image-uefi.conf
RPROVIDES:${PN} += "virtual-grub-bootconf"
SRC_URI = "file://grub.cfg"
S = "${UNPACKDIR}"
inherit deploy

do_compile() {
    printf '# GRUB Environment Block\n' > ${B}/grubenv
    dd if=/dev/zero bs=1 count=999 2>/dev/null | tr '\000' '#' >> ${B}/grubenv
}
do_install() {
    install -d ${D}${EFI_FILES_PATH}
    install -m 0644 ${S}/grub.cfg ${D}${EFI_FILES_PATH}/grub.cfg
}
FILES:${PN} += "${EFI_FILES_PATH}"
do_deploy[depends] += "dosfstools-native:do_populate_sysroot mtools-native:do_populate_sysroot"
do_deploy() {
    install -m 0644 ${S}/grub.cfg ${DEPLOYDIR}/grub.cfg
    install -m 0644 ${B}/grubenv ${DEPLOYDIR}/grubenv
    rm -f ${B}/omnios-grubenv.vfat
    mkfs.vfat -n OMNIOS-ENV -S 512 -C ${B}/omnios-grubenv.vfat 16384
    mcopy -i ${B}/omnios-grubenv.vfat ${B}/grubenv ::/grubenv
    install -m 0644 ${B}/omnios-grubenv.vfat ${DEPLOYDIR}/omnios-grubenv.vfat
}
addtask deploy after do_compile before do_build
