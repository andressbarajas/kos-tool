# host/tools/elf2prx/Makefile — build the ELF -> PSP PRX converter.

CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11

all: elf2prx

elf2prx: elf2prx.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f elf2prx

.PHONY: all clean
