# OmniOS top-level Makefile — thin wrapper over scripts/build.sh
# Targets map 1:1 to build stages. Run `make help` for the list.

.PHONY: help fetch toolchain musl userspace rootfs kernel iso full clean

help:
	@echo "OmniOS build targets:"
	@echo "  make fetch       download upstream sources"
	@echo "  make toolchain   bc + kernel headers"
	@echo "  make musl        musl libc into build/sysroot"
	@echo "  make userspace   musl + busybox + microwindows"
	@echo "  make rootfs      assemble build/rootfs"
	@echo "  make kernel      configure + build the Linux kernel"
	@echo "  make iso         assemble the bootable ISO"
	@echo "  make full        everything above, in order"
	@echo "  make clean       remove build outputs"

fetch:
	scripts/build.sh fetch

toolchain:
	scripts/build.sh toolchain

musl:
	scripts/build.sh musl

userspace:
	scripts/build.sh userspace

rootfs:
	scripts/build.sh rootfs

kernel:
	scripts/build.sh kernel

iso:
	scripts/build.sh iso

full:
	scripts/build.sh full

clean:
	scripts/build.sh clean
