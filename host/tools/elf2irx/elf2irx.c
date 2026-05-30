/* elf2irx — convert a relocatable MIPS ELF (output of `ld -r`) into a
 * PS2 IRX module that the IOP-side LoadModuleBuffer can load.
 *
 * Independent reimplementation based on reverse-engineering of the
 * BIOS LOADCORE/MODLOAD.
 *
 * Usage: elf2irx input.elf output.irx
 *
 * Accepted input: ELF32, little-endian, MIPS, ET_REL (relocatable from
 * `ld -r`). If no `.iopmod` section is present, it is synthesized from
 * `_irx_id`, the ELF entry point, and `.reginfo`.
 *
 * Output: IRX file (ELF32 with e_type = 0xFF80) containing:
 *   - ELF header
 *   - 2 program headers (iopmod info, load segment)
 *   - Allocated data (text, rodata, sdata, data)
 *   - .iopmod section
 *   - Section headers (only for kept sections)
 *   - Relocation tables (only types 1, 2, 3, 4, 5, 6 — others rejected)
 *   - Section header string table
 *
 * The MIPS R3000 / IOP loader applies relocation types 1..6 in the
 * LOADCORE type-4 path. GPREL16 (7) is resolved statically here because
 * the BIOS relocation loop ignores relocation types >= 7.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define ET_REL              1
#define ET_EXEC             2
#define EM_MIPS             8
#define ELF_IRX_TYPE        0xFF80

/* ELF section header types we care about. */
#define SHT_NULL            0
#define SHT_PROGBITS        1
#define SHT_SYMTAB          2
#define SHT_STRTAB          3
#define SHT_REL             9
#define SHT_NOBITS          8
#define SHT_LOPROC          0x70000000
#define SHT_LOPROC_IOPMOD   (SHT_LOPROC | 0x80)
#define SHT_MIPS_REGINFO    0x70000006

#define SHF_WRITE           1
#define SHF_ALLOC           2
#define SHF_EXECINSTR       4

#define PT_LOAD             1

/* Relocation types (MIPS) the IOP loader handles. */
#define R_MIPS_NONE         0
#define R_MIPS_16           1
#define R_MIPS_32           2
#define R_MIPS_REL32        3
#define R_MIPS_26           4
#define R_MIPS_HI16         5
#define R_MIPS_LO16         6
#define R_MIPS_GPREL16      7

/* Helper: extract type/sym from r_info. */
#define ELF32_R_SYM(i)      ((i) >> 8)
#define ELF32_R_TYPE(i)     ((i) & 0xff)
#define ELF32_R_INFO(s, t)  (((s) << 8) | ((t) & 0xff))

/* Read/write 32-bit little-endian. The host might be big-endian, so
 * always go through these accessors. */
static uint32_t rd32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static uint16_t rd16(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}
static void wr32(void *p, uint32_t v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)(v & 0xff);
    b[1] = (uint8_t)((v >> 8) & 0xff);
    b[2] = (uint8_t)((v >> 16) & 0xff);
    b[3] = (uint8_t)((v >> 24) & 0xff);
}
static void wr16(void *p, uint16_t v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)(v & 0xff);
    b[1] = (uint8_t)((v >> 8) & 0xff);
}

/* In-memory section descriptor, parsed from the input ELF. */
struct Section {
    uint32_t sh_name; /* offset into shstrtab in input */
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;   /* relative to load=0 */
    uint32_t sh_offset; /* in input file */
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;

    char name[64];       /* readable name from shstrtab */
    int keep;            /* 1 if we output this section */
    int out_index;       /* assigned section index in output */
    uint32_t out_offset; /* file offset in the output IRX */
    uint32_t out_addr;   /* compact IRX-relative address */
    uint32_t out_size;   /* output size; may filter relocation records */
    uint32_t out_name;   /* offset of name in output shstrtab */
};

struct Symbol {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    char     name[96];
};

/* Globals — small tool, not worth threading through everything. */
static const char *g_in_name = NULL;
static const char *g_out_name = NULL;
static uint8_t    *g_in = NULL;
static size_t      g_in_size = 0;
static int         g_verbose = 0;

/* Parsed ELF header (input). */
static uint16_t g_e_type;
static uint16_t g_e_machine;
static uint32_t g_e_entry;
static uint32_t g_e_phoff;
static uint32_t g_e_shoff;
static uint32_t g_e_flags;
static uint16_t g_e_ehsize;
static uint16_t g_e_shentsize;
static uint16_t g_e_shnum;
static uint16_t g_e_shstrndx;

/* Section table (parsed). */
static struct Section *g_sections = NULL;

/* Index of .iopmod section. */
static int g_iopmod_idx = -1;
static int g_synth_iopmod = 0;
static uint8_t g_synth_iopmod_data[27];

/* Index of input symtab (for sym lookups, optional). */
static int g_symtab_idx = -1;
static struct Symbol *g_symbols = NULL;
static uint32_t g_symbol_count = 0;

/* Output layout — file offsets. */
static uint32_t g_out_phoff;
static uint32_t g_out_alloc_off; /* start of allocated data block */
static uint32_t g_out_iopmod_off;
static uint32_t g_out_shoff;
static uint32_t g_out_relocbase;
static uint32_t g_out_shstrtab_off;
static uint32_t g_out_symtab_off;
static uint32_t g_out_strtab_off;
static uint32_t g_out_filesz; /* total allocated payload (no .bss) */
static uint32_t g_out_memsz;  /* same plus .bss */
static uint32_t g_out_shnum;  /* number of section headers in output */
static uint32_t g_out_total_size;
static uint32_t g_out_shstr_size;
static uint32_t g_e_entry_out;
static uint32_t g_gp_out;

/* Stray HI16 entries: HI16s whose ABI-paired LO16 is shared with an
 * earlier HI16 in the same reloc table. LOADCORE's pair handler
 * patches the LO16 instruction in place, so a second HI16 reading
 * the same instruction's bits gets garbage. We work around this by
 * emitting a synthesized scratch LO16 target per stray, in a new
 * `.elf2irx_padlo` PROGBITS section. The synthesized LO16 reloc
 * entry sits adjacent to the stray HI16 in the emitted reloc table. */
struct StrayHi16 {
    int      rel_sec_idx;    /* SHT_REL section in g_sections[] */
    uint32_t hi16_in_idx;    /* original index in src reloc table */
    uint32_t hi16_off_in;    /* HI16 instruction offset within target section */
    uint32_t lo16_off_in;    /* shared LO16 instruction offset within target section */
    int      target_sec_idx; /* target (e.g. .text) section index */
    uint32_t sym;            /* shared symbol index */
};
static struct StrayHi16 *g_strays = NULL;
static uint32_t g_stray_count = 0;
static uint32_t g_stray_capacity = 0;

/* Extra LO16 entries: LO16s after the first LO16 paired with a given
 * HI16. GCC can emit one `lui` with multiple %lo consumers; LOADCORE
 * only applies the LO16 immediately after a HI16 relocation. For each
 * extra LO16, emit a duplicate HI16 relocation pointing at the original
 * `lui` immediately before the LO16 relocation. */
struct ExtraLo16 {
    int      rel_sec_idx;
    uint32_t lo16_in_idx;    /* LO16 relocation index within input rel section */
    uint32_t hi16_off_in;    /* original shared HI16 instruction offset */
    int      target_sec_idx; /* target (e.g. .text) section index */
};
static struct ExtraLo16 *g_extra_los = NULL;
static uint32_t g_extra_lo_count = 0;
static uint32_t g_extra_lo_capacity = 0;

/* Synthesized `.elf2irx_padlo` section. Created only when scratch
 * HI/LO relocation words are needed; otherwise zero. Lives between the
 * last PROGBITS and the first NOBITS in the LOAD segment. */
#define PADLO_NAME ".elf2irx_padlo"
static uint32_t g_padlo_addr = 0;     /* IRX-relative address */
static uint32_t g_padlo_size = 0;     /* bytes (= 4 * scratch reloc words) */
static uint32_t g_padlo_offset = 0;   /* output file offset */
static uint32_t g_padlo_name_off = 0; /* offset of PADLO_NAME in shstrtab */
static int      g_padlo_index = 0;    /* output section index */

