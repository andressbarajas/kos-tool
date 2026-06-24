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
