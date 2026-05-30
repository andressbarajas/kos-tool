/* make-cd/tools/mkwad/banner.c */
/*
 * Wii channel banner generator (content index 0).
 *
 * content0 = 0x40 header + IMET header (0x600) + outer U8 archive:
 *   meta/banner.bin, meta/icon.bin
 * Each *.bin = IMD5(0x20) + LZ77 + inner U8 archive:
 *   arc/blyt/<name>.brlyt   (RLYT v8: RootPane -> {background pane, logo pane})
 *   arc/anim/<name>.brlan   (RLAN v8: one RLVC pane-alpha track, held opaque)
 *   arc/timg/bg.tpl         (RGB5A3 gradient background)
 *   arc/timg/logo.tpl       (RGB5A3 logo plate with a drawn mark)
 *
 * This mirrors the two-pane structure of a retail icon/banner (RE'd from a
 * retail channel binary; see AGENT/wii-wad-re.md) with our own art: a
 * background picture pane that overscans the canvas plus a smaller centred logo
 * pane, two draw-texture materials, and a non-degenerate RLVC animation so the
 * System Menu has a real track to play.  The LZ77 stream is emitted as
 * all-literals (valid LZ10, no real compression needed).  All multibyte fields
 * are big-endian.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "md5.h"
#include "banner.h"

/* ---- growable buffer ----------------------------------------------------- */

typedef struct {
    uint8_t *p;
    size_t len, cap;
} buf_t;

