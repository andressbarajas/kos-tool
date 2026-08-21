/* host/tools/elf2xbe/elf2xbe.c
 *
 * Cleanroom ELF -> XBE converter for the original Microsoft Xbox.
 *
 * The i686-pc-xbox toolchain targets ELF32:
 *
 *     i686-pc-xbox-gcc ... -Wl,-T,kosload-ip.ld -o xbox-load-ip.elf
 *     elf2xbe -o default.xbe -b 0x10000 -t "xbox-load-ip" xbox-load-ip.elf
 *
 * One XBE section is emitted per PT_LOAD:
 *
 *     p_vaddr            -> section virtual address
 *     p_filesz           -> raw (file) size
 *     p_memsz            -> virtual size; the .bss tail is memsz - filesz
 *     p_flags & PF_W     -> XBE_SEC_WRITABLE
 *     p_flags & PF_X     -> XBE_SEC_EXECUTABLE
 *     e_entry            -> XBE entry point (XOR-obfuscated, retail key)
 *
 * ELF headers are parsed by explicit little-endian byte offsets rather than
 * <elf.h>, which does not exist on every host this tool is built on (macOS).
 *
 * Scaffold limitations:
 *   * UNSIGNED: the 256-byte header signature and the per-section SHA-1
 *     section digests are left zero.  Accepted where signature checks are
 *     disabled (xemu / modchip).
 *   * The kernel thunk table is taken from an ELF section named ".xbethnk" if
 *     present (a loader importing kernel exports would define its ordinal
 *     list there).  xbox-load-ip imports nothing, so an empty NULL-terminated
 *     table is emitted.
 *   * Retail XOR keys only.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* ---- little-endian byte helpers (host-endianness independent) ---- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
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
#define SHF_ALLOC   2

/* Certificate defaults: permissive for homebrew */
#define XBE_MEDIA_ALLOW  0x00FFFFFFu  /* MEDIA_MASK: all standard media types */
#define XBE_REGION_ALL   0x80000007u  /* NA | JP | RestOfWorld | Manufacturing */

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
static void die(const char *msg) { fprintf(stderr, "elf2xbe: %s\n", msg); exit(1); }

/* One PT_LOAD, flattened into the shape the XBE emitter below wants. */
#define SEG_NAME_MAX 32

