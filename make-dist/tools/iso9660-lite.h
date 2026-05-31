#ifndef ISO9660_LITE_H
#define ISO9660_LITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define ISO_LITE_FILE_NAME_SIZE 16
#define ISO_LITE_VOLUME_ID_SIZE 33
#define ISO_LITE_SECTOR_SIZE    2048

typedef struct {
    const char *iso_name;
    const uint8_t *data;
    size_t size;
} iso_lite_file_t;

typedef struct {
    const char *volume_id;
    /* PVD System Identifier (offset 8, 32 bytes).  NULL keeps the default
     * "KOS-TOOL".  PS2 discs must set this to "PLAYSTATION". */
    const char *system_id;
    const uint8_t *system_area;
    size_t system_area_size;
    uint32_t start_lba;
    const char *eltorito_boot_name;
} iso_lite_config_t;

typedef struct {
    uint32_t total_sectors;
} iso_lite_stats_t;

void iso_lite_normalize_file_name(const char *input, char *output, size_t output_size);
void iso_lite_normalize_volume_id(const char *input, char *output, size_t output_size);

int iso_lite_write_stream(FILE *output, const iso_lite_config_t *config, const iso_lite_file_t *files,
                          size_t file_count, iso_lite_stats_t *stats, char *error_buf, size_t error_buf_size);

int iso_lite_write_image(const char *output_path, const iso_lite_config_t *config,
                         const iso_lite_file_t *files, size_t file_count, iso_lite_stats_t *stats,
                         char *error_buf, size_t error_buf_size);

#endif /* ISO9660_LITE_H */