/* Diag helper. */
static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "elf2irx: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/* Load the input file into a buffer. */
static void load_input(void) {
    FILE *f = fopen(g_in_name, "rb");
    if(!f)
        die("cannot open input %s", g_in_name);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if(sz < 52)
        die("input too small to be ELF (%ld bytes)", sz);
    fseek(f, 0, SEEK_SET);
    g_in = (uint8_t *)malloc((size_t)sz);
    if(!g_in)
        die("oom %ld bytes", sz);
    if(fread(g_in, 1, (size_t)sz, f) != (size_t)sz)
        die("short read");
    fclose(f);
    g_in_size = (size_t)sz;
}

/* Validate ELF header and parse interesting fields. */
static void parse_elf_header(void) {
    if(rd32(g_in) != 0x464C457F)
        die("not ELF (bad magic)");
    if(g_in[4] != 1)
        die("not ELFCLASS32");
    if(g_in[5] != 1)
        die("not little-endian");
    g_e_type = rd16(g_in + 16);
    g_e_machine = rd16(g_in + 18);
    g_e_entry = rd32(g_in + 24);
    g_e_phoff = rd32(g_in + 28);
    g_e_shoff = rd32(g_in + 32);
    g_e_flags = rd32(g_in + 36);
    g_e_ehsize = rd16(g_in + 40);
    g_e_shentsize = rd16(g_in + 46);
    g_e_shnum = rd16(g_in + 48);
    g_e_shstrndx = rd16(g_in + 50);

    if(g_e_machine != EM_MIPS)
        die("not MIPS (e_machine=%u)", g_e_machine);
    if(g_e_type != ET_REL && g_e_type != ELF_IRX_TYPE)
        die("not relocatable (e_type=%u, expected ET_REL=1 or "
            "ELF_IRX_TYPE=0xFF80)",
            g_e_type);
    if(g_e_shentsize != 40)
        die("section header size %u != 40", g_e_shentsize);
    if(g_e_shnum == 0)
        die("no sections");
    if(g_e_shstrndx >= g_e_shnum)
        die("invalid shstrndx %u (only %u sections)", g_e_shstrndx, g_e_shnum);
}

/* Parse the section header table into g_sections[]. */
static void parse_section_headers(void) {
    g_sections = calloc(g_e_shnum, sizeof(*g_sections));
    if(!g_sections)
        die("oom");

    for(int i = 0; i < g_e_shnum; i++) {
        const uint8_t  *sh = g_in + g_e_shoff + i * 40;
        struct Section *s = &g_sections[i];
        s->sh_name = rd32(sh);
        s->sh_type = rd32(sh + 4);
        s->sh_flags = rd32(sh + 8);
        s->sh_addr = rd32(sh + 12);
        s->sh_offset = rd32(sh + 16);
        s->sh_size = rd32(sh + 20);
        s->sh_link = rd32(sh + 24);
        s->sh_info = rd32(sh + 28);
        s->sh_addralign = rd32(sh + 32);
        s->sh_entsize = rd32(sh + 36);
    }

    /* Resolve section names from the shstrtab. */
    const struct Section *shstr = &g_sections[g_e_shstrndx];
    for(int i = 0; i < g_e_shnum; i++) {
        struct Section *s = &g_sections[i];
        if(s->sh_name >= shstr->sh_size)
            continue;
        const char *nm = (const char *)(g_in + shstr->sh_offset + s->sh_name);
        size_t n = strnlen(nm, sizeof(s->name) - 1);
        memcpy(s->name, nm, n);
        s->name[n] = 0;
    }

    /* Find .iopmod and .symtab. */
    for(int i = 0; i < g_e_shnum; i++) {
        struct Section *s = &g_sections[i];
        if(strcmp(s->name, ".iopmod") == 0)
            g_iopmod_idx = i;
        if(s->sh_type == SHT_SYMTAB)
            g_symtab_idx = i;
    }
    if(g_iopmod_idx < 0) {
        struct Section *grown = realloc(g_sections, (g_e_shnum + 1) * sizeof(*g_sections));
        if(!grown)
            die("oom");
        g_sections = grown;
        memset(&g_sections[g_e_shnum], 0, sizeof(g_sections[g_e_shnum]));
        g_iopmod_idx = g_e_shnum;
        g_synth_iopmod = 1;
        struct Section *s = &g_sections[g_iopmod_idx];
        strcpy(s->name, ".iopmod");
        s->sh_type = SHT_LOPROC_IOPMOD;
        s->sh_addralign = 4;
        s->sh_size = sizeof(g_synth_iopmod_data);
        g_e_shnum++;
    }
}

static void parse_symbols(void) {
    if(g_symtab_idx < 0)
        return;
    struct Section *symtab = &g_sections[g_symtab_idx];
    if(symtab->sh_entsize != 16 || symtab->sh_size % 16 != 0)
        die("invalid .symtab");
    if(symtab->sh_link >= g_e_shnum)
        die("invalid .symtab sh_link");
    struct Section *strtab = &g_sections[symtab->sh_link];
    g_symbol_count = symtab->sh_size / 16;
    g_symbols = calloc(g_symbol_count, sizeof(*g_symbols));
    if(!g_symbols)
        die("oom symbols");

    for(uint32_t i = 0; i < g_symbol_count; i++) {
        const uint8_t *p = g_in + symtab->sh_offset + i * 16;
        struct Symbol *sym = &g_symbols[i];
        sym->st_name = rd32(p);
        sym->st_value = rd32(p + 4);
        sym->st_size = rd32(p + 8);
        sym->st_info = p[12];
        sym->st_other = p[13];
        sym->st_shndx = rd16(p + 14);
        if(sym->st_name < strtab->sh_size) {
            const char *nm = (const char *)(g_in + strtab->sh_offset + sym->st_name);
            size_t n = strnlen(nm, sizeof(sym->name) - 1);
            memcpy(sym->name, nm, n);
            sym->name[n] = 0;
        }
    }
}

static const struct Symbol *find_symbol(const char *name) {
    for(uint32_t i = 0; i < g_symbol_count; i++) {
        if(strcmp(g_symbols[i].name, name) == 0)
            return &g_symbols[i];
    }
    return NULL;
}

static uint32_t symbol_out_addr(const struct Symbol *sym) {
    if(!sym || sym->st_shndx >= g_e_shnum)
        die("cannot translate symbol");
    const struct Section *s = &g_sections[sym->st_shndx];
    return s->out_addr + sym->st_value;
}

static int translate_input_addr(uint32_t in_addr, uint32_t *out_addr) {
    const struct Section *best = NULL;
    for(int i = 0; i < g_e_shnum; i++) {
        const struct Section *s = &g_sections[i];
        if(!(s->sh_flags & SHF_ALLOC))
            continue;
        if(s->sh_addr > in_addr)
            continue;
        if(!best || s->sh_addr > best->sh_addr)
            best = s;
    }
    if(!best)
        return 0;
    *out_addr = best->out_addr + (in_addr - best->sh_addr);
    return 1;
}

/* Decide which sections to keep in the output. */
static void mark_sections_to_keep(void) {
    /* Always keep .iopmod and any allocated section + its .rel. */
    for(int i = 0; i < g_e_shnum; i++) {
        struct Section *s = &g_sections[i];
        if(i == g_iopmod_idx) {
            s->keep = 1;
            continue;
        }
        if(s->sh_type == SHT_MIPS_REGINFO) {
            continue;
        }
        if(s->sh_flags & SHF_ALLOC) {
            s->keep = 1;
            continue;
        }
    }
    /* Then keep .rel.* sections whose target (sh_info) is allocated. */
    for(int i = 0; i < g_e_shnum; i++) {
        struct Section *s = &g_sections[i];
        if(s->sh_type != SHT_REL)
            continue;
        if(s->sh_info >= (uint32_t)g_e_shnum)
            continue;
        if(g_sections[s->sh_info].sh_flags & SHF_ALLOC)
            s->keep = 1;
    }
}

