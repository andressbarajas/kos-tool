/* client/playstation2/iop/kosload_iop.h
 *
 * Clean-room IOP IRX scaffolding for kosload.
 *
 * Usage:
 *
 *   #include "kosload_iop.h"
 *
 *   KOSLOAD_IRX_ID("smaptest", 1, 0)
 *
 *   int _start(int argc, char **argv)
 *   {
 *       (void)argc;
 *       (void)argv;
 *       return KOSLOAD_MODULE_NO_RESIDENT_END;
 *   }
 *
 * The companion linker script kosload_iop.ld provides the
 * `_kosload_text_size` / `_kosload_data_size` / `_kosload_bss_size`
 * symbols whose values are filled into the .iopmod struct at link
 * time.
 */

#ifndef KOSLOAD_IOP_H
#define KOSLOAD_IOP_H

/* IOP _start return codes — what BIOS LOADCORE inspects to decide
 * whether to keep or drop the module after _start returns.
 *
 *   0 = keep module resident (used by drivers like sifrpc, dev9, etc.)
 *   1 = drop module after _start returns (used by one-shot IRXs that
 *       only do init work).
 *
 * Confirmed by observing existing IRX behavior: sony_*.irx return 0
 * and stay loaded; our dev9_init.irx returns 1 and is unloaded after
 * its setup work completes. */
#define KOSLOAD_MODULE_RESIDENT_END     0
#define KOSLOAD_MODULE_NO_RESIDENT_END  1

/* Common LOADCORE import/export table header.
 *
 * Resident IOP libraries are linked by table name and numeric slot, not
 * by symbol name. Export tables use magic 0x41C00000 and import tables
 * use 0x41E00000; the remaining 16-byte header is shared.
 *
 * The version field is little-endian `(major << 8) | minor`, so version
 * 1.01 is 0x0101. The mode field starts as 0 in static tables and is
 * used by LOADCORE as a runtime flag word while linking/unlinking. The
 * table name is exactly eight bytes; names shorter than eight bytes are
 * NUL-padded, while eight-byte names have no terminator in the header.
 */
#define KOSLOAD_EXPORT_TABLE_MAGIC 0x41C00000
#define KOSLOAD_IMPORT_TABLE_MAGIC 0x41E00000
#define KOSLOAD_IMPORT_STUB_JR_RA  0x03E00008
#define KOSLOAD_IOP_LINK_TABLE_HEADER_SIZE 20

typedef struct kosload_iop_link_table_header {
    unsigned int magic;                         /* +0x00 */
    struct kosload_iop_link_table_header *next; /* +0x04, written by LOADCORE */
    unsigned short version;                     /* +0x08 */
    unsigned short mode;                        /* +0x0a, LOADCORE flags */
    char name[8];                               /* +0x0c */
} kosload_iop_link_table_header_t;

typedef kosload_iop_link_table_header_t kosload_iop_export_table_t;
typedef kosload_iop_link_table_header_t kosload_iop_import_table_t;

enum {
    KOSLOAD_IOP_LINK_TABLE_HEADER_LAYOUT_CHECK =
        1 / (sizeof(kosload_iop_link_table_header_t) == 20)
};

/* Declare an `_irx_id` global symbol that elf2irx uses to synthesize
 * the .iopmod section at IRX-conversion time.  This matches the
 * pattern observed in working IRXs: the source does NOT emit a .iopmod
 * section directly — instead it provides:
 *
 *   _irx_id  : a 2-word struct in .data containing
 *              { const char *name, uint32_t version }
 *   __bss_start : the linker-provided BSS base, used by elf2irx to
 *                 compute the GP register value (= __bss_start + 0x7fe8)
 *
 * elf2irx then synthesizes a 27-byte .iopmod with:
 *   +0x00  irx_id-struct address
 *   +0x04  entry point (e_entry)
 *   +0x08  gp value
 *   +0x0c  resolved name pointer (name string in .rodata)
 *   +0x10  0x14 (loader magic)
 *   +0x14  total bss size
 *   +0x18  version (3-byte LE: major<<8 | minor in low 2 bytes)
 *
 * Hardware-tested: our first probe attempt provided its own .iopmod
 * (with zeroes at +0x08/+0x0c/+0x10) and LOADFILE hung on _start
 * because the GP setup was wrong.  Adopting the synthesized layout
 * is what works.
 *
 * Place exactly one KOSLOAD_IRX_ID() at file scope per IRX. */
