/*
 * Application: elf2prx
 *
 * Why we need it:
 * The linker creates an ordinary ELF executable, but the PSP loads movable
 * program modules in PRX format. This tool changes the linked ELF into the PRX
 * that the PSP can place at an available address in memory.
 *
 * What it does:
 * elf2prx reads a 32-bit, little-endian MIPS ELF linked as though it starts at
 * address zero. It marks the file as a PSP module, points the loader to the
 * program's module-information block, and converts the linker's address-fixup
 * records into the smaller form used by a PRX. Debugging and other link-only
 * tables are removed because the PSP does not need them to run the program.
 *
 * Input requirements:
 *   - one loadable program area linked at address zero
 *   - retained address-fixup and symbol information
 *   - a 52-byte `.rodata.sceModuleInfo` block describing the module
 *
 * Output file structure:
 *   - ELF header identifying the file as a PSP PRX
 *   - one load-description header
 *   - module information, program code, and initialized data
 *   - PSP address-fixup tables
 *   - section names and section descriptions
 *
 * Uninitialized data is described by its size and is filled with zero when the
 * module is loaded. The PRX produced here becomes the executable payload that
 * the `pbp` tool places inside `EBOOT.PBP`.
 *
 * Usage:
 *   elf2prx [-v] -o output.prx input.elf
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>

/* ---- ELF32 constants ---- */

#define ET_EXEC        2
#define ET_PSP_PRX     0xFFA0
#define EM_MIPS        8
#define PT_LOAD        1

#define SHT_NULL       0
#define SHT_PROGBITS   1
#define SHT_SYMTAB     2
#define SHT_STRTAB     3
#define SHT_NOBITS     8
#define SHT_REL        9
#define SHT_PRXRELOC   0x700000A0

#define SHF_ALLOC      0x2
#define SHF_INFO_LINK  0x40

#define SHN_UNDEF      0
#define SHN_ABS        0xFFF1

#define R_MIPS_NONE    0

/* binutils encodes the Allegrex core in the EF_MIPS_MACH field.  Modern
 * readelf prints this value as "gs464" because the same encoding was later
 * reused for a Loongson core; PSP modules all carry it. */
#define EF_MIPS_MACH_ALLEGREX 0x00A20000

#define MODINFO_SECTION ".rodata.sceModuleInfo"

/* ---- little-endian accessors (host endianness independent) ---- */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

#if defined(_MSC_VER)
#define ELF2PRX_NORETURN __declspec(noreturn)
#elif defined(__GNUC__) || defined(__clang__)
#define ELF2PRX_NORETURN __attribute__((noreturn))
#else
#define ELF2PRX_NORETURN
#endif

static ELF2PRX_NORETURN void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "elf2prx: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static uint32_t checked_add_u32(uint32_t a, uint32_t b, const char *what) {
    if(a > UINT32_MAX - b)
        die("%s overflows ELF32 address space", what);
    return a + b;
}

static uint32_t checked_mul_u32(uint32_t a, uint32_t b, const char *what) {
    if(a != 0 && b > UINT32_MAX / a)
        die("%s overflows ELF32 address space", what);
    return a * b;
}

