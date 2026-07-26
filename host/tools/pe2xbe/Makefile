# host/tools/pe2xbe/Makefile — cleanroom PE->XBE converter

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11

all: pe2xbe

pe2xbe: pe2xbe.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	-rm -f pe2xbe

.PHONY: all clean
