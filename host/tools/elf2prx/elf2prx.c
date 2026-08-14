/* host/tools/elf2prx/elf2prx.c
 *
 * Clean-room ELF -> PSP PRX converter.
 *
 * Provenance: every PSP-specific constant below (ET_PSP_PRX, SHT_PRXRELOC, the
 * Allegrex e_flags bits, the p_paddr convention, the reduced r_info encoding,
 * the 52-byte descriptor) was read directly out of unencrypted reference PSP
 * firmware modules, on top of the published ELF32 layout.  No pspsdk /
 * prxgen source was consulted.  The one rule NOT observed from those binaries
 * is the SHN_ABS/SHN_UNDEF drop below, which is derived from first principles
 * and flagged as such at the point of use.
 *
 * Takes a fully linked, base-0 ET_EXEC image built with
 * `-Wl,--emit-relocs` and rewrites it into the relocatable ET_PSP_PRX form the
 * firmware module loader expects:
 *
 *   - e_type            ET_EXEC (2)      -> ET_PSP_PRX (0xFFA0)
 *   - e_flags           |= the Allegrex machine bits (0x00A20000)
 *   - phdr[0].p_paddr   -> file offset of .rodata.sceModuleInfo, which is how
 *                          the loader locates the module descriptor
 *   - .rel.<sec>        SHT_REL (9)      -> SHT_PRXRELOC (0x700000A0), with
 *                          r_info reduced to the bare relocation type
 *   - .symtab/.strtab and every non-allocated section are dropped
 *
 * PRX relocation entries keep the ELF32_Rel shape (r_offset, r_info) but carry
 * no symbol and no addend: the linker has already resolved every reference
 * against a load base of zero, and the loader simply re-applies the relocation
 * type with the real load address as the base.  That is why the input must be
 * linked at 0 — an image linked at a fixed address already has that address
 * baked into the instruction fields, and the loader would add its own base on
 * top of it.
 *
 * Relocations against SHN_ABS symbols are dropped: those values are absolute by
 * construction (hardware register addresses, linker-script constants) and must
 * survive relocation unchanged.  NOTE: this rule is reasoned, not observed --
 * the reference modules are stripped, so their symbol bindings cannot be
 * inspected to confirm it.  If a base-0 image ever relies on an absolute symbol
 * that DOES need rebasing, this is the first place to look.
 *
 *   elf2prx -o psp-load-usb.prx psp-load-usb.elf
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* ---- ELF32 constants ---- */

#define ET_EXEC        2
#define ET_PSP_PRX     0xFFA0
#define EM_MIPS        8
#define PT_LOAD        1

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

static void die(const char *m) { fprintf(stderr, "elf2prx: %s\n", m); exit(1); }
static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

/* ---- growable output buffer ---- */

typedef struct {
    uint8_t *p;
    uint32_t len, cap;
} buf_t;

