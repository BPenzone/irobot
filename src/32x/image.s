! The prepared I, Robot ROM image (build/irobot.img, made by tools/mkimage
! from the user's own ROM set). Never committed.

	.section .rodata
	.align	4
	.global	_ir_rom_image
_ir_rom_image:
	.incbin	"irobot.img"
