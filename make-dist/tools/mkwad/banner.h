/* make-cd/tools/mkwad/banner.h */
/* Generates a Wii channel banner content (content index 0): IMET header + outer
 * U8 archive (meta/banner.bin + meta/icon.bin), each an IMD5 + LZ77 wrapper
 * around an inner U8 of arc/blyt (brlyt), arc/anim (brlan), arc/timg (tpl).
 * See AGENT/wii-wad-re.md for the reverse-engineered format. */
#ifndef KOSTOOL_WII_BANNER_H
#define KOSTOOL_WII_BANNER_H

#include <stddef.h>
#include <stdint.h>

/* Returns a malloc'd banner content of *out_len bytes (caller frees), or NULL.
 * If sound_bin is non-NULL it is a prebuilt meta/sound.bin (IMD5 + BNS) added
 * as the 3rd banner file; otherwise the banner ships icon + banner only. */
uint8_t *wii_build_banner(const char *title, const uint8_t *sound_bin, size_t sound_bin_len, size_t *out_len);

#endif /* KOSTOOL_WII_BANNER_H */
