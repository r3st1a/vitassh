#include "render.h"

#include <psp2/gxm.h>
#include <vita2d.h>

#include <string.h>

#include "font8x8.h"
#include "log.h"

#define ATLAS_COLS 16
#define ATLAS_ROWS 8
#define ATLAS_W (ATLAS_COLS * 8)
#define ATLAS_H (ATLAS_ROWS * 8)

/* The ANSI palette, matching term.c's so a colour index means the same thing on
 * both sides. Written as 0xAABBGGRR. */
static const uint32_t palette[TERM_PALETTE_SIZE] = {
	0xFF000000, /* 0  black */
	0xFF0000AA, /* 1  red */
	0xFF00AA00, /* 2  green */
	0xFF0055AA, /* 3  yellow */
	0xFFAA0000, /* 4  blue */
	0xFFAA00AA, /* 5  magenta */
	0xFFAAAA00, /* 6  cyan */
	0xFFAAAAAA, /* 7  white */
	0xFF555555, /* 8  bright black */
	0xFF5555FF, /* 9  bright red */
	0xFF55FF55, /* 10 bright green */
	0xFF55FFFF, /* 11 bright yellow */
	0xFFFF5555, /* 12 bright blue */
	0xFFFF55FF, /* 13 bright magenta */
	0xFFFFFF55, /* 14 bright cyan */
	0xFFFFFFFF, /* 15 bright white */
};

#define DEFAULT_FG 0xFFC8C8C8u
#define DEFAULT_BG 0xFF101010u

static vita2d_texture *atlas;

uint32_t render_fg(int index)
{
	if (index < 0 || index >= TERM_PALETTE_SIZE) return DEFAULT_FG;
	return palette[index];
}

uint32_t render_bg(int index)
{
	if (index < 0 || index >= TERM_PALETTE_SIZE) return DEFAULT_BG;
	return palette[index];
}

int render_init(void)
{
	uint32_t *pixels;
	int i, x, y;

	if (vita2d_init() < 0) return -1;

	vita2d_set_clear_color(DEFAULT_BG);

	/* A8B8G8R8 with white RGB and the glyph in alpha: the tint argument of
	 * draw_texture_tint then supplies the actual colour. */
	atlas = vita2d_create_empty_texture_format(ATLAS_W, ATLAS_H,
	                                           SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
	if (atlas == NULL) {
		log_printf("render: could not create the glyph atlas");
		return -1;
	}
	vita2d_texture_set_filters(atlas, SCE_GXM_TEXTURE_FILTER_POINT,
	                           SCE_GXM_TEXTURE_FILTER_POINT);

	pixels = (uint32_t *)vita2d_texture_get_datap(atlas);

	for (i = 0; i < FONT_GLYPHS; i++) {
		int sx = (i % ATLAS_COLS) * 8;
		int sy = (i / ATLAS_COLS) * 8;

		for (y = 0; y < 8; y++) {
			/* font8x8 stores one byte per row with bit 0 leftmost. */
			uint8_t bits = font8x8[i][y];

			for (x = 0; x < 8; x++)
				pixels[(sy + y) * ATLAS_W + sx + x] =
					(bits & (1u << x)) ? 0xFFFFFFFFu : 0x00000000u;
		}
	}

	log_printf("render: ready (%dx%d atlas)", ATLAS_W, ATLAS_H);
	return 0;
}

void render_shutdown(void)
{
	if (atlas != NULL) {
		vita2d_free_texture(atlas);
		atlas = NULL;
	}
	vita2d_fini();
}

void render_begin(void)
{
	vita2d_start_drawing();
	vita2d_clear_screen();
}

void render_end(void)
{
	vita2d_end_drawing();
}

void render_dialog_update(void)
{
	vita2d_common_dialog_update();
	vita2d_swap_buffers();
}

void render_fill(int x, int y, int w, int h, uint32_t color)
{
	if (w <= 0 || h <= 0) return;
	vita2d_draw_rectangle((float)x, (float)y, (float)w, (float)h, color);
}

void render_frame(int x, int y, int w, int h, uint32_t color, int thickness)
{
	if (w <= 0 || h <= 0 || thickness <= 0) return;

	render_fill(x, y, w, thickness, color);
	render_fill(x, y + h - thickness, w, thickness, color);
	render_fill(x, y, thickness, h, color);
	render_fill(x + w - thickness, y, thickness, h, color);
}

static void draw_glyph(int x, int y, int scale, int ch, uint32_t color)
{
	int index, sx, sy;

	if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
	index = ch - FONT_FIRST;
	sx = (index % ATLAS_COLS) * 8;
	sy = (index / ATLAS_COLS) * 8;

	vita2d_draw_texture_tint_part_scale(atlas, (float)x, (float)y, (float)sx,
	                                    (float)sy, 8.0f, 8.0f, (float)scale,
	                                    (float)scale, color);
}

void render_text(int x, int y, int scale, uint32_t color, const char *text)
{
	if (text == NULL) return;

	for (; *text != '\0'; text++, x += 8 * scale) {
		if (*text == ' ') continue;
		draw_glyph(x, y, scale, (unsigned char)*text, color);
	}
}

int render_text_width(int scale, const char *text)
{
	return text == NULL ? 0 : (int)strlen(text) * 8 * scale;
}

void render_terminal(const Term *t, int offset, bool cursor_on, int y_offset)
{
	int row, col;

	for (row = 0; row < TERM_ROWS; row++) {
		const TermCell *line;
		int py = y_offset + row * CELL_H;

		/* The keyboard slides the grid up; rows above the screen would be
		 * clipped anyway, so do not submit them. */
		if (py < 0 || py >= SCREEN_H) continue;

		line = term_view_line(t, offset, row);
		if (line == NULL) continue;

		/* Backgrounds first, merging runs, so a coloured `clear` is one quad
		 * rather than sixty. The default background is already on screen. */
		col = 0;
		while (col < TERM_COLS) {
			uint32_t color = render_bg(line[col].bg);
			int start = col;

			while (col < TERM_COLS && render_bg(line[col].bg) == color) col++;
			if (color != DEFAULT_BG)
				render_fill(start * CELL_W, py, (col - start) * CELL_W, CELL_H, color);
		}

		for (col = 0; col < TERM_COLS; col++) {
			uint32_t ch = line[col].ch;
			uint32_t fg;

			if (ch == ' ') continue;

			fg = render_fg(line[col].fg);
			draw_glyph(col * CELL_W, py, 1, (int)ch, fg);

			if (line[col].attr & TERM_ATTR_UNDERLINE)
				render_fill(col * CELL_W, py + CELL_H - 2, CELL_W, 1, fg);
		}
	}

	if (!cursor_on || !t->cursor_visible || offset != 0) return;

	{
		int cx = t->cx * CELL_W;
		int cy = y_offset + t->cy * CELL_H;
		const TermCell *cell = &t->grid[t->cy][t->cx];

		/* A block cursor: fill with the foreground colour and put the glyph
		 * back in the background colour so it stays readable. */
		render_fill(cx, cy, CELL_W, CELL_H, render_fg(cell->fg));
		if (cell->ch != ' ')
			draw_glyph(cx, cy, 1, (int)cell->ch, render_bg(cell->bg));
	}
}
