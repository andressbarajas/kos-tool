#ifndef KOSLOAD_STRUTIL_H
#define KOSLOAD_STRUTIL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline size_t compat_str_copy(char *dst, size_t dst_size, const char *src) {
    size_t len = 0;

    if(!dst || dst_size == 0)
        return 0;

    if(!src)
        src = "";

    while(len + 1 < dst_size && src[len] != '\0') {
        dst[len] = src[len];
        len++;
    }

    dst[len] = '\0';
    return len;
}

static inline size_t compat_str_copy_bytes(char *dst, size_t dst_size, const uint8_t *src, size_t src_size) {
    size_t len = 0;

    if(!dst || dst_size == 0)
        return 0;

    if(!src || src_size == 0) {
        dst[0] = '\0';
        return 0;
    }

    while(len + 1 < dst_size && len < src_size && src[len] != '\0') {
        dst[len] = (char)src[len];
        len++;
    }

    dst[len] = '\0';
    return len;
}

static inline size_t compat_str_append_bytes(char *dst, size_t dst_size, size_t pos, const void *src,
                                             size_t src_size) {
    const uint8_t *bytes = (const uint8_t *)src;
    size_t len = pos;

    if(!dst || dst_size == 0)
        return 0;

    if(len >= dst_size)
        len = dst_size - 1;

    if(!bytes || src_size == 0) {
        dst[len] = '\0';
        return len;
    }

    while(len + 1 < dst_size && src_size > 0) {
        dst[len++] = (char)*bytes++;
        src_size--;
    }

    dst[len] = '\0';
    return len;
}

static inline size_t compat_str_append(char *dst, size_t dst_size, size_t pos, const char *src) {
    if(!src)
        src = "";

    return compat_str_append_bytes(dst, dst_size, pos, src, strlen(src));
}

static inline void compat_path_join(char *dst, size_t dst_size, const char *dir, const char *name) {
    size_t len;

    if(!dst || dst_size == 0)
        return;

    len = compat_str_copy(dst, dst_size, dir);
    if(len > 0 && dst[len - 1] != '/' && dst[len - 1] != '\\' && len + 1 < dst_size) {
#if defined(_WIN32)
        dst[len++] = '\\';
#else
        dst[len++] = '/';
#endif
        dst[len] = '\0';
    }

    if(len < dst_size)
        compat_str_copy(dst + len, dst_size - len, name);
}

#endif /* KOSLOAD_STRUTIL_H */