#define KOSLOAD_IRX_ID(name_str, major_v, minor_v)                                   \
    static const char _kosload_irx_id_name[] = name_str;                             \
    struct kosload_irx_id_t {                                                        \
        const char    *name;                                                         \
        unsigned short version;                                                      \
    };                                                                               \
    struct kosload_irx_id_t _irx_id __attribute__((used, section(".sdata"))) = {     \
        _kosload_irx_id_name,                                                        \
        (unsigned short)(((major_v) << 8) | (minor_v)),                              \
    }

/* ---------------------------------------------------------------- *
 * Imports from other IRX libraries (intrman, sysmem, dmacman, etc.)
 *
 * Format derived from binary RE of BIOS modules and LOADCORE's
 * import-linker. Each import block in our IRX's .text section
 * consists of:
 *
 *   +0x00  uint32_t  magic     = 0x41E00000
 *   +0x04  uint32_t  next      = 0            (LOADCORE runtime link)
 *   +0x08  uint16_t  version   = (major << 8) | minor
 *   +0x0a  uint16_t  mode      = 0            (LOADCORE runtime flags)
 *   +0x0c  char[8]   libname   (NUL-padded; an exactly-8-char name
 *                               like "loadcore" has no NUL terminator)
 *   +0x14  ...stubs...         (8 bytes each; jr $ra; li $0, N)
 *
 * LOADCORE export #8 (LinkImports) scans the loaded module for the
 * 0x41E00000 magic, matches `libname` and a compatible version against
 * its registered-libraries list, and then iterates 8-byte stubs after
 * the header until it hits a zero terminator. For each stub it reads
 * the export slot number from the second word's low 16 bits, indexes the
 * export table's function-pointer array, and overwrites the first word
 * with `j export_addr`. The second word remains `li $0, N`, which is a
 * harmless delay-slot instruction and still records the numeric slot.
 *
 * Pre-link the stub is `jr $ra; li $0, N` — calling it returns to the
 * caller immediately with v0 undefined. Post-link it is
 * `j addr; li $0, N` — calling it dispatches to the real function with
 * normal MIPS o32 argument passing.
 *
 * Usage:
 *
 *   KOSLOAD_IMPORT_TABLE(intrman, 1, 2);
 *   KOSLOAD_IMPORT(intrman, 17, CpuSuspendIntr, int, (int *state));
 *   KOSLOAD_IMPORT(intrman, 18, CpuResumeIntr,  int, (int state));
 *   KOSLOAD_IMPORT_TABLE_END(intrman);
 *
 *   int _start(int argc, char **argv) {
 *       int s;
 *       CpuSuspendIntr(&s);
 *       ...
 *       CpuResumeIntr(s);
 *       return KOSLOAD_MODULE_NO_RESIDENT_END;
 *   }
 *
 * Constraints:
 *   - Each library's KOSLOAD_IMPORT_TABLE must appear EXACTLY ONCE
 *     per IRX, must be followed by KOSLOAD_IMPORT_TABLE_END for the
 *     same library (the END emits an 8-byte zero terminator that
 *     LOADCORE's verifier requires; without it the whole block gets
 *     rejected — see the END macro's comment for the verifier-
 *     lookahead detail), and all KOSLOAD_IMPORT calls for that
 *     library must sit BETWEEN the TABLE and TABLE_END in the same
 *     translation unit (they all emit into the same named section
 *     so they end up adjacent post-assembly).
 *   - Library name passed as a C identifier (not a string literal).
 *     Length must be 1..8 chars; longer names trip a `.org` overflow
 *     in the assembler.  Names < 8 chars are NUL-padded automatically.
 *   - Export number must fit in a MIPS `li` immediate (16 bits signed,
 *     so 0..32767).  Export tables we've observed don't exceed ~30. */

/* Emit the 20-byte header for one library's import block.  Place
 * exactly once per library before any KOSLOAD_IMPORT for that
 * library.  Local label `9` is reused safely — GAS scopes numeric
 * local labels to the most-recent definition. */
