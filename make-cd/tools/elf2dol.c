/* make-cd/tools/elf2dol.c */
/*
 * elf2dol - Convert ELF executable to GameCube/Wii DOL format
 *
 * The DOL format is the GameCube's native executable format.
 * It consists of a 256-byte header followed by raw section data.
 *
 * DOL header layout (all big-endian):
 *   0x000-0x01B: 7 text section file offsets
 *   0x01C-0x047: 11 data section file offsets
 *   0x048-0x063: 7 text section load addresses
 *   0x064-0x08F: 11 data section load addresses
 *   0x090-0x0AB: 7 text section sizes
 *   0x0AC-0x0D7: 11 data section sizes
 *   0x0D8-0x0DB: BSS load address
 *   0x0DC-0x0DF: BSS size
 *   0x0E0-0x0E3: Entry point
 *   0x0E4-0x0FF: Padding (zeros)
 *
 * Sections are loaded in order: text sections first, then data.
 * Text = executable (PF_X set), Data = non-executable.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Minimal ELF definitions (avoid dependency on system elf.h) */

#define EI_NIDENT   16
#define ELFMAG0     0x7f
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'
#define ELFCLASS32  1
#define ELFDATA2MSB 2
#define EM_PPC      20
#define PT_LOAD     1
#define PF_X        1

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

/* DOL constants */
#define DOL_HEADER_SIZE     256
#define DOL_MAX_TEXT        7
#define DOL_MAX_DATA        11
#define DOL_ALIGN           32

/* DOL header (256 bytes, all fields big-endian) */
typedef struct {
    uint32_t text_offset[DOL_MAX_TEXT];
    uint32_t data_offset[DOL_MAX_DATA];
    uint32_t text_address[DOL_MAX_TEXT];
    uint32_t data_address[DOL_MAX_DATA];
    uint32_t text_size[DOL_MAX_TEXT];
    uint32_t data_size[DOL_MAX_DATA];
    uint32_t bss_address;
    uint32_t bss_size;
    uint32_t entry_point;
    uint8_t  padding[28];
} DOL_Header;