static void bput(buf_t *b, const void *d, size_t n) {
    if(b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 64;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, d, n);
    b->len += n;
}
static void bu8(buf_t *b, uint8_t v) {
    bput(b, &v, 1);
}
static void bu16(buf_t *b, uint16_t v) {
    uint8_t t[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    bput(b, t, 2);
}
static void bu32(buf_t *b, uint32_t v) {
    uint8_t t[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    bput(b, t, 4);
}
static void bf32(buf_t *b, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    bu32(b, u);
}
static void bzeros(buf_t *b, size_t n) {
    while(n--)
        bu8(b, 0);
}
static void balign(buf_t *b, size_t a) {
    while(b->len % a)
        bu8(b, 0);
}
static void put32at(buf_t *b, size_t off, uint32_t v) {
    b->p[off] = (uint8_t)(v >> 24);
    b->p[off + 1] = (uint8_t)(v >> 16);
    b->p[off + 2] = (uint8_t)(v >> 8);
    b->p[off + 3] = (uint8_t)v;
}

/* section helpers: write magic + placeholder size, patch at close */
static size_t sec_begin(buf_t *b, const char *magic) {
    bput(b, magic, 4);
    size_t szf = b->len;
    bu32(b, 0);
    return szf;
}
static void sec_end(buf_t *b, size_t szf) {
    put32at(b, szf, (uint32_t)(b->len - (szf - 4)));
}

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((size_t)(a) - 1))

/* ---- GX "draw texture" material (RE'd from a retail banner) ---------------
 */
/* Reproduced from a retail icon material: a clean draw-texture material,
 * flags @0x3c = 0x00000111 (1 texMap/texSRT/texCoordGen), texMap @0x40, NO
 * material-color block. (Our earlier HBC "white" template carried flag
 * 0x00800111 + a matColor of alpha 0, which rendered the pane transparent.) */
static const uint8_t MAT_TEMPLATE[0x5c] = {
    0x50, 0x69, 0x63, 0x74, 0x75, 0x72, 0x65, 0x5f, 0x30, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff,
    0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x01, 0x11,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x3f, 0x80, 0x00, 0x00, 0x3f, 0x80, 0x00, 0x00, 0x01, 0x04, 0x1e, 0x00,
};

/* ---- RGBA8 textures (background + logo) -----------------------------------
 */

static void fill_rect(uint8_t *rgba, int w, int h, int x0, int y0, int x1, int y1, uint8_t R, uint8_t G,
                      uint8_t B, uint8_t A) {
    if(x0 < 0)
        x0 = 0;
    if(y0 < 0)
        y0 = 0;
    if(x1 > w)
        x1 = w;
    if(y1 > h)
        y1 = h;
    for(int y = y0; y < y1; y++)
        for(int x = x0; x < x1; x++) {
            uint8_t *p = rgba + ((size_t)y * w + x) * 4;
            p[0] = R;
            p[1] = G;
            p[2] = B;
            p[3] = A;
        }
}

/* Background: deep-blue -> cyan vertical gradient, fully opaque. */
static uint8_t *make_bg(int w, int h) {
    uint8_t *rgba = malloc((size_t)w * h * 4);
    for(int y = 0; y < h; y++) {
        float t = (float)y / (float)(h > 1 ? h - 1 : 1);
        uint8_t R = (uint8_t)(0x10 * (1 - t) + 0x06 * t);
        uint8_t G = (uint8_t)(0x2a * (1 - t) + 0x66 * t);
        uint8_t B = (uint8_t)(0x5c * (1 - t) + 0x9c * t);
        for(int x = 0; x < w; x++) {
            uint8_t *p = rgba + ((size_t)y * w + x) * 4;
            p[0] = R;
            p[1] = G;
            p[2] = B;
            p[3] = 0xff;
        }
    }
    return rgba;
}

/* Logo tile: opaque orange plate, white border, bold white "K" mark. Opaque
 * everywhere so it renders regardless of the SM enabling alpha blending. */
static uint8_t *make_logo(int w, int h) {
    uint8_t *rgba = malloc((size_t)w * h * 4);
    fill_rect(rgba, w, h, 0, 0, w, h, 0xff, 0x88, 0x10, 0xff); /* orange plate */
    int bd = (w < h ? w : h) / 12;
    if(bd < 2)
        bd = 2; /* white border  */
    fill_rect(rgba, w, h, 0, 0, w, bd, 0xff, 0xff, 0xff, 0xff);
    fill_rect(rgba, w, h, 0, h - bd, w, h, 0xff, 0xff, 0xff, 0xff);
    fill_rect(rgba, w, h, 0, 0, bd, h, 0xff, 0xff, 0xff, 0xff);
    fill_rect(rgba, w, h, w - bd, 0, w, h, 0xff, 0xff, 0xff, 0xff);

    /* Bold white "K" centred in the plate. */
    int gx0 = w / 4, gx1 = w - w / 4;
    int gy0 = h / 5, gy1 = h - h / 5;
    int t = (gx1 - gx0) / 5;
    if(t < 2)
        t = 2; /* stroke width  */
    int midY = (gy0 + gy1) / 2;
    fill_rect(rgba, w, h, gx0, gy0, gx0 + t, gy1, 0xff, 0xff, 0xff, 0xff); /* stem */
    for(int y = gy0; y < gy1; y++) {                                       /* two arms      */
        float u = (y <= midY) ? (float)(midY - y) / (float)(midY - gy0 + 1)
                              : (float)(y - midY) / (float)(gy1 - midY + 1);
        int cx = (int)((gx0 + t) + ((gx1 - (gx0 + t)) * u));
        fill_rect(rgba, w, h, cx - t / 2, y, cx + t - t / 2, y + 1, 0xff, 0xff, 0xff, 0xff);
    }
    return rgba;
}

/* RGB5A3 pixel: opaque -> RGB555 (MSB set), else ARGB3444.  Our art is opaque
 * so the RGB555 path is taken; either way it is half the size of RGBA8. */
static uint16_t to_rgb5a3(uint8_t R, uint8_t G, uint8_t B, uint8_t A) {
    if(A >= 0xe0)
        return (uint16_t)(0x8000 | ((R >> 3) << 10) | ((G >> 3) << 5) | (B >> 3));
    return (uint16_t)(((A >> 5) << 12) | ((R >> 4) << 8) | ((G >> 4) << 4) | (B >> 4));
}

/* GX_TF_RGB5A3 (format 5): 4x4 tiles of 32 bytes (16 big-endian u16 pixels).
 * Retail banners use RGB5A3/RGB565 (never RGBA8): half the size of RGBA8, which
 * matters because the System Menu caps the *decompressed* icon at ~0x10000
 * bytes.  An RGBA8 icon overflows that buffer, the icon load fails, and the SM
 * silently drops the channel from the grid. */
static uint8_t *tpl_rgb5a3(const uint8_t *rgba, int w, int h, size_t *out_len) {
    int tw = (w + 3) / 4, th = (h + 3) / 4;
    size_t img = (size_t)tw * th * 32;
    size_t data_off = 0x40;
    size_t total = data_off + img;
    uint8_t *t = calloc(1, total);
    buf_t b = {t, 0, total};
    bu32(&b, 0x0020AF30);
    bu32(&b, 1);
    bu32(&b, 0x0c); /* magic, n images, table */
    bu32(&b, 0x14);
    bu32(&b, 0); /* image hdr off, palette off */
    bu16(&b, (uint16_t)h);
    bu16(&b, (uint16_t)w);
    bu32(&b, 5);                  /* height,width,format=RGB5A3 */
    bu32(&b, (uint32_t)data_off); /* data offset */
    bu32(&b, 0);
    bu32(&b, 0); /* wrap s,t = clamp */
    bu32(&b, 1);
    bu32(&b, 1); /* min,mag filter = linear */
    bu32(&b, 0); /* lod bias 0.0 */
    bu32(&b, 0); /* edge/min/max lod, unpacked */

    uint8_t *dst = t + data_off;
    for(int ty = 0; ty < th; ty++)
        for(int tx = 0; tx < tw; tx++) {
            uint8_t *tile = dst + ((size_t)(ty * tw + tx)) * 32;
            for(int i = 0; i < 16; i++) {
                int x = tx * 4 + (i & 3), y = ty * 4 + (i >> 2);
                uint8_t R = 0, G = 0, B = 0, A = 0;
                if(x < w && y < h) {
                    const uint8_t *p = rgba + ((size_t)y * w + x) * 4;
                    R = p[0];
                    G = p[1];
                    B = p[2];
                    A = p[3];
                }
                uint16_t px = to_rgb5a3(R, G, B, A);
                tile[i * 2] = (uint8_t)(px >> 8);
                tile[i * 2 + 1] = (uint8_t)px;
            }
        }
    *out_len = total;
    return t;
}

/* ---- LZ77 (LZ10) real compression ---------------------------------------- */
/* Every retail/HBC banner stores its .bin LZ77-compressed (not "store" mode).
 * A greedy compressor with a 3-byte hash chain: header "LZ77" + 0x10 + 24-bit
 * LE decompressed size, then flag-byte groups (MSB first; 1 = 2-byte
 * back-reference len 3..18 / disp 1..4096, 0 = literal). Matches the standard
 * the SM decodes. */
#define LZ_HBITS 15
#define LZ_HSIZE (1 << LZ_HBITS)
static unsigned lz_hash3(const uint8_t *p) {
    return (((unsigned)p[0] << 10) ^ ((unsigned)p[1] << 5) ^ (unsigned)p[2]) & (LZ_HSIZE - 1);
}
static uint8_t *lz77_store(const uint8_t *src, size_t n, size_t *out_len) {
    size_t cap = 8 + n + (n / 8) + 32;
    uint8_t *o = malloc(cap);
    buf_t b = {o, 0, cap};
    bput(&b, "LZ77", 4);
    bu8(&b, 0x10);
    bu8(&b, (uint8_t)n);
    bu8(&b, (uint8_t)(n >> 8));
    bu8(&b, (uint8_t)(n >> 16));

    int *head = malloc(LZ_HSIZE * sizeof(int));
    int *prev = malloc((n ? n : 1) * sizeof(int));
    for(int i = 0; i < LZ_HSIZE; i++)
        head[i] = -1;

    size_t pos = 0;
    while(pos < n) {
        size_t flag_pos = b.len;
        bu8(&b, 0); /* placeholder flag byte */
        uint8_t flag = 0;
        for(int bit = 0; bit < 8 && pos < n; bit++) {
            size_t best_len = 0, best_disp = 0;
            size_t max_len = n - pos;
            if(max_len > 18)
                max_len = 18;
            if(max_len >= 3) {
                size_t   win_start = pos > 4096 ? pos - 4096 : 0;
                unsigned h = lz_hash3(src + pos);
                int cand = head[h], depth = 64;
                while(cand >= 0 && (size_t)cand >= win_start && depth-- > 0) {
                    if(src[cand + best_len] == src[pos + best_len]) { /* cheap reject */
                        size_t l = 0;
                        while(l < max_len && src[cand + l] == src[pos + l])
                            l++;
                        if(l > best_len) {
                            best_len = l;
                            best_disp = pos - (size_t)cand;
                            if(l == max_len)
                                break;
                        }
                    }
                    cand = prev[cand];
                }
            }
            if(best_len >= 3) {
                flag |= (uint8_t)(0x80 >> bit);
                uint16_t tok = (uint16_t)(((best_len - 3) << 12) | (best_disp - 1));
                bu8(&b, (uint8_t)(tok >> 8));
                bu8(&b, (uint8_t)tok);
                for(size_t k = 0; k < best_len; k++)
                    if(pos + k + 2 < n) {
                        unsigned hh = lz_hash3(src + pos + k);
                        prev[pos + k] = head[hh];
                        head[hh] = (int)(pos + k);
                    }
                pos += best_len;
            } else {
                if(pos + 2 < n) {
                    unsigned hh = lz_hash3(src + pos);
                    prev[pos] = head[hh];
                    head[hh] = (int)pos;
                }
                bu8(&b, src[pos]);
                pos++;
            }
        }
        b.p[flag_pos] = flag;
    }
    free(head);
    free(prev);
    *out_len = b.len;
    return o;
}

/* ---- IMD5 wrapper -------------------------------------------------------- */
static uint8_t *imd5_wrap(const uint8_t *body, size_t n, size_t *out_len) {
    uint8_t *o = calloc(1, 0x20 + n);
    buf_t b = {o, 0, 0x20 + n};
    bput(&b, "IMD5", 4);
    bu32(&b, (uint32_t)n);
    bzeros(&b, 8);
    md5(body, n, o + 0x10);
    b.len = 0x20;
    bput(&b, body, n);
    *out_len = b.len;
    return o;
}

/* ---- sound.bin (silent) -------------------------------------------------- */
/* The System Menu banner loader extracts all three sections an IMET declares
 * (icon/banner/sound); every retail banner ships a meta/sound.bin, so we must
 * too or the loader can abort and the channel tile renders blank.  Retail
 * accepts a raw RIFF/WAVE here, which is far simpler to
 * author than a BNS, so we emit a tiny silent little-endian PCM WAVE. */
static uint8_t *make_silent_wave(size_t *out_len) {
    const uint32_t rate = 32000, frames = 1024;
    const uint16_t ch = 2, bits = 16;
    uint32_t block = (uint32_t)ch * (bits / 8);
    uint32_t datalen = frames * block;
    uint32_t total = 44 + datalen;
    uint8_t *w = calloc(1, total);
#define LE16(o, v)                                                                                           \
    do {                                                                                                     \
        w[o] = (uint8_t)(v);                                                                                 \
        w[(o) + 1] = (uint8_t)((v) >> 8);                                                                    \
    } while(0)
#define LE32(o, v)                                                                                           \
    do {                                                                                                     \
        w[o] = (uint8_t)(v);                                                                                 \
        w[(o) + 1] = (uint8_t)((v) >> 8);                                                                    \
        w[(o) + 2] = (uint8_t)((v) >> 16);                                                                   \
        w[(o) + 3] = (uint8_t)((v) >> 24);                                                                   \
    } while(0)
    memcpy(w + 0, "RIFF", 4);
    LE32(4, total - 8);
    memcpy(w + 8, "WAVE", 4);
    memcpy(w + 12, "fmt ", 4);
    LE32(16, 16);
    LE16(20, 1);
    LE16(22, ch);
    LE32(24, rate);
    LE32(28, rate * block);
    LE16(32, block);
    LE16(34, bits);
    memcpy(w + 36, "data", 4);
    LE32(40, datalen); /* data stays zero = silence */
#undef LE16
#undef LE32
    *out_len = total;
    return w;
}

/* LZ77-compress the silent WAVE and IMD5-wrap it, matching retail compressed
 * RIFF/WAVE sound.bin.  IMET soundSize = the decompressed WAVE length. */
static uint8_t *make_sound_bin(size_t *out_len, size_t *inner_len) {
    size_t wav_len;
    uint8_t *wav = make_silent_wave(&wav_len);
    size_t lz_len;
    uint8_t *lz = lz77_store(wav, wav_len, &lz_len);
    uint8_t *bin = imd5_wrap(lz, lz_len, out_len);
    free(wav);
    free(lz);
    *inner_len = wav_len;
    return bin;
}

/* ---- U8 archive ---------------------------------------------------------- */
typedef struct {
    int is_dir;
    const char *name;
    uint32_t a4, a8;
    const uint8_t *data;
} u8node;

static uint8_t *build_u8(u8node *nodes, int N, size_t *out_len) {
    uint32_t noff[64];
    size_t strsz = 0;
    for(int i = 0; i < N; i++) {
        noff[i] = (uint32_t)strsz;
        strsz += strlen(nodes[i].name) + 1;
    }
    size_t nodetbl = (size_t)N * 12;
    size_t hdrsz = nodetbl + strsz;
    size_t data_start = ALIGN_UP(0x20 + hdrsz, 0x20);
    size_t total = data_start;
    for(int i = 0; i < N; i++)
        if(!nodes[i].is_dir)
            total += ALIGN_UP(nodes[i].a8, 0x20);

    uint8_t *u = calloc(1, total);
    buf_t hb = {u, 0, total};
    bu32(&hb, 0x55AA382D);
    bu32(&hb, 0x20);
    bu32(&hb, (uint32_t)hdrsz);
    bu32(&hb, (uint32_t)data_start);

    uint8_t *node = u + 0x20, *strs = u + 0x20 + nodetbl;
    size_t cur = data_start;
    for(int i = 0; i < N; i++) {
        node[0] = nodes[i].is_dir ? 1 : 0;
        node[1] = (uint8_t)(noff[i] >> 16);
        node[2] = (uint8_t)(noff[i] >> 8);
        node[3] = (uint8_t)noff[i];
        uint32_t a4 = nodes[i].a4, a8 = nodes[i].a8;
        if(!nodes[i].is_dir) {
            a4 = (uint32_t)cur;
            if(nodes[i].data && a8)
                memcpy(u + cur, nodes[i].data, a8);
            cur += ALIGN_UP(a8, 0x20);
        }
        node[4] = (uint8_t)(a4 >> 24);
        node[5] = (uint8_t)(a4 >> 16);
        node[6] = (uint8_t)(a4 >> 8);
        node[7] = (uint8_t)a4;
        node[8] = (uint8_t)(a8 >> 24);
        node[9] = (uint8_t)(a8 >> 16);
        node[10] = (uint8_t)(a8 >> 8);
        node[11] = (uint8_t)a8;
        memcpy(strs + noff[i], nodes[i].name, strlen(nodes[i].name) + 1);
        node += 12;
    }
    *out_len = total;
    return u;
}

/* ---- brlyt / brlan ------------------------------------------------------- */

static void emit_pane(buf_t *b, uint8_t flags, uint8_t origin, uint8_t alpha, const char *name, float W,
                      float H) {
    bu8(b, flags);
    bu8(b, origin);
    bu8(b, alpha);
    bu8(b, 0);
    char nm[0x10];
    memset(nm, 0, 0x10);
    memcpy(nm, name, strnlen(name, 0x10));
    bput(b, nm, 0x10);
    bzeros(b, 8); /* user data */
    bf32(b, 0);
    bf32(b, 0);
    bf32(b, 0); /* translate xyz */
    bf32(b, 0);
    bf32(b, 0);
    bf32(b, 0); /* rotate xyz */
    bf32(b, 1.0f);
    bf32(b, 1.0f); /* scale xy */
    bf32(b, W);
    bf32(b, H); /* width, height */
}

/* one mat1 material: clone MAT_TEMPLATE, patch the 0x14-byte name + texMap idx
 */
static void emit_material(buf_t *b, const char *name, uint16_t tex_idx) {
    uint8_t mat[0x5c];
    memcpy(mat, MAT_TEMPLATE, sizeof mat);
    memset(mat, 0, 0x14);
    memcpy(mat, name, strnlen(name, 0x13));
    mat[0x40] = (uint8_t)(tex_idx >> 8); /* texMap: texture index */
    mat[0x41] = (uint8_t)tex_idx;
    bput(b, mat, sizeof mat);
}

/* one pic1 pane: full body (flags 0x01 visible, origin centre), white vertex
 * colours, material index, and a single full-texture coord set. */
static void emit_pic(buf_t *b, const char *name, float W, float H, uint16_t mat_idx) {
    size_t s = sec_begin(b, "pic1");
    emit_pane(b, 0x01, 0x04, 0xff, name, W, H);
    for(int i = 0; i < 4; i++) {
        bu8(b, 0xff);
        bu8(b, 0xff);
        bu8(b, 0xff);
        bu8(b, 0xff);
    }
    bu16(b, mat_idx);
    bu8(b, 1);
    bu8(b, 0);                                         /* num tex coords, pad */
    float uv[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}}; /* TL,TR,BL,BR */
    for(int i = 0; i < 4; i++) {
        bf32(b, uv[i][0]);
        bf32(b, uv[i][1]);
    }
    sec_end(b, s);
}

