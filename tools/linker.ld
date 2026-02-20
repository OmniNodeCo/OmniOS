ENTRY(start)
OUTPUT_FORMAT(elf32-i386)
SECTIONS
{
    KERNEL_VMA = 0x100000; /* Load kernel at 1MB physical address */
    . = KERNEL_VMA;
    .text : {
        *(.multiboot)
        *(.text)
    }
    .rodata : {
        *(.rodata)
    }
    .data : {
        *(.data)
        *(.bss)
    }
}