/* Sort each kept SHT_REL section in place by r_offset (stable).
 *
 * GCC with `-ffunction-sections` emits each function's relocations
 * into its own per-function `.rel.<func>` section.  When `ld -r`
 * merges them into one `.rel.text`, the entries are concatenated in
 * link/emission order — which doesn't match offset order if the
 * linker placed functions in the merged `.text` differently than the
 * order it walked their input `.rel` sections.
 *
 * The MIPS ELF spec says each R_MIPS_HI16 must be paired with the
 * R_MIPS_LO16 that "follows" it in the relocation list, where
 * "follows" is conventionally interpreted as table order.  But the
 * compiler emits HI16 + paired LO16 in code order (lui must precede
 * its consumer); when ld -r scrambles the table, HI16 and its real
 * pair end up far apart, with unrelated relocations between them.
 *
 * Both BIOS LOADCORE's pair handler and our HI16/LO16 detection
 * passes assume "next entry after HI16 in table = paired LO16",
 * which only holds if the table is in offset order.  Pre-sorting
 * here makes that invariant true and keeps both algorithms correct.
 *
 * Insertion sort: stable, in-place, O(n²) worst case but n is small
 * (typically <200 entries per section), and stability matters when
 * relocations share an offset (rare but legal). */
static void sort_relocations(void) {
    for(int i = 0; i < g_e_shnum; i++) {
        struct Section *s = &g_sections[i];
        if(!s->keep || s->sh_type != SHT_REL)
            continue;
        uint8_t *base = g_in + s->sh_offset;
        uint32_t n = s->sh_size / 8;
        for(uint32_t j = 1; j < n; j++) {
            uint8_t entry[8];
            memcpy(entry, base + j * 8, 8);
            uint32_t off_j = rd32(entry);
            int32_t  k = (int32_t)j - 1;
            while(k >= 0) {
                uint32_t off_k = rd32(base + (uint32_t)k * 8);
                if(off_k <= off_j)
                    break;
                memcpy(base + ((uint32_t)k + 1) * 8, base + (uint32_t)k * 8, 8);
                k--;
            }
            memcpy(base + ((uint32_t)k + 1) * 8, entry, 8);
        }
    }
}

/* Validate that all relocation entries we're keeping use supported types. */
static void validate_relocations(void) {
    for(int i = 0; i < g_e_shnum; i++) {
        struct Section *s = &g_sections[i];
        if(!s->keep || s->sh_type != SHT_REL)
            continue;
        if(s->sh_size % 8 != 0)
            die(".rel section %s has invalid size %u", s->name, s->sh_size);
        size_t n = s->sh_size / 8;
        for(size_t k = 0; k < n; k++) {
            uint32_t info = rd32(g_in + s->sh_offset + k * 8 + 4);
            uint32_t type = ELF32_R_TYPE(info);
            switch(type) {
            case R_MIPS_NONE:
            case R_MIPS_16:
            case R_MIPS_32:
            case R_MIPS_REL32:
            case R_MIPS_26:
            case R_MIPS_HI16:
            case R_MIPS_LO16:
            case R_MIPS_GPREL16:
                break;
            default:
                die("unsupported relocation type %u in %s entry %zu", type, s->name, k);
            }
        }
    }
}

static int relocation_kept_for_loader(uint32_t type) {
    return type >= R_MIPS_16 && type <= R_MIPS_LO16;
}

static uint32_t section_stray_count(int rel_sec_idx) {
    uint32_t count = 0;
    for(uint32_t i = 0; i < g_stray_count; i++)
        if(g_strays[i].rel_sec_idx == rel_sec_idx)
            count++;
    return count;
}

static uint32_t section_extra_lo_count(int rel_sec_idx) {
    uint32_t count = 0;
    for(uint32_t i = 0; i < g_extra_lo_count; i++)
        if(g_extra_los[i].rel_sec_idx == rel_sec_idx)
            count++;
    return count;
}

static uint32_t scratch_reloc_word_count(void) {
    return g_stray_count + g_extra_lo_count;
}

static uint32_t output_relocation_size(const struct Section *s) {
    if(s->sh_type != SHT_REL)
        return s->sh_size;
    uint32_t kept = 0;
    uint32_t n = s->sh_size / 8;
    for(uint32_t k = 0; k < n; k++) {
        uint32_t info = rd32(g_in + s->sh_offset + k * 8 + 4);
        if(relocation_kept_for_loader(ELF32_R_TYPE(info)))
            kept++;
    }
    /* Find the section's index in g_sections[] (pointer arithmetic). */
    int rel_sec_idx = (int)(s - g_sections);
    return (kept + section_stray_count(rel_sec_idx) + section_extra_lo_count(rel_sec_idx)) * 8;
}

/* Walk every kept SHT_REL section and identify stray HI16 entries
 * (HI16s whose ABI-paired LO16 is shared with an earlier HI16). For
 * each stray, record the info needed to synthesize a per-stray
 * scratch LO16 later. */
static void detect_stray_hi16(void) {
    for(int i = 0; i < g_e_shnum; i++) {
        struct Section *s = &g_sections[i];
        if(!s->keep || s->sh_type != SHT_REL)
            continue;
        if(s->sh_info >= g_e_shnum)
            continue;
        const uint8_t *src = g_in + s->sh_offset;
        uint32_t       n = s->sh_size / 8;

        /* For each LO16 entry, track the first HI16 that ABI-pairs
         * with it. A second HI16 finding the same LO16 is stray. */
        int *lo_user = malloc(n * sizeof(int));
        if(!lo_user)
            die("oom for lo_user");
        for(uint32_t k = 0; k < n; k++)
            lo_user[k] = -1;

        for(uint32_t k = 0; k < n; k++) {
            uint32_t info_k = rd32(src + k * 8 + 4);
            if(ELF32_R_TYPE(info_k) != R_MIPS_HI16)
                continue;
            uint32_t sym_k = ELF32_R_SYM(info_k);
            int m = -1;
            for(uint32_t j = k + 1; j < n; j++) {
                uint32_t info_j = rd32(src + j * 8 + 4);
                if(ELF32_R_TYPE(info_j) != R_MIPS_LO16)
                    continue;
                if(ELF32_R_SYM(info_j) != sym_k)
                    continue;
                m = (int)j;
                break;
            }
            if(m < 0)
                continue; /* stray-no-match handled later */

            if(lo_user[m] >= 0) {
                if(g_stray_count == g_stray_capacity) {
                    g_stray_capacity = g_stray_capacity ? g_stray_capacity * 2 : 8;
                    g_strays = realloc(g_strays, g_stray_capacity * sizeof(*g_strays));
                    if(!g_strays)
                        die("oom for strays");
                }
                struct StrayHi16 *sh = &g_strays[g_stray_count++];
                sh->rel_sec_idx = i;
                sh->hi16_in_idx = k;
                sh->hi16_off_in = rd32(src + k * 8);
                sh->lo16_off_in = rd32(src + m * 8);
                sh->target_sec_idx = (int)s->sh_info;
                sh->sym = sym_k;
            } else {
                lo_user[m] = (int)k;
            }
        }
        free(lo_user);
    }

    if(g_verbose && g_stray_count > 0) {
        fprintf(stderr, "Stray HI16 entries detected: %u\n", g_stray_count);
        for(uint32_t i = 0; i < g_stray_count; i++) {
            const struct StrayHi16 *sh = &g_strays[i];
            fprintf(stderr, "  [%u] section %s offset 0x%x (paired LO16 at 0x%x)\n", i,
                    g_sections[sh->rel_sec_idx].name, sh->hi16_off_in, sh->lo16_off_in);
        }
    }
}

/* Find LO16 relocations that share a HI16 relocation with an earlier
 * LO16. These are valid MIPS codegen, but LOADCORE only patches the
 * first LO16 following a HI16 relocation entry. */
