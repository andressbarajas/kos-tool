#include "iso9660-lite.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../include/kosload/strutil.h"

#include "time-compat.h"

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef MKCDI_LITE_IP_TEMPLATE
#define MKCDI_LITE_IP_TEMPLATE "make-cd/tools/IP.BIN"
#endif

#define DC_IP_TEMPLATE_SIZE   32768
#define DC_AUDIO_TRACK_BYTES  710304
#define DC_AUDIO_HEADER_BYTES 352800
#define DC_GAP_SECTOR_SIZE    2336
#define DC_GAP_TRACK_SECTORS  75
#define DC_GAP_TRACK_BYTES    (DC_GAP_SECTOR_SIZE * DC_GAP_TRACK_SECTORS * 2)
#define DC_DATA_START_LBA     11702
#define DC_BOOT_FILE_NAME     "1ST_READ.BIN;1"
#define DC_BOOT_FILE_LABEL    "1ST_READ.BIN"
#define DC_COMPANY_NAME       "kos-tool"
#define DC_PRODUCT_NO         "T0300"
#define DC_VERSION            "V1.000"
#define DC_DEVICE_INFO        "CD-ROM1/1"
#define DC_AREA_SYMBOLS       "JUE"
#define DC_PERIPHERALS        "E000F10"
#define DC_MAX_CHUNK          (2048U * 1024U)

typedef struct {
    uint16_t offset;
    uint8_t value;
} dcdisc_patch_t;

static const uint8_t dcdisc_track_start_mark[10] = {
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff
};

static const dcdisc_patch_t dcdisc_gap1[] = {
    {0x002, 0x20}, {0x006, 0x20}, {0x91c, 0x3f}, {0x91d, 0x13}, {0x91e, 0xb0}, {0x91f, 0xbe},
};

static const dcdisc_patch_t dcdisc_sector1[] = {
    {0x0000, 0x02}, {0x0002, 0x96}, {0x0006, 0x2e}, {0x0007, 0x01}, {0x0024, 0xc4}, {0x0025, 0x01},
    {0x0038, 0x02}, {0x0041, 0xc4}, {0x0042, 0x01}, {0x005a, 0xff}, {0x005b, 0xff}, {0x005c, 0xff},
    {0x005d, 0xff}, {0x005e, 0xff}, {0x005f, 0xff}, {0x0060, 0xff}, {0x0061, 0xff}, {0x0062, 0x01},
    {0x0066, 0x80}, {0x006a, 0x02}, {0x006e, 0x10}, {0x0072, 0x44}, {0x0073, 0xac}, {0x00a0, 0xff},
    {0x00a1, 0xff}, {0x00a2, 0xff}, {0x00a3, 0xff}, {0x00bd, 0x01},
};

static const dcdisc_patch_t dcdisc_sector2[] = {
    {0x0000, 0x02}, {0x0002, 0x96}, {0x0006, 0x9c}, {0x0007, 0x04}, {0x0010, 0x02}, {0x0018, 0x01},
    {0x0020, 0xb6}, {0x0021, 0x2d}, {0x0038, 0x01}, {0x003c, 0x04}, {0x005a, 0xff}, {0x005b, 0xff},
    {0x005c, 0xff}, {0x005d, 0xff}, {0x005e, 0xff}, {0x005f, 0xff}, {0x0060, 0xff}, {0x0061, 0xff},
    {0x0062, 0x01}, {0x0066, 0x80}, {0x006a, 0x02}, {0x006e, 0x10}, {0x0072, 0x44}, {0x0073, 0xac},
    {0x00a0, 0xff}, {0x00a1, 0xff}, {0x00a2, 0xff}, {0x00a3, 0xff}, {0x00b0, 0x02}, {0x00b8, 0xb6},
    {0x00b9, 0x2d},
};

static const dcdisc_patch_t dcdisc_head_next[] = {
    {0x000b, 0x02}, {0x0016, 0x80}, {0x0017, 0x40}, {0x0018, 0x7e}, {0x0019, 0x05}, {0x001d, 0x98},
};

