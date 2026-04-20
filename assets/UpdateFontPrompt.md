Please import this edited GC font PNG back into client/gamecube/video.c.

Rules:
- Use the attached PNG as a 16x6 sprite sheet.
- Image size must remain 192x144.
- Each tile is 12x24 pixels.
- Tiles map to printable ASCII 32 through 126 in order.
- Ignore the final unused 96th tile.
- Treat white/light pixels as set bits and black/dark pixels as cleared bits.
- Update only the font_12x24 array unless a tiny helper script is useful.
- Preserve the current row encoding: uint16_t per row, top 12 bits used, MSB is leftmost pixel.