static void detect_extra_lo16(void) {
    for(int i = 0; i < g_e_shnum; i++) {
        struct Section *s = &g_sections[i];
        if(!s->keep || s->sh_type != SHT_REL)
            continue;
        const uint8_t *src = g_in + s->sh_offset;
        uint32_t n = s->sh_size / 8;

        uint8_t *hi_has_lo = calloc(n, 1);
        if(!hi_has_lo)
            die("oom for hi_has_lo");

        for(uint32_t k = 0; k < n; k++) {
            uint32_t info_k = rd32(src + k * 8 + 4);
            if(ELF32_R_TYPE(info_k) != R_MIPS_LO16)
                continue;
            uint32_t sym_k = ELF32_R_SYM(info_k);

            int hi = -1;
            for(int j = (int)k - 1; j >= 0; j--) {
                uint32_t info_j = rd32(src + (uint32_t)j * 8 + 4);
                if(ELF32_R_TYPE(info_j) != R_MIPS_HI16)
                    continue;
                if(ELF32_R_SYM(info_j) != sym_k)
                    continue;
                hi = j;
                break;
            }
            if(hi < 0)
                continue;

            if(!hi_has_lo[hi]) {
                hi_has_lo[hi] = 1;
                continue;
            }

            if(g_extra_lo_count == g_extra_lo_capacity) {
                g_extra_lo_capacity = g_extra_lo_capacity ? g_extra_lo_capacity * 2 : 8;
                g_extra_los = realloc(g_extra_los, g_extra_lo_capacity * sizeof(*g_extra_los));
                if(!g_extra_los)
                    die("oom for extra LO16 records");
            }
            struct ExtraLo16 *el = &g_extra_los[g_extra_lo_count++];
            el->rel_sec_idx = i;
            el->lo16_in_idx = k;
            el->hi16_off_in = rd32(src + (uint32_t)hi * 8);
            el->target_sec_idx = (int)s->sh_info;
        }

        free(hi_has_lo);
    }

    if(g_verbose && g_extra_lo_count > 0) {
        fprintf(stderr, "Extra LO16 entries detected: %u\n", g_extra_lo_count);
        for(uint32_t i = 0; i < g_extra_lo_count; i++) {
            const struct ExtraLo16 *el = &g_extra_los[i];
            fprintf(stderr, "  [%u] section %s LO16 index %u duplicates HI16 at 0x%x\n", i,
                    g_sections[el->rel_sec_idx].name, el->lo16_in_idx, el->hi16_off_in);
        }
    }
}

/* Compute output file layout. */
static void compute_layout(void) {
    /* Count sections we'll output. Plus NULL, shstrtab, and the dummy
     * symtab/strtab pair expected by the BIOS parser shape. */
    int      nkeep = 0;
    uint32_t alloc_cursor = 0;
    uint32_t file_top = 0;
    uint32_t mem_top = 0;

    for(int i = 0; i < g_e_shnum; i++) {
        struct Section *s = &g_sections[i];
        if(!s->keep)
            continue;
        s->out_size = output_relocation_size(s);
        nkeep++;
        if(s->sh_flags & SHF_ALLOC) {
            uint32_t align = s->sh_addralign ? s->sh_addralign : 1;
            if(align > 1)
                alloc_cursor = (alloc_cursor + align - 1) & ~(align - 1);
            s->out_addr = alloc_cursor;
            if(s->sh_type != SHT_NOBITS)
                file_top = alloc_cursor + s->sh_size;
            alloc_cursor += s->sh_size;
            mem_top = alloc_cursor;
        }
    }

    /* Insert the synthesized .elf2irx_padlo section between the last
     * PROGBITS and the first NOBITS in the LOAD segment. We need
     * file-backed bytes for it (each scratch word is pre-initialized
     * with the low addend), so it must come before any NOBITS. */
    if(scratch_reloc_word_count() > 0) {
        g_padlo_size = scratch_reloc_word_count() * 4;
        g_padlo_addr = (file_top + 3) & ~3;
        uint32_t padlo_end = g_padlo_addr + g_padlo_size;
        uint32_t shift = padlo_end - file_top;

        /* Move any NOBITS sections that started at or after file_top
         * forward by `shift`. NOBITS sections placed earlier in the
         * input (before the last PROGBITS) keep their addresses. */
        for(int i = 0; i < g_e_shnum; i++) {
            struct Section *s = &g_sections[i];
            if(!s->keep)
                continue;
            if((s->sh_flags & SHF_ALLOC) && s->sh_type == SHT_NOBITS) {
                if(s->out_addr >= file_top)
                    s->out_addr += shift;
            }
        }

        file_top = padlo_end;
        if(mem_top < padlo_end)
            mem_top = padlo_end;
        else
            mem_top += shift;
    }

    g_out_filesz = (file_top + 3) & ~3;
    g_out_memsz = (mem_top + 3) & ~3;

    /* Synthesize the new shstrtab. */
    g_out_shstr_size = 1; /* leading NUL */
    for(int i = 0; i < g_e_shnum; i++) {
        struct Section *s = &g_sections[i];
        if(!s->keep)
            continue;
        g_out_shstr_size += (uint32_t)strlen(s->name) + 1;
    }
    if(scratch_reloc_word_count() > 0)
        g_out_shstr_size += (uint32_t)strlen(PADLO_NAME) + 1;
    g_out_shstr_size += (uint32_t)strlen(".shstrtab") + 1;
    g_out_shstr_size += (uint32_t)strlen(".symtab") + 1;
    g_out_shstr_size += (uint32_t)strlen(".strtab") + 1;

    /* Layout:
     *   [0..52)         ELF header
     *   [52..52+64)     2 program headers
     *   .iopmod
     *   [pad to 16)
     *   alloc data      (size g_out_filesz)
     *   .shstrtab
     *   [pad to 4)
     *   section headers (out_shnum * 40)
     *   [pad to 4)
     *   relocation tables
     *   dummy .symtab
     *   dummy .strtab
     */
    g_out_phoff = 52;
    g_out_iopmod_off = g_out_phoff + 2 * 32;
    g_out_alloc_off = (g_out_iopmod_off + g_sections[g_iopmod_idx].sh_size + 15) & ~15;
    g_out_shstrtab_off = g_out_alloc_off + g_out_filesz;

    g_out_shnum = (uint32_t)nkeep + (scratch_reloc_word_count() > 0 ? 1 : 0) + 4;
    /* NULL + kept + (.elf2irx_padlo if any) + shstrtab/symtab/strtab */
    uint32_t iopmod_size = g_sections[g_iopmod_idx].sh_size;
    (void)iopmod_size;
    g_out_shoff = (g_out_shstrtab_off + g_out_shstr_size + 3) & ~3;

    g_out_relocbase = g_out_shoff + g_out_shnum * 40;
    /* Reloc data has same size as input (we copy entries unchanged). */
    uint32_t reloc_total = 0;
    for(int i = 0; i < g_e_shnum; i++) {
        struct Section *s = &g_sections[i];
        if(s->keep && s->sh_type == SHT_REL)
            reloc_total += s->out_size;
    }
    g_out_symtab_off = (g_out_relocbase + reloc_total + 3) & ~3;
    g_out_strtab_off = g_out_symtab_off + 16;
    g_out_total_size = g_out_strtab_off + 1;

    /* Assign output offsets and indices. */
    uint32_t cur_reloc = g_out_relocbase;
    int sect = 1;  /* 0 is NULL */
    g_sections[g_iopmod_idx].out_index = sect++;
    for(int i = 0; i < g_e_shnum; i++) {
        struct Section *s = &g_sections[i];
        if(!s->keep)
            continue;
        if(i != g_iopmod_idx)
            s->out_index = sect++;
        if(i == g_iopmod_idx) {
            s->out_offset = g_out_iopmod_off;
        } else if(s->sh_type == SHT_PROGBITS && (s->sh_flags & SHF_ALLOC)) {
            s->out_offset = g_out_alloc_off + s->out_addr;
        } else if(s->sh_type == SHT_NOBITS) {
            /* NOBITS sections don't take file space. iopfixup uses
             * the file offset where allocated PROGBITS ends (which
             * is also where the section header string table starts).
             * Same value for every NOBITS section regardless of its
             * IRX-relative address. */
            s->out_offset = g_out_alloc_off + g_out_filesz;
        } else if(s->sh_type == SHT_REL) {
            s->out_offset = cur_reloc;
            cur_reloc += s->out_size;
        } else {
            s->out_offset = 0;
        }
    }
    if(scratch_reloc_word_count() > 0) {
        g_padlo_index = sect++;
        g_padlo_offset = g_out_alloc_off + g_padlo_addr;
    }

    if(!translate_input_addr(g_e_entry, &g_e_entry_out))
        g_e_entry_out = g_e_entry;
    if(!g_synth_iopmod && g_iopmod_idx >= 0 && g_sections[g_iopmod_idx].sh_size >= 12)
        g_gp_out = rd32(g_in + g_sections[g_iopmod_idx].sh_offset + 8);

    if(g_verbose) {
        fprintf(stderr, "Layout:\n");
        fprintf(stderr, "  phoff       0x%x\n", g_out_phoff);
        fprintf(stderr, "  alloc_off   0x%x  (filesz 0x%x  memsz 0x%x)\n", g_out_alloc_off, g_out_filesz,
                g_out_memsz);
        fprintf(stderr, "  iopmod_off  0x%x\n", g_out_iopmod_off);
        fprintf(stderr, "  shoff       0x%x  (shnum %u)\n", g_out_shoff, g_out_shnum);
        fprintf(stderr, "  relocbase   0x%x\n", g_out_relocbase);
        fprintf(stderr, "  shstrtab    0x%x  (size 0x%x)\n", g_out_shstrtab_off, g_out_shstr_size);
        fprintf(stderr, "  symtab      0x%x\n", g_out_symtab_off);
        fprintf(stderr, "  total       0x%x\n", g_out_total_size);
        fprintf(stderr, "  e_entry     0x%x\n", g_e_entry_out);
    }
}

