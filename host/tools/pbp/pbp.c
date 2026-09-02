/*
 * Application: pbp
 *
 * Why we need it:
 * The PSP menu launches an application from a file named `EBOOT.PBP`. A PRX by
 * itself contains the program, but it is not the menu-ready package the PSP
 * expects. This tool creates that package.
 *
 * What it does:
 * pbp creates a small application-information block containing values such as
 * the title and then places the input executable in the package as `DATA.PSP`.
 * It does not change the executable itself; the input is normally the PRX made
 * by `elf2prx`.
 *
 * Output file structure:
 *   - PBP header with the file version and location of every part
 *   - PARAM.SFO: application title and launch settings
 *   - ICON0.PNG: menu icon (currently empty)
 *   - ICON1.PMF: animated icon (currently empty)
 *   - PIC0.PNG: information image (currently empty)
 *   - PIC1.PNG: background image (currently empty)
 *   - SND0.AT3: menu audio (currently empty)
 *   - DATA.PSP: the input executable
 *   - DATA.PSAR: optional extra data (currently empty)
 *
 * Usage:
 *   pbp [-o EBOOT.PBP] [-t "Title"] input.prx
 *
 * TODO:
 *   - Add options for menu artwork, animation, and audio.
 *   - Add options for the application identifiers and other PARAM.SFO values.
 *   - Add an option for a DATA.PSAR payload if a future application needs one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }

#if defined(_MSC_VER)
#define PBP_NORETURN __declspec(noreturn)
#elif defined(__GNUC__) || defined(__clang__)
#define PBP_NORETURN __attribute__((noreturn))
#else
#define PBP_NORETURN
#endif

static PBP_NORETURN void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "pbp: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static uint32_t checked_add_u32(uint32_t a, uint32_t b, const char *what) {
    if(a > UINT32_MAX - b)
        die("%s exceeds the PBP 32-bit size limit", what);
    return a + b;
}

static uint32_t checked_mul_u32(uint32_t a, uint32_t b, const char *what) {
    if(a != 0 && b > UINT32_MAX / a)
        die("%s exceeds the PBP 32-bit size limit", what);
    return a * b;
}

static uint32_t align4(uint32_t value, const char *what) {
    return checked_add_u32(value, 3, what) & ~3u;
}

static void write_all(FILE *file, const void *data, size_t length,
                      const char *what) {
    if(length != 0 && fwrite(data, 1, length, file) != length)
        die("short write while writing %s", what);
}

/* ---- minimal PARAM.SFO builder ---- */

#define SFO_FMT_UTF8  0x0204   /* NUL-terminated UTF-8 string */
#define SFO_FMT_INT   0x0404   /* uint32 */
#define SFO_TITLE_STORAGE_MAX 256u

/* Build the PARAM.SFO key set that shipping bootable PSP EBOOTs carry.
 *
 * The index table is emitted in the order below, which is the order observed in
 * shipping EBOOT.PBPs: MEMSIZE first, then the remaining keys alphabetically.
 * Strict ascending key order is NOT a container requirement — MEMSIZE ahead of
 * BOOTABLE breaks it in every sample checked — but matching the conventional
 * order keeps us on the path the firmware parser is known to accept.
 *
 * DISC_ID/DISC_VERSION/REGION carry the conventional homebrew placeholder
 * values; the firmware only requires them to be present and well-formed.
 * MEMSIZE=1 requests the extra RAM available beyond the base user partition. */
