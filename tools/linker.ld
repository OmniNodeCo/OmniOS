/* tools/linker.ld */
ENTRY(start)
OUTPUT_FORMAT(elf32-i386)
SECTIONS {
    . = 0x100000;

    .multiboot : {
        *(.multiboot)
    }

    .text : {
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
