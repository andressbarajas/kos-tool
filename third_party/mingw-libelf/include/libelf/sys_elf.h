/* lib/sys_elf.h.  Configured for MinGW x86_64.  */
/*
 * DO NOT USE THIS IN APPLICATIONS - #include <libelf.h> INSTEAD!
 */

/* No system <elf.h> on Windows */
/* #undef __LIBELF_HEADER_ELF_H */
/* #undef __LIBELF_NEED_LINK_H */
/* #undef __LIBELF_NEED_SYS_LINK_H */

/* 64-bit support */
#define __LIBELF64 1
/* #undef __LIBELF64_IRIX */
/* #undef __LIBELF64_LINUX */
/* #undef __LIBELF_SYMBOL_VERSIONS */

/* Type definitions for MinGW x86_64 */
#define __libelf_i64_t long long
#define __libelf_u64_t unsigned long long
#define __libelf_i32_t int
#define __libelf_u32_t unsigned int
#define __libelf_i16_t short
#define __libelf_u16_t unsigned short

/* No system elf.h on Windows, use bundled replacement */
#include <libelf/elf_repl.h>
