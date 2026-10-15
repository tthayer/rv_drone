OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    /* The DTS memory node describes DDR 0x80000000..0x8FE00000 (the top 2 MiB is
     * the C906L region), so up to 254 MiB is addressable from our load address.
     * The window covers the large delay/looper buffers in .bss (.bss is NOLOAD,
     * so it does not grow fip.bin). */
    RAM (rwx) : ORIGIN = 0x80200000, LENGTH = 224M
}

STACK_SIZE = 16K;

SECTIONS
{
    . = ORIGIN(RAM);
    __image_start = .;

    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
    } > RAM

    .rodata : ALIGN(16) {
        *(.rodata .rodata.* .srodata .srodata.*)
    } > RAM

    .data : ALIGN(16) {
        __global_pointer$ = . + 0x800;
        *(.data .data.* .sdata .sdata.*)
    } > RAM

    __bss_start = ALIGN(16);
    .bss (NOLOAD) : ALIGN(16) {
        *(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(16);
        __stack_bottom = .;
        . += STACK_SIZE;
        __stack_top = .;
    } > RAM
    __bss_end = .;
    __image_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