static void buf_need(buf_t *b, uint32_t n) {
    if(b->len + n <= b->cap)
        return;
    uint32_t cap = b->cap ? b->cap : 4096;
    while(cap < b->len + n)
        cap *= 2;
    b->p = realloc(b->p, cap);
    if(!b->p) die("out of memory");
    b->cap = cap;
}
static void buf_put(buf_t *b, const void *d, uint32_t n) {
    buf_need(b, n);
    memcpy(b->p + b->len, d, n);
    b->len += n;
}
static void buf_pad(buf_t *b, uint32_t a) {
    uint32_t want = align_up(b->len, a);
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
        if(!strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];
        else if(!strcmp(argv[i], "-v"))            verbose = 1;
        else if(argv[i][0] == '-')                 die("unknown option");
        else                                       in_path = argv[i];
    }
    if(!in_path || !out_path) {
        fprintf(stderr, "usage: elf2prx [-v] -o output.prx input.elf\n");
        return 1;
    }

    /* ---- read input ---- */

    FILE *f = fopen(in_path, "rb");
    if(!f) die("cannot open input");
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(flen < 52) die("input too small to be an ELF");
    uint8_t *e = malloc((size_t)flen);
    if(!e) die("out of memory");
    if(fread(e, 1, (size_t)flen, f) != (size_t)flen) die("short read");
    fclose(f);
    uint32_t elen = (uint32_t)flen;

    /* ---- validate ELF header ---- */

    if(memcmp(e, "\177ELF", 4))          die("not an ELF file");
    if(e[4] != 1)                        die("not ELF32");
    if(e[5] != 1)                        die("not little-endian");
    if(rd16(e + 16) != ET_EXEC)          die("input is not ET_EXEC (link the image as a static executable)");
    if(rd16(e + 18) != EM_MIPS)          die("not a MIPS ELF");

    uint32_t e_entry = rd32(e + 24);
    uint32_t e_phoff = rd32(e + 28);
    uint32_t e_shoff = rd32(e + 32);
    uint32_t e_flags = rd32(e + 36);
    uint16_t e_phentsize = rd16(e + 42), e_phnum = rd16(e + 44);
    uint16_t e_shentsize = rd16(e + 46), e_shnum = rd16(e + 48), e_shstrndx = rd16(e + 50);

    if(e_phentsize != 32 || e_shentsize != 40) die("unexpected ELF table entry sizes");
    if(!e_shnum || e_shstrndx >= e_shnum)      die("missing section header table");
    if((uint64_t)e_shoff + (uint64_t)e_shnum * 40 > elen) die("section headers out of range");
    if((uint64_t)e_phoff + (uint64_t)e_phnum * 32 > elen) die("program headers out of range");

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

    if(p_vaddr != 0)
        die("PT_LOAD p_vaddr is not 0: a PRX must be linked at base 0 so the "
            "loader can relocate it (see the linker script)");
    if((uint64_t)p_offset + p_filesz > elen) die("segment extends past end of file");

    /* ---- read section headers ---- */

    sec_t *sec = calloc(e_shnum, sizeof(*sec));
    if(!sec) die("out of memory");
    uint32_t shstr_off = rd32(e + e_shoff + (uint32_t)e_shstrndx * 40 + 16);
    uint32_t shstr_size = rd32(e + e_shoff + (uint32_t)e_shstrndx * 40 + 20);
    if((uint64_t)shstr_off + shstr_size > elen) die("shstrtab out of range");

    for(uint16_t i = 0; i < e_shnum; i++) {
        const uint8_t *sh = e + e_shoff + (uint32_t)i * 40;
        uint32_t nameoff = rd32(sh);
        if(nameoff >= shstr_size) die("bad section name offset");
        sec[i].name      = (const char *)(e + shstr_off + nameoff);
        sec[i].type      = rd32(sh + 4);
        sec[i].flags     = rd32(sh + 8);
        sec[i].addr      = rd32(sh + 12);
        sec[i].off       = rd32(sh + 16);
        sec[i].size      = rd32(sh + 20);
        sec[i].link      = rd32(sh + 24);
        sec[i].info      = rd32(sh + 28);
        sec[i].addralign = rd32(sh + 32);
        sec[i].entsize   = rd32(sh + 36);
    }

    /* ---- symbol table (needed to drop SHN_ABS relocations) ---- */

    const uint8_t *symtab = NULL;
    uint32_t symcount = 0;
    for(uint16_t i = 0; i < e_shnum; i++) {
        if(sec[i].type != SHT_SYMTAB)
            continue;
        if(sec[i].entsize != 16) die("unexpected symbol entry size");
        if((uint64_t)sec[i].off + sec[i].size > elen) die("symtab out of range");
        symtab = e + sec[i].off;
        symcount = sec[i].size / 16;
        break;
    }
    if(!symtab)
        die("no .symtab: relink with --emit-relocs and without -s/--strip-all");

    /* ---- module descriptor ---- */

    int modinfo_idx = -1;
    for(uint16_t i = 0; i < e_shnum; i++) {
        if(!strcmp(sec[i].name, MODINFO_SECTION)) {
            modinfo_idx = i;
            break;
        }
    }
    if(modinfo_idx < 0) die("no " MODINFO_SECTION " section");
    if(sec[modinfo_idx].size != 0x34)
        die(MODINFO_SECTION " is not 52 bytes: SceModuleInfo layout is wrong");
    if(!(sec[modinfo_idx].flags & SHF_ALLOC))
        die(MODINFO_SECTION " is not allocated");

    /* ---- plan the output layout ---- */

    uint32_t new_phoff  = 52;
    uint32_t seg_off    = align_up(new_phoff + 32, 16);
    uint32_t delta      = seg_off - p_offset;   /* shift applied to in-segment offsets */

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
        if(sec[i].type != SHT_NOBITS &&
           ((uint64_t)sec[i].off < p_offset ||
            (uint64_t)sec[i].off + sec[i].size > (uint64_t)p_offset + p_filesz))
            die("allocated section lies outside the PT_LOAD segment");

        outidx[i] = (int)nout;
        out[nout].name      = sec[i].name;
        out[nout].type      = sec[i].type;
        out[nout].flags     = sec[i].flags;
        out[nout].addr      = sec[i].addr;
        out[nout].off       = sec[i].off + delta;
        out[nout].size      = sec[i].size;
        out[nout].link      = 0;
        out[nout].info      = 0;
        out[nout].addralign = sec[i].addralign;
        out[nout].entsize   = sec[i].entsize;
        nout++;

        /* Find the relocation table that targets this section. */
        for(uint16_t r = 1; r < e_shnum; r++) {
            if(sec[r].type != SHT_REL || sec[r].info != i)
                continue;
            if(sec[r].entsize != 8) die("unexpected relocation entry size");
            if((uint64_t)sec[r].off + sec[r].size > elen) die("relocation table out of range");

            uint32_t n = sec[r].size / 8;
            /* Test n, not n * 8: gcc reads the product in a boolean context as
             * a probable `n && 8` typo (-Wint-in-bool-context) and warns.  The
             * two are equivalent here, since n * 8 is zero exactly when n is. */
            uint8_t *conv = malloc(n ? n * 8 : 1);
            if(!conv) die("out of memory");
            uint32_t kept = 0;

            for(uint32_t k = 0; k < n; k++) {
                const uint8_t *rel = e + sec[r].off + k * 8;
                uint32_t r_offset = rd32(rel);
                uint32_t r_info   = rd32(rel + 4);
                uint32_t type     = r_info & 0xff;
                uint32_t sym      = r_info >> 8;

                total_relocs++;
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
                out[nout].size      = kept * 8;
                out[nout].link      = 0;
                out[nout].info      = (uint32_t)outidx[i];
                out[nout].addralign = 4;
                out[nout].entsize   = 8;
                out[nout].reloc     = conv;
                out[nout].reloc_len = kept * 8;
                nout++;
            } else {
                free(conv);
            }
            break;
        }
    }

    /* .shstrtab last, as in shipping modules. */
    uint32_t shstr_idx = nout;
    out[nout].name      = ".shstrtab";
    out[nout].type      = SHT_STRTAB;
    out[nout].addralign = 1;
    nout++;

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
    buf_need(&o, nout * 40);
    memset(o.p + o.len, 0, nout * 40);
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
    o.len += nout * 40;

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

    uint32_t modinfo_file_off = sec[modinfo_idx].off + delta;
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
    fclose(of);

    printf("elf2prx: wrote %s (%u bytes): %u sections, %u relocs kept "
           "(%u absolute, %u none dropped), modinfo @ %#x\n",
           out_path, o.len, nout, total_relocs - dropped_abs - dropped_none,
           dropped_abs, dropped_none, modinfo_file_off);

    if(verbose) {
        for(uint32_t i = 1; i < nout; i++)
            printf("  %-28s type=%#010x off=%#08x size=%#08x info=%u\n",
                   out[i].name, out[i].type, out[i].off, out[i].size, out[i].info);
    }
    return 0;
}
