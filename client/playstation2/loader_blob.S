/* Wrap the flat inner-loader binary as a normal MIPS object.
 * Using the assembler instead of objcopy keeps the object's MIPS ABI flags
 * consistent with the rest of the outer ELF link. */

    .section .data.loader_blob, "aw", @progbits
    .balign 16
    .global _binary_ps2_load_ip_image_bin_start
_binary_ps2_load_ip_image_bin_start:
    .incbin "build/ip/ps2-load-ip-image.bin"
    .balign 16
    .global _binary_ps2_load_ip_image_bin_end
_binary_ps2_load_ip_image_bin_end:
