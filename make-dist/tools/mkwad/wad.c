/* make-cd/tools/mkwad/wad.c */
/*
 * Wii installable-channel WAD packer.
 *
 * Produces a fakesigned "Is" WAD: header + cert chain + ticket + TMD +
 * encrypted contents (+ optional footer), each section 0x40-aligned. Content 0
 * is a generated banner (IMET + U8); content 1 is the boot DOL (boot_index=1).
 *
 * Crypto: title key is AES-128-CBC-encrypted under the Wii common key (IV =
 * title id || 0); each content is AES-128-CBC-encrypted under the title key
 * (IV = u16be(content index) || 0). Ticket + TMD are "fakesigned": RSA sig
 * zeroed, then a reserved field brute-forced until SHA-1 of the signed region
 * starts with 0x00 (accepted by a trucha-patched IOS / cIOS).
 *
 * Format reverse-engineered from a retail WAD; see AGENT/wii-wad-re.md.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "aes.h"
#include "sha1.h"
#include "banner.h"

/* Public retail cert chain (CA+CP+XS), from wii_certs.c. */
extern const uint8_t  wii_cert_chain[];
extern const unsigned wii_cert_chain_size;

/* Wii common key (index 0). A publicly-known constant used by every WAD tool,
 * required to wrap the title key; not source code, not from any SDK. */
static uint8_t WII_COMMON_KEY[16];

/* Fixed title key for our fakesigned channels (no RNG needed; the key is only
 * meaningful once wrapped under the common key, and homebrew WADs routinely use
 * a constant — the HBC WAD literally used ASCII "TheMostAwesomest"). */
static const uint8_t KOS_TITLE_KEY[16] = {
    'k','o','s','l','o','a','d','-','w','i','i','-','c','h','a','n'
};

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((size_t)(a) - 1))

#define TICKET_SIZE 0x2a4
#define TMD_HEADER  0x1e4   /* TMD bytes before the content records */
#define CR_SIZE     0x24    /* one TMD content record */

/* ---- little helpers ------------------------------------------------------ */

static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static void put_be64(uint8_t *p, uint64_t v) {
    for(int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (56 - 8 * i));
}

static uint8_t *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if(!f) {
        fprintf(stderr, "mkwad: cannot open %s\n", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);

    if(n < 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)n ? (size_t)n : 1);
    if(buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }

    fclose(f);
    if(!buf) {
        fprintf(stderr, "mkwad: read failed %s\n", path);
        return NULL;
    }
    *len_out = (size_t)n;
    return buf;
}