static const dcdisc_patch_t dcdisc_head_end[] = {
    {0x0001, 0x01},
    {0x0005, 0x01},
    {0x0026, 0x06},
    {0x0029, 0x80},
};

static uint32_t dcdisc_edc_table[256];
static uint8_t dcdisc_ecc_f_lut[256];
static uint8_t dcdisc_ecc_b_lut[256];
static bool dcdisc_tables_ready;

static void dcdisc_usage(const char *program) {
    fprintf(stderr,
            "Usage: %s --input-bin <1st_read.bin> --output <image.cdi>"
            " --name <disc-name>\n",
            program);
}

static int dcdisc_load_file(const char *path, uint8_t **data_out, size_t *size_out) {
    FILE *input;
    long size;
    uint8_t *data = NULL;

    input = fopen(path, "rb");
    if(!input)
        return -1;

    if(fseek(input, 0, SEEK_END) < 0) {
        fclose(input);
        return -1;
    }

    size = ftell(input);
    if(size < 0) {
        fclose(input);
        return -1;
    }

    if(fseek(input, 0, SEEK_SET) < 0) {
        fclose(input);
        return -1;
    }

    if(size > 0) {
        data = malloc((size_t)size);
        if(!data) {
            fclose(input);
            return -1;
        }

        if(fread(data, 1, (size_t)size, input) != (size_t)size) {
            fclose(input);
            free(data);
            return -1;
        }
    }

    fclose(input);
    *data_out = data;
    *size_out = (size_t)size;
    return 0;
}

static void dcdisc_patch_field(uint8_t *buffer, size_t offset, size_t length, const char *value) {
    size_t i;

    memset(buffer + offset, ' ', length);
    for(i = 0; i < length && value[i]; i++)
        buffer[offset + i] = (uint8_t)value[i];
}

static void dcdisc_release_date(char out[9]) {
    time_t now = time(NULL);
    struct tm tm_now;

    if(now == (time_t)-1 || tool_localtime_compat(now, &tm_now) != 0) {
        tool_set_default_date(out);
        return;
    }

    tool_format_date_yyyymmdd(out, &tm_now);
}

static void dcdisc_patch_ip_template(uint8_t *buffer, const char *disc_name) {
    char release_date[9];
    char title[129];

    memset(title, ' ', sizeof(title));
    title[sizeof(title) - 1] = '\0';
    compat_str_copy(title, sizeof(title), disc_name);
    dcdisc_release_date(release_date);

    dcdisc_patch_field(buffer, 0x00, 0x10, "SEGA SEGAKATANA");
    dcdisc_patch_field(buffer, 0x10, 0x10, "SEGA ENTERPRISES");
    dcdisc_patch_field(buffer, 0x20, 0x10, DC_DEVICE_INFO);
    dcdisc_patch_field(buffer, 0x30, 0x08, DC_AREA_SYMBOLS);
    dcdisc_patch_field(buffer, 0x38, 0x08, DC_PERIPHERALS);
    dcdisc_patch_field(buffer, 0x40, 0x0a, DC_PRODUCT_NO);
    dcdisc_patch_field(buffer, 0x4a, 0x06, DC_VERSION);
    dcdisc_patch_field(buffer, 0x50, 0x10, release_date);
    dcdisc_patch_field(buffer, 0x60, 0x10, DC_BOOT_FILE_LABEL);
    dcdisc_patch_field(buffer, 0x70, 0x10, DC_COMPANY_NAME);
    dcdisc_patch_field(buffer, 0x80, 0x80, title);
}

static int dcdisc_load_ip_template(uint8_t **buffer_out) {
    size_t size = 0;

    if(dcdisc_load_file(MKCDI_LITE_IP_TEMPLATE, buffer_out, &size) < 0)
        return -1;

    if(size != DC_IP_TEMPLATE_SIZE) {
        free(*buffer_out);
        *buffer_out = NULL;
        return -1;
    }

    return 0;
}

static int dcdisc_write_le16(FILE *output, uint16_t value) {
    uint8_t bytes[2];
    bytes[0] = (uint8_t)(value & 0xff);
    bytes[1] = (uint8_t)((value >> 8) & 0xff);
    return fwrite(bytes, 1, sizeof(bytes), output) == sizeof(bytes) ? 0 : -1;
}

