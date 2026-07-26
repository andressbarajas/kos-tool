/* host/tools/pe2xbe/pe2xbe.c
 *
 * Cleanroom PE -> XBE converter for the original Microsoft Xbox.
 *
 * Vendored into kos-tool from the KallistiOS xbox_toolchain branch
 * (utils/pe2xbe), the author's own work.  Reads a 32-bit PE/COFF executable
 * (pei-i386) produced by the i686-pc-xbox toolchain and writes a default.xbe:
 *
 *     i686-pc-xbox-gcc ... -Wl,-T,kosload-ip.ld -o xbox-load-ip.exe
 *     pe2xbe -o default.xbe -t "xbox-load-ip" xbox-load-ip.exe
 *
 * Scaffold limitations:
 *   * UNSIGNED: the 256-byte header signature and the per-section SHA-1
 *     section digests are left zero.  Accepted where signature checks are
 *     disabled (xemu / modchip).
 *   * The kernel thunk table is taken from a PE section named ".xbethnk" if
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
#define HEADER_RESERVE  0x1000u       /* must match the reserve in the Xbox linker script */

/* Name of the PE section carrying the loader's kernel-thunk ordinal list. */
#define THUNK_SECTION   ".xbethnk"

/* XBE section flags */
#define XBE_SEC_WRITABLE   0x00000001u
#define XBE_SEC_PRELOAD    0x00000002u
#define XBE_SEC_EXECUTABLE 0x00000004u

/* PE section characteristics */
#define PE_SCN_CNT_CODE  0x00000020u
#define PE_SCN_MEM_EXEC  0x20000000u
#define PE_SCN_MEM_WRITE 0x80000000u

/* Certificate defaults: permissive for homebrew */
#define XBE_MEDIA_ALLOW  0x00FFFFFFu  /* MEDIA_MASK: all standard media types */
#define XBE_REGION_ALL   0x80000007u  /* NA | JP | RestOfWorld | Manufacturing */

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
static void die(const char *msg) { fprintf(stderr, "pe2xbe: %s\n", msg); exit(1); }

