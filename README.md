# Project Nova — Minimal x86 OS

A freestanding OS kernel built with `gcc -ffreestanding -nostdinc`.

## Features
- Multiboot-compliant bootloader (32-bit protected mode)
- Custom in-memory FAT12-like file system
- VGA text driver
- Zero external dependencies

## Build Locally
```bash
nasm -f elf32 boot/bootloader.asm -o boot/bootloader.o
gcc -m32 -c kernel/*.c -o kernel/*.o \
  -ffreestanding -nostdinc -fno-stack-protector -Ikernel -I.
ld -m elf_i386 -T tools/linker.ld -o build/kernel.bin boot/bootloader.o kernel/*.o
qemu-system-i386 -kernel build/kernel.bin
