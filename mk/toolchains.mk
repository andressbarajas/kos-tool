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

# Tool name prefixes (the cross-compiler triple).  Combined with the bindirs
# above to form full tool paths, e.g. $(DC_TOOLCHAIN)/$(DC_PREFIX)gcc.
DC_PREFIX           := sh-elf-
GC_PREFIX           := powerpc-eabi-
PS2_PREFIX          := mips64r5900el-ps2-elf-
PS2_IOP_PREFIX      := mipsel-elf-
