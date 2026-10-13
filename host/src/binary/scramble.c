/* host/src/binary/scramble.c - Dreamcast binary descrambling */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <kostool/binary.h>

#define SCRAMBLE_MAXCHUNK (2048 * 1024)
#define SCRAMBLE_SLICE    32

static uint32_t scramble_next(uint32_t *seed) {
    *seed = (*seed * 2109 + 9273) & 0x7fff;
    return (*seed + 0xc000) & 0xffff;
}

int dc_descramble(const uint8_t *in, uint8_t *out, size_t size) {
    uint32_t *idx = malloc((SCRAMBLE_MAXCHUNK / SCRAMBLE_SLICE) * sizeof(uint32_t));
    if(!idx)
        return -1;

    uint32_t seed = (uint32_t)size & 0xffff;
    size_t   remaining = size;
    size_t   src = 0;
    size_t   base = 0;

    for(size_t chunk = SCRAMBLE_MAXCHUNK; chunk >= SCRAMBLE_SLICE; chunk >>= 1) {
        while(remaining >= chunk) {
            uint32_t slices = (uint32_t)(chunk / SCRAMBLE_SLICE);

            for(uint32_t i = 0; i < slices; i++)
                idx[i] = i;

            for(int32_t i = (int32_t)slices - 1; i >= 0; i--) {
                uint32_t x = (scramble_next(&seed) * (uint32_t)i) >> 16;
                uint32_t tmp = idx[i];
                idx[i] = idx[x];
                idx[x] = tmp;

                memcpy(out + base + (size_t)idx[i] * SCRAMBLE_SLICE, in + src, SCRAMBLE_SLICE);
                src += SCRAMBLE_SLICE;
            }

            remaining -= chunk;
            base += chunk;
        }
    }

    if(remaining)
        memcpy(out + base, in + src, remaining);

    free(idx);
    return 0;
}