static int dcdisc_write_le32(FILE *output, uint32_t value) {
    uint8_t bytes[4];
    bytes[0] = (uint8_t)(value & 0xff);
    bytes[1] = (uint8_t)((value >> 8) & 0xff);
    bytes[2] = (uint8_t)((value >> 16) & 0xff);
    bytes[3] = (uint8_t)((value >> 24) & 0xff);
    return fwrite(bytes, 1, sizeof(bytes), output) == sizeof(bytes) ? 0 : -1;
}

static void dcdisc_fill_patched(uint8_t *buffer, size_t size, const dcdisc_patch_t *patches, size_t count) {
    size_t i;

    memset(buffer, 0, size);
    for(i = 0; i < count; i++)
        buffer[patches[i].offset] = patches[i].value;
}

static int dcdisc_write_zeros(FILE *output, size_t bytes) {
    static const uint8_t zeros[ISO_LITE_SECTOR_SIZE] = {0};

    while(bytes > 0) {
        size_t chunk = bytes > sizeof(zeros) ? sizeof(zeros) : bytes;
        if(fwrite(zeros, 1, chunk, output) != chunk)
            return -1;
        bytes -= chunk;
    }

    return 0;
}

static int dcdisc_write_gap_track(FILE *output) {
    uint8_t sector[DC_GAP_SECTOR_SIZE];
    size_t i;

    dcdisc_fill_patched(sector, sizeof(sector), dcdisc_gap1, sizeof(dcdisc_gap1) / sizeof(dcdisc_gap1[0]));
    for(i = 0; i < DC_GAP_TRACK_SECTORS; i++) {
        if(fwrite(sector, 1, sizeof(sector), output) != sizeof(sector))
            return -1;
    }

    for(i = 0; i < DC_GAP_TRACK_SECTORS; i++) {
        if(fwrite(sector, 1, sizeof(sector), output) != sizeof(sector))
            return -1;
    }

    return 0;
}

static void dcdisc_init_tables(void) {
    size_t i;

    if(dcdisc_tables_ready)
        return;

    for(i = 0; i < 256; i++) {
        uint32_t r = (uint32_t)i;
        uint8_t j = (uint8_t)(((i << 1) ^ ((i & 0x80) ? 0x11d : 0)) & 0xff);
        size_t bit;

        for(bit = 0; bit < 8; bit++) {
            if(r & 1)
                r = (r >> 1) ^ 0xd8018001U;
            else
                r >>= 1;
        }

        dcdisc_edc_table[i] = r;
        dcdisc_ecc_f_lut[i] = j;
        dcdisc_ecc_b_lut[i ^ j] = (uint8_t)i;
    }

    dcdisc_tables_ready = true;
}

static uint32_t dcdisc_build_edc(const uint8_t *data, size_t start, size_t end) {
    uint32_t edc = 0;
    size_t i;

    for(i = start; i <= end; i++)
        edc = (edc >> 8) ^ dcdisc_edc_table[(edc ^ data[i]) & 0xff];

    return edc;
}

static void dcdisc_encode_ecc(uint8_t *raw, int major_count, int minor_count, int major_mult, int minor_inc,
                              size_t dest_offset) {
    int major;
    int size = major_count * minor_count;

    for(major = 0; major < major_count; major++) {
        int index = ((major >> 1) * major_mult) + (major & 1);
        uint8_t ecc_a = 0;
        uint8_t ecc_b = 0;
        int minor;

        for(minor = 0; minor < minor_count; minor++) {
            uint8_t temp = raw[12 + index];
            index += minor_inc;
            if(index >= size)
                index -= size;
            ecc_a ^= temp;
            ecc_b ^= temp;
            ecc_a = dcdisc_ecc_f_lut[ecc_a];
        }

        ecc_a = dcdisc_ecc_b_lut[dcdisc_ecc_f_lut[ecc_a] ^ ecc_b];
        raw[dest_offset + major] = ecc_a;
        raw[dest_offset + major + major_count] = (uint8_t)(ecc_a ^ ecc_b);
    }
}

