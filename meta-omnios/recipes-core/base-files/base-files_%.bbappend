FILESEXTRAPATHS:prepend := "${THISDIR}/${BPN}:"
SRC_URI:append = " file://fstab file://motd"
hostname = "omnios"
dirs755 += "/data /grubenv"
