#include "iso9660-lite.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../include/kosload/strutil.h"

#include "time-compat.h"

typedef struct {
    char iso_name[ISO_LITE_FILE_NAME_SIZE];
    const uint8_t *data;
    uint32_t size;
    uint32_t extent_lba;
} iso_lite_layout_file_t;

#define ISO_LITE_ELTORITO_PAD_SECTORS 150U

static void iso_lite_set_error(char *error_buf, size_t error_buf_size, const char *fmt, ...) {
    va_list ap;

    if(!error_buf || !error_buf_size)
        return;

    va_start(ap, fmt);
    vsnprintf(error_buf, error_buf_size, fmt, ap);
    va_end(ap);
}

static void iso_lite_write_le16(uint8_t *dst, uint16_t value) {
    dst[0] = (uint8_t)(value & 0xff);
    dst[1] = (uint8_t)((value >> 8) & 0xff);
}

static void iso_lite_write_be16(uint8_t *dst, uint16_t value) {
    dst[0] = (uint8_t)((value >> 8) & 0xff);
    dst[1] = (uint8_t)(value & 0xff);
}

static void iso_lite_write_le32(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)(value & 0xff);
    dst[1] = (uint8_t)((value >> 8) & 0xff);
    dst[2] = (uint8_t)((value >> 16) & 0xff);
    dst[3] = (uint8_t)((value >> 24) & 0xff);
}

static void iso_lite_write_be32(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)((value >> 24) & 0xff);
    dst[1] = (uint8_t)((value >> 16) & 0xff);
    dst[2] = (uint8_t)((value >> 8) & 0xff);
    dst[3] = (uint8_t)(value & 0xff);
}

static void iso_lite_write_both16(uint8_t *dst, uint16_t value) {
    iso_lite_write_le16(dst, value);
    iso_lite_write_be16(dst + 2, value);
}

static void iso_lite_write_both32(uint8_t *dst, uint32_t value) {
    iso_lite_write_le32(dst, value);
    iso_lite_write_be32(dst + 4, value);
}

static void iso_lite_write_padded_field(uint8_t *dst, size_t width, const char *value) {
    size_t i;

    memset(dst, ' ', width);
    if(!value)
        return;

    for(i = 0; i < width && value[i]; i++)
        dst[i] = (uint8_t)value[i];
}

static int iso_lite_write_zeros(FILE *output, size_t count) {
    static const uint8_t zeroes[ISO_LITE_SECTOR_SIZE] = {0};

    while(count > 0) {
        size_t chunk = count > sizeof(zeroes) ? sizeof(zeroes) : count;
        if(fwrite(zeroes, 1, chunk, output) != chunk)
            return -1;
        count -= chunk;
    }

    return 0;
}

static int iso_lite_write_padded(FILE *output, const uint8_t *data, size_t size) {
    size_t padded = (size + ISO_LITE_SECTOR_SIZE - 1) & ~(ISO_LITE_SECTOR_SIZE - 1);

    if(size && fwrite(data, 1, size, output) != size)
        return -1;

    return iso_lite_write_zeros(output, padded - size);
}

static int iso_lite_get_current_times(struct tm *local_tm, struct tm *gm_tm) {
    time_t now = time(NULL);

    if(now == (time_t)-1)
        return -1;

    if(tool_localtime_compat(now, local_tm) != 0)
        return -1;

    if(tool_gmtime_compat(now, gm_tm) != 0)
        return -1;

    return 0;
}

static void iso_lite_fill_dir_time(uint8_t out[7]) {
    struct tm local_tm;
    struct tm gm_tm;
    time_t local_secs;
    time_t gm_secs;
    long offset_minutes;

    if(iso_lite_get_current_times(&local_tm, &gm_tm) != 0) {
        memset(out, 0, 7);
        return;
    }

    local_secs = mktime(&local_tm);
    gm_secs = mktime(&gm_tm);
    offset_minutes = (long)((local_secs - gm_secs) / 60);

    out[0] = (uint8_t)(local_tm.tm_year);
    out[1] = (uint8_t)(local_tm.tm_mon + 1);
    out[2] = (uint8_t)local_tm.tm_mday;
    out[3] = (uint8_t)local_tm.tm_hour;
    out[4] = (uint8_t)local_tm.tm_min;
    out[5] = (uint8_t)local_tm.tm_sec;
    out[6] = (int8_t)(offset_minutes / 15);
}