static void dcdisc_encode_sector(const uint8_t *user_data, uint8_t *out_sector) {
    static const uint8_t subheader[8] = {0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x09, 0x00};
    uint8_t raw[2352];
    uint8_t saved_header[4];
    uint32_t edc;

    memset(raw, 0, sizeof(raw));
    raw[0] = 0x00;
    memset(raw + 1, 0xff, 10);
    raw[11] = 0x00;
    raw[15] = 0x02;
    memcpy(raw + 16, subheader, sizeof(subheader));
    memcpy(raw + 24, user_data, ISO_LITE_SECTOR_SIZE);

    edc = dcdisc_build_edc(raw, 16, 16 + 8 + ISO_LITE_SECTOR_SIZE - 1);
    raw[2072] = (uint8_t)(edc & 0xff);
    raw[2073] = (uint8_t)((edc >> 8) & 0xff);
    raw[2074] = (uint8_t)((edc >> 16) & 0xff);
    raw[2075] = (uint8_t)((edc >> 24) & 0xff);

    memcpy(saved_header, raw + 12, sizeof(saved_header));
    memset(raw + 12, 0, sizeof(saved_header));
    dcdisc_encode_ecc(raw, 86, 24, 2, 86, 2076);
    dcdisc_encode_ecc(raw, 52, 43, 86, 88, 2248);
    memcpy(raw + 12, saved_header, sizeof(saved_header));

    memcpy(out_sector, raw + 16, DC_GAP_SECTOR_SIZE);
}

static void dcdisc_apply_patches(uint8_t *buffer, size_t size, const dcdisc_patch_t *patches, size_t count) {
    size_t i;

    memset(buffer, 0, size);
    for(i = 0; i < count; i++) {
        if(patches[i].offset < size)
            buffer[patches[i].offset] = patches[i].value;
    }
}

static int dcdisc_write_header_start(FILE *output, const char *output_path) {
    uint8_t marks[20];
    uint8_t next[31];
    uint16_t unknown = 0x00ab;
    uint16_t unknown2 = 0x0210;
    uint8_t path_length;
    size_t i;

    for(i = 0; i < sizeof(marks); i++)
        marks[i] = dcdisc_track_start_mark[i % sizeof(dcdisc_track_start_mark)];

    path_length = (uint8_t)strlen(output_path);
    dcdisc_apply_patches(next, sizeof(next), dcdisc_head_next,
                         sizeof(dcdisc_head_next) / sizeof(dcdisc_head_next[0]));

    if(fwrite(marks, 1, sizeof(marks), output) != sizeof(marks))
        return -1;
    if(dcdisc_write_le16(output, unknown) < 0)
        return -1;
    if(dcdisc_write_le16(output, unknown2) < 0)
        return -1;
    if(fwrite(&path_length, 1, sizeof(path_length), output) != sizeof(path_length))
        return -1;
    if(fwrite(output_path, 1, path_length, output) != path_length)
        return -1;
    if(fwrite(next, 1, sizeof(next), output) != sizeof(next))
        return -1;

    return 0;
}

static int dcdisc_write_header_end(FILE *output, const char *volume_name, uint32_t total_space_used,
                                   long image_bytes_before_header) {
    uint8_t footer[42];
    uint8_t volume_len;
    uint32_t header_pos;

    volume_len = (uint8_t)strlen(volume_name);
    dcdisc_apply_patches(footer, sizeof(footer), dcdisc_head_end,
                         sizeof(dcdisc_head_end) / sizeof(dcdisc_head_end[0]));

    if(dcdisc_write_le32(output, total_space_used) < 0)
        return -1;
    if(fwrite(&volume_len, 1, sizeof(volume_len), output) != sizeof(volume_len))
        return -1;
    if(fwrite(volume_name, 1, volume_len, output) != volume_len)
        return -1;
    if(fwrite(footer, 1, sizeof(footer), output) != sizeof(footer))
        return -1;

    header_pos = (uint32_t)((ftell(output) + 4) - image_bytes_before_header);
    if(dcdisc_write_le32(output, header_pos) < 0)
        return -1;

    return 0;
}

