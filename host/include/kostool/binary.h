/* host/include/kostool/binary.h */
#ifndef KOSTOOL_BINARY_H
#define KOSTOOL_BINARY_H

#include <stdint.h>
#include <stddef.h>

/* Represents a single loadable section */
typedef struct {
    const char    *name;
    uint32_t       load_addr;
    uint32_t       size;
    const uint8_t *data;
} binary_section_t;

/* Callback invoked for each loadable section */
typedef int (*binary_section_cb)(const binary_section_t *section, void *user_data);

/* Binary format parser interface */
typedef struct binary_ops {
    const char *name;
    int (*probe)(const char *filename);
    int (*load)(const char *filename, uint32_t *entry_addr, binary_section_cb callback, void *user_data);
} binary_ops_t;

/* Format implementations */
extern const binary_ops_t elf_binary_ops;
extern const binary_ops_t bfd_binary_ops;
extern const binary_ops_t srec_binary_ops;
extern const binary_ops_t dol_binary_ops;
extern const binary_ops_t raw_binary_ops;

/* Auto-detect format and load: tries ELF, SREC, DOL, then raw */
int binary_auto_load(const char *filename, uint32_t default_addr, uint32_t *entry_addr, binary_section_cb cb,
                     void *ud);

#endif /* KOSTOOL_BINARY_H */
