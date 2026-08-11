/* host/tools/pbp/pbp.c
 *
 * Clean-room EBOOT.PBP packer for the PSP.
 *
 * Implemented from the public PBP + PARAM.SFO container documentation only
 * (no pspsdk/pack-pbp/mksfo source).  Wraps a PSP executable as DATA.PSP inside
 * a PBP, alongside a minimal generated PARAM.SFO (the other six sub-files are
 * left empty).  The PBP header is a magic + version + eight u32 section offsets.
 *
 *   pbp -o EBOOT.PBP -t "psp-load-usb" psp-load-usb.elf
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void die(const char *m) { fprintf(stderr, "pbp: %s\n", m); exit(1); }
static uint32_t align4(uint32_t v) { return (v + 3) & ~3u; }

/* ---- minimal PARAM.SFO builder ---- */

#define SFO_FMT_UTF8  0x0204   /* NUL-terminated UTF-8 string */
#define SFO_FMT_INT   0x0404   /* uint32 */

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
    uint32_t title_max = align4((uint32_t)strlen(title) + 1);
    struct { const char *key; uint16_t fmt; const char *sval; uint32_t ival; uint32_t maxlen; } ent[] = {
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
    int n = (int)(sizeof(ent) / sizeof(ent[0]));

    uint32_t key_table_start = 20 + (uint32_t)n * 16;

    /* Key table. */
    uint8_t keytab[256];
    if(title_max > 256) die("title too long");
    uint32_t koff[16], kpos = 0;
    for(int i = 0; i < n; i++) {
        koff[i] = kpos;
        size_t l = strlen(ent[i].key) + 1;
        memcpy(keytab + kpos, ent[i].key, l);
        kpos += (uint32_t)l;
    }
    uint32_t key_table_len = align4(kpos);
    for(uint32_t i = kpos; i < key_table_len; i++)
        keytab[i] = 0;

    uint32_t data_table_start = key_table_start + key_table_len;

    /* Data table. */
    uint8_t datatab[512];
    uint32_t doff[16], dpos = 0, dlen[16];
    for(int i = 0; i < n; i++) {
        doff[i] = dpos;
        if(ent[i].fmt == SFO_FMT_INT) {
            dlen[i] = 4;
            wr32(datatab + dpos, ent[i].ival);
        } else {
            size_t l = strlen(ent[i].sval) + 1;
            dlen[i] = (uint32_t)l;
            memcpy(datatab + dpos, ent[i].sval, l);
        }
        if(dpos + ent[i].maxlen > sizeof(datatab)) die("SFO data table overflow");
        memset(datatab + dpos + dlen[i], 0, ent[i].maxlen - dlen[i]);
        dpos += ent[i].maxlen;
    }
    uint32_t data_table_len = align4(dpos);

    uint32_t total = data_table_start + data_table_len;
    uint8_t *sfo = calloc(1, total);
    if(!sfo) die("out of memory");

    /* Header. */
    sfo[0] = 0x00; sfo[1] = 'P'; sfo[2] = 'S'; sfo[3] = 'F';
    wr32(sfo + 4, 0x00000101);
    wr32(sfo + 8, key_table_start);
    wr32(sfo + 12, data_table_start);
    wr32(sfo + 16, (uint32_t)n);

    /* Index table. */
    for(int i = 0; i < n; i++) {
        uint8_t *e = sfo + 20 + (uint32_t)i * 16;
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
    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "-o") && i + 1 < argc)      out_path = argv[++i];
        else if(!strcmp(argv[i], "-t") && i + 1 < argc) title = argv[++i];
        else if(argv[i][0] == '-')                      die("unknown option");
        else                                            in_path = argv[i];
    }
    if(!in_path) {
        fprintf(stderr, "usage: pbp [-o EBOOT.PBP] [-t \"Title\"] executable.elf\n");
        return 1;
    }

    /* Read the executable (DATA.PSP). */
    FILE *f = fopen(in_path, "rb");
    if(!f) die("cannot open input");
    fseek(f, 0, SEEK_END);
    long elf_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(elf_len <= 0) die("empty input");
    uint8_t *elf = malloc((size_t)elf_len);
    if(!elf) die("out of memory");
    if(fread(elf, 1, (size_t)elf_len, f) != (size_t)elf_len) die("short read");
    fclose(f);

    uint32_t sfo_len = 0;
    uint8_t *sfo = build_sfo(title, &sfo_len);

    /* PBP header: magic + version + 8 section offsets. */
    uint32_t hdr = 0x28;
    uint32_t off[8];
    off[0] = hdr;                 /* PARAM.SFO */
    off[1] = off[0] + sfo_len;    /* ICON0.PNG (empty) */
    off[2] = off[1];              /* ICON1.PMF */
    off[3] = off[2];              /* PIC0.PNG  */
    off[4] = off[3];              /* PIC1.PNG  */
    off[5] = off[4];              /* SND0.AT3  */
    off[6] = off[5];              /* DATA.PSP  <- executable */
    off[7] = off[6] + (uint32_t)elf_len; /* DATA.PSAR (empty) */
    uint32_t total = off[7];

    FILE *o = fopen(out_path, "wb");
    if(!o) die("cannot open output");
    uint8_t h[0x28];
    h[0] = 0x00; h[1] = 'P'; h[2] = 'B'; h[3] = 'P';
    wr32(h + 4, 0x00010000);
    for(int i = 0; i < 8; i++)
        wr32(h + 8 + i * 4, off[i]);
    if(fwrite(h, 1, sizeof(h), o) != sizeof(h)) die("short write");
    if(fwrite(sfo, 1, sfo_len, o) != sfo_len) die("short write");
    if(fwrite(elf, 1, (size_t)elf_len, o) != (size_t)elf_len) die("short write");
    fclose(o);

    printf("pbp: wrote %s (%u bytes): PARAM.SFO=%u DATA.PSP=%ld\n",
           out_path, total, sfo_len, elf_len);
    free(sfo); free(elf);
    return 0;
}
