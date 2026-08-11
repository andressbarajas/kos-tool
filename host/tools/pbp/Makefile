# host/tools/pbp/Makefile — clean-room EBOOT.PBP packer

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11

all: pbp

pbp: pbp.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	-rm -f pbp

.PHONY: all clean
