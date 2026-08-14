/* mkxiso-lite — cleanroom XDVDFS (XISO) writer for the original Xbox
 *
 * make-dist/tools/mkxiso-lite.c
 * Copyright (C) 2026 Andress Barajas
 *
 * Packs a directory tree into an original-Xbox XISO (XDVDFS) disc image
 * suitable for xemu or a modchipped Xbox. The input directory should contain
 * default.xbe at its root.
 *
 * The XDVDFS on-disc format was reverse-engineered from a known-good sample
 * image plus public format documentation; this does NOT use extract-xiso (or
 * any other tool's) source.
 *
 * Usage:
 *   mkxiso-lite --source-dir DIR --output game.xiso [--quiet]
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define SECTOR        2048u
#define RESERVED_SEC  32u            /* sectors before the volume descriptor */
#define ALIGN_SECTORS 32u            /* pad whole image up to this many sectors */
#define XDVDFS_MAGIC  "MICROSOFT*XBOX*MEDIA"   /* 20 bytes */
#define ATTR_DIR      0x10u
#define ATTR_ARCHIVE  0x20u

static bool g_quiet = false;

static void die(const char *msg) { fprintf(stderr, "mkxiso-lite: %s\n", msg); exit(1); }
static void *xmalloc(size_t n) { void *p = calloc(1, n ? n : 1); if(!p) die("out of memory"); return p; }
static uint32_t sec_count(uint32_t bytes) { return (bytes + SECTOR - 1) / SECTOR; }
static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

static void wr16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

/* ---- node tree ---- */
typedef struct node {
    char         name[256];
    char         path[PATH_MAX];   /* host path (files only) */
    bool         is_dir;
    uint32_t     size;             /* file size, or directory-table size */
    uint32_t     start_sector;
    /* children (directories only), kept sorted by xdvdfs_cmp */
    struct node **kids;
    size_t        nkids, cap;
    /* table layout (assigned during build): byte offset of this entry within
       its parent's table, and left/right child entry offsets (dwords) */
    uint32_t      ent_off;
    uint16_t      left, right;
} node_t;

/* XDVDFS name comparison: case-insensitive (uppercased) ASCII; shorter is
   "less" on a common prefix. */
static int up(int c) { return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c; }
static int xdvdfs_cmp(const char *a, const char *b) {
    while(*a && *b) {
        int d = up((unsigned char)*a) - up((unsigned char)*b);
        if(d) return d;
        a++; b++;
    }
    return (int)((unsigned char)*a) - (int)((unsigned char)*b);
}
static int node_cmp(const void *x, const void *y) {
    const node_t *a = *(const node_t *const *)x, *b = *(const node_t *const *)y;
    return xdvdfs_cmp(a->name, b->name);
}

static bool skip_name(const char *n) {
    if(!strcmp(n, ".") || !strcmp(n, "..")) return true;
    if(!strcmp(n, ".DS_Store") || !strcmp(n, "Thumbs.db")) return true;
    if(n[0] == '.' && n[1] == '_') return true;   /* AppleDouble */
    return false;
}

static void add_kid(node_t *dir, node_t *kid) {
    if(dir->nkids == dir->cap) {
        dir->cap = dir->cap ? dir->cap * 2 : 8;
        dir->kids = realloc(dir->kids, dir->cap * sizeof *dir->kids);
        if(!dir->kids) die("out of memory");
    }
    dir->kids[dir->nkids++] = kid;
}

/* recursively scan a host directory into a node tree */
static node_t *scan(const char *path, const char *name) {
    node_t *n = xmalloc(sizeof *n);
    snprintf(n->name, sizeof n->name, "%s", name);
    snprintf(n->path, sizeof n->path, "%s", path);

    struct stat st;
    if(stat(path, &st) != 0) { fprintf(stderr, "mkxiso-lite: stat %s: %s\n", path, strerror(errno)); exit(1); }

    if(S_ISDIR(st.st_mode)) {
        n->is_dir = true;
        DIR *d = opendir(path);
        if(!d) { fprintf(stderr, "mkxiso-lite: opendir %s: %s\n", path, strerror(errno)); exit(1); }
        struct dirent *e;
        while((e = readdir(d))) {
            if(skip_name(e->d_name)) continue;
            char child[PATH_MAX];
            snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            add_kid(n, scan(child, e->d_name));
        }
        closedir(d);
        qsort(n->kids, n->nkids, sizeof *n->kids, node_cmp);
    } else {
        n->is_dir = false;
        if(st.st_size > 0xFFFFFFFFLL) die("file larger than 4 GiB");
        n->size = (uint32_t)st.st_size;
    }
    return n;
}

/* ---- directory table layout (balanced BST, pre-order placement) ---- */
static uint32_t entry_bytes(const node_t *n) { return align_up(14u + (uint32_t)strlen(n->name), 4u); }

/* assign table offsets in pre-order over the balanced BST of kids[lo..hi];
   *cursor is the next free byte offset. returns the dword-offset of the
   subtree root (0 for an empty subtree). */
static uint16_t layout_tree(node_t **kids, int lo, int hi, uint32_t *cursor) {
    if(lo > hi) return 0;                 /* 0 = no child */
    int mid = (lo + hi) / 2;
    node_t *r = kids[mid];
    r->ent_off = *cursor;
    *cursor += entry_bytes(r);
    r->left  = layout_tree(kids, lo, mid - 1, cursor);
    r->right = layout_tree(kids, mid + 1, hi, cursor);
    return (uint16_t)(r->ent_off / 4u);
}

