# host/tools/elf2xbe/Makefile — cleanroom ELF->XBE converter

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11

all: elf2xbe

elf2xbe: elf2xbe.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	-rm -f elf2xbe

.PHONY: all clean
