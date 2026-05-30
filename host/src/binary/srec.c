/* host/src/binary/srec.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <kostool/binary.h>

static int hex_byte(const char *s) {
    int hi, lo;
    if(s[0] >= '0' && s[0] <= '9')
        hi = s[0] - '0';
    else if(s[0] >= 'A' && s[0] <= 'F')
        hi = s[0] - 'A' + 10;
    else if(s[0] >= 'a' && s[0] <= 'f')
        hi = s[0] - 'a' + 10;
    else
        return -1;
    if(s[1] >= '0' && s[1] <= '9')
        lo = s[1] - '0';
    else if(s[1] >= 'A' && s[1] <= 'F')
        lo = s[1] - 'A' + 10;
    else if(s[1] >= 'a' && s[1] <= 'f')
        lo = s[1] - 'a' + 10;
    else
        return -1;
    return (hi << 4) | lo;
}

static int srec_probe(const char *filename) {
    FILE *f = fopen(filename, "r");
    if(!f)
        return 0;
    int ch = fgetc(f);
    fclose(f);
    return (ch == 'S');
}

/*
 * S-Record loader
 *
 * Supports S0 (header), S1 (16-bit addr), S2 (24-bit addr), S3 (32-bit addr),
 * S7 (32-bit entry), S8 (24-bit entry), S9 (16-bit entry).
 *
 * Strategy: two-pass. First pass finds the address range. Second pass
 * fills a contiguous buffer and delivers it as one section.
 */
static int srec_load(const char *filename, uint32_t *entry_addr, binary_section_cb callback,
                     void *user_data) {
    FILE *f = fopen(filename, "r");
    if(!f) {
        perror(filename);
        return -1;
    }

    /* Pass 1: determine address range */
    uint32_t addr_min = 0xFFFFFFFF, addr_max = 0;
    int has_entry = 0;
    char line[1024];
    int line_num = 0;

    while(fgets(line, sizeof(line), f)) {
        line_num++;
        if(line[0] != 'S')
            continue;
        int type = line[1] - '0';
        if(type < 1 || type > 3)
            continue; /* only data records */

        int byte_count = hex_byte(&line[2]);
        if(byte_count < 0) {
            fprintf(stderr, "srec: bad byte count at line %d\n", line_num);
            fclose(f);
            return -1;
        }

        int addr_bytes = type + 1;  /* S1=2, S2=3, S3=4 */
        uint32_t addr = 0;
        for(int i = 0; i < addr_bytes; i++) {
            int b = hex_byte(&line[4 + i * 2]);
            if(b < 0) {
                fclose(f);
                return -1;
            }
            addr = (addr << 8) | b;
        }

        int data_bytes = byte_count - addr_bytes - 1; /* -1 for checksum */
        if(data_bytes < 0)
            continue;

        if(addr < addr_min)
            addr_min = addr;
        if(addr + (uint32_t)data_bytes > addr_max)
            addr_max = addr + (uint32_t)data_bytes;
    }

    if(addr_min > addr_max) {
        fprintf(stderr, "srec: no data records found in %s\n", filename);
        fclose(f);
        return -1;
    }

    uint32_t total_size = addr_max - addr_min;
    uint8_t *data = calloc(1, total_size);
    if(!data) {
        fclose(f);
        return -1;
    }

    /* Pass 2: fill buffer and find entry point */
    rewind(f);
    line_num = 0;

    while(fgets(line, sizeof(line), f)) {
        line_num++;
        if(line[0] != 'S')
            continue;
        int type = line[1] - '0';

        if(type >= 1 && type <= 3) {
            /* Data record */
            int byte_count = hex_byte(&line[2]);
            int addr_bytes = type + 1;
            uint32_t addr = 0;
            for(int i = 0; i < addr_bytes; i++) {
                int b = hex_byte(&line[4 + i * 2]);
                addr = (addr << 8) | b;
            }

            int data_bytes = byte_count - addr_bytes - 1;
            int data_offset = 4 + addr_bytes * 2;

            /* Verify checksum */
            uint8_t sum = 0;
            for(int i = 0; i < byte_count; i++) {
                int b = hex_byte(&line[2 + i * 2]);
                if(b < 0) {
                    free(data);
                    fclose(f);
                    return -1;
                }
                sum += (uint8_t)b;
            }
            int cksum = hex_byte(&line[2 + byte_count * 2]);
            if(cksum < 0) {
                free(data);
                fclose(f);
                return -1;
            }
            sum += (uint8_t)cksum;
            if(sum != 0xFF) {
                fprintf(stderr, "srec: checksum error at line %d\n", line_num);
                free(data);
                fclose(f);
                return -1;
            }

            for(int i = 0; i < data_bytes; i++) {
                int b = hex_byte(&line[data_offset + i * 2]);
                if(b < 0) {
                    free(data);
                    fclose(f);
                    return -1;
                }
                data[addr - addr_min + i] = (uint8_t)b;
            }
        } else if(type >= 7 && type <= 9) {
            /* Entry point record: S9=16-bit, S8=24-bit, S7=32-bit */
            int      addr_bytes = 11 - type; /* S7=4, S8=3, S9=2 */
            uint32_t addr = 0;
            for(int i = 0; i < addr_bytes; i++) {
                int b = hex_byte(&line[4 + i * 2]);
                if(b < 0)
                    break;
                addr = (addr << 8) | b;
            }
            *entry_addr = addr;
            has_entry = 1;
        }
    }

    fclose(f);

    printf("File format is S-Record, ");
    if(has_entry)
        printf("entry point 0x%08x, ", *entry_addr);
    printf("address range 0x%08x-0x%08x (%u bytes)\n", addr_min, addr_max, total_size);

    binary_section_t section = {
        .name = ".srec",
        .load_addr = addr_min,
        .size = total_size,
        .data = data,
    };

    int ret = callback(&section, user_data);
    free(data);
    return ret;
}

const binary_ops_t srec_binary_ops = {
    .name = "S-Record",
    .probe = srec_probe,
    .load = srec_load,
};