static uint8_t *build_sfo(const char *title, uint32_t *out_len) {
    size_t title_length = strlen(title);
    if(title_length >= SFO_TITLE_STORAGE_MAX)
        die("title is too long (maximum %u bytes)", SFO_TITLE_STORAGE_MAX - 1);
    uint32_t title_data_length = (uint32_t)title_length + 1;
    uint32_t title_max = align4(title_data_length, "SFO title storage");
    struct {
        const char *key;
        uint16_t fmt;
        const char *sval;
        uint32_t ival;
        uint32_t maxlen;
    } ent[] = {
        { "MEMSIZE",        SFO_FMT_INT,  NULL,    1,     4 },
        { "BOOTABLE",       SFO_FMT_INT,  NULL,    1,     4 },
        { "CATEGORY",       SFO_FMT_UTF8, "MG",    0,     4 },
        { "DISC_ID",        SFO_FMT_UTF8, "UCJS10041", 0, 12 },
        { "DISC_VERSION",   SFO_FMT_UTF8, "1.00",  0,     8 },
        { "PARENTAL_LEVEL", SFO_FMT_INT,  NULL,    1,     4 },
        { "PSP_SYSTEM_VER", SFO_FMT_UTF8, "1.00",  0,     8 },
        { "REGION",         SFO_FMT_INT,  NULL,    0x8000, 4 },
        { "TITLE",          SFO_FMT_UTF8, title,   0,     title_max },
    };
    uint32_t n = (uint32_t)(sizeof(ent) / sizeof(ent[0]));
    if(n > 16)
        die("internal error: too many SFO entries");

    uint32_t key_table_start = checked_add_u32(
        20, checked_mul_u32(n, 16, "SFO index table"), "SFO key table offset");

    /* Key table. */
    uint8_t keytab[256];
    uint32_t koff[16], kpos = 0;
    for(uint32_t i = 0; i < n; i++) {
        size_t length = strlen(ent[i].key) + 1;
        if(kpos > sizeof(keytab) || length > sizeof(keytab) - kpos)
            die("SFO key table overflow");
        if(kpos > UINT16_MAX)
            die("SFO key offset exceeds its 16-bit field");
        koff[i] = kpos;
        memcpy(keytab + kpos, ent[i].key, length);
        kpos = checked_add_u32(kpos, (uint32_t)length, "SFO key table");
    }
    uint32_t key_table_len = align4(kpos, "SFO key table");
    for(uint32_t i = kpos; i < key_table_len; i++)
        keytab[i] = 0;

    uint32_t data_table_start = checked_add_u32(
        key_table_start, key_table_len, "SFO data table offset");

    /* Data table. */
    uint8_t datatab[512];
    uint32_t doff[16], dpos = 0, dlen[16];
    for(uint32_t i = 0; i < n; i++) {
        doff[i] = dpos;
        if(ent[i].fmt == SFO_FMT_INT) {
            dlen[i] = 4;
        } else {
            size_t length = strlen(ent[i].sval) + 1;
            if(length > UINT32_MAX)
                die("SFO string value is too large");
            dlen[i] = (uint32_t)length;
        }
        if(dlen[i] > ent[i].maxlen)
            die("SFO value for %s exceeds its declared storage", ent[i].key);
        if(dpos > sizeof(datatab) || ent[i].maxlen > sizeof(datatab) - dpos)
            die("SFO data table overflow");

        if(ent[i].fmt == SFO_FMT_INT)
            wr32(datatab + dpos, ent[i].ival);
        else
            memcpy(datatab + dpos, ent[i].sval, dlen[i]);
        memset(datatab + dpos + dlen[i], 0, ent[i].maxlen - dlen[i]);
        dpos = checked_add_u32(dpos, ent[i].maxlen, "SFO data table");
    }
    uint32_t data_table_len = align4(dpos, "SFO data table");

    uint32_t total = checked_add_u32(data_table_start, data_table_len,
                                     "PARAM.SFO size");
    uint8_t *sfo = calloc(1, total);
    if(!sfo) die("out of memory");

    /* Header. */
    sfo[0] = 0x00; sfo[1] = 'P'; sfo[2] = 'S'; sfo[3] = 'F';
    wr32(sfo + 4, 0x00000101);
    wr32(sfo + 8, key_table_start);
    wr32(sfo + 12, data_table_start);
    wr32(sfo + 16, (uint32_t)n);

    /* Index table. */
    for(uint32_t i = 0; i < n; i++) {
        uint8_t *e = sfo + 20 + i * 16;
        wr16(e + 0, (uint16_t)koff[i]);
        wr16(e + 2, ent[i].fmt);
        wr32(e + 4, dlen[i]);
        wr32(e + 8, ent[i].maxlen);
        wr32(e + 12, doff[i]);
    }
    memcpy(sfo + key_table_start, keytab, key_table_len);
    memcpy(sfo + data_table_start, datatab, data_table_len);

    *out_len = total;
    return sfo;
}