/* Two-pane layout (background + logo), mirroring a retail icon/banner brlyt:
 * 9 sections, two textures, two draw-texture materials, RootPane -> {bg, logo}.
 */
static uint8_t *build_brlyt(float W, float H, float bgW, float bgH, float logoW, float logoH,
                            size_t *out_len) {
    buf_t b = {0};
    bput(&b, "RLYT", 4);
    bu16(&b, 0xFEFF);
    bu16(&b, 0x0008);
    size_t fs = b.len;
    bu32(&b, 0);
    bu16(&b, 0x0010);
    bu16(&b, 9); /* filesize, hdr, 9 sections */

    size_t s = sec_begin(&b, "lyt1");
    bu8(&b, 1);
    bzeros(&b, 3);
    bf32(&b, W);
    bf32(&b, H);
    sec_end(&b, s);

    s = sec_begin(&b, "txl1");
    bu16(&b, 2);
    bu16(&b, 0);        /* count, pad */
    size_t tbl = b.len; /* name offsets relative to table start */
    size_t of0 = b.len;
    bu32(&b, 0);
    bu32(&b, 0);
    size_t of1 = b.len;
    bu32(&b, 0);
    bu32(&b, 0);
    size_t p0 = b.len;
    bput(&b, "bg.tpl", 7);
    size_t p1 = b.len;
    bput(&b, "logo.tpl", 9);
    put32at(&b, of0, (uint32_t)(p0 - tbl));
    put32at(&b, of1, (uint32_t)(p1 - tbl));
    balign(&b, 4);
    sec_end(&b, s);

    s = sec_begin(&b, "mat1");
    bu16(&b, 2);
    bu16(&b, 0);
    size_t sec_start = s - 4;
    size_t eo0 = b.len;
    bu32(&b, 0);
    size_t eo1 = b.len;
    bu32(&b, 0);
    size_t m0 = b.len;
    emit_material(&b, "Picture_01", 0); /* background -> bg.tpl   */
    size_t m1 = b.len;
    emit_material(&b, "Picture_00", 1); /* logo       -> logo.tpl */
    put32at(&b, eo0, (uint32_t)(m0 - sec_start));
    put32at(&b, eo1, (uint32_t)(m1 - sec_start));
    sec_end(&b, s);

    s = sec_begin(&b, "pan1");
    emit_pane(&b, 0x01, 0x04, 0xff, "RootPane", W, H);
    sec_end(&b, s);
    s = sec_begin(&b, "pas1");
    sec_end(&b, s);
    emit_pic(&b, "Picture_01", bgW, bgH, 0);     /* background pane (overscans)   */
    emit_pic(&b, "Picture_00", logoW, logoH, 1); /* logo pane (centred, smaller)  */
    s = sec_begin(&b, "pae1");
    sec_end(&b, s);

    s = sec_begin(&b, "grp1");
    char gn[0x10];
    memset(gn, 0, 0x10);
    memcpy(gn, "RootGroup", 9);
    bput(&b, gn, 0x10);
    bu16(&b, 0);
    bu16(&b, 0); /* 0 grouped panes */
    sec_end(&b, s);

    put32at(&b, fs, (uint32_t)b.len);
    *out_len = b.len;
    return b.p;
}