static void iso_lite_fill_volume_time(uint8_t out[17]) {
    struct tm local_tm;
    time_t local_secs;
    struct tm gm_tm;
    time_t gm_secs;
    long offset_minutes;

    if(iso_lite_get_current_times(&local_tm, &gm_tm) != 0) {
        tool_set_default_iso9660_volume_time(out);
        return;
    }

    local_secs = mktime(&local_tm);
    gm_secs = mktime(&gm_tm);
    offset_minutes = (long)((local_secs - gm_secs) / 60);

    if(tool_format_iso9660_volume_time(out, &local_tm) != 0)
        return;

    out[16] = (int8_t)(offset_minutes / 15);
}

static uint8_t iso_lite_dir_record_length(uint8_t name_length) {
    uint8_t length = (uint8_t)(33 + name_length);

    if((length & 1) == 1)
        length++;

    return length;
}

static uint8_t iso_lite_write_dir_record(uint8_t *dst, uint32_t extent_lba, uint32_t data_size, uint8_t flags,
                                         const uint8_t *name, uint8_t name_length) {
    uint8_t record_length = iso_lite_dir_record_length(name_length);

    memset(dst, 0, record_length);
    dst[0] = record_length;
    dst[1] = 0;
    iso_lite_write_both32(dst + 2, extent_lba);
    iso_lite_write_both32(dst + 10, data_size);
    iso_lite_fill_dir_time(dst + 18);
    dst[25] = flags;
    dst[26] = 0;
    dst[27] = 0;
    iso_lite_write_both16(dst + 28, 1);
    dst[32] = name_length;
    memcpy(dst + 33, name, name_length);

    return record_length;
}

static void iso_lite_write_validation_entry(uint8_t *dst) {
    uint16_t checksum = 0;
    int i;

    memset(dst, 0, ISO_LITE_SECTOR_SIZE);
    dst[0] = 0x01;
    dst[1] = 0x00;
    dst[0x1e] = 0x55;
    dst[0x1f] = 0xaa;

    for(i = 0; i < 16; i++) {
        if(i == 0x0e)
            continue;

        checksum = (uint16_t)(checksum + ((uint16_t)dst[i * 2] | ((uint16_t)dst[i * 2 + 1] << 8)));
    }

    checksum = (uint16_t)(0 - checksum);
    iso_lite_write_le16(dst + 0x1c, checksum);
}

static void iso_lite_write_boot_entry(uint8_t *dst, uint32_t boot_lba, uint16_t boot_load_sectors) {
    dst[0] = 0x88;
    dst[1] = 0x00;
    iso_lite_write_le16(dst + 2, 0x0000);
    dst[4] = 0x00;
    dst[5] = 0x00;
    iso_lite_write_le16(dst + 6, boot_load_sectors);
    iso_lite_write_le32(dst + 8, boot_lba);
}

void iso_lite_normalize_file_name(const char *input, char *output, size_t output_size) {
    char base[9];
    char ext[4];
    const char *dot = strrchr(input ? input : "", '.');
    size_t i = 0;
    size_t j = 0;
    const char *name = input ? input : "";

    if(!output_size)
        return;

    memset(base, 0, sizeof(base));
    memset(ext, 0, sizeof(ext));

    if(dot && dot != name) {
        for(i = 0; i < 8 && name[i] && &name[i] < dot; i++) {
            char c = name[i];
            base[i] = (char)(isalnum((unsigned char)c) || c == '_' ? toupper((unsigned char)c) : '_');
        }

        for(j = 0; j < 3 && dot[1 + j]; j++) {
            char c = dot[1 + j];
            ext[j] = (char)(isalnum((unsigned char)c) || c == '_' ? toupper((unsigned char)c) : '_');
        }
    } else {
        for(i = 0; i < 8 && name[i]; i++) {
            char c = name[i];
            base[i] = (char)(isalnum((unsigned char)c) || c == '_' ? toupper((unsigned char)c) : '_');
        }
    }

    output[0] = '\0';
    i = compat_str_append(output, output_size, 0, base);
    if(ext[0]) {
        i = compat_str_append(output, output_size, i, ".");
        i = compat_str_append(output, output_size, i, ext);
    }
    compat_str_append(output, output_size, i, ";1");
}

