OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    /* Load kernel 2MB into QEMU virt RAM (0x40000000 holds DTB) */
    . = 0x40200000;
    __kernel_start = .;

    .text.boot : ALIGN(4096) {
        KEEP(*(.text.boot))
    }

    .text : ALIGN(4096) {
        *(.text .text.*)
    }

    .rodata : ALIGN(4096) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(4096) {
        *(.data .data.*)
        *(.got .got.*)
        *(.got.plt)
        . = . + 1;
        . = ALIGN(4096) - 1;
        BYTE(0)
    }

    .bss : ALIGN(4096) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    . = ALIGN(4096);
    __stack_bottom = .;
    . += 65536; /* 64 KB EL1 boot/idle kernel stack */
    . = ALIGN(16);
    __stack_top = .;

    . = ALIGN(4096);
    __kernel_end = .;

    __pe_image_size = __kernel_end - __kernel_start;
    __pe_code_size  = __bss_start - (__kernel_start + 0x1000);
    __pe_data_size  = __kernel_end - __bss_start;
    __pe_virt_size  = __kernel_end - (__kernel_start + 0x1000);

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame*)
        *(.note*)
    }
}