#define KOSLOAD_IMPORT_TABLE(libname, major_ver, minor_ver)                           \
    __asm__(".pushsection .text.imports." #libname ", \"ax\", @progbits\n"            \
            ".balign 4\n"                                                             \
            "9:\n"                                                                    \
            ".long 0x41E00000\n"                                                      \
            ".long 0\n"                                                               \
            ".long ((" #major_ver ") << 8) | (" #minor_ver ")\n"                      \
            ".ascii \"" #libname "\"\n"                                               \
            ".org 9b + 20\n"                                                          \
            ".popsection\n")

/* Emit one 8-byte stub for an imported function and declare its C
 * prototype.  After this macro, `sym(...)` is callable from C with
 * the given return type and argument list.  Pre-link the stub is a
 * no-op return; post-link LOADCORE patches it to jump to the real
 * function. */
#define KOSLOAD_IMPORT(libname, num, sym, ret, args)                                  \
    extern ret sym args;                                                              \
    __asm__(".pushsection .text.imports." #libname ", \"ax\", @progbits\n"            \
            ".global " #sym "\n"                                                      \
            ".type " #sym ", @function\n"                                             \
            ".set push\n"                                                             \
            ".set noreorder\n" #sym ":\n"                                             \
            "    jr $ra\n"                                                            \
            "    li $0, " #num "\n"                                                   \
            ".set pop\n"                                                              \
            ".size " #sym ", . - " #sym "\n"                                          \
            ".popsection\n")

/* ---------------------------------------------------------------- *
 * Thread descriptor for thbase::CreateThread (export #4).
 *
 * 20-byte struct passed by pointer to CreateThread.  Layout RE'd
 * from binary disassembly of thbase's CreateThread (LOADCORE
 * extracted/11_Multi_Thread_Manager.bin .text 0xc5c) plus a worked
 * call site in extracted/22_RebootByEE.bin _start.
 *
 * Field constraints (validated by CreateThread):
 *   attr      — `(attr & 0x1cfffff7) == 0` required.
 *               KOSLOAD_TH_C = 0x02000000 = bit 25 = "C-language
 *               thread" attribute.  The value Sony's RebootByEE
 *               passes verbatim.
 *   option    — opaque, copied to the kernel thread struct.  Pass 0.
 *   thread    — function pointer; must be 4-byte aligned (low 2
 *               bits = 0).  Signature: `void thread_fn(void *arg)`,
 *               where arg comes from StartThread's second parameter.
 *   stacksize — bytes; must be >= 304.  CreateThread rounds up to
 *               the nearest multiple of 256.  Common values:
 *               2 KB (small drivers) to 16 KB (heavier work).
 *   priority  — must be in [1, 126].  Lower = higher priority.
 *               Sony BIOS modules use 10..127. */
typedef struct kosload_iop_thread {
    int   attr;      /* +0 */
    int   option;    /* +4 */
    void *thread;    /* +8 — `void (*)(void *arg)` */
    int   stacksize; /* +12 */
    int   priority;  /* +16 */
} kosload_iop_thread_t;

#define KOSLOAD_TH_C  0x02000000 /* C-language thread attribute */

/* Emit 8 zero bytes after the last stub for one library's import
 * block.  REQUIRED after the last KOSLOAD_IMPORT for each library —
 * without it, LOADCORE's import verifier (LOADCORE .text 0xf2c)
 * rejects the whole block.
 *
 * Why: the verifier walks 8-byte stubs after the header.  After
 * finding the last valid stub it reads `lw v0, 0(v1)` (next stub's
 * first word) — if zero, exit loop.  Then reads `lw v0, 4(v1)` (next
 * stub's SECOND word) — if non-zero, REJECT the entire block.  So we
 * need at least 8 zero bytes after the last stub for the look-ahead
 * read to find zero on both sides.
 *
 * BIOS blocks get this for free from `.balign 16` padding between
 * import blocks; ours don't because each library has its own
 * `.text.imports.<libname>` section and the next section in memory
 * may not be zero (in our probe, `.rodata` follows immediately and
 * starts with the test-string bytes). */
#define KOSLOAD_IMPORT_TABLE_END(libname)                                             \
    __asm__(".pushsection .text.imports." #libname ", \"ax\", @progbits\n"            \
            ".long 0\n"                                                               \
            ".long 0\n"                                                               \
            ".popsection\n")

