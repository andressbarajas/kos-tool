/* client/playstation2/iop_handoff_marker.h */
#ifndef KOSLOAD_PS2_IOP_HANDOFF_MARKER_H
#define KOSLOAD_PS2_IOP_HANDOFF_MARKER_H

/*
 * Cross-loader handoff marker for firmware-update IOP-reset deduplication.
 *
 * An OLD loader doing a firmware update (-F) issues a full IOP reset before
 * jumping to the trampoline so that the incoming NEW loader sees the same
 * cold-boot IOP state.  The NEW loader's normal bring-up path then issues a
 * *second* SifIopReset, which is ~1.5 s of dead time the user sees as a long
 * black screen.  When the OLD loader writes this marker, the NEW loader's
 * dev9 bootstrap reads it, skips the redundant reset, and clears it.
 *
 * Placement: physical 0x001000F4, KSEG1 alias 0xA01000F4.  This is the last
 * 4 zero bytes of the firmware-update trampoline's tail padding, immediately
 * before the size patch at 0x001000F8 (host/src/firmware_update.c).  Three
 * properties make this address safe:
 *
 *   1. The trampoline's copy loop reads from 0x00100100 onwards, so it never
 *      touches the marker.
 *   2. The NEW loader's link region ends at physical 0x00100000 (see
 *      build/ip/kosload-inner.ld, LENGTH 0xFFD80), so no new-loader code,
 *      .bss, .data, or stack can land on 0x001000F4.
 *   3. Cold boot — and any non-firmware-update boot — leaves the trampoline
 *      bytes as plain zeros, so the marker check is self-protecting against
 *      false positives.
 *
 * Coherence: the OLD loader writes via KSEG1 AFTER its cache_flush_range
 * sweep, so no dirty D-cache line maps back over the marker.  The NEW loader
 * reads and clears via KSEG1 for the same reason.  KSEG1 bypasses D-cache
 * and goes directly to physical RAM.
 */
#define KOSLOAD_IOP_HANDOFF_MAGIC   0x4B53464Du     /* 'KSFM' */
#define KOSLOAD_IOP_HANDOFF_ADDR    0xA01000F4u

#endif /* KOSLOAD_PS2_IOP_HANDOFF_MARKER_H */
