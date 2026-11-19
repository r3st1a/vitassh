/*
 * Embedded 8x8 bitmap font.
 *
 * The data lives in the generated font8x8.c; see tools/make_font.js and the
 * public-domain notice there. Only ASCII is covered, which matches what the
 * SSH side of the client can display.
 */
#ifndef VITASSH_FONT8X8_H
#define VITASSH_FONT8X8_H

#include <stdint.h>

#define FONT_FIRST 0x20
#define FONT_LAST 0x7E
#define FONT_GLYPHS (FONT_LAST - FONT_FIRST + 1)
#define FONT_GLYPH_BYTES 8

/* One byte per row, top row first; bit 0 is the leftmost pixel. */
extern const uint8_t font8x8[FONT_GLYPHS][FONT_GLYPH_BYTES];

#endif /* VITASSH_FONT8X8_H */