int main(int argc, char **argv) {
    const char *in_path = NULL;
    const char *out_path = "default.xbe";
    const char *title = "KallistiOS Xbox";
    uint32_t title_id = 0x4B4F0001u; /* "KO" 0001 - arbitrary homebrew id */

    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "-o") && i + 1 < argc)      out_path = argv[++i];
        else if(!strcmp(argv[i], "-t") && i + 1 < argc) title = argv[++i];
        else if(argv[i][0] == '-')                      die("unknown option");
        else                                            in_path = argv[i];
    }
    if(!in_path) {
        fprintf(stderr, "usage: pe2xbe [-o out.xbe] [-t \"Title\"] input.exe\n");
        return 1;
    }

    /* ---- read the PE image ---- */
    FILE *f = fopen(in_path, "rb");
    if(!f) die("cannot open input");
    fseek(f, 0, SEEK_END);
    long pe_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(pe_len < 0x40) die("input too small to be a PE");
    uint8_t *pe = malloc((size_t)pe_len);
    if(!pe) die("out of memory");
    if(fread(pe, 1, (size_t)pe_len, f) != (size_t)pe_len) die("short read");
    fclose(f);

    /* ---- parse PE/COFF headers ---- */
    if(pe[0] != 'M' || pe[1] != 'Z') die("not a PE image (missing MZ)");
    uint32_t e_lfanew = rd32(pe + 0x3C);
    if(e_lfanew + 24 > (uint32_t)pe_len) die("bad e_lfanew");
    if(memcmp(pe + e_lfanew, "PE\0\0", 4)) die("not a PE image (missing PE signature)");

    const uint8_t *coff = pe + e_lfanew + 4;
    if(rd16(coff + 0) != 0x014C) die("not an i386 PE (machine != 0x14c)");
    uint16_t num_sec     = rd16(coff + 2);
    uint32_t pe_timedate = rd32(coff + 4);
    uint16_t opt_size    = rd16(coff + 16);

    const uint8_t *opt = coff + 20;
    if(opt_size < 88 || opt + opt_size > pe + pe_len)
        die("optional header out of range");
    if(rd16(opt + 0) != 0x010B) die("not PE32 (optional header magic != 0x10b)");
    uint32_t entry_rva   = rd32(opt + 16);
    uint32_t image_base  = rd32(opt + 28);
    uint32_t pe_sizeimg  = rd32(opt + 56);
    uint32_t pe_checksum = rd32(opt + 64);
    uint32_t stack_res   = rd32(opt + 72);
    uint32_t heap_res    = rd32(opt + 80);
    uint32_t heap_commit = rd32(opt + 84);

    const uint8_t *sectab = opt + opt_size;
    if(num_sec == 0) die("no sections");
    if(sectab + (size_t)num_sec * 40 > pe + pe_len) die("section table out of range");

    typedef struct { char name[9]; uint32_t vsize, vaddr, rsize, rptr, chars; } pesec_t;
    pesec_t *S = calloc(num_sec, sizeof *S);
    if(!S) die("out of memory");
    for(uint16_t i = 0; i < num_sec; i++) {
        const uint8_t *sh = sectab + (size_t)i * 40;
        memcpy(S[i].name, sh, 8); S[i].name[8] = 0;
        S[i].vsize = rd32(sh + 8);
        S[i].vaddr = rd32(sh + 12);
        S[i].rsize = rd32(sh + 16);
        S[i].rptr  = rd32(sh + 20);
        S[i].chars = rd32(sh + 36);
        /* PE BSS/NOLOAD sections have virtual size but no backing file data.
         * GNU ld represents these with PointerToRawData == 0; never interpret
         * their virtual size as bytes to copy from the DOS header. */
        if(S[i].rptr == 0) S[i].rsize = 0;
        /* don't copy file padding past the section's virtual size */
        if(S[i].rsize > S[i].vsize && S[i].vsize != 0) S[i].rsize = S[i].vsize;
    }

    /* Locate the loader's kernel-thunk ordinal list, if any.  The list is a
     * NULL-terminated u32 array (each entry = kernelExportOrdinal | 0x80000000)
     * built by the loader in a dedicated PE section; we point the XBE header at
     * its virtual address so the Xbox kernel patches it in place at load. */
    uint32_t thunk_vaddr = 0;
    for(uint16_t i = 0; i < num_sec; i++) {
        if(!strncmp(S[i].name, THUNK_SECTION, 8)) {
            thunk_vaddr = image_base + S[i].vaddr;
            break;
        }
    }

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
    wr32(H + 0x10C, pe_sizeimg);
    wr32(H + 0x110, XBE_HDR_SIZE);
    wr32(H + 0x114, (uint32_t)time(NULL));
    wr32(H + 0x118, image_base + cert_off);
    wr32(H + 0x11C, num_sec);
    wr32(H + 0x120, image_base + sec_off);
    wr32(H + 0x124, 0);                                         /* init flags */
    wr32(H + 0x128, (image_base + entry_rva) ^ XOR_EP_RETAIL);  /* entry point */
    wr32(H + 0x12C, 0);                                         /* TLS addr */
    wr32(H + 0x130, stack_res ? stack_res : 0x10000);
    wr32(H + 0x134, heap_res);
    wr32(H + 0x138, heap_commit);
    wr32(H + 0x13C, image_base);
    wr32(H + 0x140, pe_sizeimg);
    wr32(H + 0x144, pe_checksum);
    wr32(H + 0x148, pe_timedate);
    wr32(H + 0x14C, 0);                                         /* debug pathname addr */
    wr32(H + 0x150, 0);                                         /* debug filename addr */
    wr32(H + 0x154, 0);                                         /* utf16 debug filename addr */
    wr32(H + 0x158, kt_addr ^ XOR_KT_RETAIL);                   /* kernel thunk addr */
    wr32(H + 0x15C, 0);                                         /* non-kernel import dir */
    wr32(H + 0x160, 1);                                         /* number of library versions */
    wr32(H + 0x164, image_base + libver_off);
    wr32(H + 0x168, image_base + libver_off);                   /* kernel library version */
    wr32(H + 0x16C, 0);                                         /* XAPI library version */
    wr32(H + 0x170, 0);                                         /* logo bitmap addr */
    wr32(H + 0x174, 0);                                         /* logo bitmap size */

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
        if(S[i].chars & (PE_SCN_MEM_EXEC | PE_SCN_CNT_CODE)) flags |= XBE_SEC_EXECUTABLE;
        if((S[i].chars & PE_SCN_MEM_WRITE) || !strcmp(S[i].name, ".guest"))
            flags |= XBE_SEC_WRITABLE;
        wr32(SH + 0x00, flags);
        wr32(SH + 0x04, image_base + S[i].vaddr);
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
        if((uint64_t)S[i].rptr + S[i].rsize > (uint32_t)pe_len) die("section raw data out of range");
        memcpy(xbe + raw_addr[i], pe + S[i].rptr, S[i].rsize);
    }

    /* ---- write the XBE ---- */
    FILE *o = fopen(out_path, "wb");
    if(!o) die("cannot open output");
    if(fwrite(xbe, 1, xbe_len, o) != xbe_len) die("short write");
    fclose(o);

    printf("pe2xbe: wrote %s (%u bytes, UNSIGNED)\n", out_path, xbe_len);
    printf("  base=0x%08x  entry=0x%08x  headers=0x%x  sections=%u  thunks@0x%08x%s\n",
           image_base, image_base + entry_rva, headers_size, num_sec, kt_addr,
           thunk_vaddr ? " (loader)" : " (empty)");
    return 0;
}