static int dcdisc_write_audio_header(FILE *output) {
    uint8_t sector1[195];

    dcdisc_apply_patches(sector1, sizeof(sector1), dcdisc_sector1,
                         sizeof(dcdisc_sector1) / sizeof(dcdisc_sector1[0]));

    if(fwrite(sector1, 1, sizeof(sector1), output) != sizeof(sector1))
        return -1;

    return 0;
}

static int dcdisc_write_data_header(FILE *output, uint32_t data_sectors) {
    uint8_t  sector2[195];
    uint32_t with_leadout = data_sectors + 150;

    dcdisc_apply_patches(sector2, sizeof(sector2), dcdisc_sector2,
                         sizeof(dcdisc_sector2) / sizeof(dcdisc_sector2[0]));

    sector2[0x06] = (uint8_t)(data_sectors & 0xff);
    sector2[0x07] = (uint8_t)((data_sectors >> 8) & 0xff);
    sector2[0x08] = (uint8_t)((data_sectors >> 16) & 0xff);
    sector2[0x09] = (uint8_t)((data_sectors >> 24) & 0xff);

    sector2[0x24] = (uint8_t)(with_leadout & 0xff);
    sector2[0x25] = (uint8_t)((with_leadout >> 8) & 0xff);
    sector2[0x26] = (uint8_t)((with_leadout >> 16) & 0xff);
    sector2[0x27] = (uint8_t)((with_leadout >> 24) & 0xff);

    sector2[0x41] = (uint8_t)(with_leadout & 0xff);
    sector2[0x42] = (uint8_t)((with_leadout >> 8) & 0xff);
    sector2[0x43] = (uint8_t)((with_leadout >> 16) & 0xff);
    sector2[0x44] = (uint8_t)((with_leadout >> 24) & 0xff);

    if(fwrite(sector2, 1, sizeof(sector2), output) != sizeof(sector2))
        return -1;

    return 0;
}

static uint32_t dcdisc_total_space_used(uint32_t data_sector_count) {
    return (301 + DC_DATA_START_LBA + (data_sector_count - 1)) - 150;
}

static void dcdisc_srand(unsigned int *seed, unsigned int value) {
    *seed = value & 0xffffU;
}

static unsigned int dcdisc_rand(unsigned int *seed) {
    *seed = (*seed * 2109U + 9273U) & 0x7fffU;
    return (*seed + 0xc000U) & 0xffffU;
}

static void dcdisc_scramble_chunk(const uint8_t *src, uint8_t *dst, size_t chunk_size, unsigned int *seed) {
    size_t slices = chunk_size / 32U;
    int *indices;
    size_t i;
    size_t out_offset = 0;

    indices = malloc(slices * sizeof(*indices));
    if(!indices)
        return;

    for(i = 0; i < slices; i++)
        indices[i] = (int)i;

    for(i = slices; i-- > 0;) {
        size_t x = (dcdisc_rand(seed) * i) >> 16;
        int tmp = indices[i];
        indices[i] = indices[x];
        indices[x] = tmp;
        memcpy(dst + out_offset, src + 32U * (size_t)indices[i], 32U);
        out_offset += 32U;
    }
    free(indices);
}

static uint8_t *dcdisc_scramble_binary(const uint8_t *src, size_t size) {
    uint8_t     *dst;
    unsigned int seed;
    size_t offset = 0;
    size_t remaining = size;
    size_t chunk_size;

    dst = malloc(size);
    if(!dst)
        return NULL;

    dcdisc_srand(&seed, (unsigned int)size);

    for(chunk_size = DC_MAX_CHUNK; chunk_size >= 32U; chunk_size >>= 1) {
        while(remaining >= chunk_size) {
            dcdisc_scramble_chunk(src + offset, dst + offset, chunk_size, &seed);
            remaining -= chunk_size;
            offset += chunk_size;
        }
    }

    if(remaining > 0)
        memcpy(dst + offset, src + offset, remaining);

    return dst;
}