static uint32_t resolve_relocation_value(uint32_t relsec_index, uint32_t rel_index) {
    const struct Section *rel = &g_sections[relsec_index];
    const uint8_t *rp = g_in + rel->sh_offset + rel_index * 8;
    uint32_t off = rd32(rp);
    uint32_t info = rd32(rp + 4);
    uint32_t sym_index = ELF32_R_SYM(info);
    if (sym_index >= g_symbol_count)
        die("relocation references invalid symbol %u", sym_index);
    const struct Symbol *sym = &g_symbols[sym_index];
    if(rel->sh_info >= g_e_shnum)
        die("relocation section has invalid target");
    const struct Section *target = &g_sections[rel->sh_info];
    uint32_t addend = 0;
    if(target->sh_type != SHT_NOBITS && off + 4 <= target->sh_size)
        addend = rd32(g_in + target->sh_offset + off);
    if(sym->st_shndx >= g_e_shnum)
        die("relocation references non-section symbol %s", sym->name);
    return symbol_out_addr(sym) + addend;
}

static uint32_t relocation_symbol_addr(const struct Section *rel, uint32_t rel_index) {
    const uint8_t *rp = g_in + rel->sh_offset + rel_index * 8;
    uint32_t info = rd32(rp + 4);
    uint32_t sym_index = ELF32_R_SYM(info);
    if (sym_index >= g_symbol_count)
        die("relocation references invalid symbol %u", sym_index);
    const struct Symbol *sym = &g_symbols[sym_index];
    if(sym->st_shndx >= g_e_shnum)
        die("relocation references non-section symbol %s", sym->name);
    return symbol_out_addr(sym);
}

static int32_t sign_extend16(uint32_t v) {
    return (int16_t)(v & 0xffff);
}

static void apply_static_relocation_addends(uint8_t *out) {
    for(int i = 0; i < g_e_shnum; i++) {
        struct Section *rel = &g_sections[i];
        if(!rel->keep || rel->sh_type != SHT_REL)
            continue;
        if(rel->sh_info >= g_e_shnum)
            continue;
        struct Section *target = &g_sections[rel->sh_info];
        if(!(target->sh_flags & SHF_ALLOC) || target->sh_type == SHT_NOBITS)
            continue;
        uint32_t n = rel->sh_size / 8;
        for(uint32_t k = 0; k < n; k++) {
            uint32_t off = rd32(g_in + rel->sh_offset + k * 8);
            uint32_t info = rd32(g_in + rel->sh_offset + k * 8 + 4);
            uint32_t type = ELF32_R_TYPE(info);
            if(type == R_MIPS_NONE)
                continue;
            uint32_t sym_addr = relocation_symbol_addr(rel, k);
            uint8_t *place = out + g_out_alloc_off + target->out_addr + off;
            uint32_t word = rd32(place);

            switch(type) {
            case R_MIPS_16: {
                uint32_t value = (uint32_t)(sign_extend16(word) + sym_addr);
                wr32(place, (word & 0xffff0000) | (value & 0xffff));
                break;
            }
            case R_MIPS_32:
            case R_MIPS_REL32:
                wr32(place, word + sym_addr);
                break;
            case R_MIPS_26: {
                uint32_t value = ((word & 0x03ffffff) << 2) + sym_addr;
                wr32(place, (word & 0xfc000000) | ((value >> 2) & 0x03ffffff));
                break;
            }
            case R_MIPS_HI16: {
                uint32_t lo_off = 0;
                uint8_t *lo_place = NULL;
                for(uint32_t j = k + 1; j < n; j++) {
                    uint32_t info2 = rd32(g_in + rel->sh_offset + j * 8 + 4);
                    if(ELF32_R_TYPE(info2) != R_MIPS_LO16)
                        continue;
                    if(ELF32_R_SYM(info2) != ELF32_R_SYM(info))
                        continue;
                    lo_off = rd32(g_in + rel->sh_offset + j * 8);
                    lo_place = out + g_out_alloc_off + target->out_addr + lo_off;
                    break;
                }
                if(!lo_place) {
                    /* Orphan HI16 with no matching LO16.  Observed in
                     * smap.irx as a stale reloc on a loop-counter
                     * `addiu` instruction (no actual address-load
                     * needed there) — apparently a `ld -r` artifact
                     * after dead-code elimination.  Skip the reloc
                     * (leave the instruction's immediate field as
                     * compiled).  Emit a warning so genuinely-broken
                     * cases stand out. */
                    if(g_verbose)
                        fprintf(stderr,
                                "warning: orphan R_MIPS_HI16 in %s at "
                                "offset 0x%x — leaving instruction unchanged\n",
                                rel->name, off);
                    break;
                }
                uint32_t lo_word = rd32(lo_place);
                int32_t  addend = (int32_t)((word & 0xffff) << 16) + sign_extend16(lo_word);
                uint32_t value = (uint32_t)(addend + (int32_t)sym_addr);
                wr32(place, (word & 0xffff0000) | (((value + 0x8000) >> 16) & 0xffff));
                break;
            }
            case R_MIPS_LO16: {
                int32_t  addend = sign_extend16(word);
                uint32_t value = (uint32_t)(addend + (int32_t)sym_addr);
                wr32(place, (word & 0xffff0000) | (value & 0xffff));
                break;
            }
            case R_MIPS_GPREL16:
                /* `ld -r` has already computed the GP-relative offset
                 * for us. The offset is invariant under module-base
                 * translation (both target and gp shift by the same
                 * delta), so we must NOT reapply it here — that would
                 * double the offset and break the access. Leave the
                 * instruction's existing immediate alone. */
                break;
            default:
                break;
            }
        }
    }
}

