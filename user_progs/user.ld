OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    . = 0x44000000;

    .text : ALIGN(16) {
        KEEP(*(.text.entry))
        *(.text .text.*)
    }

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(16) {
        *(.data .data.*)
    }

    .bss : ALIGN(16) {
        __user_bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __user_bss_end = .;
    }
}