/* compute each directory's table size (recursively, bottom-up) */
static void compute_tables(node_t *n) {
    if(!n->is_dir) return;
    for(size_t i = 0; i < n->nkids; i++) compute_tables(n->kids[i]);
    uint32_t cursor = 0;
    layout_tree(n->kids, 0, (int)n->nkids - 1, &cursor);
    n->size = cursor;                     /* directory-table size in bytes */
}

/* ---- sector assignment (BFS, matches the reference) ---- */
typedef struct { node_t **q; size_t head, tail, cap; } queue_t;
static void q_push(queue_t *q, node_t *n) {
    if(q->tail == q->cap) { q->cap = q->cap ? q->cap * 2 : 16; q->q = realloc(q->q, q->cap * sizeof *q->q); if(!q->q) die("oom"); }
    q->q[q->tail++] = n;
}

static uint32_t assign_sectors(node_t *root) {
    uint32_t cursor = RESERVED_SEC + 1;   /* sector 33: first table */
    queue_t q = {0};
    root->start_sector = cursor;
    cursor += sec_count(root->size);
    q_push(&q, root);
    while(q.head < q.tail) {
        node_t *dir = q.q[q.head++];
        /* Place children in pre-order (== ascending entry offset): file data
           inline, subdir tables inline and enqueued for their own children. */
        node_t **pre = xmalloc(dir->nkids * sizeof *pre);
        for(size_t i = 0; i < dir->nkids; i++) pre[i] = dir->kids[i];
        /* insertion sort by ent_off */
        for(size_t i = 1; i < dir->nkids; i++) {
            node_t *t = pre[i]; size_t j = i;
            while(j > 0 && pre[j-1]->ent_off > t->ent_off) { pre[j] = pre[j-1]; j--; }
            pre[j] = t;
        }
        for(size_t i = 0; i < dir->nkids; i++) {
            node_t *c = pre[i];
            if(c->is_dir) {
                c->start_sector = c->size ? cursor : 0;
                cursor += sec_count(c->size);
                q_push(&q, c);
            } else {
                c->start_sector = c->size ? cursor : 0;
                cursor += sec_count(c->size);
            }
        }
        free(pre);
    }
    free(q.q);
    return cursor;   /* total sectors used (before final padding) */
}

/* ---- emit ---- */
static void emit_dir_table(node_t *dir, uint8_t *img) {
    uint8_t *t = img + (size_t)dir->start_sector * SECTOR;
    /* 0xFF-fill the table region first (entry padding convention) */
    memset(t, 0xFF, dir->size);
    for(size_t i = 0; i < dir->nkids; i++) {
        node_t *c = dir->kids[i];
        uint8_t *e = t + c->ent_off;
        wr16(e + 0x00, c->left);
        wr16(e + 0x02, c->right);
        wr32(e + 0x04, c->start_sector);
        wr32(e + 0x08, c->size);
        e[0x0C] = c->is_dir ? ATTR_DIR : ATTR_ARCHIVE;
        e[0x0D] = (uint8_t)strlen(c->name);
        memcpy(e + 0x0E, c->name, strlen(c->name));
    }
}

static void emit_file(node_t *f, uint8_t *img) {
    if(!f->size) return;
    FILE *in = fopen(f->path, "rb");
    if(!in) { fprintf(stderr, "mkxiso-lite: open %s: %s\n", f->path, strerror(errno)); exit(1); }
    if(fread(img + (size_t)f->start_sector * SECTOR, 1, f->size, in) != f->size) die("short read of input file");
    fclose(in);
}

static void emit_all(node_t *dir, uint8_t *img) {
    emit_dir_table(dir, img);
    for(size_t i = 0; i < dir->nkids; i++) {
        node_t *c = dir->kids[i];
        if(c->is_dir) emit_all(c, img);
        else emit_file(c, img);
    }
}

static void usage(const char *p) {
    fprintf(stderr, "usage: %s --source-dir DIR --output game.xiso [--quiet]\n", p);
}

int main(int argc, char **argv) {
    const char *src = NULL, *out = NULL;
    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "--source-dir") && i + 1 < argc) src = argv[++i];
        else if(!strcmp(argv[i], "--output") && i + 1 < argc) out = argv[++i];
        else if(!strcmp(argv[i], "--quiet")) g_quiet = true;
        else { usage(argv[0]); return 1; }
    }
    if(!src || !out) { usage(argv[0]); return 1; }

    node_t *root = scan(src, "");
    if(!root->is_dir) die("source must be a directory");

    compute_tables(root);
    uint32_t total_sec = assign_sectors(root);
    total_sec = align_up(total_sec, ALIGN_SECTORS);

    uint64_t img_bytes = (uint64_t)total_sec * SECTOR;
    if(img_bytes > (uint64_t)SIZE_MAX) die("image too large for memory");
    uint8_t *img = xmalloc((size_t)img_bytes);

    /* volume descriptor at sector 32 */
    uint8_t *vd = img + (size_t)RESERVED_SEC * SECTOR;
    memcpy(vd + 0x000, XDVDFS_MAGIC, 20);
    wr32(vd + 0x014, root->start_sector);
    wr32(vd + 0x018, root->size);
    /* +0x1C FILETIME left 0 */
    memcpy(vd + 0x7EC, XDVDFS_MAGIC, 20);

    emit_all(root, img);

    FILE *o = fopen(out, "wb");
    if(!o) { fprintf(stderr, "mkxiso-lite: open %s: %s\n", out, strerror(errno)); return 1; }
    if(fwrite(img, 1, (size_t)img_bytes, o) != (size_t)img_bytes) die("short write");
    fclose(o);

    if(!g_quiet)
        printf("mkxiso-lite: wrote %s (%llu bytes, %u sectors)\n",
               out, (unsigned long long)img_bytes, total_sec);
    return 0;
}