static void dcdisc_output_path(const char *output_path, char *resolved, size_t resolved_size) {
    size_t len;
    int is_absolute = 0;

    if(!output_path || !output_path[0]) {
        compat_str_copy(resolved, resolved_size, "");
        return;
    }

    if(output_path[0] == '/' || output_path[0] == '\\')
        is_absolute = 1;
#if defined(_WIN32)
    else if(((output_path[0] >= 'A' && output_path[0] <= 'Z') ||
             (output_path[0] >= 'a' && output_path[0] <= 'z')) &&
            output_path[1] == ':')
        is_absolute = 1;
#endif

    if(is_absolute) {
        compat_str_copy(resolved, resolved_size, output_path);
        return;
    }

#if defined(_WIN32)
    if(!_getcwd(resolved, (int)resolved_size)) {
#else
    if(!getcwd(resolved, resolved_size)) {
#endif
        compat_str_copy(resolved, resolved_size, output_path);
        return;
    }

    len = strlen(resolved);
    if(len > 0 && resolved[len - 1] != '/' && resolved[len - 1] != '\\') {
#if defined(_WIN32)
        if(len + 1 < resolved_size) {
            resolved[len++] = '\\';
            resolved[len] = '\0';
        }
#else
        if(len + 1 < resolved_size) {
            resolved[len++] = '/';
            resolved[len] = '\0';
        }
#endif
    }

    if(len < resolved_size)
        compat_str_copy(resolved + len, resolved_size - len, output_path);
}

int main(int argc, char **argv) {
    const char *input_bin = NULL;
    const char *output_path = NULL;
    const char *disc_name = NULL;
    uint8_t *boot_bin = NULL;
    size_t boot_bin_size = 0;
    uint8_t *scrambled = NULL;
    uint8_t *ip_template = NULL;
    FILE *iso = NULL;
    FILE *cdi = NULL;
    char resolved_output[PATH_MAX];
    char volume_id[ISO_LITE_VOLUME_ID_SIZE];
    iso_lite_file_t iso_file;
    iso_lite_config_t iso_config;
    iso_lite_stats_t iso_stats;
    char error_buf[256];
    uint8_t user_sector[ISO_LITE_SECTOR_SIZE];
    uint8_t raw_sector[DC_GAP_SECTOR_SIZE];
    uint8_t gap1_sector[DC_GAP_SECTOR_SIZE];
    uint32_t data_sector_count;
    long image_bytes_before_header;
    size_t i;
    int rc = 1;

    memset(&iso_file, 0, sizeof(iso_file));
    memset(&iso_config, 0, sizeof(iso_config));
    memset(&iso_stats, 0, sizeof(iso_stats));
    memset(error_buf, 0, sizeof(error_buf));
    memset(resolved_output, 0, sizeof(resolved_output));

    for(i = 1; i < (size_t)argc; i++) {
        if(!strcmp(argv[i], "--input-bin") && i + 1 < (size_t)argc)
            input_bin = argv[++i];
        else if(!strcmp(argv[i], "--output") && i + 1 < (size_t)argc)
            output_path = argv[++i];
        else if(!strcmp(argv[i], "--name") && i + 1 < (size_t)argc)
            disc_name = argv[++i];
        else {
            dcdisc_usage(argv[0]);
            return 1;
        }
    }

    if(!input_bin || !output_path || !disc_name) {
        dcdisc_usage(argv[0]);
        return 1;
    }

    if(dcdisc_load_file(input_bin, &boot_bin, &boot_bin_size) < 0) {
        fprintf(stderr, "mkcdi-lite: failed to read %s\n", input_bin);
        goto cleanup;
    }

    scrambled = dcdisc_scramble_binary(boot_bin, boot_bin_size);
    if(!scrambled) {
        fprintf(stderr, "mkcdi-lite: failed to scramble %s\n", input_bin);
        goto cleanup;
    }

    if(dcdisc_load_ip_template(&ip_template) < 0) {
        fprintf(stderr, "mkcdi-lite: failed to load %s\n", MKCDI_LITE_IP_TEMPLATE);
        goto cleanup;
    }

    dcdisc_patch_ip_template(ip_template, disc_name);
    iso_lite_normalize_volume_id(disc_name, volume_id, sizeof(volume_id));
    dcdisc_output_path(output_path, resolved_output, sizeof(resolved_output));
    dcdisc_init_tables();
    dcdisc_apply_patches(gap1_sector, sizeof(gap1_sector), dcdisc_gap1,
                         sizeof(dcdisc_gap1) / sizeof(dcdisc_gap1[0]));

    iso = tmpfile();
    if(!iso) {
        fprintf(stderr, "mkcdi-lite: failed to create a temporary ISO\n");
        goto cleanup;
    }

    iso_file.iso_name = DC_BOOT_FILE_NAME;
    iso_file.data = scrambled;
    iso_file.size = boot_bin_size;

    iso_config.volume_id = volume_id;
    iso_config.system_area = ip_template;
    iso_config.system_area_size = DC_IP_TEMPLATE_SIZE;
    iso_config.start_lba = DC_DATA_START_LBA;
    iso_config.eltorito_boot_name = NULL;

    if(iso_lite_write_stream(iso, &iso_config, &iso_file, 1, &iso_stats, error_buf, sizeof(error_buf)) < 0) {
        fprintf(stderr, "mkcdi-lite: %s\n", error_buf);
        goto cleanup;
    }

    rewind(iso);

    cdi = fopen(output_path, "wb");
    if(!cdi) {
        fprintf(stderr, "mkcdi-lite: failed to open %s for writing\n", output_path);
        goto cleanup;
    }

    if(dcdisc_write_zeros(cdi, DC_AUDIO_HEADER_BYTES) < 0 ||
       dcdisc_write_zeros(cdi, DC_AUDIO_TRACK_BYTES) < 0 || dcdisc_write_gap_track(cdi) < 0) {
        fprintf(stderr, "mkcdi-lite: failed while writing the CDI preamble\n");
        goto cleanup;
    }

    for(i = 0; i < iso_stats.total_sectors; i++) {
        if(fread(user_sector, 1, sizeof(user_sector), iso) != sizeof(user_sector)) {
            fprintf(stderr, "mkcdi-lite: failed while reading the temporary ISO\n");
            goto cleanup;
        }

        dcdisc_encode_sector(user_sector, raw_sector);
        if(fwrite(raw_sector, 1, sizeof(raw_sector), cdi) != sizeof(raw_sector)) {
            fprintf(stderr, "mkcdi-lite: failed while writing the CDI data track\n");
            goto cleanup;
        }
    }

    if(fwrite(gap1_sector, 1, sizeof(gap1_sector), cdi) != sizeof(gap1_sector) ||
       fwrite(gap1_sector, 1, sizeof(gap1_sector), cdi) != sizeof(gap1_sector)) {
        fprintf(stderr, "mkcdi-lite: failed while writing the CDI lead-out gap\n");
        goto cleanup;
    }

    image_bytes_before_header = ftell(cdi);
    data_sector_count = iso_stats.total_sectors + 2;

    if(dcdisc_write_le16(cdi, 0x0002) < 0 || dcdisc_write_le16(cdi, 0x0001) < 0 ||
       dcdisc_write_le32(cdi, 0x00000000) < 0 || dcdisc_write_header_start(cdi, resolved_output) < 0 ||
       dcdisc_write_audio_header(cdi) < 0 || dcdisc_write_header_start(cdi, resolved_output) < 0 ||
       dcdisc_write_data_header(cdi, data_sector_count) < 0 ||
       dcdisc_write_header_start(cdi, resolved_output) < 0 ||
       dcdisc_write_header_end(cdi, volume_id, dcdisc_total_space_used(data_sector_count),
                               image_bytes_before_header) < 0) {
        fprintf(stderr, "mkcdi-lite: failed while writing the CDI header\n");
        goto cleanup;
    }

    rc = 0;

cleanup:
    if(cdi)
        fclose(cdi);
    if(iso)
        fclose(iso);
    free(ip_template);
    free(scrambled);
    free(boot_bin);
    return rc;
}
