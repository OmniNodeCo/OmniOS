; boot/bootloader.asm
; BIOS-compatible multiboot bootloader

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
    mov esp, 0x9f000          ; Setup stack (stack grows down from 0x9f000)
    call kernel_main          ; Jump to C kernel
    cli                      ; Disable interrupts
hang:
    hlt                      ; Halt CPU
    jmp hang