static void synthesize_iopmod(void) {
    if(!g_synth_iopmod)
        return;
    const struct Symbol *irx = find_symbol("_irx_id");
    if(!irx)
        die("cannot synthesize .iopmod: missing _irx_id symbol");
    if(irx->st_shndx >= g_e_shnum)
        die("cannot synthesize .iopmod: invalid _irx_id section");

    uint32_t irx_id = symbol_out_addr(irx);
    uint32_t gp = 0;
    for(int i = 0; i < g_e_shnum; i++) {
        const struct Section *s = &g_sections[i];
        if(s->sh_type == SHT_MIPS_REGINFO && s->sh_size >= 24) {
            uint32_t gp_in = rd32(g_in + s->sh_offset + 20);
            if(!translate_input_addr(gp_in, &gp)) {
                const struct Symbol *bss = find_symbol("__bss_start");
                gp = bss ? symbol_out_addr(bss) + 0x7fe8 : 0;
            }
            break;
        }
    }
    if(gp == 0) {
        const struct Symbol *bss = find_symbol("__bss_start");
        gp = bss ? symbol_out_addr(bss) + 0x7fe8 : 0;
    }
    g_gp_out = gp;

    uint32_t name = 0;
    uint32_t version = 0;
    const struct Section *irx_sec = &g_sections[irx->st_shndx];
    if(irx_sec->sh_type != SHT_NOBITS && irx->st_value + 8 <= irx_sec->sh_size)
        version = rd32(g_in + irx_sec->sh_offset + irx->st_value + 4);
    for(int i = 0; i < g_e_shnum; i++) {
        const struct Section *rel = &g_sections[i];
        if(rel->sh_type != SHT_REL || rel->sh_info != irx->st_shndx)
            continue;
        uint32_t n = rel->sh_size / 8;
        for(uint32_t k = 0; k < n; k++) {
            uint32_t off = rd32(g_in + rel->sh_offset + k * 8);
            uint32_t type = ELF32_R_TYPE(rd32(g_in + rel->sh_offset + k * 8 + 4));
            if(off == irx->st_value && type == R_MIPS_32) {
                name = resolve_relocation_value(i, k);
                break;
            }
        }
    }
    if(name == 0)
        die("cannot synthesize .iopmod: _irx_id name relocation not found");

    /* Compute initialized-data size for .iopmod offset 0x10:
     *   sum of sections that are SHF_ALLOC, NOT SHT_NOBITS, NOT
     *   SHF_EXECINSTR (i.e. .rodata + .data + .sdata, not .text or
     *   .bss).  This matches what BIOS-shipped IRX modules write at
     *   that field — verified across all 54 BIOS modules in
     *   docs/re/cleanroom-iop-loader/02-iopmod-section.md.
     *
     * NOTE: clean-room RE shows neither LOADCORE nor MODLOAD ever
     * READS this field (it gets copied to elf_struct[+0x14] by
     * LOADCORE export #22 but no later code consumes it).  Writing
     * the real value rather than the previous 0x14 magic is purely
     * format hygiene — no functional behavior changes.
     *
     * Also compute total bss size (sum of NOBITS allocated sections,
     * = .bss + .sbss when both present) for offset 0x14. */
    uint32_t init_data_size = 0;
    uint32_t bss_size = 0;
    for(int i = 0; i < g_e_shnum; i++) {
        const struct Section *s = &g_sections[i];
        if(!s->keep)
            continue;
        if(!(s->sh_flags & SHF_ALLOC))
            continue;
        if(s->sh_type == SHT_NOBITS) {
            bss_size += s->sh_size;
        } else if(!(s->sh_flags & SHF_EXECINSTR)) {
            init_data_size += s->sh_size;
        }
    }

    memset(g_synth_iopmod_data, 0, sizeof(g_synth_iopmod_data));
    wr32(g_synth_iopmod_data + 0x00, irx_id);
    wr32(g_synth_iopmod_data + 0x04, g_e_entry_out);
    wr32(g_synth_iopmod_data + 0x08, gp);
    wr32(g_synth_iopmod_data + 0x0c, name);
    wr32(g_synth_iopmod_data + 0x10, init_data_size);
    wr32(g_synth_iopmod_data + 0x14, bss_size);
    g_synth_iopmod_data[0x18] = (uint8_t)(version & 0xff);
    g_synth_iopmod_data[0x19] = (uint8_t)((version >> 8) & 0xff);
    g_synth_iopmod_data[0x1a] = (uint8_t)((version >> 16) & 0xff);
}

/* Emit the IRX file. */
/* Emit one SHT_REL section to the output buffer, applying three
 * transforms relative to the input ELF:
 *
 *   1. Drop relocations the loader doesn't accept (R_MIPS_GPREL16 and
 *      anything else outside types 1..6).
 *   2. Reorder so each R_MIPS_HI16 is immediately followed by its
 *      paired R_MIPS_LO16. LOADCORE's pair handler at LOADCORE 0x1810
 *      reads the next reloc entry's r_offset blindly and writes the
 *      computed low-16 there — non-adjacent HI16/LO16 pairs would
 *      therefore corrupt an unrelated instruction at load time.
 *   3. For stray HI16s (shared-LO16 cases identified earlier in
 *      detect_stray_hi16), synthesize an adjacent LO16 reloc entry
 *      pointing into the .elf2irx_padlo section. The scratch word
 *      there has been pre-initialized with the low addend bits, so
 *      the pair handler reads the correct addend and writes its
 *      computed low-16 to a harmless location.
 *   4. For extra LO16s (shared-HI16 cases identified earlier in
 *      detect_extra_lo16), synthesize an adjacent duplicate HI16 reloc
 *      entry that points back at the original `lui`, so LOADCORE sees
 *      a HI16/LO16 pair for every real LO16 relocation.
 *
 * Per the MIPS SysV ABI: the matching LO16 for a given HI16 is the
 * next R_MIPS_LO16 entry with the same r_sym. */
struct KeptRel {
    uint32_t off_in;
    uint32_t off_out;
    uint32_t type;
    uint32_t sym;
    int stray_idx;    /* -1 if not stray; else index into g_strays */
    int extra_lo_idx; /* -1 unless LO16 needs a scratch HI16 */
};

