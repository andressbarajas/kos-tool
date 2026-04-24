#include "iso9660-lite.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <kosload/strutil.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    iso_lite_file_t iso;
    void *owned_data;
    char iso_name[ISO_LITE_FILE_NAME_SIZE];
} mkiso_input_file_t;

static void mkiso_set_path_error(char *error_buf, size_t error_buf_size,
                                 const char *prefix, const char *path)
{
    size_t prefix_len;
    size_t available;
    size_t path_len;

    if (!error_buf || error_buf_size == 0)
        return;

    if (!prefix)
        prefix = "";
    if (!path)
        path = "(null)";

    prefix_len = compat_str_copy(error_buf, error_buf_size, prefix);
    if (prefix_len + 1 >= error_buf_size)
        return;
    available = error_buf_size - prefix_len - 1;
    path_len = strlen(path);

    if (path_len <= available) {
        memcpy(error_buf + prefix_len, path, path_len);
        error_buf[prefix_len + path_len] = '\0';
        return;
    }

    if (available > 3) {
        size_t tail_len = available - 3;
        memcpy(error_buf + prefix_len, "...", 3);
        memcpy(error_buf + prefix_len + 3, path + path_len - tail_len, tail_len);
        error_buf[prefix_len + available] = '\0';
        return;
    }

    memcpy(error_buf + prefix_len, path + path_len - available, available);
    error_buf[prefix_len + available] = '\0';
}

static void mkiso_free_files(mkiso_input_file_t *files, size_t count)
{
    if (!files)
        return;

    while (count > 0)
        free(files[--count].owned_data);

    free(files);
}

static const char *mkiso_path_basename(const char *path)
{
    const char *base = path ? path : "";
    const char *slash;
    const char *backslash;

    slash = strrchr(base, '/');
    backslash = strrchr(base, '\\');

    if (slash && (!backslash || slash > backslash))
        return slash[1] ? slash + 1 : slash;

    if (backslash)
        return backslash[1] ? backslash + 1 : backslash;

    return base;
}

static void mkiso_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s --output <image.iso> --source-dir <dir>\n"
            "          [--volume-id <label>] [--system-area <file>]\n"
            "          [--eltorito-boot <file>] [--start-lba <lba>] [--quiet]\n",
            program);
}

static int mkiso_compare_files(const void *a, const void *b)
{
    const mkiso_input_file_t *fa = (const mkiso_input_file_t *)a;
    const mkiso_input_file_t *fb = (const mkiso_input_file_t *)b;

    return strcmp(fa->iso.iso_name, fb->iso.iso_name);
}

