/*
 * Application: elf2xbe
 *
 * Why we need it:
 * The Xbox compiler and linker create an ELF executable, but the original
 * Xbox launches programs in XBE format. This tool creates the XBE wrapper and
 * layout needed to start the program.
 *
 * What it does:
 * elf2xbe turns each loadable area of the ELF into an XBE section, copies the
 * program's code and data, records how much zero-filled memory it needs, and
 * builds the Xbox headers. It also stores the application title, start
 * address, access flags, build time, and list of Xbox system functions used by
 * the program. If the ELF has no `.xbethnk` system-function list, the tool
 * writes an empty one.
 *
 * Output file structure:
 *   - XBE header with the start address and overall image layout
 *   - certificate with the application title and launch permissions
 *   - section descriptions, names, and library version
 *   - system-function lookup table
 *   - program code and initialized data, split into XBE sections
 *
 * Uninitialized data is recorded by size and filled with zero when the program
 * starts. The generated XBE is unsigned, so it is intended for a development
 * environment or a console configured to run homebrew applications. Set
 * `SOURCE_DATE_EPOCH` to use a repeatable build time.
 *
 * Usage:
 *   elf2xbe [-o output.xbe] [-b image_base] [-t "Title"] input.elf
 *
 * TODO:
 *   - Generate the SHA-1 digest stored in each XBE section description.
 *   - Add an option for the alternate start-address and system-function keys
 *     used by Xbox development environments.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <limits.h>
#include <errno.h>

/* ---- little-endian byte helpers (host-endianness independent) ---- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

/* ---- XBE constants (see xboxdevwiki XBE format) ---- */
#define XOR_EP_RETAIL   0xA8FC57ABu   /* entry-point obfuscation, retail kernel */
#define XOR_KT_RETAIL   0x5B6D40B6u   /* kernel-thunk obfuscation, retail kernel */

#define XBE_HDR_SIZE    0x178u
#define XBE_CERT_SIZE   0x1D0u        /* modern certificate (WCHAR[40] name + alt sig keys) */
#define XBE_SECHDR_SIZE 0x38u
#define XBE_LIBVER_SIZE 0x10u
#define HEADER_RESERVE  0x1000u       /* must match the reserve in the Xbox linker script
                                       * and XBOX_HEADER_RESERVE in mk/memory.mk */
#define MAX_XBE_SECTIONS \
    ((HEADER_RESERVE - XBE_HDR_SIZE - XBE_CERT_SIZE - XBE_LIBVER_SIZE) / XBE_SECHDR_SIZE)

/* The loader switches to its own linker-reserved stack in crt0, so the kernel
 * only has to get it as far as the entry point.  ELF carries no equivalent of
 * PE's SizeOfStackReserve/SizeOfHeapReserve, so name the values here. */
#define XBE_STACK_RESERVE 0x00010000u
#define XBE_HEAP_RESERVE  0x00100000u
#define XBE_HEAP_COMMIT   0x00001000u

/* Name of the ELF section carrying the loader's kernel-thunk ordinal list. */
#define THUNK_SECTION   ".xbethnk"

/* XBE section flags */
#define XBE_SEC_WRITABLE   0x00000001u
#define XBE_SEC_PRELOAD    0x00000002u
#define XBE_SEC_EXECUTABLE 0x00000004u

/* ELF32 constants */
#define ET_EXEC     2
#define EM_386      3
#define PT_LOAD     1
#define PF_X        1
#define PF_W        2
#define PF_R        4
#define SHT_NULL    0
#define SHT_PROGBITS 1
#define SHT_STRTAB  3
#define SHT_NOBITS  8
#define SHF_ALLOC   2

/* Certificate defaults: permissive for homebrew */
#define XBE_MEDIA_ALLOW  0x00FFFFFFu  /* MEDIA_MASK: all standard media types */
#define XBE_REGION_ALL   0x80000007u  /* NA | JP | RestOfWorld | Manufacturing */

