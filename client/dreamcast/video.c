/* client/dreamcast/video.c */
/*
 * Video startup support for Dreamcast.
 *
 * Based on dcload-ip: dcload-ip/target-src/dcload/startup_support.c
 * and dcload-ip: dcload-ip/target-src/dcload/dcload.c
 *
 * C helper functions used by crt0.S and the exception handler.
 */

#include <stdint.h>

/* Assembly-level video functions (from video.S).
 * SH-ELF compiler prepends _ to C symbols, so these match
 * the _init_video, _clrscr, etc. labels in video.S. */
extern void init_video(int cabletype, int pixelmode);
extern void clrscr(int color);
extern void draw_string(int x, int y, const char *str, int color);
extern int  check_cable(void);

/* FPSCR setup wrapper for crt0.S */
#if __GNUC__ <= 4
extern void __set_fpscr(unsigned int value);
void __call_builtin_sh_set_fpscr(unsigned int value) {
    __set_fpscr(value);
}
#else
void __call_builtin_sh_set_fpscr(unsigned int value) {
    __builtin_sh_set_fpscr(value);
}
#endif

/*
 * setup_video: Called by exception handler and main startup.
 * mode parameter is the pixel mode (0=RGB555, 1=RGB565, 3=RGB888).
 * bg_color is the background color for clrscr.
 */
void setup_video(uint32_t mode, uint32_t bg_color) {
    (void)mode;
#ifdef FORCE_VGA
    init_video(0, 1); /* Naomi/System SP: skip cable detect, force VGA */
#else
    init_video(check_cable(), 1);
#endif
    clrscr(bg_color);
}

/* clear_lines: Clear n lines starting at line y to value c.
 * Uses 16-bit writes matching dcload-ip implementation. */
void clear_lines(unsigned int y, unsigned int n, unsigned int c) {
    unsigned short *vmem = (unsigned short *)(0xa5000000 + y * 640 * 2);
    n = n * 640;
    while(n-- > 0)
        *vmem++ = c;
}

/* Exception code to string — stub, formatting moved to host (kos-tool) */
char *exception_code_to_string(unsigned int expevt) {
    (void)expevt;
    return "";
}

/* uint_to_string: hex conversion, used by exception.S via address table */
void uint_to_string(unsigned int value, unsigned char *buf) {
    char hexdigit[17] = "0123456789abcdef";
    for(int i = 7; i >= 0; i--) {
        buf[i] = hexdigit[value & 0x0f];
        value >>= 4;
    }
    buf[8] = 0;
}