typedef struct {
    char     name[SEG_NAME_MAX]; /* borrowed from the ELF section starting at vaddr */
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

    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "-o") && i + 1 < argc)      out_path = argv[++i];
        else if(!strcmp(argv[i], "-t") && i + 1 < argc) title = argv[++i];
        else if(!strcmp(argv[i], "-b") && i + 1 < argc) {
            image_base = (uint32_t)strtoul(argv[++i], NULL, 0);
            have_base = 1;
        }
        else if(argv[i][0] == '-')                      die("unknown option");
        else                                            in_path = argv[i];
    }
    if(!in_path) {
        fprintf(stderr,
                "usage: elf2xbe [-o out.xbe] [-b image_base] [-t \"Title\"] input.elf\n"
                "  -b  XBE image base (default: first PT_LOAD page - 0x%x header reserve)\n",
                HEADER_RESERVE);
        return 1;
    }

    /* ---- read the ELF image ---- */
    FILE *f = fopen(in_path, "rb");
    if(!f) die("cannot open input");
    fseek(f, 0, SEEK_END);
    long elf_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(elf_len < 52) die("input too small to be an ELF32");
    uint8_t *elf = malloc((size_t)elf_len);
    if(!elf) die("out of memory");
    if(fread(elf, 1, (size_t)elf_len, f) != (size_t)elf_len) die("short read");
    fclose(f);

    /* ---- parse the ELF header ---- */
    if(memcmp(elf, "\177ELF", 4))  die("not an ELF image (bad magic)");
    if(elf[4] != 1)                die("not ELF32 (EI_CLASS != ELFCLASS32)");
    if(elf[5] != 1)                die("not little-endian (EI_DATA != ELFDATA2LSB)");
    if(rd16(elf + 0x10) != ET_EXEC) die("not a fixed-address executable (e_type != ET_EXEC)");
    if(rd16(elf + 0x12) != EM_386)  die("not an i386 ELF (e_machine != EM_386)");

    uint32_t e_entry     = rd32(elf + 0x18);
    uint32_t e_phoff     = rd32(elf + 0x1C);
    uint32_t e_shoff     = rd32(elf + 0x20);
    uint16_t e_phentsize = rd16(elf + 0x2A);
    uint16_t e_phnum     = rd16(elf + 0x2C);
    uint16_t e_shentsize = rd16(elf + 0x2E);
    uint16_t e_shnum     = rd16(elf + 0x30);
    uint16_t e_shstrndx  = rd16(elf + 0x32);

    if(e_phnum == 0) die("no program headers");
    if(e_phentsize < 32) die("bad e_phentsize");
    if((uint64_t)e_phoff + (uint64_t)e_phnum * e_phentsize > (uint64_t)elf_len)
        die("program header table out of range");

    /* ---- collect the PT_LOAD segments ---- */
    seg_t *S = calloc(e_phnum, sizeof *S);
    if(!S) die("out of memory");
    uint16_t num_sec = 0;
    for(uint16_t i = 0; i < e_phnum; i++) {
        const uint8_t *ph = elf + e_phoff + (size_t)i * e_phentsize;
        if(rd32(ph + 0x00) != PT_LOAD) continue;
        uint32_t memsz = rd32(ph + 0x14);
        if(memsz == 0) continue;               /* nothing for the kernel to map */
        seg_t *s = &S[num_sec++];
        s->roff  = rd32(ph + 0x04);
        s->vaddr = rd32(ph + 0x08);
        s->rsize = rd32(ph + 0x10);
        s->vsize = memsz;
        s->flags = rd32(ph + 0x18);
        if(s->rsize > s->vsize) die("PT_LOAD p_filesz exceeds p_memsz");
        if((uint64_t)s->roff + s->rsize > (uint64_t)elf_len)
            die("PT_LOAD file data out of range");
        snprintf(s->name, sizeof s->name, ".seg%u", (unsigned)(num_sec - 1));
    }
    if(num_sec == 0) die("no loadable PT_LOAD segments");

    /* ---- name each segment after the ELF section that starts it, and find
     * the kernel-thunk ordinal list.  The names are cosmetic (they show up in
     * XBE dumps), but keeping .text/.data/.guest_arena keeps the output
     * readable. ---- */
    uint32_t thunk_vaddr = 0;
    if(e_shnum && e_shentsize >= 40 && e_shstrndx < e_shnum &&
       (uint64_t)e_shoff + (uint64_t)e_shnum * e_shentsize <= (uint64_t)elf_len) {
        const uint8_t *shstr_hdr = elf + e_shoff + (size_t)e_shstrndx * e_shentsize;
        uint32_t strtab_off = rd32(shstr_hdr + 0x10);
        uint32_t strtab_len = rd32(shstr_hdr + 0x14);
        if((uint64_t)strtab_off + strtab_len <= (uint64_t)elf_len) {
            for(uint16_t i = 0; i < e_shnum; i++) {
                const uint8_t *sh = elf + e_shoff + (size_t)i * e_shentsize;
                uint32_t sh_name  = rd32(sh + 0x00);
                uint32_t sh_flags = rd32(sh + 0x08);
                uint32_t sh_addr  = rd32(sh + 0x0C);
                if(!(sh_flags & SHF_ALLOC) || sh_name >= strtab_len) continue;
                const char *nm = (const char *)elf + strtab_off + sh_name;
                if(!strcmp(nm, THUNK_SECTION)) thunk_vaddr = sh_addr;
                for(uint16_t j = 0; j < num_sec; j++) {
                    if(S[j].vaddr == sh_addr && !strncmp(S[j].name, ".seg", 4)) {
                        snprintf(S[j].name, sizeof S[j].name, "%s", nm);
                        break;
                    }
                }
            }
        }
    }

    /* ---- image base ---- */
    uint32_t min_vaddr = S[0].vaddr, max_end = 0;
    for(uint16_t i = 0; i < num_sec; i++) {
        if(S[i].vaddr < min_vaddr) min_vaddr = S[i].vaddr;
        if(S[i].vaddr + S[i].vsize > max_end) max_end = S[i].vaddr + S[i].vsize;
    }
    if(!have_base) image_base = (min_vaddr & ~0xFFFu) - HEADER_RESERVE;
    if(min_vaddr < image_base)
        die("a PT_LOAD lies below the image base");
    if(min_vaddr - image_base < HEADER_RESERVE)
        die("first PT_LOAD overlaps the reserved XBE header page; "
            "the linker script must leave image_base..+0x1000 free");
    uint32_t size_of_image = align_up(max_end - image_base, 0x1000);

    /* ---- XBE header region layout (loaded 1:1 at image_base) ---- */
    uint32_t off = 0;
    uint32_t hdr_off    = off; off += XBE_HDR_SIZE;
    uint32_t cert_off   = off; off += XBE_CERT_SIZE;
    uint32_t sec_off    = off; off += (uint32_t)num_sec * XBE_SECHDR_SIZE;
    uint32_t libver_off = off; off += XBE_LIBVER_SIZE;

    uint32_t *name_off = calloc(num_sec, sizeof(uint32_t));
    if(!name_off) die("out of memory");
    for(uint16_t i = 0; i < num_sec; i++) {
        name_off[i] = off;
        off += (uint32_t)strlen(S[i].name) + 1;
    }
    off = align_up(off, 4);

    /* per-section head/tail page reference-count words (shared between
       neighbours): (num_sec + 1) u16 entries */
    uint32_t ref_off = off; off += (uint32_t)(num_sec + 1) * 2; off = align_up(off, 4);

    /* fallback empty kernel thunk table: a single NULL terminator */
    uint32_t thunk_off = off; off += 4;

    uint32_t headers_size = off;
    if(headers_size > HEADER_RESERVE)
        die("XBE headers exceed the reserved page; raise HEADER_RESERVE and the linker reserve");

    /* ---- section raw-data layout in the XBE file ---- */
    uint32_t file_off = align_up(headers_size, 0x20);
    uint32_t *raw_addr = calloc(num_sec, sizeof(uint32_t));
    if(!raw_addr) die("out of memory");
    for(uint16_t i = 0; i < num_sec; i++) {
        if(S[i].rsize) {
            raw_addr[i] = file_off;
            file_off = align_up(file_off + S[i].rsize, 0x20);
        }
    }
    uint32_t xbe_len = file_off;

    uint8_t *xbe = calloc(1, xbe_len);
    if(!xbe) die("out of memory");

    uint32_t kt_addr = thunk_vaddr ? thunk_vaddr : (image_base + thunk_off);

    /* ---- image header ---- */
    uint8_t *H = xbe + hdr_off;
    memcpy(H + 0x000, "XBEH", 4);
    /* 0x004 digital signature: left zero (unsigned) */
    wr32(H + 0x104, image_base);
    wr32(H + 0x108, headers_size);
    wr32(H + 0x10C, size_of_image);
    wr32(H + 0x110, XBE_HDR_SIZE);
    wr32(H + 0x114, (uint32_t)time(NULL));
    wr32(H + 0x118, image_base + cert_off);
    wr32(H + 0x11C, num_sec);
    wr32(H + 0x120, image_base + sec_off);
    wr32(H + 0x124, 0);                                 /* init flags */
    wr32(H + 0x128, e_entry ^ XOR_EP_RETAIL);           /* entry point */
    wr32(H + 0x12C, 0);                                 /* TLS addr */
    wr32(H + 0x130, XBE_STACK_RESERVE);
    wr32(H + 0x134, XBE_HEAP_RESERVE);
    wr32(H + 0x138, XBE_HEAP_COMMIT);
    wr32(H + 0x13C, image_base);
    wr32(H + 0x140, size_of_image);
    wr32(H + 0x144, 0);                                 /* PE checksum (none in ELF) */
    wr32(H + 0x148, (uint32_t)time(NULL));              /* PE timedate */
    wr32(H + 0x14C, 0);                                 /* debug pathname addr */
    wr32(H + 0x150, 0);                                 /* debug filename addr */
    wr32(H + 0x154, 0);                                 /* utf16 debug filename addr */
    wr32(H + 0x158, kt_addr ^ XOR_KT_RETAIL);           /* kernel thunk addr */
    wr32(H + 0x15C, 0);                                 /* non-kernel import dir */
    wr32(H + 0x160, 1);                                 /* number of library versions */
    wr32(H + 0x164, image_base + libver_off);
    wr32(H + 0x168, image_base + libver_off);           /* kernel library version */
    wr32(H + 0x16C, 0);                                 /* XAPI library version */
    wr32(H + 0x170, 0);                                 /* logo bitmap addr */
    wr32(H + 0x174, 0);                                 /* logo bitmap size */

    /* ---- certificate ---- */
    uint8_t *C = xbe + cert_off;
    wr32(C + 0x000, XBE_CERT_SIZE);
    wr32(C + 0x004, (uint32_t)time(NULL));
    wr32(C + 0x008, title_id);
    for(int i = 0; i < 40 && title[i]; i++)      /* 0x00C wszTitleName[40], UTF-16LE */
        wr16(C + 0x00C + i * 2, (uint8_t)title[i]);
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
        wr32(SH + 0x14, image_base + name_off[i]);
        wr32(SH + 0x18, 0);                                            /* name reference count */
        wr32(SH + 0x1C, image_base + ref_off + (uint32_t)i * 2);       /* head page ref count */
        wr32(SH + 0x20, image_base + ref_off + (uint32_t)(i + 1) * 2); /* tail page ref count */
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
    fclose(o);

    printf("elf2xbe: wrote %s (%u bytes, UNSIGNED)\n", out_path, xbe_len);
    printf("  base=0x%08x  entry=0x%08x  headers=0x%x  sections=%u  thunks@0x%08x%s\n",
           image_base, e_entry, headers_size, num_sec, kt_addr,
           thunk_vaddr ? " (loader)" : " (empty)");
    for(uint16_t i = 0; i < num_sec; i++)
        printf("    %-14s va=0x%08x  vsize=0x%08x  raw=0x%06x  rsize=0x%08x  %c%c%c\n",
               S[i].name, S[i].vaddr, S[i].vsize, raw_addr[i], S[i].rsize,
               (S[i].flags & 4) ? 'r' : '-',
               (S[i].flags & PF_W) ? 'w' : '-',
               (S[i].flags & PF_X) ? 'x' : '-');
    return 0;
}