#if defined(_MSC_VER)
#define ELF2XBE_NORETURN __declspec(noreturn)
#elif defined(__GNUC__) || defined(__clang__)
#define ELF2XBE_NORETURN __attribute__((noreturn))
#else
#define ELF2XBE_NORETURN
#endif

static ELF2XBE_NORETURN void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "elf2xbe: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static uint32_t checked_add_u32(uint32_t a, uint32_t b, const char *what) {
    if(a > UINT32_MAX - b)
        die("%s overflows the 32-bit XBE address space", what);
    return a + b;
}

static uint32_t checked_mul_u32(uint32_t a, uint32_t b, const char *what) {
    if(a != 0 && b > UINT32_MAX / a)
        die("%s overflows the 32-bit XBE address space", what);
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

static int ranges_overlap(uint32_t a_start, uint32_t a_size,
                          uint32_t b_start, uint32_t b_size) {
    return a_size != 0 && b_size != 0 &&
           (uint64_t)a_start < (uint64_t)b_start + b_size &&
           (uint64_t)b_start < (uint64_t)a_start + a_size;
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

static uint32_t parse_u32(const char *text, int base, const char *what) {
    if(!text[0] || text[0] == '-')
        die("invalid %s: %s", what, text);
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, base);
    if(errno == ERANGE || *end != '\0' || value > UINT32_MAX)
        die("invalid %s: %s", what, text);
    return (uint32_t)value;
}

static uint32_t build_timestamp(void) {
    const char *epoch = getenv("SOURCE_DATE_EPOCH");
    if(epoch)
        return parse_u32(epoch, 10, "SOURCE_DATE_EPOCH");

    time_t now = time(NULL);
    if(now == (time_t)-1 || (uint64_t)now > UINT32_MAX)
        die("current time cannot be represented in an XBE timestamp");
    return (uint32_t)now;
}

static uint32_t next_utf8_codepoint(const unsigned char **cursor) {
    const unsigned char *text = *cursor;
    uint32_t codepoint;

    if(text[0] < 0x80) {
        codepoint = text[0];
        *cursor += 1;
    } else if(text[0] >= 0xC2 && text[0] <= 0xDF) {
        if(text[1] == 0 || (text[1] & 0xC0) != 0x80)
            die("title is not valid UTF-8");
        codepoint = ((uint32_t)(text[0] & 0x1F) << 6) |
                    (uint32_t)(text[1] & 0x3F);
        *cursor += 2;
    } else if(text[0] >= 0xE0 && text[0] <= 0xEF) {
        if(text[1] == 0 || text[2] == 0 ||
           (text[1] & 0xC0) != 0x80 || (text[2] & 0xC0) != 0x80 ||
           (text[0] == 0xE0 && text[1] < 0xA0) ||
           (text[0] == 0xED && text[1] >= 0xA0))
            die("title is not valid UTF-8");
        codepoint = ((uint32_t)(text[0] & 0x0F) << 12) |
                    ((uint32_t)(text[1] & 0x3F) << 6) |
                    (uint32_t)(text[2] & 0x3F);
        *cursor += 3;
    } else if(text[0] >= 0xF0 && text[0] <= 0xF4) {
        if(text[1] == 0 || text[2] == 0 || text[3] == 0 ||
           (text[1] & 0xC0) != 0x80 || (text[2] & 0xC0) != 0x80 ||
           (text[3] & 0xC0) != 0x80 ||
           (text[0] == 0xF0 && text[1] < 0x90) ||
           (text[0] == 0xF4 && text[1] > 0x8F))
            die("title is not valid UTF-8");
        codepoint = ((uint32_t)(text[0] & 0x07) << 18) |
                    ((uint32_t)(text[1] & 0x3F) << 12) |
                    ((uint32_t)(text[2] & 0x3F) << 6) |
                    (uint32_t)(text[3] & 0x3F);
        *cursor += 4;
    } else {
        die("title is not valid UTF-8");
    }
    return codepoint;
}

static void write_title_utf16le(uint8_t *destination, const char *title) {
    const unsigned char *cursor = (const unsigned char *)title;
    uint32_t units = 0;
    while(*cursor) {
        uint32_t codepoint = next_utf8_codepoint(&cursor);
        if(codepoint <= 0xFFFF) {
            if(units >= 40)
                die("title exceeds the 40-character XBE certificate field");
            wr16(destination + units * 2, (uint16_t)codepoint);
            units++;
        } else {
            if(units > 38)
                die("title exceeds the 40-character XBE certificate field");
            codepoint -= 0x10000;
            wr16(destination + units * 2,
                 (uint16_t)(0xD800u + (codepoint >> 10)));
            wr16(destination + (units + 1) * 2,
                 (uint16_t)(0xDC00u + (codepoint & 0x3FF)));
            units += 2;
        }
    }
}

/* One PT_LOAD, flattened into the shape the XBE emitter below wants. */
#define SEG_NAME_MAX 32

typedef struct {
    const char *name;    /* borrowed from the ELF section starting at vaddr */
    char     fallback_name[SEG_NAME_MAX];
    int      has_elf_name;
    uint32_t vaddr;     /* absolute virtual address (not an RVA) */
    uint32_t vsize;     /* p_memsz */
    uint32_t rsize;     /* p_filesz */
    uint32_t roff;      /* p_offset into the input file */
    uint32_t flags;     /* p_flags */
} seg_t;

int main(int argc, char **argv) {
    const char *in_path = NULL;
    const char *out_path = "default.xbe";
    const char *title = "KallistiOS Xbox";
    uint32_t title_id = 0x4B4F0001u; /* "KO" 0001 - arbitrary homebrew id */
    uint32_t image_base = 0;
    int have_base = 0;
    int output_was_set = 0, title_was_set = 0;

    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "-o")) {
            if(output_was_set)
                die("output path specified more than once");
            if(i + 1 >= argc)
                die("-o requires an output path");
            out_path = argv[++i];
            output_was_set = 1;
        } else if(!strcmp(argv[i], "-t")) {
            if(title_was_set)
                die("title specified more than once");
            if(i + 1 >= argc)
                die("-t requires a title");
            title = argv[++i];
            title_was_set = 1;
        } else if(!strcmp(argv[i], "-b")) {
            if(have_base)
                die("image base specified more than once");
            if(i + 1 >= argc)
                die("-b requires an image base");
            image_base = parse_u32(argv[++i], 0, "image base");
            have_base = 1;
        } else if(argv[i][0] == '-') {
            die("unknown option %s", argv[i]);
        } else {
            if(in_path)
                die("more than one input file specified");
            in_path = argv[i];
        }
    }
    if(!in_path) {
        fprintf(stderr,
                "usage: elf2xbe [-o out.xbe] [-b image_base] [-t \"Title\"] input.elf\n"
                "  -b  XBE image base (default: first PT_LOAD page - 0x%x header reserve)\n",
                HEADER_RESERVE);
        return 1;
    }

    uint8_t title_utf16[40 * 2] = {0};
    write_title_utf16le(title_utf16, title);
    uint32_t timestamp = build_timestamp();

    /* ---- read the ELF image ---- */
    FILE *f = fopen(in_path, "rb");
    if(!f) die("cannot open input");
    if(fseek(f, 0, SEEK_END) != 0)
        die("cannot seek input");
    long elf_len = ftell(f);
    if(elf_len < 0)
        die("cannot determine input size");
    if(elf_len < 52)
        die("input too small to be an ELF32");
    if((uint64_t)elf_len > UINT32_MAX)
        die("input is too large for ELF32");
    if(fseek(f, 0, SEEK_SET) != 0)
        die("cannot rewind input");
    uint8_t *elf = malloc((size_t)elf_len);
    if(!elf) die("out of memory");
    if(fread(elf, 1, (size_t)elf_len, f) != (size_t)elf_len) die("short read");
    if(fclose(f) != 0)
        die("cannot close input");
    uint32_t elf_size = (uint32_t)elf_len;

    /* ---- parse the ELF header ---- */
    if(memcmp(elf, "\177ELF", 4))  die("not an ELF image (bad magic)");
    if(elf[4] != 1)                die("not ELF32 (EI_CLASS != ELFCLASS32)");
    if(elf[5] != 1)                die("not little-endian (EI_DATA != ELFDATA2LSB)");
    if(elf[6] != 1)                die("unsupported ELF identification version");
    if(rd16(elf + 0x10) != ET_EXEC) die("not a fixed-address executable (e_type != ET_EXEC)");
    if(rd16(elf + 0x12) != EM_386)  die("not an i386 ELF (e_machine != EM_386)");
    if(rd32(elf + 0x14) != 1)       die("unsupported ELF header version");

    uint32_t e_entry     = rd32(elf + 0x18);
    uint32_t e_phoff     = rd32(elf + 0x1C);
    uint32_t e_shoff     = rd32(elf + 0x20);
    uint16_t e_ehsize    = rd16(elf + 0x28);
    uint16_t e_phentsize = rd16(elf + 0x2A);
    uint16_t e_phnum     = rd16(elf + 0x2C);
    uint16_t e_shentsize = rd16(elf + 0x2E);
    uint16_t e_shnum     = rd16(elf + 0x30);
    uint16_t e_shstrndx  = rd16(elf + 0x32);

    if(e_ehsize != 52) die("unexpected ELF header size");
    if(e_phnum == 0) die("no program headers");
    if(e_phentsize != 32) die("unexpected program header size");
    require_range(e_phoff, (uint64_t)e_phnum * e_phentsize, elf_size,
                  "program header table");
    if(e_shnum == 0) {
        if(e_shoff != 0 || e_shstrndx != 0)
            die("extended or malformed section numbering is not supported");
    } else {
        if(e_shentsize != 40)
            die("unexpected section header size");
        if(e_shstrndx >= e_shnum)
            die("invalid section-name table index");
        require_range(e_shoff, (uint64_t)e_shnum * e_shentsize, elf_size,
                      "section header table");
    }

    /* ---- collect the PT_LOAD segments ---- */
    seg_t *S = calloc(e_phnum, sizeof *S);
    if(!S) die("out of memory");
    uint16_t num_sec = 0;
    for(uint16_t i = 0; i < e_phnum; i++) {
        const uint8_t *ph = elf + e_phoff + (size_t)i * e_phentsize;
        if(rd32(ph + 0x00) != PT_LOAD) continue;
        uint32_t memsz = rd32(ph + 0x14);
        if(memsz == 0) continue;               /* nothing for the kernel to map */
        if(num_sec >= MAX_XBE_SECTIONS)
            die("too many PT_LOAD segments for the reserved XBE header page");
        seg_t *s = &S[num_sec++];
        s->roff  = rd32(ph + 0x04);
        s->vaddr = rd32(ph + 0x08);
        s->rsize = rd32(ph + 0x10);
        s->vsize = memsz;
        s->flags = rd32(ph + 0x18);
        uint32_t alignment = rd32(ph + 0x1C);
        if(s->rsize > s->vsize)
            die("PT_LOAD p_filesz exceeds p_memsz");
        if(alignment > 1 && !is_power_of_two_u32(alignment))
            die("PT_LOAD has invalid alignment %u", alignment);
        if(alignment > 1 &&
           (s->roff & (alignment - 1)) != (s->vaddr & (alignment - 1)))
            die("PT_LOAD file and memory offsets violate its alignment");
        require_range(s->roff, s->rsize, elf_size, "PT_LOAD file data");
        checked_add_u32(s->vaddr, s->vsize, "PT_LOAD virtual range");

        for(uint16_t j = 0; j + 1 < num_sec; j++) {
            if(ranges_overlap(s->vaddr, s->vsize, S[j].vaddr, S[j].vsize))
                die("PT_LOAD virtual ranges overlap");
            if(ranges_overlap(s->roff, s->rsize, S[j].roff, S[j].rsize))
                die("PT_LOAD file ranges overlap");
        }

        snprintf(s->fallback_name, sizeof s->fallback_name, ".seg%u",
                 (unsigned)(num_sec - 1));
        s->name = s->fallback_name;
    }
    if(num_sec == 0) die("no loadable PT_LOAD segments");

    int entry_is_executable = 0;
    for(uint16_t i = 0; i < num_sec; i++) {
        if((S[i].flags & PF_X) && e_entry >= S[i].vaddr &&
           (uint64_t)e_entry < (uint64_t)S[i].vaddr + S[i].rsize) {
            entry_is_executable = 1;
            break;
        }
    }
    if(!entry_is_executable)
        die("ELF entry point does not lie in a file-backed executable PT_LOAD");

    /* ---- name each segment after the ELF section that starts it, and find
     * the kernel-thunk ordinal list.  The names are cosmetic (they show up in
     * XBE dumps), but keeping .text/.data/.guest_arena keeps the output
     * readable. ---- */
    uint32_t thunk_vaddr = 0;
    if(e_shnum != 0) {
        const uint8_t *section_zero = elf + e_shoff;
        if(rd32(section_zero + 0x04) != SHT_NULL)
            die("section zero is not SHT_NULL");

        const uint8_t *shstr_hdr = elf + e_shoff + (size_t)e_shstrndx * e_shentsize;
        if(rd32(shstr_hdr + 0x04) != SHT_STRTAB)
            die("section-name table is not SHT_STRTAB");
        uint32_t strtab_off = rd32(shstr_hdr + 0x10);
        uint32_t strtab_len = rd32(shstr_hdr + 0x14);
        require_range(strtab_off, strtab_len, elf_size,
                      "section-name string table");

        int found_thunk = 0;
        for(uint16_t i = 0; i < e_shnum; i++) {
            const uint8_t *sh = elf + e_shoff + (size_t)i * e_shentsize;
            uint32_t sh_name = rd32(sh + 0x00);
            uint32_t sh_type = rd32(sh + 0x04);
            uint32_t sh_flags = rd32(sh + 0x08);
            uint32_t sh_addr = rd32(sh + 0x0C);
            uint32_t sh_offset = rd32(sh + 0x10);
            uint32_t sh_size = rd32(sh + 0x14);
            uint32_t sh_alignment = rd32(sh + 0x20);

            char description[48];
            snprintf(description, sizeof(description), "section %u name", i);
            input_string_length(elf, strtab_off, strtab_len, sh_name,
                                description);
            const char *name = (const char *)elf + strtab_off + sh_name;

            if(sh_alignment != 0 && !is_power_of_two_u32(sh_alignment))
                die("section %u has invalid alignment %u", i, sh_alignment);
            if(sh_type != SHT_NOBITS)
                require_range(sh_offset, sh_size, elf_size, "section data");

            if(!(sh_flags & SHF_ALLOC)) {
                if(!strcmp(name, THUNK_SECTION))
                    die(THUNK_SECTION " is not allocated");
                continue;
            }

            checked_add_u32(sh_addr, sh_size, "allocated section virtual range");
            int containing_segment = -1;
            int memory_segment_found = 0;
            for(uint16_t j = 0; j < num_sec; j++) {
                if(sh_addr < S[j].vaddr ||
                   (uint64_t)(sh_addr - S[j].vaddr) + sh_size > S[j].vsize)
                    continue;
                memory_segment_found = 1;
                if(sh_type != SHT_NOBITS &&
                   (sh_offset < S[j].roff ||
                    sh_offset - S[j].roff != sh_addr - S[j].vaddr ||
                    (uint64_t)(sh_offset - S[j].roff) + sh_size > S[j].rsize))
                    continue;
                containing_segment = j;
                if(sh_addr == S[j].vaddr)
                    break;
            }
            if(containing_segment < 0) {
                if(memory_segment_found)
                    die("allocated section %s has an invalid PT_LOAD file mapping",
                        name);
                die("allocated section %s lies outside every PT_LOAD", name);
            }

            for(uint16_t j = 0; j < num_sec; j++) {
                if(S[j].vaddr == sh_addr && !S[j].has_elf_name) {
                    S[j].name = name;
                    S[j].has_elf_name = 1;
                    break;
                }
            }

            if(!strcmp(name, THUNK_SECTION)) {
                if(found_thunk)
                    die("multiple " THUNK_SECTION " sections");
                if(sh_type != SHT_PROGBITS || sh_size < 4 || sh_size % 4 != 0)
                    die(THUNK_SECTION " has an invalid size or type");
                if(rd32(elf + sh_offset + sh_size - 4) != 0)
                    die(THUNK_SECTION " is not NULL-terminated");
                thunk_vaddr = sh_addr;
                found_thunk = 1;
            }
        }
    }

    /* ---- image base ---- */
    uint32_t min_vaddr = S[0].vaddr, max_end = 0;
    for(uint16_t i = 0; i < num_sec; i++) {
        if(S[i].vaddr < min_vaddr) min_vaddr = S[i].vaddr;
        uint32_t end = checked_add_u32(S[i].vaddr, S[i].vsize,
                                       "PT_LOAD virtual range");
        if(end > max_end) max_end = end;
    }
    if(!have_base) {
        uint32_t first_page = min_vaddr & ~0xFFFu;
        if(first_page < HEADER_RESERVE)
            die("cannot derive an image base below the reserved header page");
        image_base = first_page - HEADER_RESERVE;
    }
    if((image_base & 0xFFFu) != 0)
        die("image base must be aligned to 0x1000 bytes");
    if(min_vaddr < image_base)
        die("a PT_LOAD lies below the image base");
    if(min_vaddr - image_base < HEADER_RESERVE)
        die("first PT_LOAD overlaps the reserved XBE header page; "
            "the linker script must leave image_base..+0x1000 free");
    uint32_t size_of_image = align_up(max_end - image_base, 0x1000,
                                      "XBE image size");
    checked_add_u32(image_base, size_of_image, "XBE image range");

    /* ---- XBE header region layout (loaded 1:1 at image_base) ---- */
    uint32_t off = 0;
    uint32_t hdr_off = off;
    off = checked_add_u32(off, XBE_HDR_SIZE, "XBE image header");
    uint32_t cert_off = off;
    off = checked_add_u32(off, XBE_CERT_SIZE, "XBE certificate");
    uint32_t sec_off = off;
    off = checked_add_u32(
        off, checked_mul_u32(num_sec, XBE_SECHDR_SIZE, "XBE section headers"),
        "XBE section headers");
    uint32_t libver_off = off;
    off = checked_add_u32(off, XBE_LIBVER_SIZE, "XBE library version");

    uint32_t *name_off = calloc(num_sec, sizeof(uint32_t));
    if(!name_off) die("out of memory");
    for(uint16_t i = 0; i < num_sec; i++) {
        name_off[i] = off;
        size_t name_length = strlen(S[i].name) + 1;
        if(name_length > UINT32_MAX)
            die("XBE section name is too long");
        off = checked_add_u32(off, (uint32_t)name_length,
                              "XBE section names");
    }
    off = align_up(off, 4, "XBE section names");

    /* per-section head/tail page reference-count words (shared between
       neighbours): (num_sec + 1) u16 entries */
    uint32_t ref_off = off;
    off = checked_add_u32(
        off, checked_mul_u32((uint32_t)num_sec + 1, 2,
                             "XBE section reference counts"),
        "XBE section reference counts");
    off = align_up(off, 4, "XBE section reference counts");

    /* fallback empty kernel thunk table: a single NULL terminator */
    uint32_t thunk_off = off;
    off = checked_add_u32(off, 4, "fallback kernel thunk table");

    uint32_t headers_size = off;
    if(headers_size > HEADER_RESERVE)
        die("XBE headers exceed the reserved page; raise HEADER_RESERVE and the linker reserve");

    /* ---- section raw-data layout in the XBE file ---- */
    uint32_t file_off = align_up(headers_size, 0x20, "XBE raw data offset");
    uint32_t *raw_addr = calloc(num_sec, sizeof(uint32_t));
    if(!raw_addr) die("out of memory");
    for(uint16_t i = 0; i < num_sec; i++) {
        if(S[i].rsize) {
            raw_addr[i] = file_off;
            file_off = checked_add_u32(file_off, S[i].rsize,
                                       "XBE raw section data");
            file_off = align_up(file_off, 0x20, "XBE raw section data");
        }
    }
    uint32_t xbe_len = file_off;

    uint8_t *xbe = calloc(1, xbe_len);
    if(!xbe) die("out of memory");

    uint32_t kt_addr = thunk_vaddr
                           ? thunk_vaddr
                           : checked_add_u32(image_base, thunk_off,
                                             "fallback kernel thunk address");

    /* ---- image header ---- */
    uint8_t *H = xbe + hdr_off;
    memcpy(H + 0x000, "XBEH", 4);
    /* 0x004 digital signature: left zero (unsigned) */
    wr32(H + 0x104, image_base);
    wr32(H + 0x108, headers_size);
    wr32(H + 0x10C, size_of_image);
    wr32(H + 0x110, XBE_HDR_SIZE);
    wr32(H + 0x114, timestamp);
    wr32(H + 0x118, checked_add_u32(image_base, cert_off,
                                    "XBE certificate address"));
    wr32(H + 0x11C, num_sec);
    wr32(H + 0x120, checked_add_u32(image_base, sec_off,
                                    "XBE section-header address"));
    wr32(H + 0x124, 0);                                 /* init flags */
    wr32(H + 0x128, e_entry ^ XOR_EP_RETAIL);           /* entry point */
    wr32(H + 0x12C, 0);                                 /* TLS addr */
    wr32(H + 0x130, XBE_STACK_RESERVE);
    wr32(H + 0x134, XBE_HEAP_RESERVE);
    wr32(H + 0x138, XBE_HEAP_COMMIT);
    wr32(H + 0x13C, image_base);
    wr32(H + 0x140, size_of_image);
    wr32(H + 0x144, 0);                                 /* PE checksum (none in ELF) */
    wr32(H + 0x148, timestamp);                         /* PE timedate */
    wr32(H + 0x14C, 0);                                 /* debug pathname addr */
    wr32(H + 0x150, 0);                                 /* debug filename addr */
    wr32(H + 0x154, 0);                                 /* utf16 debug filename addr */
    wr32(H + 0x158, kt_addr ^ XOR_KT_RETAIL);           /* kernel thunk addr */
    wr32(H + 0x15C, 0);                                 /* non-kernel import dir */
    wr32(H + 0x160, 1);                                 /* number of library versions */
    uint32_t libver_addr = checked_add_u32(image_base, libver_off,
                                           "XBE library-version address");
    wr32(H + 0x164, libver_addr);
    wr32(H + 0x168, libver_addr);                       /* kernel library version */
    wr32(H + 0x16C, 0);                                 /* XAPI library version */
    wr32(H + 0x170, 0);                                 /* logo bitmap addr */
    wr32(H + 0x174, 0);                                 /* logo bitmap size */

    /* ---- certificate ---- */
    uint8_t *C = xbe + cert_off;
    wr32(C + 0x000, XBE_CERT_SIZE);
    wr32(C + 0x004, timestamp);
    wr32(C + 0x008, title_id);
    memcpy(C + 0x00C, title_utf16, sizeof(title_utf16)); /* wszTitleName[40] */
    /* 0x05C alt title ids [16] = 0 */
    wr32(C + 0x09C, XBE_MEDIA_ALLOW);
    wr32(C + 0x0A0, XBE_REGION_ALL);
    wr32(C + 0x0A4, 0);                          /* ratings */
    wr32(C + 0x0A8, 0);                          /* disk number */
    wr32(C + 0x0AC, 1);                          /* version */
    /* 0x0B0 lan key, 0x0C0 signature key, 0x0D0 alt signature keys: zero */

    /* ---- section headers + name strings ---- */
    for(uint16_t i = 0; i < num_sec; i++) {
        uint8_t *SH = xbe + sec_off + (uint32_t)i * XBE_SECHDR_SIZE;
        uint32_t flags = XBE_SEC_PRELOAD;
        if(S[i].flags & PF_X) flags |= XBE_SEC_EXECUTABLE;
        if(S[i].flags & PF_W) flags |= XBE_SEC_WRITABLE;
        wr32(SH + 0x00, flags);
        wr32(SH + 0x04, S[i].vaddr);
        wr32(SH + 0x08, S[i].vsize);
        wr32(SH + 0x0C, raw_addr[i]);
        wr32(SH + 0x10, S[i].rsize);
        wr32(SH + 0x14, checked_add_u32(image_base, name_off[i],
                                        "XBE section-name address"));
        wr32(SH + 0x18, 0);                                            /* name reference count */
        uint32_t head_ref = checked_add_u32(
            ref_off, checked_mul_u32(i, 2, "XBE section reference address"),
            "XBE section reference address");
        uint32_t tail_ref = checked_add_u32(head_ref, 2,
                                            "XBE section reference address");
        wr32(SH + 0x1C, checked_add_u32(image_base, head_ref,
                                        "XBE head-page reference address"));
        wr32(SH + 0x20, checked_add_u32(image_base, tail_ref,
                                        "XBE tail-page reference address"));
        /* 0x24 section digest [20]: left zero (unsigned) */
        memcpy(xbe + name_off[i], S[i].name, strlen(S[i].name) + 1);
    }

    /* ---- one library version: the kernel ---- */
    {
        uint8_t *L = xbe + libver_off;
        memcpy(L + 0x00, "XBOXKRNL", 8);
        wr16(L + 0x08, 1);       /* major */
        wr16(L + 0x0A, 0);       /* minor */
        wr16(L + 0x0C, 5838);    /* build (placeholder retail kernel build) */
        wr16(L + 0x0E, 0);       /* flags */
    }

    /* fallback kernel thunk table + ref counts are already zero (calloc) */

    /* ---- copy section raw data ---- */
    for(uint16_t i = 0; i < num_sec; i++) {
        if(!S[i].rsize) continue;
        memcpy(xbe + raw_addr[i], elf + S[i].roff, S[i].rsize);
    }

    /* ---- write the XBE ---- */
    FILE *o = fopen(out_path, "wb");
    if(!o) die("cannot open output");
    if(fwrite(xbe, 1, xbe_len, o) != xbe_len) die("short write");
    if(fclose(o) != 0)
        die("cannot close output");

    printf("elf2xbe: wrote %s (%u bytes, UNSIGNED)\n", out_path, xbe_len);
    printf("  base=0x%08x  entry=0x%08x  headers=0x%x  sections=%u  thunks@0x%08x%s\n",
           image_base, e_entry, headers_size, num_sec, kt_addr,
           thunk_vaddr ? " (loader)" : " (empty)");
    for(uint16_t i = 0; i < num_sec; i++)
        printf("    %-14s va=0x%08x  vsize=0x%08x  raw=0x%06x  rsize=0x%08x  %c%c%c\n",
               S[i].name, S[i].vaddr, S[i].vsize, raw_addr[i], S[i].rsize,
               (S[i].flags & PF_R) ? 'r' : '-',
               (S[i].flags & PF_W) ? 'w' : '-',
               (S[i].flags & PF_X) ? 'x' : '-');
    free(xbe);
    free(raw_addr);
    free(name_off);
    free(S);
    free(elf);
    return 0;
}
