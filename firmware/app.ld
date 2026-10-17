/* SPDX-License-Identifier: GPL-3.0-only */
/* FM-1 Doom memory map (after Felucca's firmware/app.ld).
 * Code, constants and part a of the game data execute / read in place from XIP (app.bin offset 0 =
 * 0x02000120); the app slot ends at 0x0208E0DC (tools/fm1pkg_make.py APP_SLOT).
 * 0x01C00000..0x01C07C00 holds .ram_text (the flash driver: it runs while the flash is busy), then .lowbss
 * (buffers, zeroed at boot; crt0 notes the boot state at 0x01C07F00).
 * RAM 0x01C08000..0x01C76000 (440 KiB): .data, .bss, .pool (big buffers), then the engine's zone, all
 * that is left (_zone_start.._zone_end).
 * STACKS with 256-byte guard bands (user 16 KiB, system 8 KiB); NOINIT survives resets and UBOOT entry,
 * and holds scratch buffers that need no clearing.
 * 0x01C7FD50.. is reserved: boot_info, boot-state mailbox, vector table. */
MEMORY
{
    XIP    (rx)  : ORIGIN = 0x02000120, LENGTH = 0x8DFBC
    RAMTEXT (rwx): ORIGIN = 0x01C00000, LENGTH = 0x7C00
    RAM    (rwx) : ORIGIN = 0x01C08000, LENGTH = 0x6E000
    NOINIT (rw)  : ORIGIN = 0x01C7C000, LENGTH = 0x3D50
}
SECTIONS
{
    .text : { KEEP(*(.text.crt0)) *(.text*) *(.rodata*) . = ALIGN(4); _etext = .; } > XIP
    .ram_text : { _rt_start = .; KEEP(*(.ram_text*)) . = ALIGN(4); _rt_end = .; } > RAMTEXT AT > XIP
    _rt_load = LOADADDR(.ram_text);
    .lowbss (NOLOAD) : { _lowbss_start = .; *(.lowbss*) . = ALIGN(4); _lowbss_end = .; } > RAMTEXT
    .data : { . = ALIGN(4); _data_start = .; *(.data*) *(.sdata*) . = ALIGN(4); _data_end = .; } > RAM AT > XIP
    _data_load = LOADADDR(.data);
    .bss (NOLOAD) : { _bss_start = .; *(.bss*) *(.sbss*) *(COMMON) . = ALIGN(4); _bss_end = .; } > RAM
    .pool (NOLOAD) : { _pool_start = .; *(.pool*) . = ALIGN(8); _pool_end = .; } > RAM
    _zone_start = _pool_end;
    _zone_end = 0x01C76000;
    .noinit (NOLOAD) : { *(.noinit*) } > NOINIT
    _guard0 = 0x01C76000; _ustack_lo = 0x01C76100; _ustack_top = 0x01C7A000;
    _guard1 = 0x01C7A000; _sstack_lo = 0x01C7A100; _sstack_top = 0x01C7C000;
    /DISCARD/ : { *(.comment) *(.note*) }
}