static void emit_relocation_section(const struct Section *s, uint8_t *out) {
    const uint8_t *src = g_in + s->sh_offset;
    uint8_t *dst = out + s->out_offset;
    size_t n_in = s->sh_size / 8;
    uint32_t base = (s->sh_info < g_e_shnum) ? g_sections[s->sh_info].out_addr : 0;
    int rel_sec_idx = (int)(s - g_sections);

    /* Pass 1: collect kept entries; tag any stray HI16s. */
    struct KeptRel *kept = malloc(n_in * sizeof(*kept));
    if(!kept)
        die("oom for relocation buffer");
    size_t nk = 0;
    for(size_t k = 0; k < n_in; k++) {
        uint32_t off = rd32(src + k * 8);
        uint32_t info = rd32(src + k * 8 + 4);
        uint32_t type = ELF32_R_TYPE(info);
        if(!relocation_kept_for_loader(type))
            continue;
        kept[nk].off_in = off;
        kept[nk].off_out = off + base;
        kept[nk].type = type;
        kept[nk].sym = (uint32_t)ELF32_R_SYM(info);
        kept[nk].stray_idx = -1;
        kept[nk].extra_lo_idx = -1;
        if(type == R_MIPS_HI16) {
            for(uint32_t i = 0; i < g_stray_count; i++) {
                if(g_strays[i].rel_sec_idx == rel_sec_idx && g_strays[i].hi16_in_idx == (uint32_t)k) {
                    kept[nk].stray_idx = (int)i;
                    break;
                }
            }
        } else if(type == R_MIPS_LO16) {
            for(uint32_t i = 0; i < g_extra_lo_count; i++) {
                if(g_extra_los[i].rel_sec_idx == rel_sec_idx && g_extra_los[i].lo16_in_idx == (uint32_t)k) {
                    kept[nk].extra_lo_idx = (int)i;
                    break;
                }
            }
        }
        nk++;
    }

    /* Pass 2: pair non-stray HI16s with their matching LO16. Stray
     * HI16s get a synthesized LO16 (in pass 3) and are skipped here. */
    int *pair = malloc(nk * sizeof(int));
    int *lo_user = malloc(nk * sizeof(int));
    if(!pair || !lo_user)
        die("oom for pair tables");
    for(size_t k = 0; k < nk; k++) {
        pair[k] = -1;
        lo_user[k] = -1;
    }
    for(size_t k = 0; k < nk; k++) {
        if(kept[k].type != R_MIPS_HI16)
            continue;
        if(kept[k].stray_idx >= 0)
            continue;
        int m = -1;
        for(size_t j = k + 1; j < nk; j++) {
            if(kept[j].type == R_MIPS_LO16 && kept[j].sym == kept[k].sym) {
                m = (int)j;
                break;
            }
        }
        if(m < 0) {
            /* Truly orphan HI16 — no matching LO16 anywhere in the
             * table.  Observed in smap.irx as a stale reloc on a
             * loop-counter `addiu` instruction (no actual address
             * computation), apparently a `ld -r` artifact after
             * dead-code elimination.  Mark the entry to be dropped
             * from the output reloc table so LOADCORE doesn't try
             * to apply it at runtime; the apply-relocations pass
             * earlier already left the instruction unchanged. */
            if(g_verbose)
                fprintf(stderr,
                        "warning: dropping orphan R_MIPS_HI16 in %s "
                        "at offset 0x%x — no matching LO16\n",
                        s->name, kept[k].off_in);
            kept[k].type = R_MIPS_NONE; /* sentinel: skip in pass 3 */
            continue;
        }
        if(lo_user[m] >= 0)
            die("%s: internal error — non-stray HI16 at offset 0x%x and "
                "0x%x both pair with LO16 at 0x%x; detect_stray_hi16() "
                "should have caught this",
                s->name, kept[(size_t)lo_user[m]].off_in, kept[k].off_in, kept[m].off_in);
        lo_user[m] = (int)k;
        pair[k] = m;
    }

    /* Pass 3: emit. For each non-consumed entry, write it; if it's a
     * HI16 with a non-adjacent paired LO16, also emit the LO16 next.
     * If it's a stray HI16, emit a synthesized LO16 next. */
    char *consumed = calloc(nk, 1);
    if(!consumed)
        die("oom for consumed flags");
    size_t out_k = 0;
    for(size_t k = 0; k < nk; k++) {
        if(consumed[k])
            continue;
        if(kept[k].extra_lo_idx >= 0) {
            uint32_t scratch_hi_off = g_padlo_addr + g_stray_count * 4 + (uint32_t)kept[k].extra_lo_idx * 4;
            wr32(dst + out_k * 8, scratch_hi_off);
            wr32(dst + out_k * 8 + 4, ELF32_R_INFO(0, R_MIPS_HI16));
            out_k++;
        }
        wr32(dst + out_k * 8, kept[k].off_out);
        wr32(dst + out_k * 8 + 4, ELF32_R_INFO(0, kept[k].type));
        out_k++;

        if(kept[k].stray_idx >= 0) {
            /* Synth LO16 → padlo[stray_idx*4]. */
            uint32_t padlo_off = g_padlo_addr + (uint32_t)kept[k].stray_idx * 4;
            wr32(dst + out_k * 8, padlo_off);
            wr32(dst + out_k * 8 + 4, ELF32_R_INFO(0, R_MIPS_LO16));
            out_k++;
            continue;
        }

        if(pair[k] < 0)
            continue;

        size_t next_k = k + 1;
        while(next_k < nk && consumed[next_k])
            next_k++;
        if((int)next_k != pair[k]) {
            size_t m = (size_t)pair[k];
            wr32(dst + out_k * 8, kept[m].off_out);
            wr32(dst + out_k * 8 + 4, ELF32_R_INFO(0, kept[m].type));
            out_k++;
            consumed[m] = 1;
        }
    }

    size_t expected = nk + section_stray_count(rel_sec_idx) + section_extra_lo_count(rel_sec_idx);
    if(out_k != expected)
        die("%s: internal error — emitted %zu reloc entries, expected %zu", s->name, out_k, expected);

    free(consumed);
    free(lo_user);
    free(pair);
    free(kept);
}

