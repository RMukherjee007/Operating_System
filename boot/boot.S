/* FulmenOS AArch64 Bootloader Entry, PE32+ EFI Stub & EL2 -> EL1 Initialization */

.section ".text.boot"
.global _start

/*
 * Dual-format AArch64 Header:
 * 1) Valid 64-byte ARM64 Linux/Image header (magic "ARM\x64" at 0x38)
 *    with "MZ" disguised as harmless `add x13, x18, #0x16` + branch to _boot_entry.
 * 2) Valid PE32+ (0xAA64) EFI Application header at .Lpe_header (offset 0x40)
 *    so UEFI and GNU GRUB 2.12 (`linux /boot/fulmen.bin`) execute _efi_entry.
 */
_start:
    .inst   0x91005a4d                  /* "MZ" + harmless ARM64 add x13, x18, #0x16 */
    b       _boot_entry                 /* Direct QEMU -kernel branch to kernel setup */
    .quad   0x00200000                  /* text_offset: 2 MB from RAM base (0x40200000) */
    .quad   __pe_image_size             /* image_size */
    .quad   0x000000000000000A          /* flags: Little-Endian, 4K pages */
    .quad   0                           /* res2 */
    .quad   0                           /* res3 */
    .quad   0                           /* res4 */
    .word   0x644d5241                  /* magic: "ARM\x64" */
    .word   .Lpe_header - _start        /* PE/COFF header offset (0x40) */

.Lpe_header:
    .ascii  "PE\0\0"                    /* PE signature */
    /* COFF File Header (20 bytes) */
    .short  0xaa64                      /* Machine: ARM64 */
    .short  1                           /* NumberOfSections: 1 (.text) */
    .word   0                           /* TimeDateStamp */
    .word   0                           /* PointerToSymbolTable */
    .word   0                           /* NumberOfSymbols */
    .short  .Lsection_table - .Lopt_header /* SizeOfOptionalHeader (240) */
    .short  0x0206                      /* Characteristics: EXEC | LINE_STRIPPED | DEBUG_STRIPPED */

.Lopt_header:
    /* Standard PE32+ Fields (24 bytes) */
    .short  0x020b                      /* Magic: PE32+ */
    .byte   0x02                        /* MajorLinkerVersion */
    .byte   0x14                        /* MinorLinkerVersion */
    .word   __pe_code_size              /* SizeOfCode */
    .word   __pe_data_size              /* SizeOfInitializedData */
    .word   0                           /* SizeOfUninitializedData */
    .word   _efi_entry - _start         /* AddressOfEntryPoint */
    .word   .Lsection_end - _start      /* BaseOfCode */

    /* Windows/UEFI-specific PE32+ Fields (88 bytes) */
    .quad   0                           /* ImageBase */
    .word   0x1000                      /* SectionAlignment (4 KB) */
    .word   0x1000                      /* FileAlignment (4 KB) */
    .short  0, 0, 0, 0, 0, 0            /* OS / Image / Subsystem versions */
    .word   0                           /* Win32VersionValue */
    .word   __pe_image_size             /* SizeOfImage */
    .word   .Lsection_end - _start      /* SizeOfHeaders (4 KB) */
    .word   0                           /* CheckSum */
    .short  10                          /* Subsystem: EFI Application */
    .short  0                           /* DllCharacteristics */
    .quad   0, 0, 0, 0                  /* Stack/Heap reserve & commit */
    .word   0                           /* LoaderFlags */
    .word   16                          /* NumberOfRvaAndSizes */

    /* Data Directories (16 * 8 = 128 bytes) */
    .fill   16, 8, 0

.Lsection_table:
    /* .text Section Header (40 bytes) */
    .ascii  ".text\0\0\0"
    .word   __pe_virt_size              /* VirtualSize */
    .word   .Lsection_end - _start      /* VirtualAddress */
    .word   __pe_code_size              /* SizeOfRawData */
    .word   .Lsection_end - _start      /* PointerToRawData */
    .word   0, 0                         /* Relocations / Linenumbers */
    .short  0, 0
    .word   0xE0000020                   /* CNT_CODE | MEM_READ | MEM_WRITE | MEM_EXECUTE */

    .balign 4096
.Lsection_end:

/*
 * UEFI Entry Point: x0 = ImageHandle, x1 = SystemTable
 * Exits UEFI boot services, relocates kernel to linked address 0x40200000,
 * flushes caches, and enters _boot_entry.
 */
_efi_entry:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    bl      efi_exit_boot_services
    ldp     x29, x30, [sp], #16

    msr     daifset, #0xf

    /* Determine runtime load base via PC-relative addressing */
    adrp    x0, _start
    add     x0, x0, :lo12:_start
    ldr     x1, =0x40200000

    /* Copy image [__bss_start - _start] bytes to 0x40200000 if relocated */
    cmp     x0, x1
    b.eq    .Lefi_disable_mmu

    adrp    x2, __bss_start
    add     x2, x2, :lo12:__bss_start
    sub     x2, x2, x0                  /* x2 = raw byte length to copy */

    cmp     x0, x1
    b.hs    .Lcopy_fwd

    /* x0 < x1: copy backwards to avoid overlap */
    add     x3, x0, x2
    add     x4, x1, x2
