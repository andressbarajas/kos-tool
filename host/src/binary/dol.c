/* host/src/binary/dol.c */
/*
 * Nintendo DOL binary loader for kostool.
 *
 * The DOL format is the GameCube/Wii native executable format.
 * It consists of a 256-byte header followed by raw section data.
 *
 * DOL header layout (all big-endian):
 *   0x000-0x01B: 7 text section file offsets
 *   0x01C-0x047: 11 data section file offsets
 *   0x048-0x063: 7 text section load addresses
 *   0x064-0x08F: 11 data section load addresses
 *   0x090-0x0AB: 7 text section sizes
 *   0x0AC-0x0D7: 11 data section sizes
 *   0x0D8-0x0DB: BSS load address
 *   0x0DC-0x0DF: BSS size
 *   0x0E0-0x0E3: Entry point
 *   0x0E4-0x0FF: Padding (zeros)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <kostool/binary.h>

#define DOL_HEADER_SIZE     256
#define DOL_MAX_TEXT        7
#define DOL_MAX_DATA        11

typedef struct {
    uint32_t text_offset[DOL_MAX_TEXT];
    uint32_t data_offset[DOL_MAX_DATA];
    uint32_t text_address[DOL_MAX_TEXT];
    uint32_t data_address[DOL_MAX_DATA];
    uint32_t text_size[DOL_MAX_TEXT];
    uint32_t data_size[DOL_MAX_DATA];
    uint32_t bss_address;
    uint32_t bss_size;
    uint32_t entry_point;
} dol_header_t;

static uint32_t read32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void parse_header(const uint8_t *raw, dol_header_t *hdr)
{
    int i;
    for (i = 0; i < DOL_MAX_TEXT; i++) {
        hdr->text_offset[i]  = read32be(raw + 0x00 + i * 4);
        hdr->text_address[i] = read32be(raw + 0x48 + i * 4);
        hdr->text_size[i]    = read32be(raw + 0x90 + i * 4);
    }
    for (i = 0; i < DOL_MAX_DATA; i++) {
        hdr->data_offset[i]  = read32be(raw + 0x1C + i * 4);
        hdr->data_address[i] = read32be(raw + 0x64 + i * 4);
        hdr->data_size[i]    = read32be(raw + 0xAC + i * 4);
    }
    hdr->bss_address = read32be(raw + 0xD8);
    hdr->bss_size    = read32be(raw + 0xDC);
    hdr->entry_point = read32be(raw + 0xE0);
}

/*
 * DOL has no magic number. Validate structurally:
 *   - File >= 256 bytes
 *   - Entry point in GC MEM1 range (0x80000000-0x817FFFFF)
 *   - At least one text section with non-zero size
 *   - First non-empty section offset >= DOL_HEADER_SIZE
 *   - .dol extension as additional hint
 */
static int dol_probe(const char *filename)
{
    struct stat st;
    uint8_t raw[DOL_HEADER_SIZE];
    dol_header_t hdr;
    int fd, i;
    int has_section = 0;
    const char *ext;

    if (stat(filename, &st) != 0 || st.st_size < DOL_HEADER_SIZE)
        return 0;

    fd = open(filename, O_RDONLY);
    if (fd < 0)
        return 0;
    if (read(fd, raw, DOL_HEADER_SIZE) != DOL_HEADER_SIZE) {
        close(fd);
        return 0;
    }
    close(fd);

    parse_header(raw, &hdr);

    /* Entry point must be in GC MEM1 (cached) range */
    if (hdr.entry_point < 0x80000000 || hdr.entry_point > 0x817FFFFF)
        return 0;

    /* Need at least one text or data section */
    for (i = 0; i < DOL_MAX_TEXT; i++) {
        if (hdr.text_size[i] > 0) {
            /* Section offset must be past header */
            if (hdr.text_offset[i] < DOL_HEADER_SIZE)
                return 0;
            /* Load address must be in GC memory */
            if (hdr.text_address[i] < 0x80000000)
                return 0;
            has_section = 1;
        }
    }
    for (i = 0; i < DOL_MAX_DATA; i++) {
        if (hdr.data_size[i] > 0) {
            if (hdr.data_offset[i] < DOL_HEADER_SIZE)
                return 0;
            if (hdr.data_address[i] < 0x80000000)
                return 0;
            has_section = 1;
        }
    }

    if (!has_section) {
        /* No sections found — check extension as last resort */
        ext = strrchr(filename, '.');
        if (!ext || strcasecmp(ext, ".dol") != 0)
            return 0;
    }

    return 1;
}