static void emit_irx(void) {
    uint8_t *out = calloc(1, g_out_total_size);
    if(!out)
        die("oom output buffer %u bytes", g_out_total_size);

    /* ELF header. */
    wr32(out + 0, 0x464C457F); /* magic */
    out[4] = 1;                /* ELFCLASS32 */
    out[5] = 1;                /* little-endian */
    out[6] = 1;                /* EI_VERSION */
    /* out[7..15] padding stays zero */
    wr16(out + 16, ELF_IRX_TYPE);  /* e_type = 0xFF80 */
    wr16(out + 18, EM_MIPS);       /* e_machine */
    wr32(out + 20, 1);             /* e_version */
    wr32(out + 24, g_e_entry_out); /* e_entry */
    wr32(out + 28, g_out_phoff);   /* e_phoff */
    wr32(out + 32, g_out_shoff);   /* e_shoff */
    wr32(out + 36, g_e_flags);     /* e_flags — preserve from input */
    wr16(out + 40, 52);            /* e_ehsize */
    wr16(out + 42, 32);            /* e_phentsize */
    wr16(out + 44, 2);             /* e_phnum */
    wr16(out + 46, 40);            /* e_shentsize */
    wr16(out + 48, (uint16_t)g_out_shnum);

    /* Program header 0 — .iopmod info (PT_LOPROC | 0x80). */
    uint8_t *ph = out + g_out_phoff;
    wr32(ph + 0, SHT_LOPROC | 0x80);
    wr32(ph + 4, g_out_iopmod_off);
    wr32(ph + 8, 0);
    wr32(ph + 12, 0);
    wr32(ph + 16, g_sections[g_iopmod_idx].sh_size);
    wr32(ph + 20, 0);
    wr32(ph + 24, 4);
    wr32(ph + 28, 4);

    /* Program header 1 — PT_LOAD with all allocated data. */
    ph += 32;
    wr32(ph + 0, PT_LOAD);
    wr32(ph + 4, g_out_alloc_off);
    wr32(ph + 8, 0);  /* p_vaddr = 0 (loader relocates) */
    wr32(ph + 12, 0); /* p_paddr */
    wr32(ph + 16, g_out_filesz);
    wr32(ph + 20, g_out_memsz);
    wr32(ph + 24, 7); /* RWX */
    wr32(ph + 28, 16);

    /* Allocated data — copy each PROGBITS section to its sh_addr. */
    for(int i = 0; i < g_e_shnum; i++) {
        struct Section *s = &g_sections[i];
        if(!s->keep)
            continue;
        if(i == g_iopmod_idx)
            continue;
        if(s->sh_type != SHT_PROGBITS)
            continue;
        if(!(s->sh_flags & SHF_ALLOC))
            continue;
        memcpy(out + g_out_alloc_off + s->out_addr, g_in + s->sh_offset, s->sh_size);
    }
    apply_static_relocation_addends(out);

    /* .elf2irx_padlo data: pre-initialize scratch words with the
     * instruction bits LOADCORE will use as relocation addends. Stray
     * HI16 records need a scratch LO16 word; extra LO16 records need a
     * scratch HI16 word so the real shared `lui` is not patched twice. */
    if(scratch_reloc_word_count() > 0) {
        for(uint32_t i = 0; i < g_stray_count; i++) {
            const struct StrayHi16 *sh = &g_strays[i];
            uint32_t target_addr = g_sections[sh->target_sec_idx].out_addr;
            const uint8_t *lo_instr = out + g_out_alloc_off + target_addr + sh->lo16_off_in;
            uint16_t lo_imm = rd16(lo_instr);
            uint8_t *scratch = out + g_padlo_offset + i * 4;
            wr32(scratch, (uint32_t)lo_imm);
        }
        for(uint32_t i = 0; i < g_extra_lo_count; i++) {
            const struct ExtraLo16 *el = &g_extra_los[i];
            uint32_t target_addr = g_sections[el->target_sec_idx].out_addr;
            const uint8_t *hi_instr = out + g_out_alloc_off + target_addr + el->hi16_off_in;
            uint8_t *scratch = out + g_padlo_offset + g_stray_count * 4 + i * 4;
            wr32(scratch, rd32(hi_instr));
        }
    }

    /* .iopmod data. */
    {
        struct Section *s = &g_sections[g_iopmod_idx];
        if(g_synth_iopmod) {
            memcpy(out + g_out_iopmod_off, g_synth_iopmod_data, s->sh_size);
        } else {
            memcpy(out + g_out_iopmod_off, g_in + s->sh_offset, s->sh_size);
            /* User-provided .iopmod: relocations against the input
             * are not applied (we copy bytes verbatim).  Patch the
             * entry-point slot at offset +0x04 with the resolved
             * e_entry so a `_start` reference in the user's macro
             * doesn't end up zero. */
            if(s->sh_size >= 8)
                wr32(out + g_out_iopmod_off + 0x04, g_e_entry_out);
        }
    }

    /* Build the new shstrtab and remember each section's name offset. */
    uint8_t *shstr = out + g_out_shstrtab_off;
    shstr[0] = 0;
    uint32_t cur = 1;
    g_sections[g_iopmod_idx].out_name = cur;
    {
        const char *nm = g_sections[g_iopmod_idx].name;
        size_t n = strlen(nm);
        memcpy(shstr + cur, nm, n + 1);
        cur += (uint32_t)n + 1;
    }
    for(int i = 0; i < g_e_shnum; i++) {
        struct Section *s = &g_sections[i];
        if(!s->keep)
            continue;
        if(i == g_iopmod_idx)
            continue;
        s->out_name = cur;
        size_t n = strlen(s->name);
        memcpy(shstr + cur, s->name, n + 1);
        cur += (uint32_t)n + 1;
    }
    if(scratch_reloc_word_count() > 0) {
        g_padlo_name_off = cur;
        size_t n = strlen(PADLO_NAME);
        memcpy(shstr + cur, PADLO_NAME, n + 1);
        cur += (uint32_t)n + 1;
    }
    /* Add ".shstrtab" entry for our synthesized shstrtab section. */
    uint32_t shstr_name = cur;
    {
        const char *nm = ".shstrtab";
        size_t n = strlen(nm);
        memcpy(shstr + cur, nm, n + 1);
        cur += (uint32_t)n + 1;
    }
    if(cur > g_out_shstr_size)
        die("shstrtab overflow (%u > %u)", cur, g_out_shstr_size);

    uint32_t symtab_name = cur;
    {
        const char *nm = ".symtab";
        size_t n = strlen(nm);
        memcpy(shstr + cur, nm, n + 1);
        cur += (uint32_t)n + 1;
    }
    uint32_t strtab_name = cur;
    {
        const char *nm = ".strtab";
        size_t n = strlen(nm);
        memcpy(shstr + cur, nm, n + 1);
        cur += (uint32_t)n + 1;
    }
    if(cur > g_out_shstr_size)
        die("shstrtab overflow (%u > %u)", cur, g_out_shstr_size);

    uint16_t shstr_index = 0;
    uint16_t symtab_index = 0;
    uint16_t strtab_index = 0;

    /* Section headers. Index 0 is NULL. */
    uint8_t *sh = out + g_out_shoff;  /* already zeroed */
    int idx = 1;
    {
        struct Section *s = &g_sections[g_iopmod_idx];
        uint8_t *e = sh + idx * 40;
        wr32(e + 0, s->out_name);
        wr32(e + 4, SHT_LOPROC_IOPMOD);
        wr32(e + 8, s->sh_flags);
        wr32(e + 12, s->out_addr);
        wr32(e + 16, s->out_offset);
        wr32(e + 20, s->out_size);
        wr32(e + 24, 0);
        wr32(e + 28, 0);
        wr32(e + 32, s->sh_addralign);
        wr32(e + 36, s->sh_entsize);
        idx++;
    }
    for(int i = 0; i < g_e_shnum; i++) {
        struct Section *s = &g_sections[i];
        if(!s->keep)
            continue;
        if(i == g_iopmod_idx)
            continue;
        uint8_t *e = sh + idx * 40;
        wr32(e + 0, s->out_name);
        /* Normalize section type for .iopmod. */
        uint32_t t = s->sh_type;
        wr32(e + 4, t);
        wr32(e + 8, s->sh_flags);
        wr32(e + 12, s->out_addr);
        wr32(e + 16, s->out_offset);
        wr32(e + 20, s->out_size);
        if(s->sh_type == SHT_REL) {
            wr32(e + 24, g_out_shnum - 2); /* .symtab */
            if(s->sh_info < g_e_shnum)
                wr32(e + 28, g_sections[s->sh_info].out_index);
        }
        wr32(e + 32, s->sh_addralign);
        wr32(e + 36, s->sh_entsize);
        idx++;
    }
    if(scratch_reloc_word_count() > 0) {
        uint8_t *e = sh + idx * 40;
        wr32(e + 0, g_padlo_name_off);
        wr32(e + 4, SHT_PROGBITS);
        wr32(e + 8, SHF_ALLOC | SHF_WRITE);
        wr32(e + 12, g_padlo_addr);
        wr32(e + 16, g_padlo_offset);
        wr32(e + 20, g_padlo_size);
        wr32(e + 24, 0);
        wr32(e + 28, 0);
        wr32(e + 32, 4);
        wr32(e + 36, 0);
        idx++;
    }
    /* .shstrtab, .symtab, and .strtab are the final three sections. */
    {
        uint8_t *e = sh + idx * 40;
        shstr_index = (uint16_t)idx;
        wr32(e + 0, shstr_name);
        wr32(e + 4, SHT_STRTAB);
        wr32(e + 8, 0);
        wr32(e + 12, 0);
        wr32(e + 16, g_out_shstrtab_off);
        wr32(e + 20, g_out_shstr_size);
        wr32(e + 24, 0);
        wr32(e + 28, 0);
        wr32(e + 32, 1);
        wr32(e + 36, 0);
        idx++;
    }
    {
        uint8_t *e = sh + idx * 40;
        symtab_index = (uint16_t)idx;
        wr32(e + 0, symtab_name);
        wr32(e + 4, SHT_SYMTAB);
        wr32(e + 8, 0);
        wr32(e + 12, 0);
        wr32(e + 16, g_out_symtab_off);
        wr32(e + 20, 16);
        wr32(e + 24, idx + 1); /* .strtab */
        wr32(e + 28, 1);
        wr32(e + 32, 4);
        wr32(e + 36, 16);
        idx++;
    }
    {
        uint8_t *e = sh + idx * 40;
        strtab_index = (uint16_t)idx;
        wr32(e + 0, strtab_name);
        wr32(e + 4, SHT_STRTAB);
        wr32(e + 8, 0);
        wr32(e + 12, 0);
        wr32(e + 16, g_out_strtab_off);
        wr32(e + 20, 1);
        wr32(e + 24, 0);
        wr32(e + 28, 0);
        wr32(e + 32, 1);
        wr32(e + 36, 0);
    }
    (void)symtab_index;
    (void)strtab_index;
    wr16(out + 50, shstr_index);

    /* Relocation tables. The helper drops loader-incompatible types,
     * reorders HI16/LO16 pairs to be adjacent, and synthesizes missing
     * partner entries for LOADCORE-hostile shared-HI/shared-LO shapes.
     * Symbol indices are zeroed in the output: the IRX loader has no
     * symbol table, and `ld -r` plus our static-addend pass have already
     * baked the addends into the instructions. */
    for(int i = 0; i < g_e_shnum; i++) {
        struct Section *s = &g_sections[i];
        if(!s->keep || s->sh_type != SHT_REL)
            continue;
        emit_relocation_section(s, out);
    }

    /* Write the file. */
    FILE *f = fopen(g_out_name, "wb");
    if(!f)
        die("cannot open output %s", g_out_name);
    if(fwrite(out, 1, g_out_total_size, f) != g_out_total_size)
        die("short write");
    fclose(f);
    free(out);

    if(g_verbose) {
        fprintf(stderr, "Wrote %u bytes to %s\n", g_out_total_size, g_out_name);
    }
}

int main(int argc, char **argv) {
    int argi = 1;
    while(argi < argc && argv[argi][0] == '-') {
        if(strcmp(argv[argi], "-v") == 0) {
            g_verbose = 1;
            argi++;
            continue;
        }
        if(strcmp(argv[argi], "--") == 0) {
            argi++;
            break;
        }
        die("unknown option %s", argv[argi]);
    }
    if(argc - argi != 2) {
        fprintf(stderr, "Usage: %s [-v] input.elf output.irx\n", argv[0]);
        return 2;
    }
    g_in_name = argv[argi];
    g_out_name = argv[argi + 1];

    load_input();
    parse_elf_header();
    parse_section_headers();
    parse_symbols();
    mark_sections_to_keep();
    sort_relocations();
    validate_relocations();
    detect_stray_hi16();
    detect_extra_lo16();
    compute_layout();
    synthesize_iopmod();
    emit_irx();
    return 0;
}