.Lcopy_bwd:
    cbz     x2, .Lcache_sync
    sub     x3, x3, #8
    sub     x4, x4, #8
    ldr     x5, [x3]
    str     x5, [x4]
    sub     x2, x2, #8
    b       .Lcopy_bwd

.Lcopy_fwd:
    mov     x3, x0
    mov     x4, x1
    mov     x6, x2
.Lcopy_loop:
    cbz     x6, .Lcache_sync
    ldr     x5, [x3], #8
    str     x5, [x4], #8
    sub     x6, x6, #8
    b       .Lcopy_loop

.Lcache_sync:
    /* Clean D-cache to PoC across copied image [0x40200000 .. __bss_start] */
    ldr     x3, =0x40200000
    ldr     x4, =__bss_start
.Lclean_loop:
    cmp     x3, x4
    b.hs    .Lclean_done
    dc      cvac, x3
    add     x3, x3, #64
    b       .Lclean_loop
.Lclean_done:
    dsb     sy

.Lefi_disable_mmu:
    /*
     * Disable UEFI MMU & D-cache while executing in UEFI's executable mapping,
     * because EDK2 maps unowned RAM at 0x40200000 as Execute-Never (PXN).
     */
    mrs     x5, CurrentEL
    lsr     x5, x5, #2
    cmp     x5, #2
    b.eq    .Lefi_el2_mmu_off

    mrs     x5, sctlr_el1
    bic     x5, x5, #(1 << 0)           /* M = 0 */
    bic     x5, x5, #(1 << 2)           /* C = 0 */
    msr     sctlr_el1, x5
    isb
    tlbi    vmalle1
    dsb     sy
    isb
    b       .Lefi_icache_inv

.Lefi_el2_mmu_off:
    mrs     x5, sctlr_el2
    bic     x5, x5, #(1 << 0)           /* M = 0 */
    bic     x5, x5, #(1 << 2)           /* C = 0 */
    msr     sctlr_el2, x5
    isb
    tlbi    alle2
    dsb     sy
    isb

.Lefi_icache_inv:
    ic      iallu
    dsb     sy
    isb

.Lefi_jump:
    ldr     x2, =_boot_entry
    br      x2

_boot_entry:
    /* Mask all interrupts (D, A, I, F) */
    msr     daifset, #0xf

    /* Park secondary CPU cores; only core 0 runs */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xff
    cbnz    x0, .Lpark

    /* Inspect CurrentEL (bits [3:2]) */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.eq    .Ldrop_from_el2
    cmp     x0, #1
    b.eq    .Lensure_el1_mmu_off

.Lpark:
    wfe
    b       .Lpark

.Ldrop_from_el2:
    /* If UEFI/GRUB had EL2 MMU enabled, disable it cleanly first */
    mrs     x0, sctlr_el2
    tbz     x0, #0, .Lel2_setup
    bic     x0, x0, #(1 << 0)           /* M = 0 (MMU off) */
    bic     x0, x0, #(1 << 2)           /* C = 0 (D-cache off) */
    msr     sctlr_el2, x0
    isb
    tlbi    alle2
    dsb     sy
    isb

.Lel2_setup:
    /* Set EL1 to AArch64 mode (HCR_EL2.RW = 1) */
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0

    /* Allow EL0/EL1 access to physical counter & timer */
    mov     x0, #3
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr

    /* Initialize clean SCTLR_EL1 (MMU off, little-endian) */
    ldr     x0, =0x30D00800
    msr     sctlr_el1, x0

    /* Return to EL1h with DAIF masked (0x3c5) */
    mov     x0, #0x3c5
    msr     spsr_el2, x0
    adr     x0, .Lel1_entry
    msr     elr_el2, x0
    eret

.Lensure_el1_mmu_off:
    /* If already in EL1 (e.g. direct -kernel or EFI EL1), disable MMU if active */
    mrs     x0, sctlr_el1
    tbz     x0, #0, .Lel1_entry
    bic     x0, x0, #(1 << 0)
    bic     x0, x0, #(1 << 2)
    msr     sctlr_el1, x0
    isb
    tlbi    vmalle1
    dsb     sy
    isb

.Lel1_entry:
    /* Ensure SP_EL1 is selected */
    msr     spsel, #1

    /* Enable FP/SIMD for EL1 and EL0 (CPACR_EL1.FPEN = 0b11) */
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb

    /* Install 2KB-aligned Exception Vector Table */
    adrp    x0, vector_table
    add     x0, x0, :lo12:vector_table
    msr     vbar_el1, x0
    isb

    /* Initialize 16-byte aligned kernel boot stack */
    adrp    x0, __stack_top
    add     x0, x0, :lo12:__stack_top
    mov     sp, x0

    /* Clear .bss section */
    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start
    adrp    x1, __bss_end
    add     x1, x1, :lo12:__bss_end
.Lzero_bss:
    cmp     x0, x1
    b.hs    .Lbss_done
    stp     xzr, xzr, [x0], #16
    b       .Lzero_bss

.Lbss_done:
    /* Jump into freestanding C kernel */
    bl      kernel_main

    /* Should never return */
    b       .Lpark