void iso_lite_normalize_volume_id(const char *input, char *output, size_t output_size) {
    size_t i = 0;

    if(!output_size)
        return;

    memset(output, 0, output_size);

    if(!input || !input[0]) {
        compat_str_append(output, output_size, 0, "KOSLOAD");
        return;
    }

    for(; i + 1 < output_size && input[i] && i < 32; i++) {
        unsigned char c = (unsigned char)input[i];

        if(isalnum(c) || c == '_' || c == '-')
            output[i] = (char)toupper(c);
        else if(c == ' ')
            output[i] = '_';
        else
            output[i] = '_';
    }

    if(!output[0])
        compat_str_append(output, output_size, 0, "KOSLOAD");
}

int iso_lite_write_stream(FILE *output, const iso_lite_config_t *config, const iso_lite_file_t *files,
                          size_t file_count, iso_lite_stats_t *stats, char *error_buf,
                          size_t error_buf_size) {
    char volume_id[ISO_LITE_VOLUME_ID_SIZE];
    iso_lite_layout_file_t *layout = NULL;
    bool use_eltorito;
    const char *boot_name;
    size_t i;
    size_t root_bytes = 0;
    uint32_t lba;
    uint32_t path_table_l_lba;
    uint32_t path_table_m_lba;
    uint32_t root_dir_lba;
    uint32_t boot_catalog_lba = 0;
    uint32_t total_sectors;
    uint32_t trailing_pad_sectors = 0;
    uint32_t boot_image_lba = 0;
    uint32_t current_lba;
    uint16_t boot_load_sectors = 0;
    uint8_t pvd[ISO_LITE_SECTOR_SIZE];
    uint8_t boot_record[ISO_LITE_SECTOR_SIZE];
    uint8_t terminator[ISO_LITE_SECTOR_SIZE];
    uint8_t path_table_l[ISO_LITE_SECTOR_SIZE];
    uint8_t path_table_m[ISO_LITE_SECTOR_SIZE];
    uint8_t root_dir[ISO_LITE_SECTOR_SIZE];
    uint8_t boot_catalog[ISO_LITE_SECTOR_SIZE];
    uint8_t root_name[1] = {0};
    uint8_t parent_name[1] = {1};

    if(!output || !config || !files || !file_count) {
        iso_lite_set_error(error_buf, error_buf_size, "mkiso-lite requires at least one input file");
        return -1;
    }

    use_eltorito = config->eltorito_boot_name && config->eltorito_boot_name[0];
    boot_name = config->eltorito_boot_name;

    layout = calloc(file_count, sizeof(*layout));
    if(!layout) {
        iso_lite_set_error(error_buf, error_buf_size, "out of memory");
        return -1;
    }

    iso_lite_normalize_volume_id(config->volume_id, volume_id, sizeof(volume_id));

    for(i = 0; i < file_count; i++) {
        if(!files[i].iso_name || !files[i].iso_name[0]) {
            free(layout);
            iso_lite_set_error(error_buf, error_buf_size, "input file %zu is missing an ISO name", i);
            return -1;
        }

        if(files[i].size > UINT32_MAX) {
            free(layout);
            iso_lite_set_error(error_buf, error_buf_size, "input file %s is too large", files[i].iso_name);
            return -1;
        }

        compat_str_copy(layout[i].iso_name, sizeof(layout[i].iso_name), files[i].iso_name);
        layout[i].data = files[i].data;
        layout[i].size = (uint32_t)files[i].size;
    }

    root_bytes += iso_lite_dir_record_length(1);
    root_bytes += iso_lite_dir_record_length(1);

    if(use_eltorito)
        root_bytes += iso_lite_dir_record_length(10);

    for(i = 0; i < file_count; i++)
        root_bytes += iso_lite_dir_record_length((uint8_t)strlen(layout[i].iso_name));

    if(root_bytes > ISO_LITE_SECTOR_SIZE) {
        free(layout);
        iso_lite_set_error(error_buf, error_buf_size, "root directory would exceed one ISO sector");
        return -1;
    }

    if(use_eltorito) {
        /* Match the known-good GameCube layout produced by mkisofs. */
        path_table_l_lba = 20;
        path_table_m_lba = 22;
        root_dir_lba = 24;
        trailing_pad_sectors = ISO_LITE_ELTORITO_PAD_SECTORS;
    } else {
        path_table_l_lba = 18;
        path_table_m_lba = 19;
        root_dir_lba = 20;
    }

    lba = root_dir_lba + 1;

    if(use_eltorito) {
        boot_catalog_lba = lba++;
    }

    for(i = 0; i < file_count; i++) {
        layout[i].extent_lba = lba;
        lba += (layout[i].size + ISO_LITE_SECTOR_SIZE - 1) / ISO_LITE_SECTOR_SIZE;
    }

    if(use_eltorito) {
        for(i = 0; i < file_count; i++) {
            if(strcmp(layout[i].iso_name, boot_name) == 0) {
                boot_image_lba = layout[i].extent_lba + config->start_lba;
                boot_load_sectors =
                    (uint16_t)(((layout[i].size + ISO_LITE_SECTOR_SIZE - 1) / ISO_LITE_SECTOR_SIZE) * 4U);
                break;
            }
        }

        if(!boot_image_lba || !boot_load_sectors) {
            free(layout);
            iso_lite_set_error(error_buf, error_buf_size, "El Torito boot file %s was not found", boot_name);
            return -1;
        }
    }

    total_sectors = lba + trailing_pad_sectors;

    if(stats)
        stats->total_sectors = total_sectors;

    if(config->system_area_size > 16U * ISO_LITE_SECTOR_SIZE) {
        free(layout);
        iso_lite_set_error(error_buf, error_buf_size, "system area is larger than 16 ISO sectors");
        return -1;
    }

    if(config->system_area && config->system_area_size) {
        if(fwrite(config->system_area, 1, config->system_area_size, output) != config->system_area_size) {
            free(layout);
            iso_lite_set_error(error_buf, error_buf_size, "failed to write system area");
            return -1;
        }
    }

    if(iso_lite_write_zeros(output, 16U * ISO_LITE_SECTOR_SIZE - config->system_area_size) < 0) {
        free(layout);
        iso_lite_set_error(error_buf, error_buf_size, "failed to pad system area");
        return -1;
    }

    current_lba = 16;

    memset(pvd, 0, sizeof(pvd));
    pvd[0] = 0x01;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 0x01;
    iso_lite_write_padded_field(pvd + 8, 32, config->system_id ? config->system_id : "KOS-TOOL");
    iso_lite_write_padded_field(pvd + 40, 32, volume_id);
    iso_lite_write_both32(pvd + 80, total_sectors);
    iso_lite_write_both16(pvd + 120, 1);
    iso_lite_write_both16(pvd + 124, 1);
    iso_lite_write_both16(pvd + 128, ISO_LITE_SECTOR_SIZE);
    iso_lite_write_both32(pvd + 132, 10);
    iso_lite_write_le32(pvd + 140, path_table_l_lba + config->start_lba);
    iso_lite_write_le32(pvd + 144, 0);
    iso_lite_write_be32(pvd + 148, path_table_m_lba + config->start_lba);
    iso_lite_write_be32(pvd + 152, 0);
    iso_lite_write_dir_record(pvd + 156, root_dir_lba + config->start_lba, ISO_LITE_SECTOR_SIZE, 0x02,
                              root_name, 1);
    iso_lite_write_padded_field(pvd + 190, 128, volume_id);
    iso_lite_write_padded_field(pvd + 318, 128, "KOS-TOOL");
    iso_lite_write_padded_field(pvd + 446, 128, "KOS-TOOL");
    iso_lite_write_padded_field(pvd + 574, 128, "KOS-TOOL");
    iso_lite_fill_volume_time(pvd + 813);
    iso_lite_fill_volume_time(pvd + 830);
    iso_lite_fill_volume_time(pvd + 847);
    memset(pvd + 864, '0', 16);
    pvd[880] = 0x01;

    if(fwrite(pvd, 1, sizeof(pvd), output) != sizeof(pvd)) {
        free(layout);
        iso_lite_set_error(error_buf, error_buf_size, "failed to write primary volume descriptor");
        return -1;
    }

    current_lba++;

    if(use_eltorito) {
        memset(boot_record, 0, sizeof(boot_record));
        boot_record[0] = 0x00;
        memcpy(boot_record + 1, "CD001", 5);
        boot_record[6] = 0x01;
        memcpy(boot_record + 7, "EL TORITO SPECIFICATION", 23);
        iso_lite_write_le32(boot_record + 71, boot_catalog_lba + config->start_lba);

        if(fwrite(boot_record, 1, sizeof(boot_record), output) != sizeof(boot_record)) {
            free(layout);
            iso_lite_set_error(error_buf, error_buf_size, "failed to write boot descriptor");
            return -1;
        }

        current_lba++;
    }

    memset(terminator, 0, sizeof(terminator));
    terminator[0] = 0xff;
    memcpy(terminator + 1, "CD001", 5);
    terminator[6] = 0x01;

    if(fwrite(terminator, 1, sizeof(terminator), output) != sizeof(terminator)) {
        free(layout);
        iso_lite_set_error(error_buf, error_buf_size, "failed to write volume descriptor terminator");
        return -1;
    }

    current_lba++;

    if(current_lba < path_table_l_lba &&
       iso_lite_write_zeros(output, (size_t)(path_table_l_lba - current_lba) * ISO_LITE_SECTOR_SIZE) < 0) {
        free(layout);
        iso_lite_set_error(error_buf, error_buf_size, "failed to pad before path table");
        return -1;
    }

    current_lba = path_table_l_lba;

    memset(path_table_l, 0, sizeof(path_table_l));
    path_table_l[0] = 1;
    path_table_l[1] = 0;
    iso_lite_write_le32(path_table_l + 2, root_dir_lba + config->start_lba);
    iso_lite_write_le16(path_table_l + 6, 1);
    path_table_l[8] = 0;
    path_table_l[9] = 0;

    memset(path_table_m, 0, sizeof(path_table_m));
    path_table_m[0] = 1;
    path_table_m[1] = 0;
    iso_lite_write_be32(path_table_m + 2, root_dir_lba + config->start_lba);
    iso_lite_write_be16(path_table_m + 6, 1);
    path_table_m[8] = 0;
    path_table_m[9] = 0;

    if(fwrite(path_table_l, 1, sizeof(path_table_l), output) != sizeof(path_table_l)) {
        free(layout);
        iso_lite_set_error(error_buf, error_buf_size, "failed to write little-endian path table");
        return -1;
    }

    current_lba = path_table_l_lba + 1;

    if(current_lba < path_table_m_lba &&
       iso_lite_write_zeros(output, (size_t)(path_table_m_lba - current_lba) * ISO_LITE_SECTOR_SIZE) < 0) {
        free(layout);
        iso_lite_set_error(error_buf, error_buf_size, "failed to pad before big-endian path table");
        return -1;
    }

    current_lba = path_table_m_lba;

    if(fwrite(path_table_m, 1, sizeof(path_table_m), output) != sizeof(path_table_m)) {
        free(layout);
        iso_lite_set_error(error_buf, error_buf_size, "failed to write big-endian path table");
        return -1;
    }

    current_lba++;

    if(current_lba < root_dir_lba &&
       iso_lite_write_zeros(output, (size_t)(root_dir_lba - current_lba) * ISO_LITE_SECTOR_SIZE) < 0) {
        free(layout);
        iso_lite_set_error(error_buf, error_buf_size, "failed to pad before root directory");
        return -1;
    }

    current_lba = root_dir_lba;

    memset(root_dir, 0, sizeof(root_dir));
    lba = 0;
    lba += iso_lite_write_dir_record(root_dir + lba, root_dir_lba + config->start_lba, ISO_LITE_SECTOR_SIZE,
                                     0x02, root_name, 1);
    lba += iso_lite_write_dir_record(root_dir + lba, root_dir_lba + config->start_lba, ISO_LITE_SECTOR_SIZE,
                                     0x02, parent_name, 1);

    if(use_eltorito) {
        static const uint8_t boot_cat_name[] = "BOOT.CAT;1";
        lba += iso_lite_write_dir_record(root_dir + lba, boot_catalog_lba + config->start_lba,
                                         ISO_LITE_SECTOR_SIZE, 0x00, boot_cat_name,
                                         (uint8_t)(sizeof(boot_cat_name) - 1));
    }

    for(i = 0; i < file_count; i++) {
        uint8_t name_length = (uint8_t)strlen(layout[i].iso_name);

        lba +=
            iso_lite_write_dir_record(root_dir + lba, layout[i].extent_lba + config->start_lba,
                                      layout[i].size, 0x00, (const uint8_t *)layout[i].iso_name, name_length);
    }

    if(fwrite(root_dir, 1, sizeof(root_dir), output) != sizeof(root_dir)) {
        free(layout);
        iso_lite_set_error(error_buf, error_buf_size, "failed to write root directory");
        return -1;
    }

    current_lba++;

    if(use_eltorito) {
        if(current_lba < boot_catalog_lba &&
           iso_lite_write_zeros(output, (size_t)(boot_catalog_lba - current_lba) * ISO_LITE_SECTOR_SIZE) <
               0) {
            free(layout);
            iso_lite_set_error(error_buf, error_buf_size, "failed to pad before boot catalog");
            return -1;
        }

        current_lba = boot_catalog_lba;
        memset(boot_catalog, 0, sizeof(boot_catalog));
        iso_lite_write_validation_entry(boot_catalog);
        iso_lite_write_boot_entry(boot_catalog + 0x20, boot_image_lba, boot_load_sectors);

        if(fwrite(boot_catalog, 1, sizeof(boot_catalog), output) != sizeof(boot_catalog)) {
            free(layout);
            iso_lite_set_error(error_buf, error_buf_size, "failed to write boot catalog");
            return -1;
        }

        current_lba++;
    }

    if(current_lba < layout[0].extent_lba &&
       iso_lite_write_zeros(output, (size_t)(layout[0].extent_lba - current_lba) * ISO_LITE_SECTOR_SIZE) <
           0) {
        free(layout);
        iso_lite_set_error(error_buf, error_buf_size, "failed to pad before file data");
        return -1;
    }

    for(i = 0; i < file_count; i++) {
        if(iso_lite_write_padded(output, layout[i].data, layout[i].size) < 0) {
            iso_lite_set_error(error_buf, error_buf_size, "failed to write file %s", layout[i].iso_name);
            free(layout);
            return -1;
        }
    }

    if(trailing_pad_sectors &&
       iso_lite_write_zeros(output, (size_t)trailing_pad_sectors * ISO_LITE_SECTOR_SIZE) < 0) {
        free(layout);
        iso_lite_set_error(error_buf, error_buf_size, "failed to write trailing pad sectors");
        return -1;
    }

    free(layout);
    return 0;
}

int iso_lite_write_image(const char *output_path, const iso_lite_config_t *config,
                         const iso_lite_file_t *files, size_t file_count, iso_lite_stats_t *stats,
                         char *error_buf, size_t error_buf_size) {
    FILE *output;
    int rc;

    output = fopen(output_path, "wb");
    if(!output) {
        iso_lite_set_error(error_buf, error_buf_size, "failed to open %s for writing", output_path);
        return -1;
    }

    rc = iso_lite_write_stream(output, config, files, file_count, stats, error_buf, error_buf_size);
    fclose(output);
    return rc;
}
