/* SPDX-License-Identifier: GPL-3.0-only */
/* Part a of the game data (tools/wadpack.py, cut by tools/build.py): read in place from XIP. */
	.section .rodata.dpk_a,"a",@progbits
	.balign 4
	.globl dpk_part_a
dpk_part_a:
	.incbin DPK_A_BIN