/* Big-endian read helpers */
static uint16_t read16be(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t read32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* Big-endian write helper */
static void write32be(uint8_t *p, uint32_t val)
{
    p[0] = (val >> 24) & 0xff;
    p[1] = (val >> 16) & 0xff;
    p[2] = (val >> 8) & 0xff;
    p[3] = val & 0xff;
}

/* Align value up to DOL_ALIGN boundary */
static uint32_t align_up(uint32_t val, uint32_t align)
{
    return (val + align - 1) & ~(align - 1);
}

int main(int argc, char *argv[])
{
    FILE *fin, *fout;
    uint8_t *elf_data;
    long elf_size;
    Elf32_Ehdr ehdr;
    Elf32_Phdr *phdrs;
    uint8_t dol_header[DOL_HEADER_SIZE];
    int num_text = 0, num_data = 0;
    uint32_t file_offset;
    int i;
    uint32_t bss_addr = 0, bss_end = 0;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input.elf> <output.dol>\n", argv[0]);
        return 1;
    }

    /* Read entire ELF file */
    fin = fopen(argv[1], "rb");
    if (!fin) {
        fprintf(stderr, "Error: Cannot open '%s'\n", argv[1]);
        return 1;
    }
    fseek(fin, 0, SEEK_END);
    elf_size = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    elf_data = malloc(elf_size);
    if (!elf_data) {
        fprintf(stderr, "Error: Out of memory\n");
        fclose(fin);
        return 1;
    }
    if (fread(elf_data, 1, elf_size, fin) != (size_t)elf_size) {
        fprintf(stderr, "Error: Failed to read ELF file\n");
        free(elf_data);
        fclose(fin);
        return 1;
    }
    fclose(fin);

    /* Validate ELF header */
    if (elf_size < (long)sizeof(Elf32_Ehdr) ||
        elf_data[0] != ELFMAG0 || elf_data[1] != ELFMAG1 ||
        elf_data[2] != ELFMAG2 || elf_data[3] != ELFMAG3) {
        fprintf(stderr, "Error: Not a valid ELF file\n");
        free(elf_data);
        return 1;
    }

    if (elf_data[4] != ELFCLASS32 || elf_data[5] != ELFDATA2MSB) {
        fprintf(stderr, "Error: Not a 32-bit big-endian ELF\n");
        free(elf_data);
        return 1;
    }

    /* Parse ELF header (big-endian) */
    ehdr.e_machine = read16be(elf_data + 18);
    ehdr.e_entry = read32be(elf_data + 24);
    ehdr.e_phoff = read32be(elf_data + 28);
    ehdr.e_phentsize = read16be(elf_data + 42);
    ehdr.e_phnum = read16be(elf_data + 44);

    if (ehdr.e_machine != EM_PPC) {
        fprintf(stderr, "Warning: ELF machine type is not PowerPC (%d)\n",
                ehdr.e_machine);
    }

    /* Parse program headers */
    phdrs = calloc(ehdr.e_phnum, sizeof(Elf32_Phdr));
    if (!phdrs) {
        fprintf(stderr, "Error: Out of memory\n");
        free(elf_data);
        return 1;
    }

    for (i = 0; i < ehdr.e_phnum; i++) {
        uint8_t *p = elf_data + ehdr.e_phoff + i * ehdr.e_phentsize;
        phdrs[i].p_type = read32be(p + 0);
        phdrs[i].p_offset = read32be(p + 4);
        phdrs[i].p_vaddr = read32be(p + 8);
        phdrs[i].p_paddr = read32be(p + 12);
        phdrs[i].p_filesz = read32be(p + 16);
        phdrs[i].p_memsz = read32be(p + 20);
        phdrs[i].p_flags = read32be(p + 24);
        phdrs[i].p_align = read32be(p + 28);
    }

    /* Initialize DOL header */
    memset(dol_header, 0, DOL_HEADER_SIZE);

    /* First pass: classify segments and compute BSS */
    for (i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD)
            continue;

        if (phdrs[i].p_filesz > 0) {
            if (phdrs[i].p_flags & PF_X) {
                if (num_text >= DOL_MAX_TEXT) {
                    fprintf(stderr, "Error: Too many text sections "
                                    "(max %d)\n", DOL_MAX_TEXT);
                    free(phdrs);
                    free(elf_data);
                    return 1;
                }
                num_text++;
            } else {
                if (num_data >= DOL_MAX_DATA) {
                    fprintf(stderr, "Error: Too many data sections "
                                    "(max %d)\n", DOL_MAX_DATA);
                    free(phdrs);
                    free(elf_data);
                    return 1;
                }
                num_data++;
            }
        }

        /* Track BSS (memsz > filesz portion) */
        if (phdrs[i].p_memsz > phdrs[i].p_filesz) {
            uint32_t seg_bss_addr = phdrs[i].p_vaddr + phdrs[i].p_filesz;
            uint32_t seg_bss_end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
            if (bss_addr == 0 || seg_bss_addr < bss_addr)
                bss_addr = seg_bss_addr;
            if (seg_bss_end > bss_end)
                bss_end = seg_bss_end;
        }
    }

    /* Second pass: fill DOL header and write sections */
    fout = fopen(argv[2], "wb");
    if (!fout) {
        fprintf(stderr, "Error: Cannot create '%s'\n", argv[2]);
        free(phdrs);
        free(elf_data);
        return 1;
    }

    /* Reserve space for header */
    file_offset = DOL_HEADER_SIZE;
    num_text = 0;
    num_data = 0;

    /* Write header placeholder (will be overwritten at the end) */
    fwrite(dol_header, 1, DOL_HEADER_SIZE, fout);

    /* Write text sections first, then data sections */
    for (i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_filesz == 0)
            continue;
        if (!(phdrs[i].p_flags & PF_X))
            continue;

        /* Align file offset */
        file_offset = align_up(file_offset, DOL_ALIGN);

        /* Pad to alignment */
        while ((uint32_t)ftell(fout) < file_offset) {
            uint8_t zero = 0;
            fwrite(&zero, 1, 1, fout);
        }

        /* Write section data */
        fwrite(elf_data + phdrs[i].p_offset, 1, phdrs[i].p_filesz, fout);

        /* Fill DOL header fields */
        write32be(dol_header + 0x00 + num_text * 4, file_offset);
        write32be(dol_header + 0x48 + num_text * 4, phdrs[i].p_vaddr);
        write32be(dol_header + 0x90 + num_text * 4, phdrs[i].p_filesz);

        file_offset += phdrs[i].p_filesz;
        num_text++;
    }

    /* Data sections */
    for (i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_filesz == 0)
            continue;
        if (phdrs[i].p_flags & PF_X)
            continue;

        /* Align file offset */
        file_offset = align_up(file_offset, DOL_ALIGN);

        /* Pad to alignment */
        while ((uint32_t)ftell(fout) < file_offset) {
            uint8_t zero = 0;
            fwrite(&zero, 1, 1, fout);
        }

        /* Write section data */
        fwrite(elf_data + phdrs[i].p_offset, 1, phdrs[i].p_filesz, fout);

        /* Fill DOL header fields */
        write32be(dol_header + 0x1C + num_data * 4, file_offset);
        write32be(dol_header + 0x64 + num_data * 4, phdrs[i].p_vaddr);
        write32be(dol_header + 0xAC + num_data * 4, phdrs[i].p_filesz);

        file_offset += phdrs[i].p_filesz;
        num_data++;
    }

    /* Fill BSS and entry point */
    if (bss_end > bss_addr) {
        write32be(dol_header + 0xD8, bss_addr);
        write32be(dol_header + 0xDC, bss_end - bss_addr);
    }
    write32be(dol_header + 0xE0, ehdr.e_entry);

    /* Rewrite header with correct values */
    fseek(fout, 0, SEEK_SET);
    fwrite(dol_header, 1, DOL_HEADER_SIZE, fout);

    fclose(fout);
    free(phdrs);
    free(elf_data);

    printf("Created %s: %d text + %d data sections, entry=0x%08x\n",
           argv[2], num_text, num_data, ehdr.e_entry);
    if (bss_end > bss_addr) {
        printf("  BSS: 0x%08x - 0x%08x (%u bytes)\n",
               bss_addr, bss_end, bss_end - bss_addr);
    }

    return 0;
}
