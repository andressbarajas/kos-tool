# host/tools/elf2irx/Makefile
#
# Build the elf2irx tool. Plain C99, no external deps.
# Output: ./elf2irx (or .exe on Windows).

CC      ?= cc
CFLAGS  := -std=c99 -O2 -Wall -Wextra -Wpedantic -Wshadow

elf2irx: elf2irx.c
	$(CC) $(CFLAGS) -o $@ $<

.PHONY: clean
clean:
	rm -f elf2irx elf2irx.exe
