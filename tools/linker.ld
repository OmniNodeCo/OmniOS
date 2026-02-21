/* tools/linker.ld */
ENTRY(start)
OUTPUT_FORMAT(elf32-i386)
SECTIONS {
    . = 0x100000;            /* Kernel loads at 1MB physical address */
    
    .text : {
        *(.multiboot)        /* Multiboot header must be first */
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
