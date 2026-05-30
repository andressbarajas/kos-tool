/* host/src/binary/bfd.c */
#include <stdio.h>
#include <stdint.h>

#include <kostool/binary.h>

/*
 * BFD (libbfd) binary parser — optional.
 * Only compiled when WITH_BFD is defined.
 */

#ifdef WITH_BFD
#include <bfd.h>

static int bfd_probe(const char *filename) {
    bfd *abfd = bfd_openr(filename, NULL);
    if(!abfd)
        return 0;
    int ok = bfd_check_format(abfd, bfd_object);
    bfd_close(abfd);
    return ok;
}

static int bfd_load(const char *filename, uint32_t *entry_addr, binary_section_cb callback, void *user_data) {
    (void)filename;
    (void)entry_addr;
    (void)callback;
    (void)user_data;
    fprintf(stderr, "BFD loader not yet implemented\n");
    return -1;
}

const binary_ops_t bfd_binary_ops = {
    .name = "BFD",
    .probe = bfd_probe,
    .load = bfd_load,
};

#else

static int bfd_probe_stub(const char *filename) {
    (void)filename;
    return 0;
}

static int bfd_load_stub(const char *filename, uint32_t *entry_addr, binary_section_cb callback,
                         void *user_data) {
    (void)filename;
    (void)entry_addr;
    (void)callback;
    (void)user_data;
    return -1;
}

const binary_ops_t bfd_binary_ops = {
    .name = "BFD",
    .probe = bfd_probe_stub,
    .load = bfd_load_stub,
};

#endif /* WITH_BFD */
