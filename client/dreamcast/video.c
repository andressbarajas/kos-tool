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

#include "hardware.h"
#include "video.h"

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
 * bg_color is the background color for dc_video_clear.
 */
void setup_video(uint32_t mode, uint32_t bg_color) {
    (void)mode;
    /* Only retail Dreamcasts have the cable-detect line; NAOMI / System SP
       and any other hardware type default to VGA (0). */
    int cabletype = (dc_hardware_type() == DC_HW_TYPE_RETAIL) ? dc_video_check_cable() : 0;
    dc_video_init(cabletype, 1);
    dc_video_clear(bg_color);
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
