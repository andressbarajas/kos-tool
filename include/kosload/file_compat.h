#ifndef KOSLOAD_FILE_COMPAT_H
#define KOSLOAD_FILE_COMPAT_H

/*
 * Windows distinguishes between text and binary file descriptors.
 * Host-side binary assets such as ELF/ISO/images must opt into binary mode,
 * while POSIX platforms can treat O_BINARY as a no-op.
 */
#ifndef O_BINARY
#define O_BINARY 0
#endif

#endif /* KOSLOAD_FILE_COMPAT_H */
