KOSLOAD_VERSION_MAJOR := 3
KOSLOAD_VERSION_MINOR := 0
KOSLOAD_VERSION_PATCH := 1
KOSLOAD_VERSION       := $(KOSLOAD_VERSION_MAJOR).$(KOSLOAD_VERSION_MINOR).$(KOSLOAD_VERSION_PATCH)

# Git revision sub-version stamp: short commit hash, plus "-dirty" when the
# working tree has uncommitted tracked changes. Falls back to "unknown" outside
# a git checkout (e.g. a source tarball). Captured at build time and used for
# DISPLAY ONLY (host --help banner + firmware on-screen banner) -- it is
# deliberately NOT part of the VERS handshake or serial NAME wire strings.
KOSLOAD_GIT_SHA := $(shell git -C $(ROOT) rev-parse --short HEAD 2>/dev/null)
ifeq ($(strip $(KOSLOAD_GIT_SHA)),)
KOSLOAD_GIT_REV := unknown
else ifeq ($(strip $(shell git -C $(ROOT) status --porcelain --untracked-files=no 2>/dev/null)),)
KOSLOAD_GIT_REV := $(KOSLOAD_GIT_SHA)
else
KOSLOAD_GIT_REV := $(KOSLOAD_GIT_SHA)-dirty
endif

# Single set of -D flags carrying the version into every compilation unit --
# host and all six console clients alike.  Every Makefile appends this to its
# CFLAGS rather than spelling the -D's out, so there is exactly one place that
# decides what the compiler sees.  MAJOR/MINOR/PATCH are the numeric fields the
# host packs into the VERS handshake word (host/src/transport/network.c);
# STRING is the human-readable form used in banners and the NAME wire strings;
# GIT_REV is display-only (see above).  Sources #ifndef-#error on these instead
# of defaulting, so a Makefile that forgets them fails the build rather than
# shipping a wrong version.
KOSLOAD_VERSION_DEFS := \
    -DKOSLOAD_VERSION_MAJOR=$(KOSLOAD_VERSION_MAJOR) \
    -DKOSLOAD_VERSION_MINOR=$(KOSLOAD_VERSION_MINOR) \
    -DKOSLOAD_VERSION_PATCH=$(KOSLOAD_VERSION_PATCH) \
    -DKOSLOAD_VERSION_STRING=\"$(KOSLOAD_VERSION)\" \
    -DKOSLOAD_GIT_REV=\"$(KOSLOAD_GIT_REV)\"

# Prerequisite for every object that bakes the version in.  The old build
# generated an <kosload/version.h> and depended on that; with the version
# arriving purely as -D flags, make has nothing to notice on a bump (it does
# not track CFLAGS changes), so objects depend on this file directly.
KOSLOAD_VERSION_MK := $(ROOT)/mk/version.mk
