# third_party/minilzo/Makefile
# Builds minilzo.a with caller-provided CC/AR/CFLAGS/BUILDDIR.

CC       ?= cc
AR       ?= ar
CFLAGS   ?= -O2
BUILDDIR ?= build

.PHONY: all clean

all: $(BUILDDIR)/minilzo.a

$(BUILDDIR)/minilzo.a: $(BUILDDIR)/minilzo.o
	$(AR) rcs $@ $<

$(BUILDDIR)/minilzo.o: minilzo.c minilzo.h lzoconf.h lzodefs.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I. -c $< -o $@

$(BUILDDIR):
	@mkdir -p $@

clean:
	rm -rf build-*