int main(int argc, char **argv) {
    const char *in_path = NULL, *out_path = "EBOOT.PBP", *title = "psp-load-usb";
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
        } else if(argv[i][0] == '-') {
            die("unknown option %s", argv[i]);
        } else {
            if(in_path)
                die("more than one input file specified");
            in_path = argv[i];
        }
    }
    if(!in_path) {
        fprintf(stderr, "usage: pbp [-o EBOOT.PBP] [-t \"Title\"] executable.elf\n");
        return 1;
    }

    /* Read the executable (DATA.PSP). */
    FILE *f = fopen(in_path, "rb");
    if(!f) die("cannot open input");
    if(fseek(f, 0, SEEK_END) != 0)
        die("cannot seek input");
    long elf_len = ftell(f);
    if(elf_len < 0)
        die("cannot determine input size");
    if(elf_len == 0)
        die("empty input");
    if((uint64_t)elf_len > UINT32_MAX)
        die("input exceeds the PBP 32-bit size limit");

    uint32_t sfo_len = 0;
    uint8_t *sfo = build_sfo(title, &sfo_len);
    uint32_t data_psp_offset = checked_add_u32(0x28, sfo_len,
                                               "PBP PARAM.SFO end");
    if((uint64_t)elf_len > UINT32_MAX - data_psp_offset)
        die("input and PARAM.SFO exceed the PBP 32-bit size limit");

    if(fseek(f, 0, SEEK_SET) != 0)
        die("cannot rewind input");
    uint8_t *elf = malloc((size_t)elf_len);
    if(!elf) die("out of memory");
    if(fread(elf, 1, (size_t)elf_len, f) != (size_t)elf_len) die("short read");
    if(fclose(f) != 0)
        die("cannot close input");
    uint32_t elf_size = (uint32_t)elf_len;

    /* PBP header: magic + version + 8 section offsets. */
    uint32_t hdr = 0x28;
    uint32_t off[8];
    off[0] = hdr;                 /* PARAM.SFO */
    off[1] = data_psp_offset;     /* ICON0.PNG (empty) */
    off[2] = off[1];              /* ICON1.PMF */
    off[3] = off[2];              /* PIC0.PNG  */
    off[4] = off[3];              /* PIC1.PNG  */
    off[5] = off[4];              /* SND0.AT3  */
    off[6] = off[5];              /* DATA.PSP  <- executable */
    off[7] = checked_add_u32(off[6], elf_size, "PBP DATA.PSP end"); /* DATA.PSAR (empty) */
    uint32_t total = off[7];

    FILE *o = fopen(out_path, "wb");
    if(!o) die("cannot open output");
    uint8_t h[0x28];
    h[0] = 0x00; h[1] = 'P'; h[2] = 'B'; h[3] = 'P';
    wr32(h + 4, 0x00010000);
    for(int i = 0; i < 8; i++)
        wr32(h + 8 + i * 4, off[i]);
    write_all(o, h, sizeof(h), "PBP header");
    write_all(o, sfo, sfo_len, "PARAM.SFO");
    write_all(o, elf, elf_size, "DATA.PSP");
    if(fclose(o) != 0)
        die("cannot close output");

    printf("pbp: wrote %s (%u bytes): PARAM.SFO=%u DATA.PSP=%u\n",
           out_path, total, sfo_len, elf_size);
    free(sfo); free(elf);
    return 0;
}