static int mkiso_load_file(const char *path, void **data_out, size_t *size_out)
{
    FILE *input;
    long size;
    void *data;

    input = fopen(path, "rb");
    if (!input)
        return -1;

    if (fseek(input, 0, SEEK_END) < 0) {
        fclose(input);
        return -1;
    }

    size = ftell(input);
    if (size < 0) {
        fclose(input);
        return -1;
    }

    if (fseek(input, 0, SEEK_SET) < 0) {
        fclose(input);
        return -1;
    }

    data = NULL;
    if (size > 0) {
        data = malloc((size_t)size);
        if (!data) {
            fclose(input);
            return -1;
        }

        if (fread(data, 1, (size_t)size, input) != (size_t)size) {
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

static int mkiso_add_file(const char *full_path, const char *entry_name,
                          mkiso_input_file_t **files_out, size_t *count_out,
                          size_t *capacity_out, char *error_buf,
                          size_t error_buf_size)
{
    mkiso_input_file_t *files = *files_out;
    size_t count = *count_out;
    size_t capacity = *capacity_out;
    void *data;
    size_t size;

    if (count == capacity) {
        size_t new_capacity = capacity ? capacity * 2 : 4;
        mkiso_input_file_t *new_files =
            realloc(files, new_capacity * sizeof(*files));
        if (!new_files) {
            snprintf(error_buf, error_buf_size, "out of memory");
            return -1;
        }

        files = new_files;
        capacity = new_capacity;
        *files_out = files;
        *capacity_out = capacity;
    }

    if (mkiso_load_file(full_path, &data, &size) < 0) {
        mkiso_set_path_error(error_buf, error_buf_size, "failed to read ",
                             full_path);
        return -1;
    }

    memset(&files[count], 0, sizeof(files[count]));
    iso_lite_normalize_file_name(entry_name, files[count].iso_name,
                                 sizeof(files[count].iso_name));
    files[count].iso.iso_name = files[count].iso_name;
    files[count].iso.data = data;
    files[count].iso.size = size;
    files[count].owned_data = data;

    *files_out = files;
    *count_out = count + 1;
    *capacity_out = capacity;
    return 0;
}

static int mkiso_collect_files(const char *source_dir, mkiso_input_file_t **files_out,
                               size_t *count_out, char *error_buf, size_t error_buf_size)
{
    mkiso_input_file_t *files = NULL;
    size_t count = 0;
    size_t capacity = 0;
#if defined(_WIN32)
    char pattern[PATH_MAX];
    WIN32_FIND_DATAA find_data;
    HANDLE handle;

    compat_path_join(pattern, sizeof(pattern), source_dir, "*");
    handle = FindFirstFileA(pattern, &find_data);
    if (handle == INVALID_HANDLE_VALUE) {
        mkiso_set_path_error(error_buf, error_buf_size, "failed to open ",
                             source_dir);
        return -1;
    }

    do {
        char full_path[PATH_MAX];

        if (find_data.cFileName[0] == '.')
            continue;

        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        compat_path_join(full_path, sizeof(full_path), source_dir,
                         find_data.cFileName);
        if (mkiso_add_file(full_path, find_data.cFileName, &files, &count,
                           &capacity, error_buf, error_buf_size) < 0) {
            FindClose(handle);
            mkiso_free_files(files, count);
            return -1;
        }
    } while (FindNextFileA(handle, &find_data));

    FindClose(handle);
#else
    DIR *dir;
    struct dirent *entry;

    dir = opendir(source_dir);
    if (!dir) {
        mkiso_set_path_error(error_buf, error_buf_size, "failed to open ",
                             source_dir);
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        char full_path[PATH_MAX];
        struct stat st;

        if (entry->d_name[0] == '.')
            continue;

        compat_path_join(full_path, sizeof(full_path), source_dir, entry->d_name);
        if (stat(full_path, &st) < 0)
            continue;

        if (!S_ISREG(st.st_mode))
            continue;

        if (mkiso_add_file(full_path, entry->d_name, &files, &count,
                           &capacity, error_buf, error_buf_size) < 0) {
            closedir(dir);
            mkiso_free_files(files, count);
            return -1;
        }
    }

    closedir(dir);
#endif

    if (!count) {
        free(files);
        mkiso_set_path_error(error_buf, error_buf_size,
                             "no regular files found in ", source_dir);
        return -1;
    }

    *files_out = files;
    *count_out = count;
    return 0;
}

int main(int argc, char **argv)
{
    const char *output_path = NULL;
    const char *source_dir = NULL;
    const char *volume_id = NULL;
    const char *system_area_path = NULL;
    const char *boot_path = NULL;
    char boot_name[ISO_LITE_FILE_NAME_SIZE];
    iso_lite_config_t config;
    iso_lite_stats_t stats;
    mkiso_input_file_t *files = NULL;
    size_t file_count = 0;
    size_t i;
    void *system_area = NULL;
    size_t system_area_size = 0;
    iso_lite_file_t *iso_files = NULL;
    char error_buf[256];
    bool quiet = false;
    uint32_t start_lba = 0;
    int rc = 1;

    memset(&config, 0, sizeof(config));
    memset(&stats, 0, sizeof(stats));
    memset(boot_name, 0, sizeof(boot_name));
    memset(error_buf, 0, sizeof(error_buf));

    for (i = 1; i < (size_t)argc; i++) {
        if (!strcmp(argv[i], "--output") && i + 1 < (size_t)argc)
            output_path = argv[++i];
        else if (!strcmp(argv[i], "--source-dir") && i + 1 < (size_t)argc)
            source_dir = argv[++i];
        else if (!strcmp(argv[i], "--volume-id") && i + 1 < (size_t)argc)
            volume_id = argv[++i];
        else if (!strcmp(argv[i], "--system-area") && i + 1 < (size_t)argc)
            system_area_path = argv[++i];
        else if (!strcmp(argv[i], "--eltorito-boot") && i + 1 < (size_t)argc)
            boot_path = argv[++i];
        else if (!strcmp(argv[i], "--start-lba") && i + 1 < (size_t)argc)
            start_lba = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--quiet"))
            quiet = true;
        else {
            mkiso_usage(argv[0]);
            return 1;
        }
    }

    if (!output_path || !source_dir) {
        mkiso_usage(argv[0]);
        return 1;
    }

    if (system_area_path &&
        mkiso_load_file(system_area_path, &system_area, &system_area_size) < 0) {
        fprintf(stderr, "mkiso-lite: failed to read %s\n", system_area_path);
        goto cleanup;
    }

    if (mkiso_collect_files(source_dir, &files, &file_count, error_buf,
                            sizeof(error_buf)) < 0) {
        fprintf(stderr, "mkiso-lite: %s\n", error_buf);
        goto cleanup;
    }

    if (boot_path)
        iso_lite_normalize_file_name(mkiso_path_basename(boot_path),
                                     boot_name, sizeof(boot_name));

    qsort(files, file_count, sizeof(*files), mkiso_compare_files);

    iso_files = calloc(file_count, sizeof(*iso_files));
    if (!iso_files) {
        fprintf(stderr, "mkiso-lite: out of memory\n");
        goto cleanup;
    }

    for (i = 0; i < file_count; i++) {
        iso_files[i] = files[i].iso;
    }

    config.volume_id = volume_id;
    config.system_area = (const uint8_t *)system_area;
    config.system_area_size = system_area_size;
    config.start_lba = start_lba;
    config.eltorito_boot_name = boot_path ? boot_name : NULL;

    if (iso_lite_write_image(output_path, &config, iso_files, file_count, &stats,
                             error_buf, sizeof(error_buf)) < 0) {
        fprintf(stderr, "mkiso-lite: %s\n", error_buf);
        goto cleanup;
    }

    if (!quiet) {
        fprintf(stdout, "ISO %s (%u sectors)\n", output_path, stats.total_sectors);
    }

    rc = 0;

cleanup:
    free(system_area);
    free(iso_files);

    if (files) {
        mkiso_free_files(files, file_count);
    }

    return rc;
}