static int hex_nibble(int c) {
    if(c >= '0' && c <= '9')
        return c - '0';
    if(c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if(c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Load the Wii common key from ./common-key.bin in the current directory.
 * Auto-detects two shapes so users don't have to convert formats:
 *
 *   - Raw binary: exactly 16 bytes -> used as-is.
 *   - Text: any sequence of hex digits totalling 32 nibbles.  Whitespace,
 *     commas, semicolons, and "0x"/"0X" byte prefixes are ignored, so all
 *     of these load cleanly (bytes shown are placeholder, not a real key):
 *
 *       00112233445566778899aabbccddeeff
 *       00 11 22 33 44 55 66 77 88 99 aa bb cc dd ee ff
 *       0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
 *
 *   Any other non-hex character is rejected so a typo'd file fails loudly
 *   rather than silently producing a wrong key. */
static int load_common_key(void) {
    const char *path = "common-key.bin";
    FILE       *f = fopen(path, "rb");
    if(!f) {
        fprintf(stderr,
                "mkwad: cannot open %s in current directory.\n"
                "  Provide the Wii common key as either:\n"
                "    - a 16-byte raw binary file, or\n"
                "    - a text file containing 32 hex digits (whitespace, commas,\n"
                "      and '0x' byte prefixes are ignored).\n",
                path);
        return -1;
    }
    uint8_t buf[4096];
    size_t  n = fread(buf, 1, sizeof buf, f);
    int     hit_eof = feof(f);
    fclose(f);

    /* Raw binary: exactly 16 bytes and the whole file fit. */
    if(n == 16 && hit_eof) {
        memcpy(WII_COMMON_KEY, buf, 16);
        return 0;
    }

    /* Text parse: hex digits with permissive delimiters + "0x" prefixes. */
    int nibbles = 0;
    uint8_t k[16] = {0};
    for(size_t i = 0; i < n; i++) {
        int c = buf[i];
        if(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == ';')
            continue;
        if(c == '0' && i + 1 < n && (buf[i + 1] == 'x' || buf[i + 1] == 'X')) {
            i++; /* loop ++ steps past '0' too */
            continue;
        }
        int v = hex_nibble(c);
        if(v < 0) {
            fprintf(stderr, "mkwad: %s: unexpected character '%c' (0x%02x) at offset %zu\n", path,
                    isprint(c) ? c : '?', (unsigned)c & 0xff, i);
            return -1;
        }
        if(nibbles >= 32) {
            fprintf(stderr, "mkwad: %s: too many hex digits (>32)\n", path);
            return -1;
        }
        if((nibbles & 1) == 0)
            k[nibbles / 2] = (uint8_t)(v << 4);
        else
            k[nibbles / 2] |= (uint8_t)v;
        nibbles++;
    }
    if(nibbles != 32) {
        fprintf(stderr,
                "mkwad: %s: expected 32 hex digits or a 16-byte raw file, got "
                "%d nibbles\n",
                path, nibbles);
        return -1;
    }
    memcpy(WII_COMMON_KEY, k, 16);
    return 0;
}

/* ---- fakesign ------------------------------------------------------------ */
/*
 * Zero the RSA signature, then vary a 4-byte reserved field inside the signed
 * region until SHA-1 of that region begins with 0x00. signed_off is where the
 * signed data starts (the issuer, 0x140); scratch_off is a reserved offset
 * within it that ES ignores functionally.
 */
static void fakesign(uint8_t *blob, size_t blob_len, size_t scratch_off) {
    memset(blob + 4, 0, 0x100);      /* RSA-2048 sig (256 bytes) -> 0 */
    const size_t signed_off = 0x140; /* issuer onward is signed       */
    uint8_t digest[20];
    for(uint32_t n = 0;; n++) {
        put_be32(blob + scratch_off, n);
        sha1(blob + signed_off, blob_len - signed_off, digest);
        if(digest[0] == 0x00)
            return;
    }
}

/* ---- ticket -------------------------------------------------------------- */

static void build_ticket(uint8_t tik[TICKET_SIZE], uint64_t title_id, const uint8_t enc_title_key[16]) {
    memset(tik, 0, TICKET_SIZE);
    put_be32(tik + 0x000, 0x00010001); /* sig type RSA-2048      */
    memcpy(tik + 0x140, "Root-CA00000001-XS00000003", 26);
    memcpy(tik + 0x1bf, enc_title_key, 16);       /* wrapped title key      */
    put_be64(tik + 0x1d0, 0x0000000000000001ull); /* ticket id (arbitrary)  */
    put_be64(tik + 0x1dc, title_id);              /* title id               */
    put_be16(tik + 0x1e4, 0xffff);                /* observed standard const*/
    tik[0x1f1] = 0x00;                            /* common key index = 0   */
    /* 0x1f2..0x221 left zero to match retail tickets (the HBC "0185" here is
     * non-standard). The fakesign scratch lives in this reserved region. */
    memset(tik + 0x222, 0xff, 32);     /* content access map     */
    fakesign(tik, TICKET_SIZE, 0x1f4); /* 0x1f4: reserved scratch*/
}

/* ---- TMD ----------------------------------------------------------------- */

typedef struct {
    uint32_t cid;
    uint16_t index;
    uint16_t type;
    uint64_t size;
    uint8_t  sha1[20];
} content_rec_t;

static uint8_t *build_tmd(uint64_t title_id, uint32_t ios, uint16_t region, uint16_t title_version,
                          uint16_t boot_index, const content_rec_t *recs, int n, size_t *out_len) {
    size_t   len = TMD_HEADER + (size_t)n * CR_SIZE;
    uint8_t *m = calloc(1, len);
    if(!m)
        return NULL;

    put_be32(m + 0x000, 0x00010001); /* sig type RSA-2048      */
    memcpy(m + 0x140, "Root-CA00000001-CP00000004", 26);
    put_be64(m + 0x184, 0x0000000100000000ull | ios); /* system version = IOS */
    put_be64(m + 0x18c, title_id);
    put_be32(m + 0x194, 0);          /* title type 0 (matches HBC's working channel; was 1) */
    memcpy(m + 0x198, "HB", 2);      /* group id "HB"          */
    put_be16(m + 0x19c, region);     /* region (3 = free)      */
    put_be32(m + 0x1d8, 0x00000003); /* access rights          */
    put_be16(m + 0x1dc, title_version);
    put_be16(m + 0x1de, (uint16_t)n); /* number of contents     */
    put_be16(m + 0x1e0, boot_index);

    for(int i = 0; i < n; i++) {
        uint8_t *r = m + TMD_HEADER + (size_t)i * CR_SIZE;
        put_be32(r + 0x00, recs[i].cid);
        put_be16(r + 0x04, recs[i].index);
        put_be16(r + 0x06, recs[i].type);
        put_be64(r + 0x08, recs[i].size);
        memcpy(r + 0x10, recs[i].sha1, 20);
    }
    fakesign(m, len, 0x1c6); /* 0x1c6: reserved scratch*/
    *out_len = len;
    return m;
}

/* ---- content encryption -------------------------------------------------- */
/* Returns malloc'd ciphertext of length align(plain_len,16); fills the SHA-1
 * (over the *plaintext*) into rec. */
static uint8_t *encrypt_content(const uint8_t *plain, size_t plain_len, uint16_t index, content_rec_t *rec,
                                size_t *enc_len_out) {
    size_t   enc_len = ALIGN_UP(plain_len, 16);
    uint8_t *buf = calloc(1, enc_len ? enc_len : 16);
    if(!buf)
        return NULL;
    memcpy(buf, plain, plain_len);
    sha1(buf, plain_len, rec->sha1);
    uint8_t iv[16];
    memset(iv, 0, 16);
    put_be16(iv, index);
    aes128_cbc_encrypt(KOS_TITLE_KEY, iv, buf, enc_len);
    rec->index = index;
    rec->type = 1;
    rec->size = plain_len;
    *enc_len_out = enc_len;
    return buf;
}

/* ---- WAD assembly -------------------------------------------------------- */

static int write_wad(const char *out_path, const uint8_t *tik, const uint8_t *tmd, size_t tmd_len,
                     uint8_t **enc, size_t *enc_len, int n) {
    size_t cert_sz = wii_cert_chain_size;

    /* content data section: each content at a 0x40-aligned offset */
    size_t data_sz = 0;
    for(int i = 0; i < n; i++)
        data_sz = ALIGN_UP(data_sz, 0x40) + enc_len[i];
    data_sz = ALIGN_UP(data_sz, 0x40);

    size_t off_cert = ALIGN_UP(0x20, 0x40); /* 0x40  */
    size_t off_tik = ALIGN_UP(off_cert + cert_sz, 0x40);
    size_t off_tmd = ALIGN_UP(off_tik + TICKET_SIZE, 0x40);
    size_t off_data = ALIGN_UP(off_tmd + tmd_len, 0x40);
    size_t total = ALIGN_UP(off_data + data_sz, 0x40);

    uint8_t *w = calloc(1, total);
    if(!w)
        return -1;

    put_be32(w + 0x00, 0x20); /* header size       */
    w[0x04] = 'I';
    w[0x05] = 's';                         /* type "Is"         */
    put_be16(w + 0x06, 0);                 /* version           */
    put_be32(w + 0x08, (uint32_t)cert_sz); /* cert chain size   */
    put_be32(w + 0x0c, 0);                 /* reserved/CRL      */
    put_be32(w + 0x10, TICKET_SIZE);       /* ticket size       */
    put_be32(w + 0x14, (uint32_t)tmd_len); /* TMD size          */
    put_be32(w + 0x18, (uint32_t)data_sz); /* content data size */
    put_be32(w + 0x1c, 0);                 /* footer size       */

    memcpy(w + off_cert, wii_cert_chain, cert_sz);
    memcpy(w + off_tik, tik, TICKET_SIZE);
    memcpy(w + off_tmd, tmd, tmd_len);

    size_t cur = off_data;
    for(int i = 0; i < n; i++) {
        cur = ALIGN_UP(cur, 0x40);
        memcpy(w + cur, enc[i], enc_len[i]);
        cur += enc_len[i];
    }

    FILE *f = fopen(out_path, "wb");
    if(!f) {
        fprintf(stderr, "mkwad: cannot write %s\n", out_path);
        free(w);
        return -1;
    }
    size_t wr = fwrite(w, 1, total, f);
    fclose(f);
    free(w);
    if(wr != total) {
        fprintf(stderr, "mkwad: short write\n");
        return -1;
    }
    fprintf(stderr, "mkwad: wrote %s (%zu bytes)\n", out_path, total);
    return 0;
}

/* ---- CLI ----------------------------------------------------------------- */

static void wad_usage(void) {
    printf("\nUsage: mkwad --dol <boot.dol> -o <out.wad> [options]\n\n");
    printf("  --dol <file>      Boot DOL (becomes content index 1)\n");
    printf("  -o, --out <file>  Output .wad path\n");
    printf("  --title-id <4ch>  4-char low title id (default KOSL); high = "
           "00010001\n");
    printf("  --ios <N>         Required IOS / system version (default 58)\n");
    printf("  --title <name>    Channel name shown in the System Menu (default "
           "wii-load-ip)\n");
    printf("  --region <0-3>    0=JP 1=US 2=EU 3=free (default 3)\n");
    printf("  --banner <file>   Use a prebuilt banner content instead of the "
           "generated one\n");
    printf("  --sound <file>    Add a prebuilt meta/sound.bin (IMD5+BNS) to "
           "the generated banner\n\n");
}

int main(int argc, char **argv) {
    const char *dol_path = NULL, *out_path = NULL, *banner_path = NULL, *sound_path = NULL;
    const char *title = "wii-load-ip";
    char tid4[5] = "KOSL";
    uint32_t ios = 58;
    uint16_t region = 3, title_version = 1; /* >=1: a real channel is never v0 */

    for(int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if(!strcmp(a, "--dol") && i + 1 < argc)
            dol_path = argv[++i];
        else if((!strcmp(a, "-o") || !strcmp(a, "--out")) && i + 1 < argc)
            out_path = argv[++i];
        else if(!strcmp(a, "--title-id") && i + 1 < argc) {
            strncpy(tid4, argv[++i], 4);
            tid4[4] = 0;
        } else if(!strcmp(a, "--ios") && i + 1 < argc)
            ios = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if(!strcmp(a, "--title") && i + 1 < argc)
            title = argv[++i];
        else if(!strcmp(a, "--region") && i + 1 < argc)
            region = (uint16_t)strtoul(argv[++i], NULL, 0);
        else if(!strcmp(a, "--title-ver") && i + 1 < argc)
            title_version = (uint16_t)strtoul(argv[++i], NULL, 0);
        else if(!strcmp(a, "--banner") && i + 1 < argc)
            banner_path = argv[++i];
        else if(!strcmp(a, "--sound") && i + 1 < argc)
            sound_path = argv[++i];
        else if(!strcmp(a, "-h") || !strcmp(a, "--help")) {
            wad_usage();
            return 0;
        } else {
            fprintf(stderr, "mkwad: unknown option %s\n", a);
            wad_usage();
            return 1;
        }
    }
    if(!dol_path || !out_path) {
        wad_usage();
        return 1;
    }

    /* Common key is required to wrap the title key; load from cwd before any
     * AES op uses it.  load_common_key() prints a clear error on failure. */
    if(load_common_key() < 0)
        return 1;

    uint64_t title_id = 0x0001000100000000ull | ((uint64_t)(uint8_t)tid4[0] << 24) |
                        ((uint64_t)(uint8_t)tid4[1] << 16) | ((uint64_t)(uint8_t)tid4[2] << 8) |
                        (uint64_t)(uint8_t)tid4[3];

    /* contents */
    size_t banner_len = 0, dol_len = 0, sound_len = 0;
    uint8_t *sound = sound_path ? read_file(sound_path, &sound_len) : NULL;
    uint8_t *banner = banner_path ? read_file(banner_path, &banner_len)
                                  : wii_build_banner(title, sound, sound_len, &banner_len);
    free(sound);
    uint8_t *dol = read_file(dol_path, &dol_len);
    if(!banner || !dol) {
        free(banner);
        free(dol);
        return 1;
    }

    content_rec_t recs[2];
    size_t enc_len[2];
    uint8_t *enc[2];
    recs[0].cid = 0;
    enc[0] = encrypt_content(banner, banner_len, 0, &recs[0], &enc_len[0]);
    recs[1].cid = 1;
    enc[1] = encrypt_content(dol, dol_len, 1, &recs[1], &enc_len[1]);
    free(banner);
    free(dol);
    if(!enc[0] || !enc[1]) {
        free(enc[0]);
        free(enc[1]);
        return 1;
    }

    /* wrap title key under the common key (IV = title id || 0) */
    uint8_t enc_tk[16];
    memcpy(enc_tk, KOS_TITLE_KEY, 16);
    uint8_t iv_tk[16];
    memset(iv_tk, 0, 16);
    put_be64(iv_tk, title_id);
    aes128_cbc_encrypt(WII_COMMON_KEY, iv_tk, enc_tk, 16);

    uint8_t tik[TICKET_SIZE];
    build_ticket(tik, title_id, enc_tk);

    size_t   tmd_len = 0;
    uint8_t *tmd = build_tmd(title_id, ios, region, title_version,
                             /*boot_index*/ 1, recs, 2, &tmd_len);
    if(!tmd) {
        free(enc[0]);
        free(enc[1]);
        return 1;
    }

    int rc = write_wad(out_path, tik, tmd, tmd_len, enc, enc_len, 2);

    free(tmd);
    free(enc[0]);
    free(enc[1]);
    if(rc == 0)
        fprintf(stderr, "mkwad: title %016llx  IOS%u  \"%s\"\n", (unsigned long long)title_id, ios, title);
    return rc == 0 ? 0 : 1;
}