/* ---------------------------------------------------------------- *
 * Library exports — register a library other IRXs can import from.
 *
 * Symmetric with the import block above (same 20-byte header layout)
 * but with the magic flipped C↔E. RE'd from BIOS LOADCORE's
 * `RegisterLibraryEntries` (export #6) + the LinkImports walker.
 *
 * Format (each `KOSLOAD_EXPORT_TABLE` macro emits this header into
 * a per-library section that the EXPORT macros below extend):
 *
 *   +0x00  uint32_t  magic    = 0x41C00000   (vs 0x41E00000 for imports)
 *   +0x04  uint32_t  next_lib = 0            (LOADCORE writes at register time)
 *   +0x08  uint16_t  version  = (major << 8) | minor
 *   +0x0a  uint16_t  mode     = 0            (LOADCORE runtime flags)
 *   +0x0c  char[8]   name                    NUL-padded, exactly 8 bytes
 *   +0x14  void *    funcs[]                 NULL-terminated function pointers,
 *                                            one per export slot (0-indexed)
 *
 * The `next_lib` slot at +0x04 starts as 0 in the linked image;
 * LOADCORE overwrites it at register time as the library threads onto
 * its registered-library list (head at LOADCORE .bss[0x1c70]).
 *
 * Slot ordering: the FIRST KOSLOAD_EXPORT (or KOSLOAD_EXPORT_NULL)
 * after KOSLOAD_EXPORT_TABLE is slot 0, the second is slot 1, etc.
 * Use KOSLOAD_EXPORT_NULL to skip slots before the slot you want
 * to register at — e.g. to register a function at export #9 you
 * write 9 KOSLOAD_EXPORT_NULL calls then one KOSLOAD_EXPORT.
 *
 * Usage:
 *
 *   // Function we want to expose.  Must be a non-static file-scope
 *   // symbol so the assembler can reference it by name from .data.
 *   int dev9_read_eeprom(unsigned short *out)
 *   {
 *       // ... real bit-bang ...
 *       return 0;
 *   }
 *
 *   // Stub for unused slots — most libraries register a no-op
 *   // entrypoint at slot 0/1 (legacy "library init/exit" hooks)
 *   // and we wire those to a return-stub.
 *   int dev9_unused(void)
 *   {
 *       return 0;
 *   }
 *
 *   KOSLOAD_EXPORT_TABLE(dev9, 1, 9);
 *   KOSLOAD_EXPORT(dev9, dev9_unused);         // 0  ← stub addr
 *   KOSLOAD_EXPORT(dev9, dev9_unused);         // 1
 *   KOSLOAD_EXPORT(dev9, dev9_unused);         // 2
 *   KOSLOAD_EXPORT(dev9, dev9_unused);         // 3
 *   ...
 *   KOSLOAD_EXPORT(dev9, dev9_read_eeprom);    // 9 = real export
 *   KOSLOAD_EXPORT_TABLE_END(dev9);            // emits NULL terminator
 *
 * GOTCHA: do NOT use KOSLOAD_EXPORT_NULL for slots that come
 * BEFORE a real export — funcs[] is NULL-terminated, so a NULL slot
 * mid-array makes LOADCORE's import-resolution walker stop there
 * and never find later slots.  Use a dev9_unused-style return-stub
 * pointer for skip slots instead, like Sony's INET_SMAP_driver does
 * (all 4 of its `smap` library slots point at one shared `jr ra;
 * nop` stub).  KOSLOAD_EXPORT_NULL only makes sense if you want to
 * intentionally truncate the array; in practice the auto-emitted
 * KOSLOAD_EXPORT_TABLE_END terminator is what you want.
 *
 *   // Imports we need to actually call RegisterLibraryEntries —
 *   // loadcore #6 takes the export-table address and threads it
 *   // onto the registered-library list.
 *   KOSLOAD_IMPORT_TABLE(loadcore, 1, 1);
 *   KOSLOAD_IMPORT(loadcore, 6, RegisterLibraryEntries, int,
 *                  (void *table));
 *   KOSLOAD_IMPORT(loadcore, 7, ReleaseLibraryEntries,  int,
 *                  (void *table));
 *   KOSLOAD_IMPORT_TABLE_END(loadcore);
 *
 *   int _start(int argc, char **argv)
 *   {
 *       (void)argc; (void)argv;
 *       if (RegisterLibraryEntries(KOSLOAD_EXPORT_TABLE_PTR(dev9)) < 0)
 *           return KOSLOAD_MODULE_NO_RESIDENT_END;
 *       return KOSLOAD_MODULE_RESIDENT_END;
 *   }
 *
 * Constraints (mirrors the import-side constraints):
 *   - Library name passed as a C identifier (not a string literal),
 *     1..8 chars; longer trips an assembler `.org` overflow.  Names
 *     < 8 chars are NUL-padded automatically.
 *   - Each library's KOSLOAD_EXPORT_TABLE must appear exactly once
 *     and be paired with KOSLOAD_EXPORT_TABLE_END (the END emits the
 *     NULL-terminator that LinkImports requires to find funcs[]'s
 *     end).
 *   - All KOSLOAD_EXPORT / KOSLOAD_EXPORT_NULL for one library must
 *     sit between TABLE and TABLE_END in the same translation unit
 *     (they all emit into the same named section so they end up
 *     adjacent post-assembly).
 *   - The exported symbol must be a non-static file-scope function
 *     (or any symbol with a linker-visible name).  Static functions
 *     get name-mangled / hidden and the assembler can't reference
 *     them by name. */