/* RLAN with a single RLVC track (pane alpha) on the named pane, holding the
 * pane fully opaque across the whole timeline.  Structure mirrors a retail
 * icon.brlan (frameSize 250, one pai1 entry, one RLVC tag, pane-alpha target);
 * the keyframes are constant 255 so the pane is visible at any frame the System
 * Menu chooses to render statically. */
static uint8_t *build_brlan(const char *pane, size_t *out_len) {
    buf_t b = {0};
    bput(&b, "RLAN", 4);
    bu16(&b, 0xFEFF);
    bu16(&b, 0x0008);
    size_t fs = b.len;
    bu32(&b, 0);
    bu16(&b, 0x0010);
    bu16(&b, 1); /* 1 section */

    size_t s = sec_begin(&b, "pai1");
    size_t pai = s - 4; /* pai1 offsets are relative here */
    bu16(&b, 250);      /* frameSize */
    bu8(&b, 1);
    bu8(&b, 0);  /* flags: loop, pad */
    bu16(&b, 0); /* numTextures */
    bu16(&b, 1); /* numEntries */
    size_t entriesOff = b.len;
    bu32(&b, 0); /* -> entry-offset table */
    size_t entry0Off = b.len;
    bu32(&b, 0); /* entry[0] offset */
    put32at(&b, entriesOff, (uint32_t)(entry0Off - pai));

    size_t entry = b.len; /* per-pane animation entry */
    put32at(&b, entry0Off, (uint32_t)(entry - pai));
    char nm[20];
    memset(nm, 0, 20);
    memcpy(nm, pane, strnlen(pane, 19));
    bput(&b, nm, 20);
    bu8(&b, 1);
    bu8(&b, 0);
    bu16(&b, 0); /* numTags=1, pad */
    size_t tagOff = b.len;
    bu32(&b, 0);

    size_t tag = b.len; /* RLVC tag */
    put32at(&b, tagOff, (uint32_t)(tag - entry));
    bput(&b, "RLVC", 4);
    bu16(&b, 1);
    bu16(&b, 0); /* numTargets=1, pad */
    size_t targetOff = b.len;
    bu32(&b, 0);

    size_t target = b.len; /* pane-alpha float track */
    put32at(&b, targetOff, (uint32_t)(target - tag));
    bu8(&b, 0x00); /* index */
    bu8(&b, 0x10); /* target = pane alpha */
    bu8(&b, 0x02); /* data type = hermite float */
    bu8(&b, 0x00);
    bu16(&b, 2);
    bu16(&b, 0); /* numKeyframes=2, pad */
    size_t kfOff = b.len;
    bu32(&b, 0);
    size_t kf = b.len;
    put32at(&b, kfOff, (uint32_t)(kf - target));
    bf32(&b, 0.0f);
    bf32(&b, 255.0f);
    bf32(&b, 0.0f); /* (frame, value, slope) */
    bf32(&b, 250.0f);
    bf32(&b, 255.0f);
    bf32(&b, 0.0f);

    sec_end(&b, s);
    put32at(&b, fs, (uint32_t)b.len);
    *out_len = b.len;
    return b.p;
}

