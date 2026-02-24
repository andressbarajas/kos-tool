# client/Makefile — dispatcher for DC and GC client builds

ROOT ?= $(CURDIR)/..

.PHONY: all dc gc clean

all: dc gc

dc:
	$(MAKE) -C dreamcast ROOT=$(ROOT) $(MAKECMDGOALS_EXTRA)

gc:
	$(MAKE) -C gamecube ROOT=$(ROOT) $(MAKECMDGOALS_EXTRA)

clean:
	$(MAKE) -C dreamcast ROOT=$(ROOT) clean
	$(MAKE) -C gamecube ROOT=$(ROOT) clean
