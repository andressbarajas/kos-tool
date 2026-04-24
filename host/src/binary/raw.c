/* host/src/binary/raw.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <kosload/file_compat.h>
#include <kostool/binary.h>

static int raw_probe(const char *filename) {
    /* Raw binary is the fallback — always matches */
    struct stat st;
    if (stat(filename, &st) != 0) return 0;
    return (st.st_size > 0);
}

static int raw_load(const char *filename, uint32_t *entry_addr,
                    binary_section_cb callback, void *user_data) {
    int fd = open(filename, O_RDONLY | O_BINARY);
    if (fd < 0) {
        perror(filename);
        return -1;
    }

    off_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    uint8_t *data = malloc(size);
    if (!data) {
        close(fd);
        return -1;
    }

    if (read(fd, data, size) != size) {
        free(data);
        close(fd);
        return -1;
    }
    close(fd);

    printf("File format is raw binary, start address is 0x%08x\n", *entry_addr);

    binary_section_t section = {
        .name = ".raw",
        .load_addr = *entry_addr,
        .size = (uint32_t)size,
        .data = data,
    };

    int ret = callback(&section, user_data);
    free(data);
    return ret;
}

const binary_ops_t raw_binary_ops = {
    .name = "raw",
    .probe = raw_probe,
    .load = raw_load,
};

/* Auto-detect format and load */
int binary_auto_load(const char *filename, uint32_t default_addr,
                     uint32_t *entry_addr, binary_section_cb cb, void *ud) {
    *entry_addr = default_addr;

    static const binary_ops_t *formats[] = {
        &elf_binary_ops,
        &srec_binary_ops,
        &dol_binary_ops,
        &raw_binary_ops,
    };

    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        if (formats[i]->probe(filename)) {
            return formats[i]->load(filename, entry_addr, cb, ud);
        }
    }

    fprintf(stderr, "Unable to detect format for %s\n", filename);
    return -1;
}
