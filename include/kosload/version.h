#ifndef KOSLOAD_VERSION_H
#define KOSLOAD_VERSION_H

/*
 * Version macros, validated in one place.
 *
 * mk/version.mk is the single source of truth; it hands the fields to every
 * compilation unit -- host and all six console clients -- as the -D set in
 * KOSLOAD_VERSION_DEFS.  Nothing here defines a version: this header only
 * checks that the build system supplied one and derives the packed form.
 *
 * The #error blocks are ergonomics, not detection.  A missing -D already
 * breaks the build (an undefined macro in a string concat or a shift is a
 * compile error), just with a message that points at the use site instead of
 * at the real cause.  Keeping them here means one copy rather than one per
 * user.
 *
 * NB this header is checked in, unlike the generated version.h it replaces --
 * there is no version.h.in and no build rule behind it.
 */

#ifndef KOSLOAD_VERSION_MAJOR
#error "KOSLOAD_VERSION_MAJOR missing -- add $(KOSLOAD_VERSION_DEFS) from mk/version.mk to CFLAGS"
#endif

#ifndef KOSLOAD_VERSION_MINOR
#error "KOSLOAD_VERSION_MINOR missing -- add $(KOSLOAD_VERSION_DEFS) from mk/version.mk to CFLAGS"
#endif

#ifndef KOSLOAD_VERSION_PATCH
#error "KOSLOAD_VERSION_PATCH missing -- add $(KOSLOAD_VERSION_DEFS) from mk/version.mk to CFLAGS"
#endif

#ifndef KOSLOAD_VERSION_STRING
#error "KOSLOAD_VERSION_STRING missing -- add $(KOSLOAD_VERSION_DEFS) from mk/version.mk to CFLAGS"
#endif

#ifndef KOSLOAD_GIT_REV
#error "KOSLOAD_GIT_REV missing -- add $(KOSLOAD_VERSION_DEFS) from mk/version.mk to CFLAGS"
#endif

/*
 * Packed version word: 0x00MMmmpp.  Used for the client info block's .version
 * field and for the host's VERS handshake word -- both wire-visible, so they
 * must agree; deriving both from this macro is what guarantees that.
 */
#define KOSLOAD_VERSION_ENCODED                                                \
    (((unsigned)KOSLOAD_VERSION_MAJOR << 16) |                                 \
     ((unsigned)KOSLOAD_VERSION_MINOR << 8) | (unsigned)KOSLOAD_VERSION_PATCH)

#endif /* KOSLOAD_VERSION_H */