/* ---- one .bin (banner.bin / icon.bin) ------------------------------------ */
/* Builds the inner U8 (arc/blyt + arc/anim + arc/timg), LZ77-stores it, then
 * IMD5-wraps it.  Icon and banner share the same two-pane structure; only the
 * canvas/pane/texture sizes and the file names differ. */
static uint8_t *build_bin(int is_icon, size_t *out_len, size_t *inner_u8_len) {
    /* canvas (icon 170x96, banner 608x456); background overscans the canvas,
     * the logo plate sits centred and smaller so the gradient frames it. */
    float W = is_icon ? 170.0f : 608.0f;
    float H = is_icon ? 96.0f : 456.0f;
    float bgW = W * 1.18f, bgH = H * 1.18f;
    float logoW = is_icon ? 132.0f : 470.0f;
    float logoH = is_icon ? 64.0f : 240.0f;
    int bgw = is_icon ? 128 : 256, bgh = is_icon ? 96 : 192;
    int lgw = is_icon ? 128 : 256, lgh = is_icon ? 64 : 128;

    size_t bl_len;
    uint8_t *brlyt = build_brlyt(W, H, bgW, bgH, logoW, logoH, &bl_len);

    uint8_t *bg_rgba = make_bg(bgw, bgh);
    size_t bg_len;
    uint8_t *bg_tpl = tpl_rgb5a3(bg_rgba, bgw, bgh, &bg_len);
    free(bg_rgba);
    uint8_t *lg_rgba = make_logo(lgw, lgh);
    size_t lg_len;
    uint8_t *lg_tpl = tpl_rgb5a3(lg_rgba, lgw, lgh, &lg_len);
    free(lg_rgba);

    size_t a_len;
    uint8_t *brlan = build_brlan("Picture_00", &a_len);

    /* nodes: 0 root, 1 arc, 2 blyt, 3 brlyt, 4 anim, 5 brlan, 6 timg, 7 bg, 8
     * logo */
    u8node nodes[9];
    nodes[0] = (u8node){1, "", 0, 9, NULL};
    nodes[1] = (u8node){1, "arc", 0, 9, NULL};
    nodes[2] = (u8node){1, "blyt", 1, 4, NULL};
    nodes[3] = (u8node){0, is_icon ? "icon.brlyt" : "banner.brlyt", 0, (uint32_t)bl_len, brlyt};
    nodes[4] = (u8node){1, "anim", 1, 6, NULL};
    nodes[5] = (u8node){0, is_icon ? "icon.brlan" : "banner.brlan", 0, (uint32_t)a_len, brlan};
    nodes[6] = (u8node){1, "timg", 1, 9, NULL};
    nodes[7] = (u8node){0, "bg.tpl", 0, (uint32_t)bg_len, bg_tpl};
    nodes[8] = (u8node){0, "logo.tpl", 0, (uint32_t)lg_len, lg_tpl};

    size_t u8_len;
    uint8_t *inner = build_u8(nodes, 9, &u8_len);
    free(brlyt);
    free(brlan);
    free(bg_tpl);
    free(lg_tpl);

    size_t   lz_len;
    uint8_t *lz = lz77_store(inner, u8_len, &lz_len);
    free(inner);
    uint8_t *bin = imd5_wrap(lz, lz_len, out_len);
    free(lz);
    *inner_u8_len = u8_len; /* IMET expects the *uncompressed* size */
    return bin;
}

