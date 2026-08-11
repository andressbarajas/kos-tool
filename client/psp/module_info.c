/* client/psp/module_info.c
 *
 * Hand-rolled PSP module descriptor (SceModuleInfo).
 *
 * Defined clean-room from the public PSP module-format documentation.  The
 * loader imports NO firmware libraries (it drives hardware directly and reuses
 * the kosload serial byte protocol over USB), so both the export table and the
 * import ("stub") table are empty — ent_top == ent_end and stub_top == stub_end.
 * That keeps this descriptor free of any firmware NIDs.
 *
 * KERNEL attribute (0x1000): the loader drives the DMACPlus display + USB
 * controller MMIO directly (0xBCxxxxxx/0xBDxxxxxx), which is only accessible in
 * kernel mode — consistent with the bare-metal / replace-the-kernel goal (no
 * user-mode firmware syscalls).
 *
 * The link output is turned into a relocatable PRX by host/tools/elf2prx before
 * host/tools/pbp wraps it as DATA.PSP; the firmware locates this descriptor via
 * the program header's p_paddr, which elf2prx points at this section.
 */

#include <stdint.h>

extern unsigned int _gp;

#define PSP_MODULE_ATTR_KERNEL 0x1000

/* Exactly 52 (0x34) bytes, no padding.  `attribute` is 16-bit: making it a
 * uint32_t shifts every following field by two bytes (and the compiler then
 * pads before gp_value to keep it 4-aligned), which the firmware loader reads
 * as an empty module name, gp == 0, and export/import ranges whose top is
 * greater than their end.  Verified against the descriptors in shipping PSP
 * modules: attribute @0x00, version @0x02, name @0x04, then the five pointer
 * words at 0x20/0x24/0x28/0x2c/0x30. */
typedef struct {
    uint16_t attribute;   /* 0 = user, 0x1000 = kernel */
    uint8_t  version[2];  /* { minor, major } */
    char     name[28];    /* module name (NUL-padded) */
    uint32_t gp_value;    /* MIPS $gp */
    void    *ent_top;     /* export table start */
    void    *ent_end;     /* export table end */
    void    *stub_top;    /* import table start */
    void    *stub_end;    /* import table end */
} SceModuleInfo;

_Static_assert(sizeof(SceModuleInfo) == 0x34, "SceModuleInfo must be 52 bytes");
_Static_assert(__builtin_offsetof(SceModuleInfo, name) == 0x04, "name @0x04");
_Static_assert(__builtin_offsetof(SceModuleInfo, gp_value) == 0x20, "gp @0x20");
_Static_assert(__builtin_offsetof(SceModuleInfo, stub_end) == 0x30, "stub_end @0x30");

/* Empty export/import tables: a single zero word each so top == end points at
 * a valid (empty) range rather than NULL. */
static const uint32_t kosload_ent_end[1] __attribute__((used)) = {0};
static const uint32_t kosload_stub_end[1] __attribute__((used)) = {0};

__attribute__((used, section(".rodata.sceModuleInfo")))
const SceModuleInfo module_info = {
    .attribute = PSP_MODULE_ATTR_KERNEL,
    .version = {0, 1},
    .name = "psp-load-usb",
    .gp_value = (uint32_t)&_gp,
    .ent_top = (void *)kosload_ent_end,
    .ent_end = (void *)kosload_ent_end,
    .stub_top = (void *)kosload_stub_end,
    .stub_end = (void *)kosload_stub_end,
};