static int dol_load(const char *filename, uint32_t *entry_addr,
                    binary_section_cb callback, void *user_data)
{
    int fd;
    uint8_t raw_hdr[DOL_HEADER_SIZE];
    dol_header_t hdr;
    off_t file_size;
    int i, ret;

    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror(filename);
        return -1;
    }

    file_size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    if (read(fd, raw_hdr, DOL_HEADER_SIZE) != DOL_HEADER_SIZE) {
        fprintf(stderr, "DOL: failed to read header\n");
        close(fd);
        return -1;
    }

    parse_header(raw_hdr, &hdr);

    printf("File format is DOL, entry point is 0x%08x\n", hdr.entry_point);
    *entry_addr = hdr.entry_point;

    /* Load text sections */
    for (i = 0; i < DOL_MAX_TEXT; i++) {
        if (hdr.text_size[i] == 0)
            continue;

        if (hdr.text_offset[i] + hdr.text_size[i] > (uint32_t)file_size) {
            fprintf(stderr, "DOL: text%d extends past end of file\n", i);
            close(fd);
            return -1;
        }

        uint8_t *data = malloc(hdr.text_size[i]);
        if (!data) {
            close(fd);
            return -1;
        }

        lseek(fd, hdr.text_offset[i], SEEK_SET);
        if (read(fd, data, hdr.text_size[i]) != (ssize_t)hdr.text_size[i]) {
            fprintf(stderr, "DOL: failed to read text%d\n", i);
            free(data);
            close(fd);
            return -1;
        }

        char name[8];
        snprintf(name, sizeof(name), "text%d", i);

        binary_section_t section = {
            .name = name,
            .load_addr = hdr.text_address[i],
            .size = hdr.text_size[i],
            .data = data,
        };

        ret = callback(&section, user_data);
        free(data);
        if (ret != 0) {
            close(fd);
            return ret;
        }
    }

    /* Load data sections */
    for (i = 0; i < DOL_MAX_DATA; i++) {
        if (hdr.data_size[i] == 0)
            continue;

        if (hdr.data_offset[i] + hdr.data_size[i] > (uint32_t)file_size) {
            fprintf(stderr, "DOL: data%d extends past end of file\n", i);
            close(fd);
            return -1;
        }

        uint8_t *data = malloc(hdr.data_size[i]);
        if (!data) {
            close(fd);
            return -1;
        }

        lseek(fd, hdr.data_offset[i], SEEK_SET);
        if (read(fd, data, hdr.data_size[i]) != (ssize_t)hdr.data_size[i]) {
            fprintf(stderr, "DOL: failed to read data%d\n", i);
            free(data);
            close(fd);
            return -1;
        }

        char name[8];
        snprintf(name, sizeof(name), "data%d", i);

        binary_section_t section = {
            .name = name,
            .load_addr = hdr.data_address[i],
            .size = hdr.data_size[i],
            .data = data,
        };

        ret = callback(&section, user_data);
        free(data);
        if (ret != 0) {
            close(fd);
            return ret;
        }
    }

    close(fd);
    return 0;
}

const binary_ops_t dol_binary_ops = {
    .name = "DOL",
    .probe = dol_probe,
    .load = dol_load,
};
