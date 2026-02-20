; boot/bootloader.asm
MULTIBOOT_HEADER_MAGIC equ 0x1BADB002
MULTIBOOT_HEADER_FLAGS equ 0x00000003
CHECKSUM equ -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS)

section .multiboot
align 4
    dd MULTIBOOT_HEADER_MAGIC
    dd MULTIBOOT_HEADER_FLAGS
    dd CHECKSUM

section .text
extern kernel_main
bits 32
start:
    mov esp, 0x9f000          ; Setup a temporary stack
    call kernel_main          ; Jump to C code
    cli
hang:   hlt
    jmp hang