/* Begin export table — emits the 20-byte header into a per-library
 * .data.kosload_export.<libname> section and exposes
 * `_kosload_export_<libname>` as a global symbol pointing at the
 * table's first byte (= the magic word).  Pass that address to
 * RegisterLibraryEntries from _start (via KOSLOAD_EXPORT_TABLE_PTR
 * for ergonomics). */
#define KOSLOAD_EXPORT_TABLE(libname, major_v, minor_v)                               \
    __asm__(".pushsection .data.kosload_export." #libname ", \"aw\", @progbits\n"     \
            ".balign 4\n"                                                             \
            "9:\n"                                                                    \
            ".global _kosload_export_" #libname "\n"                                  \
            "_kosload_export_" #libname ":\n"                                         \
            ".long 0x41C00000\n"                                                      \
            ".long 0\n"                                                               \
            ".long ((" #major_v ") << 8) | (" #minor_v ")\n"                          \
            ".ascii \"" #libname "\"\n"                                               \
            ".org 9b + 20\n"                                                          \
            ".popsection\n")

/* Append one funcs[] slot pointing at `sym`.  Slot index is implicit
 * — it equals the count of preceding KOSLOAD_EXPORT[_NULL] calls for
 * the same library since the matching KOSLOAD_EXPORT_TABLE. */
#define KOSLOAD_EXPORT(libname, sym)                                                   \
    __asm__(".pushsection .data.kosload_export." #libname ", \"aw\", @progbits\n"      \
            ".long " #sym "\n"                                                         \
            ".popsection\n")

/* Append a NULL slot to funcs[].  WARNING: use this only at the
 * very end of the array (right before KOSLOAD_EXPORT_TABLE_END), or
 * not at all.  funcs[] is NULL-terminated and LOADCORE's import
 * walker stops at the first 0 it finds — a NULL slot before a real
 * export hides everything past it.  For "skip slot N" use a return-
 * stub function (Sony does this with a single `jr ra; nop` shared
 * across all unused exports).  Hardware-confirmed Stage 3.5c. */
#define KOSLOAD_EXPORT_NULL(libname)                                                   \
    __asm__(".pushsection .data.kosload_export." #libname ", \"aw\", @progbits\n"      \
            ".long 0\n"                                                                \
            ".popsection\n")

/* End export table — emits the NULL terminator that
 * RegisterLibraryEntries / LinkImports use to find the end of
 * funcs[].  REQUIRED after the last KOSLOAD_EXPORT[_NULL] for each
 * library; without it LinkImports walks past the array and patches
 * stubs against whatever happens to be next in memory. */
#define KOSLOAD_EXPORT_TABLE_END(libname)                                              \
    __asm__(".pushsection .data.kosload_export." #libname ", \"aw\", @progbits\n"      \
            ".long 0\n"                                                                \
            ".popsection\n")

/* Yield a (void *) suitable for passing to RegisterLibraryEntries
 * or ReleaseLibraryEntries.  Resolves to the address of the export
 * table's magic word (the first byte of the header). */
#define KOSLOAD_EXPORT_TABLE_PTR(libname)                                              \
    ((void *)({                                                                        \
        extern char _kosload_export_##libname[];                                       \
        _kosload_export_##libname;                                                     \
    }))

#endif /* KOSLOAD_IOP_H */
