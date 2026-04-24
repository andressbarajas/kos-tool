/* host/src/binary/elf.c */
/*
 * ELF binary loader for kostool.
 *
 * Based on dcload-serial: dcload-serial/host-src/tool/dc-tool.c
 * (libelf ELF loading path, lines 944-1018)
 *
 * Adapted to use the binary_ops callback interface instead of
 * sending data directly over serial.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <libelf.h>

#include <kosload/file_compat.h>
#include <kostool/binary.h>

static int elf_probe(const char *filename) {
    int fd = open(filename, O_RDONLY | O_BINARY);
    if (fd < 0) return 0;

    unsigned char magic[4];
    int n = read(fd, magic, 4);
    close(fd);

    if (n != 4) return 0;
    return (magic[0] == 0x7f && magic[1] == 'E' &&
            magic[2] == 'L' && magic[3] == 'F');
}

static int elf_load(const char *filename, uint32_t *entry_addr,
                    binary_section_cb callback, void *user_data) {
    if (elf_version(EV_CURRENT) == EV_NONE) {
        fprintf(stderr, "ELF library initialization error: %s\n", elf_errmsg(-1));
        return -1;
    }

    int fd = open(filename, O_RDONLY | O_BINARY);
    if (fd < 0) {
        perror(filename);
        return -1;
    }

    Elf *elf = elf_begin(fd, ELF_C_READ, NULL);
    if (!elf) {
        fprintf(stderr, "Unable to open ELF file: %s\n", elf_errmsg(-1));
        close(fd);
        return -1;
    }

    if (elf_kind(elf) != ELF_K_ELF) {
        elf_end(elf);
        close(fd);
        return -1;
    }

    Elf32_Ehdr *ehdr = elf32_getehdr(elf);
    if (!ehdr) {
        fprintf(stderr, "Unable to read ELF header: %s\n", elf_errmsg(-1));
        elf_end(elf);
        close(fd);
        return -1;
    }

    *entry_addr = ehdr->e_entry;
    printf("File format is ELF, start address is 0x%x\n", *entry_addr);

    size_t shstrndx;
    if (elf_getshdrstrndx(elf, &shstrndx)) {
        fprintf(stderr, "Unable to read section index: %s\n", elf_errmsg(-1));
        elf_end(elf);
        close(fd);
        return -1;
    }

    Elf_Scn *scn = NULL;
    while ((scn = elf_nextscn(elf, scn))) {
        Elf32_Shdr *shdr = elf32_getshdr(scn);
        if (!shdr) {
            fprintf(stderr, "Unable to read section header: %s\n", elf_errmsg(-1));
            elf_end(elf);
            close(fd);
            return -1;
        }

        char *section_name = elf_strptr(elf, shstrndx, shdr->sh_name);
        if (!section_name) {
            fprintf(stderr, "Unable to read section name: %s\n", elf_errmsg(-1));
            elf_end(elf);
            close(fd);
            return -1;
        }

        if (!shdr->sh_addr)
            continue;

        Elf_Data *data = elf_getdata(scn, NULL);
        if (!data || !data->d_buf || !data->d_size)
            continue;

        binary_section_t section = {
            .name = section_name,
            .load_addr = shdr->sh_addr,
            .size = (data->d_size < shdr->sh_size) ? data->d_size : shdr->sh_size,
            .data = data->d_buf,
        };

        if (callback(&section, user_data) != 0) {
            elf_end(elf);
            close(fd);
            return -1;
        }
    }

    elf_end(elf);
    close(fd);
    return 0;
}

const binary_ops_t elf_binary_ops = {
    .name = "ELF",
    .probe = elf_probe,
    .load = elf_load,
};
