/* client/xbox/kosload_header.c
 *
 * Guest-facing kosload header for the Xbox loader.
 *
 * The linker pins .text.kosload_header first in .text so this table lands at
 * a fixed virtual address: XBOX_LOADER_BASE + HEADER_RESERVE (XBOX_KOSLOAD_BASE,
 * 0x00011000 by default).  Uploaded guest programs read it to discover the
 * loader and its syscall entry, mirroring the DC/GC/PS2 entry-point headers:
 *
 *   +0x00  magic            0xdeadbeef (loader present)
 *   +0x04  syscall entry    int kosloadsyscall(nr, a1, a2, a3)  (cdecl)
 *   +0x08  info block ptr   &kosload_info (kosload_info_t)
 *   +0x0C  setup_video      void setup_video(mode, bg_color)
 *   +0x10  clear_screen     void clear_screen(color)
 *   +0x14  draw_string      void draw_string(x, y, str, color)
 *   +0x18  uint_to_string   void uint_to_string(val, buf)
 *   +0x1C  exc_to_string    const char *exception_code_to_string(code)
 *
 * The Xbox loads XBEs at their fixed base without relocation, so the absolute
 * addresses stored here are valid at runtime.
 */

#include <stdint.h>
#include <kosload/info.h>

extern int kosloadsyscall(int nr, int a1, int a2, int a3);
extern kosload_info_t kosload_info;

extern void setup_video(uint32_t mode, uint32_t bg_color);
extern void clear_screen(uint32_t color);
extern void draw_string(int x, int y, const char *str, uint32_t color);
extern void uint_to_string(uint32_t val, unsigned char *buf);
extern const char *exception_code_to_string(uint32_t code);

__attribute__((used, section(".text.kosload_header")))
const uint32_t kosload_header[8] = {
    0xdeadbeef,
    (uint32_t)&kosloadsyscall,
    (uint32_t)&kosload_info,
    (uint32_t)&setup_video,
    (uint32_t)&clear_screen,
    (uint32_t)&draw_string,
    (uint32_t)&uint_to_string,
    (uint32_t)&exception_code_to_string,
};