/* ---- IMET + outer U8 = content 0 ----------------------------------------- */

static void utf16be(uint8_t *dst, size_t bytes, const char *s) {
    for(size_t i = 0; s[i] && (i + 1) * 2 <= bytes; i++) {
        dst[i * 2] = 0;
        dst[i * 2 + 1] = (uint8_t)s[i];
    }
}

uint8_t *wii_build_banner(const char *title, const uint8_t *sound_bin, size_t sound_bin_len,
                          size_t *out_len) {
    size_t   banner_len, icon_len, banner_u8, icon_u8;
    uint8_t *banner_bin = build_bin(0, &banner_len, &banner_u8);
    uint8_t *icon_bin = build_bin(1, &icon_len, &icon_u8);

    /* sound.bin is mandatory: every retail/HBC banner ships one, and the SM
     * banner loader extracts all three sections an IMET declares.  Use a
     * caller- supplied sound (already IMD5-wrapped) if given, else a generated
     * silent WAVE.  IMET soundSize = the uncompressed inner size. */
    uint8_t *gen_sound = NULL;
    uint32_t sound_u8;
    if(sound_bin) {
        sound_u8 = (sound_bin_len > 0x20) ? (uint32_t)(sound_bin_len - 0x20) : 0;
    } else {
        size_t gen_len, gen_inner;
        gen_sound = make_sound_bin(&gen_len, &gen_inner);
        sound_bin = gen_sound;
        sound_bin_len = gen_len;
        sound_u8 = (uint32_t)gen_inner;
    }

    u8node meta[5] = {
        {1, "", 0, 5, NULL},
        {1, "meta", 0, 5, NULL},
        {0, "banner.bin", 0, (uint32_t)banner_len, banner_bin},
        {0, "icon.bin", 0, (uint32_t)icon_len, icon_bin},
        {0, "sound.bin", 0, (uint32_t)sound_bin_len, sound_bin},
    };
    size_t meta_len;
    uint8_t *meta_u8 = build_u8(meta, 5, &meta_len);
    free(banner_bin);
    free(icon_bin);
    free(gen_sound);

    /* content0 layout, matching retail (FAST) + HBC banners exactly:
     *   0x000  0x40   header (build/name string; the System Menu reads the
     * IMET, not this, but its presence shifts the IMET to 0x40) 0x040  0x600
     * IMET header (magic at 0x80) 0x640  ...    outer U8 archive
     * (meta/banner.bin, icon.bin, sound.bin) The IMET magic MUST land at 0x80 —
     * Dolphin and the SM read content0+0x40 for the IMET; an IMET at 0x00 (no
     * header) fails to register the channel. */
    size_t total = 0x40 + 0x600 + meta_len;
    uint8_t *c = calloc(1, total);
    buf_t b = {c, 0, total};
    memcpy(c, "wii-load-ip", 11); /* 0x00 header (name string) */

    b.len = 0x80; /* IMET magic at 0x80 (base 0x40) */
    bput(&b, "IMET", 4);
    bu32(&b, 0x600);               /* IMET header size */
    bu32(&b, 3);                   /* file count */
    bu32(&b, (uint32_t)icon_u8);   /* icon size (uncompressed) */
    bu32(&b, (uint32_t)banner_u8); /* banner size (uncompressed) */
    bu32(&b, sound_u8);            /* sound size (uncompressed) */
    bu32(&b, 0);                   /* flag @+0x18: 0 for normal
                                    * downloadable channels (FAST/
                                    * Poke all 0; only HBC=1) */
    /* IMET language-name slots (UTF-16BE).  Retail fills 0-7 and 9, leaving
     * slot 8 empty — match that. */
    for(int i = 0; i < 10; i++) {
        if(i == 8)
            continue;
        utf16be(c + 0x9c + i * 0x54, 0x54, title);
    }

    /* IMET MD5 — the System Menu VERIFIES this; a wrong/zero hash makes it
     * reject the banner and drop the channel from the grid ("???").  It is the
     * LAST 16 bytes of the 0x600-byte IMET region, i.e. at content0+0x630 (NOT
     * 0x620), computed as md5([0x40,0x640)) with that 16-byte field left zero.
     * The field is still zero here (calloc'd, written just below), so hashing
     * the whole region is correct.  Verified bit-exact against FAST/HBC. */
    uint8_t digest[16];
    md5(c + 0x40, 0x600, digest);
    memcpy(c + 0x630, digest, 16);

    memcpy(c + 0x640, meta_u8, meta_len);
    free(meta_u8);

    *out_len = total;
    return c;
}
