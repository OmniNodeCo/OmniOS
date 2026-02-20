#!/bin/bash

# 1. Compile Assembler
nasm -f elf32 boot/bootloader.asm -o boot/bootloader.o

# 2. Compile Kernel (C to Object)
gcc -m32 -c kernel/main.c -o kernel/main.o -ffreestanding -Ikernel
gcc -m32 -c kernel/video.c -o kernel/video.o -ffreestanding -Ikernel
gcc -m32 -c kernel/filesystem.c -o kernel/filesystem.o -ffreestanding -Ikernel

# 3. Link everything together
ld -m elf_i386 -T linker.ld -o kernel/kernel.bin boot/bootloader.o kernel/main.o kernel/video.o kernel/filesystem.o

# 4. Create a floppy disk image (1.44MB)
dd if=/dev/zero of=bin/os-image.img bs=512 count=2880
mkfs.msdos bin/os-image.img

# 5. (Advanced) Install the kernel to the disk image
# For this simple demo, we will just run the kernel.bin directly in QEMU
# as loading a multiboot kernel from a floppy requires precise sector layout.

echo "Building Complete. Run with: qemu-system-i386 -kernel kernel/kernel.bin"