static int is_power_of_two_u32(uint32_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static uint32_t align_up(uint32_t value, uint32_t alignment, const char *what) {
    if(!is_power_of_two_u32(alignment))
        die("%s has invalid alignment %u", what, alignment);
    return checked_add_u32(value, alignment - 1, what) & ~(alignment - 1);
}

static void require_range(uint64_t offset, uint64_t size, uint64_t limit,
                          const char *what) {
    if(offset > limit || size > limit - offset)
        die("%s lies outside its containing data", what);
}

static size_t bounded_strlen(const char *text, size_t maximum) {
    size_t length = 0;
    while(length < maximum && text[length] != '\0')
        length++;
    return length;
}

static size_t input_string_length(const uint8_t *input, uint32_t table_offset,
                                  uint32_t table_size, uint32_t string_offset,
                                  const char *what) {
    if(string_offset >= table_size)
        die("%s has an invalid string-table offset", what);

    const char *text = (const char *)input + table_offset + string_offset;
    size_t available = (size_t)(table_size - string_offset);
    size_t length = bounded_strlen(text, available);
    if(length == available)
        die("%s is not NUL-terminated inside its string table", what);
    return length;
}

/* ---- growable output buffer ---- */

typedef struct {
    uint8_t *p;
    uint32_t len, cap;
} buf_t;

static void buf_need(buf_t *b, uint32_t n) {
    uint32_t required = checked_add_u32(b->len, n, "output buffer");
    if(required <= b->cap)
        return;
    uint32_t cap = b->cap ? b->cap : 4096;
    while(cap < required) {
        if(cap > UINT32_MAX / 2) {
            cap = required;
            break;
        }
        cap *= 2;
    }
    uint8_t *grown = realloc(b->p, cap);
    if(!grown)
        die("out of memory while growing output to %u bytes", cap);
    b->p = grown;
    b->cap = cap;
}
static void buf_put(buf_t *b, const void *d, uint32_t n) {
    buf_need(b, n);
    if(n != 0)
        memcpy(b->p + b->len, d, n);
    b->len = checked_add_u32(b->len, n, "output buffer");
}
static void buf_pad(buf_t *b, uint32_t a) {
    uint32_t want = align_up(b->len, a, "output padding");
    buf_need(b, want - b->len);
    while(b->len < want)
        b->p[b->len++] = 0;
}

/* ---- section bookkeeping ---- */

typedef struct {
    const char *name;
    uint32_t type, flags, addr, off, size, link, info, addralign, entsize;
} sec_t;

/* One output section: either copied straight out of the input segment, or a
 * converted relocation table held in `reloc`. */
typedef struct {
    const char *name;
    uint32_t type, flags, addr, off, size, link, info, addralign, entsize;
    uint8_t *reloc;        /* NULL for sections carried inside the segment */
    uint32_t reloc_len;
    uint32_t name_off;
} out_t;

int main(int argc, char **argv) {
    const char *in_path = NULL, *out_path = NULL;
    int verbose = 0;

    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "-o")) {
            if(out_path)
                die("output path specified more than once");
            if(i + 1 >= argc)
                die("-o requires an output path");
            out_path = argv[++i];
        } else if(!strcmp(argv[i], "-v")) {
            verbose = 1;
        } else if(argv[i][0] == '-') {
            die("unknown option %s", argv[i]);
        } else {
            if(in_path)
                die("more than one input file specified");
            in_path = argv[i];
        }
    }
    if(!in_path || !out_path) {
        fprintf(stderr, "usage: elf2prx [-v] -o output.prx input.elf\n");
        return 1;
    }

    /* ---- read input ---- */

    FILE *f = fopen(in_path, "rb");
    if(!f) die("cannot open input");
    if(fseek(f, 0, SEEK_END) != 0)
        die("cannot seek input");
    long flen = ftell(f);
    if(flen < 0)
        die("cannot determine input size");
    if(flen < 52)
        die("input too small to be an ELF");
    if((uint64_t)flen > UINT32_MAX)
        die("input is too large for ELF32");
    if(fseek(f, 0, SEEK_SET) != 0)
        die("cannot rewind input");
    uint8_t *e = malloc((size_t)flen);
    if(!e) die("out of memory");
    if(fread(e, 1, (size_t)flen, f) != (size_t)flen) die("short read");
    fclose(f);
    uint32_t elen = (uint32_t)flen;

    /* ---- validate ELF header ---- */

    if(memcmp(e, "\177ELF", 4))          die("not an ELF file");
    if(e[4] != 1)                        die("not ELF32");
    if(e[5] != 1)                        die("not little-endian");
    if(e[6] != 1)                        die("unsupported ELF identification version");
    if(rd16(e + 16) != ET_EXEC)          die("input is not ET_EXEC (link the image as a static executable)");
    if(rd16(e + 18) != EM_MIPS)          die("not a MIPS ELF");
    if(rd32(e + 20) != 1)                die("unsupported ELF header version");

    uint32_t e_entry = rd32(e + 24);
    uint32_t e_phoff = rd32(e + 28);
    uint32_t e_shoff = rd32(e + 32);
    uint32_t e_flags = rd32(e + 36);
    uint16_t e_ehsize = rd16(e + 40);
    uint16_t e_phentsize = rd16(e + 42), e_phnum = rd16(e + 44);
    uint16_t e_shentsize = rd16(e + 46), e_shnum = rd16(e + 48), e_shstrndx = rd16(e + 50);

    if(e_ehsize != 52)                         die("unexpected ELF header size");
    if(e_phentsize != 32 || e_shentsize != 40) die("unexpected ELF table entry sizes");
    if(!e_shnum || e_shstrndx >= e_shnum)      die("missing section header table");
    require_range(e_shoff, (uint64_t)e_shnum * 40, elen, "section header table");
    require_range(e_phoff, (uint64_t)e_phnum * 32, elen, "program header table");

    /* ---- locate the single PT_LOAD segment ---- */

    const uint8_t *load = NULL;
    for(uint16_t i = 0; i < e_phnum; i++) {
        const uint8_t *ph = e + e_phoff + i * 32;
        if(rd32(ph) != PT_LOAD)
            continue;
        if(load) die("more than one PT_LOAD segment; a PRX must have exactly one");
        load = ph;
    }
    if(!load) die("no PT_LOAD segment");

    uint32_t p_offset = rd32(load + 4), p_vaddr = rd32(load + 8);
    uint32_t p_filesz = rd32(load + 16), p_memsz = rd32(load + 20);
    uint32_t p_align = rd32(load + 28);

    if(p_vaddr != 0)
        die("PT_LOAD p_vaddr is not 0: a PRX must be linked at base 0 so the "
            "loader can relocate it (see the linker script)");
    if(p_filesz > p_memsz)
        die("PT_LOAD file size exceeds its memory size");
    if(p_align > 1 && !is_power_of_two_u32(p_align))
        die("PT_LOAD has an invalid alignment");
    if(p_align > 1 && (p_offset & (p_align - 1)) != (p_vaddr & (p_align - 1)))
        die("PT_LOAD file and memory offsets violate its alignment");
    require_range(p_offset, p_filesz, elen, "PT_LOAD segment");
    if(e_entry >= p_memsz)
        die("entry point lies outside the PT_LOAD segment");

    /* ---- read section headers ---- */

    sec_t *sec = calloc(e_shnum, sizeof(*sec));
    if(!sec) die("out of memory");

    for(uint16_t i = 0; i < e_shnum; i++) {
        const uint8_t *sh = e + e_shoff + (uint32_t)i * 40;
        sec[i].type      = rd32(sh + 4);
        sec[i].flags     = rd32(sh + 8);
        sec[i].addr      = rd32(sh + 12);
        sec[i].off       = rd32(sh + 16);
        sec[i].size      = rd32(sh + 20);
        sec[i].link      = rd32(sh + 24);
        sec[i].info      = rd32(sh + 28);
        sec[i].addralign = rd32(sh + 32);
        sec[i].entsize   = rd32(sh + 36);

        if(sec[i].addralign != 0 && !is_power_of_two_u32(sec[i].addralign))
            die("section %u has invalid alignment %u", i, sec[i].addralign);
        if(sec[i].type != SHT_NOBITS)
            require_range(sec[i].off, sec[i].size, elen, "section data");
    }

    if(sec[0].type != SHT_NULL)
        die("section zero is not SHT_NULL");

    sec_t *shstr = &sec[e_shstrndx];
    if(shstr->type != SHT_STRTAB)
        die("section-name table is not SHT_STRTAB");
    uint32_t shstr_off = shstr->off;
    uint32_t shstr_size = shstr->size;
    for(uint16_t i = 0; i < e_shnum; i++) {
        const uint8_t *sh = e + e_shoff + (uint32_t)i * 40;
        uint32_t nameoff = rd32(sh);
        char description[48];
        snprintf(description, sizeof(description), "section %u name", i);
        input_string_length(e, shstr_off, shstr_size, nameoff, description);
        sec[i].name = (const char *)(e + shstr_off + nameoff);
    }

    /* ---- symbol table (needed to drop SHN_ABS relocations) ---- */

    const uint8_t *symtab = NULL;
    uint32_t symcount = 0;
    int symtab_idx = -1;
    for(uint16_t i = 0; i < e_shnum; i++) {
        if(sec[i].type != SHT_SYMTAB)
            continue;
        if(symtab_idx >= 0)
            die("multiple symbol tables are not supported");
        if(sec[i].entsize != 16 || sec[i].size % 16 != 0)
            die("symbol table has an invalid entry size or length");
        if(sec[i].link >= e_shnum || sec[sec[i].link].type != SHT_STRTAB)
            die("symbol table does not link to a string table");
        symtab_idx = i;
        symtab = e + sec[i].off;
        symcount = sec[i].size / 16;
        if(sec[i].info > symcount)
            die("symbol table local-symbol count is out of range");
    }
    if(!symtab)
        die("no .symtab: relink with --emit-relocs and without -s/--strip-all");

    /* Validate every relocation table once and map it to its target.  The
     * output loop can then avoid repeatedly scanning an attacker-controlled
     * section table, and duplicate tables cannot be silently ignored. */
    int *relidx = malloc((size_t)e_shnum * sizeof(*relidx));
    if(!relidx)
        die("out of memory");
    for(uint16_t i = 0; i < e_shnum; i++)
        relidx[i] = -1;

    for(uint16_t i = 1; i < e_shnum; i++) {
        if(sec[i].type != SHT_REL)
            continue;
        if(sec[i].entsize != 8 || sec[i].size % 8 != 0)
            die("relocation section %s has an invalid entry size or length",
                sec[i].name);
        if(sec[i].link != (uint32_t)symtab_idx)
            die("relocation section %s does not link to the symbol table",
                sec[i].name);
        if(sec[i].info == 0 || sec[i].info >= e_shnum)
            die("relocation section %s has an invalid target section",
                sec[i].name);
        if(relidx[sec[i].info] >= 0)
            die("multiple relocation sections target %s", sec[sec[i].info].name);
        relidx[sec[i].info] = i;

        const sec_t *target = &sec[sec[i].info];
        uint32_t count = sec[i].size / 8;
        if(count != 0 && target->type == SHT_NOBITS)
            die("relocation section %s targets SHT_NOBITS", sec[i].name);
        for(uint32_t k = 0; k < count; k++) {
            const uint8_t *rel = e + sec[i].off + k * 8;
            uint32_t offset = rd32(rel);
            uint32_t info = rd32(rel + 4);
            uint32_t type = info & 0xff;
            uint32_t symbol = info >> 8;

            if(type == R_MIPS_NONE)
                continue;
            if(offset < target->addr ||
               (uint64_t)(offset - target->addr) + 4 > target->size)
                die("relocation %u in %s patches outside section %s", k,
                    sec[i].name, target->name);
            if(symbol >= symcount)
                die("relocation %u in %s references invalid symbol %u", k,
                    sec[i].name, symbol);

            uint16_t shndx = rd16(symtab + symbol * 16 + 14);
            if(shndx != SHN_ABS && shndx != SHN_UNDEF && shndx >= e_shnum)
                die("relocation %u in %s references a symbol with an invalid section",
                    k, sec[i].name);
            if((target->flags & SHF_ALLOC) && shndx < e_shnum &&
               !(sec[shndx].flags & SHF_ALLOC))
                die("relocation %u in %s references a non-allocated symbol",
                    k, sec[i].name);
        }
    }

    /* ---- module descriptor ---- */

    int modinfo_idx = -1;
    for(uint16_t i = 0; i < e_shnum; i++) {
        if(!strcmp(sec[i].name, MODINFO_SECTION)) {
            if(modinfo_idx >= 0)
                die("multiple " MODINFO_SECTION " sections");
            modinfo_idx = i;
        }
    }
    if(modinfo_idx < 0) die("no " MODINFO_SECTION " section");
    if(sec[modinfo_idx].size != 0x34)
        die(MODINFO_SECTION " is not 52 bytes: SceModuleInfo layout is wrong");
    if(!(sec[modinfo_idx].flags & SHF_ALLOC))
        die(MODINFO_SECTION " is not allocated");
    if(sec[modinfo_idx].type == SHT_NOBITS)
        die(MODINFO_SECTION " has no file-backed data");

    /* ---- plan the output layout ---- */

    uint32_t new_phoff  = 52;
    uint32_t seg_off    = align_up(checked_add_u32(new_phoff, 32,
                                                   "program header table"),
                                   16, "LOAD segment offset");

    out_t *out = calloc((size_t)e_shnum * 2 + 2, sizeof(*out));
    if(!out) die("out of memory");
    uint32_t nout = 0;
    int *outidx = calloc(e_shnum, sizeof(*outidx));   /* input index -> output index */
    if(!outidx) die("out of memory");
    for(uint16_t i = 0; i < e_shnum; i++)
        outidx[i] = -1;

    /* index 0: the mandatory NULL section */
    out[nout].name = "";
    nout++;

    uint32_t dropped_abs = 0, dropped_none = 0, total_relocs = 0;

    /* Allocated sections keep their order; each converted relocation table is
     * emitted directly after the section it patches, matching the section
     * ordering shipping PRX modules use. */
    for(uint16_t i = 1; i < e_shnum; i++) {
        if(!(sec[i].flags & SHF_ALLOC))
            continue;
        require_range(sec[i].addr, sec[i].size, p_memsz,
                      "allocated section memory range");
        if(sec[i].off < p_offset)
            die("allocated section lies before the PT_LOAD segment");
        uint32_t segment_relative_off = sec[i].off - p_offset;
        if(sec[i].type != SHT_NOBITS) {
            require_range(segment_relative_off, sec[i].size, p_filesz,
                          "allocated section file range");
            if(segment_relative_off != sec[i].addr)
                die("allocated section file and memory offsets disagree");
        } else if(segment_relative_off > p_filesz) {
            die("allocated SHT_NOBITS section has an invalid file offset");
        }

        outidx[i] = (int)nout;
        out[nout].name      = sec[i].name;
        out[nout].type      = sec[i].type;
        out[nout].flags     = sec[i].flags;
        out[nout].addr      = sec[i].addr;
        out[nout].off       = checked_add_u32(seg_off, segment_relative_off,
                                              "allocated section output offset");
        out[nout].size      = sec[i].size;
        out[nout].link      = 0;
        out[nout].info      = 0;
        out[nout].addralign = sec[i].addralign;
        out[nout].entsize   = sec[i].entsize;
        nout++;

        /* Convert the relocation table that targets this section, if any. */
        if(relidx[i] >= 0) {
            uint16_t r = (uint16_t)relidx[i];

            uint32_t n = sec[r].size / 8;
            /* Test n, not n * 8: gcc reads the product in a boolean context as
             * a probable `n && 8` typo (-Wint-in-bool-context) and warns.  The
             * two are equivalent here, since n * 8 is zero exactly when n is. */
            uint32_t conv_size = checked_mul_u32(n, 8,
                                                 "converted relocation table");
            uint8_t *conv = malloc(conv_size ? conv_size : 1);
            if(!conv) die("out of memory");
            uint32_t kept = 0;

            for(uint32_t k = 0; k < n; k++) {
                const uint8_t *rel = e + sec[r].off + k * 8;
                uint32_t r_offset = rd32(rel);
                uint32_t r_info   = rd32(rel + 4);
                uint32_t type     = r_info & 0xff;
                uint32_t sym      = r_info >> 8;

                total_relocs = checked_add_u32(total_relocs, 1,
                                               "relocation count");
                if(type == R_MIPS_NONE) {
                    dropped_none++;
                    continue;
                }
                if(sym >= symcount) die("relocation references a symbol past the end of .symtab");

                uint16_t shndx = rd16(symtab + sym * 16 + 14);
                if(shndx == SHN_ABS || shndx == SHN_UNDEF) {
                    /* Absolute value: must not move with the load base.
                     * Derived rule -- see the provenance note in the file
                     * header; not confirmed against the reference modules. */
                    dropped_abs++;
                    continue;
                }

                /* r_offset is already the address within the (base-0) segment;
                 * r_info keeps only the relocation type. */
                wr32(conv + kept * 8, r_offset);
                wr32(conv + kept * 8 + 4, type);
                kept++;
            }

            if(kept) {
                out[nout].name      = sec[r].name;
                out[nout].type      = SHT_PRXRELOC;
                out[nout].flags     = SHF_INFO_LINK;
                out[nout].addr      = 0;
                out[nout].size      = checked_mul_u32(kept, 8,
                                                       "converted relocation table");
                out[nout].link      = 0;
                out[nout].info      = (uint32_t)outidx[i];
                out[nout].addralign = 4;
                out[nout].entsize   = 8;
                out[nout].reloc     = conv;
                out[nout].reloc_len = out[nout].size;
                nout++;
            } else {
                free(conv);
            }
        }
    }

    /* .shstrtab last, as in shipping modules. */
    uint32_t shstr_idx = nout;
    out[nout].name      = ".shstrtab";
    out[nout].type      = SHT_STRTAB;
    out[nout].addralign = 1;
    nout++;
    if(nout > UINT16_MAX)
        die("output has too many sections (%u)", nout);

    /* ---- build the section name table ---- */

    buf_t names = {0};
    { char z = 0; buf_put(&names, &z, 1); }
    for(uint32_t i = 1; i < nout; i++) {
        out[i].name_off = names.len;
        buf_put(&names, out[i].name, (uint32_t)strlen(out[i].name) + 1);
    }

    /* ---- assemble the file ---- */

    buf_t o = {0};
    buf_need(&o, seg_off);
    memset(o.p, 0, seg_off);
    o.len = seg_off;

    buf_put(&o, e + p_offset, p_filesz);     /* the LOAD segment, verbatim */

    for(uint32_t i = 1; i < nout; i++) {
        if(!out[i].reloc)
            continue;
        buf_pad(&o, 4);
        out[i].off = o.len;
        buf_put(&o, out[i].reloc, out[i].reloc_len);
    }

    buf_pad(&o, 4);
    out[shstr_idx].off  = o.len;
    out[shstr_idx].size = names.len;
    buf_put(&o, names.p, names.len);

    buf_pad(&o, 4);
    uint32_t new_shoff = o.len;
    uint32_t section_headers_size = checked_mul_u32(nout, 40,
                                                    "section header table");
    buf_need(&o, section_headers_size);
    memset(o.p + o.len, 0, section_headers_size);
    for(uint32_t i = 0; i < nout; i++) {
        uint8_t *sh = o.p + o.len + i * 40;
        wr32(sh,      i ? out[i].name_off : 0);
        wr32(sh + 4,  out[i].type);
        wr32(sh + 8,  out[i].flags);
        wr32(sh + 12, out[i].addr);
        wr32(sh + 16, out[i].off);
        wr32(sh + 20, out[i].size);
        wr32(sh + 24, out[i].link);
        wr32(sh + 28, out[i].info);
        wr32(sh + 32, out[i].addralign);
        wr32(sh + 36, out[i].entsize);
    }
    o.len = checked_add_u32(o.len, section_headers_size,
                            "section header table");

    /* ---- headers ---- */

    memcpy(o.p, e, 52);                                  /* keep e_ident and friends */
    wr16(o.p + 16, ET_PSP_PRX);
    wr32(o.p + 24, e_entry);
    wr32(o.p + 28, new_phoff);
    wr32(o.p + 32, new_shoff);
    wr32(o.p + 36, e_flags | EF_MIPS_MACH_ALLEGREX);
    wr16(o.p + 44, 1);                                   /* e_phnum */
    wr16(o.p + 48, (uint16_t)nout);
    wr16(o.p + 50, (uint16_t)shstr_idx);

    uint32_t modinfo_file_off = checked_add_u32(
        seg_off, sec[modinfo_idx].addr, "module descriptor output offset");
    uint8_t *ph = o.p + new_phoff;
    wr32(ph,      PT_LOAD);
    wr32(ph + 4,  seg_off);
    wr32(ph + 8,  0);                 /* p_vaddr: relocatable, base 0 */
    wr32(ph + 12, modinfo_file_off);  /* p_paddr: where the loader finds SceModuleInfo */
    wr32(ph + 16, p_filesz);
    wr32(ph + 20, p_memsz);
    wr32(ph + 24, rd32(load + 24));   /* p_flags */
    wr32(ph + 28, 0x10);              /* p_align */

    FILE *of = fopen(out_path, "wb");
    if(!of) die("cannot open output");
    if(fwrite(o.p, 1, o.len, of) != o.len) die("short write");
    if(fclose(of) != 0)
        die("cannot close output");

    printf("elf2prx: wrote %s (%u bytes): %u sections, %u relocs kept "
           "(%u absolute, %u none dropped), modinfo @ %#x\n",
           out_path, o.len, nout, total_relocs - dropped_abs - dropped_none,
           dropped_abs, dropped_none, modinfo_file_off);

    if(verbose) {
        for(uint32_t i = 1; i < nout; i++)
            printf("  %-28s type=%#010x off=%#08x size=%#08x info=%u\n",
                   out[i].name, out[i].type, out[i].off, out[i].size, out[i].info);
    }

    for(uint32_t i = 0; i < nout; i++)
        free(out[i].reloc);
    free(o.p);
    free(names.p);
    free(outidx);
    free(out);
    free(relidx);
    free(sec);
    free(e);
    return 0;
}
