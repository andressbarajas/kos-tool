# Shared cross-toolchain configuration.
#
# Precedence:
# 1. make command-line assignments (for example: make gc GC_TOOLCHAIN=/path)
# 2. exported environment variables
# 3. defaults below
#
# Edit this file if your toolchains live somewhere other than the defaults.

DC_TOOLCHAIN        ?= /opt/toolchains/dc/sh-elf/bin
GC_TOOLCHAIN        ?= /opt/toolchains/gc/powerpc-eabi/bin
PS2_EE_TOOLCHAIN    ?= /opt/toolchains/ps2/mips-elf/bin
PS2_IOP_TOOLCHAIN   ?= /opt/toolchains/ps2/mipsel-elf/bin
