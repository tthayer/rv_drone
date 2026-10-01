OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80200000, LENGTH = 64M
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
