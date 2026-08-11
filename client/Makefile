# client/Makefile — dispatcher for DC, GC, Wii, and PS2 client builds

ROOT ?= $(CURDIR)/..

.PHONY: all dc gc wii ps2 xbox psp clean

all: dc gc wii ps2 xbox psp

dc:
	$(MAKE) -C dreamcast ROOT=$(ROOT) $(MAKECMDGOALS_EXTRA)

gc:
	$(MAKE) -C gamecube ROOT=$(ROOT) $(MAKECMDGOALS_EXTRA)

wii:
	$(MAKE) -C wii ROOT=$(ROOT) $(MAKECMDGOALS_EXTRA)

ps2:
	$(MAKE) -C playstation2 ROOT=$(ROOT) $(MAKECMDGOALS_EXTRA)

xbox:
	$(MAKE) -C xbox ROOT=$(ROOT) $(MAKECMDGOALS_EXTRA)

psp:
	$(MAKE) -C psp ROOT=$(ROOT) $(MAKECMDGOALS_EXTRA)

clean:
	$(MAKE) -C dreamcast ROOT=$(ROOT) clean
	$(MAKE) -C gamecube ROOT=$(ROOT) clean
	$(MAKE) -C wii ROOT=$(ROOT) clean
	$(MAKE) -C playstation2 ROOT=$(ROOT) clean
	$(MAKE) -C xbox ROOT=$(ROOT) clean
	$(MAKE) -C psp ROOT=$(ROOT) clean
