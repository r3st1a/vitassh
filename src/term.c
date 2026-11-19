#include "term.h"

#include <string.h>

/*
 * The 16 ANSI colours, used to snap SGR 38/48 extended-colour requests back
 * into the palette an 8x8 bitmap font can actually show.
 */
static const uint8_t palette_rgb[TERM_PALETTE_SIZE][3] = {
	{0, 0, 0},       {170, 0, 0},     {0, 170, 0},     {170, 85, 0},
	{0, 0, 170},     {170, 0, 170},   {0, 170, 170},   {170, 170, 170},
	{85, 85, 85},    {255, 85, 85},   {85, 255, 85},   {255, 255, 85},
	{85, 85, 255},   {255, 85, 255},  {85, 255, 255},  {255, 255, 255},
};

static int clampi(int v, int lo, int hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

int term_nearest_color(int r, int g, int b)
{
	int best = 0;
	long best_dist = -1;
	int i;

	for (i = 0; i < TERM_PALETTE_SIZE; i++) {
		long dr = r - palette_rgb[i][0];
		long dg = g - palette_rgb[i][1];
		long db = b - palette_rgb[i][2];
		long dist = dr * dr + dg * dg + db * db;
		if (best_dist < 0 || dist < best_dist) {
			best_dist = dist;
			best = i;
		}
	}
	return best;
}

/*
 * Collapses the current rendition into the pair of colours a cell stores.
 * Reverse video swaps the pair; bold and dim move the foreground between the
 * normal and bright halves of the palette, which is the usual stand-in when
 * there is only one font weight.
 */
static void effective_colors(const Term *t, int8_t *fg, int8_t *bg)
{
	int8_t f = t->cur_fg;
	int8_t b = t->cur_bg;

	if (t->cur_attr & TERM_ATTR_REVERSE) {
		int8_t tmp = f;
		f = b;
		b = tmp;
	}
	if (t->cur_attr & TERM_ATTR_BOLD) {
		if (f >= 0 && f < 8) f = (int8_t)(f + 8);
	}
	if (t->cur_attr & TERM_ATTR_DIM) {
		if (f >= 8) f = (int8_t)(f - 8);
	}
	if (t->cur_attr & TERM_ATTR_INVISIBLE) f = b;

	*fg = f;
	*bg = b;
}

/* An erased cell keeps the current background, which is what makes a coloured
 * `clear` work. */
static void cell_blank(const Term *t, TermCell *c)
{
	int8_t fg, bg;

	effective_colors(t, &fg, &bg);
	c->ch = ' ';
	c->fg = TERM_COLOR_DEFAULT;
	c->bg = bg;
	c->attr = 0;
}

static void blank_row(const Term *t, TermCell *row)
{
	TermCell c;
	int x;

	cell_blank(t, &c);
	for (x = 0; x < TERM_COLS; x++) row[x] = c;
}

static void erase_cells(Term *t, int row, int from, int to)
{
	TermCell c;
	int x;

	if (row < 0 || row >= TERM_ROWS) return;
	if (from < 0) from = 0;
	if (to > TERM_COLS - 1) to = TERM_COLS - 1;

	cell_blank(t, &c);
	for (x = from; x <= to; x++) t->grid[row][x] = c;
}

/* --- scrollback ring ---------------------------------------------------- */

static void sb_push(Term *t, const TermCell *row)
{
	int index;

	if (t->sb_count < TERM_SB_LINES) {
		index = (t->sb_head + t->sb_count) % TERM_SB_LINES;
		t->sb_count++;
	} else {
		index = t->sb_head;
		t->sb_head = (t->sb_head + 1) % TERM_SB_LINES;
	}
	memcpy(t->sb[index], row, TERM_COLS * sizeof(TermCell));
}

int term_sb_count(const Term *t)
{
	return t->sb_count;
}

const TermCell *term_sb_line(const Term *t, int index)
{
	if (index < 0 || index >= t->sb_count) return NULL;
	return t->sb[(t->sb_head + index) % TERM_SB_LINES];
}

void term_sb_clear(Term *t)
{
	t->sb_head = 0;
	t->sb_count = 0;
}

/* --- lifecycle ---------------------------------------------------------- */

void term_reset(Term *t)
{
	static const TermCell blank = {' ', TERM_COLOR_DEFAULT, TERM_COLOR_DEFAULT, 0};
	int y, x;

	term_sb_clear(t);

	for (y = 0; y < TERM_ROWS; y++)
		for (x = 0; x < TERM_COLS; x++) t->grid[y][x] = blank;

	t->cx = 0;
	t->cy = 0;
	t->top = 0;
	t->bot = TERM_ROWS - 1;

	t->wrap_pending = false;
	t->autowrap = true;
	t->cursor_visible = true;
	t->alt_active = false;
	t->insert_mode = false;

	for (x = 0; x < TERM_COLS; x++) t->tab_stop[x] = (x % TERM_TAB_INTERVAL) == 0;

	t->cur_fg = TERM_COLOR_DEFAULT;
	t->cur_bg = TERM_COLOR_DEFAULT;
	t->cur_attr = 0;

	t->saved_cx = 0;
	t->saved_cy = 0;
	t->saved_fg = TERM_COLOR_DEFAULT;
	t->saved_bg = TERM_COLOR_DEFAULT;
	t->saved_attr = 0;

	t->alt_cx = 0;
	t->alt_cy = 0;
	t->alt_fg = TERM_COLOR_DEFAULT;
	t->alt_bg = TERM_COLOR_DEFAULT;
	t->alt_attr = 0;
}

void term_init(Term *t)
{
	memset(t, 0, sizeof(*t));
	term_reset(t);
}

/* --- writing ------------------------------------------------------------ */

void term_putc(Term *t, uint32_t ch)
{
	int8_t fg, bg;
	TermCell *cell;

	if (t->wrap_pending) {
		t->wrap_pending = false;
		t->cx = 0;
		term_lf(t);
	}

	/* IRM shifts the tail of the line right; the last cell falls off. */
	if (t->insert_mode && t->cx < TERM_COLS - 1)
		memmove(&t->grid[t->cy][t->cx + 1], &t->grid[t->cy][t->cx],
		        (size_t)(TERM_COLS - t->cx - 1) * sizeof(TermCell));

	cell = &t->grid[t->cy][t->cx];
	effective_colors(t, &fg, &bg);
	cell->ch = ch;
	cell->fg = fg;
	cell->bg = bg;
	cell->attr = (uint8_t)(t->cur_attr & TERM_ATTR_UNDERLINE);

	if (t->cx == TERM_COLS - 1) {
		/* The wrap happens lazily, on the next printable character, so a
		 * full-width line does not scroll until something follows it. */
		if (t->autowrap) t->wrap_pending = true;
	} else {
		t->cx++;
	}
}

void term_lf(Term *t)
{
	t->wrap_pending = false;
	if (t->cy == t->bot)
		term_scroll_up(t, t->top, t->bot, 1);
	else if (t->cy < TERM_ROWS - 1)
		t->cy++;
}

void term_ri(Term *t)
{
	t->wrap_pending = false;
	if (t->cy == t->top)
		term_scroll_down(t, t->top, t->bot, 1);
	else if (t->cy > 0)
		t->cy--;
}

void term_bs(Term *t)
{
	t->wrap_pending = false;
	if (t->cx > 0) t->cx--;
}

void term_cr(Term *t)
{
	t->wrap_pending = false;
	t->cx = 0;
}

void term_tab(Term *t, int count)
{
	t->wrap_pending = false;
	if (count < 1) count = 1;

	while (count-- > 0) {
		int x = t->cx + 1;
		while (x < TERM_COLS && !t->tab_stop[x]) x++;
		if (x >= TERM_COLS) {
			t->cx = TERM_COLS - 1;
			break;
		}
		t->cx = x;
	}
}

void term_back_tab(Term *t, int count)
{
	t->wrap_pending = false;
	if (count < 1) count = 1;

	while (count-- > 0) {
		int x = t->cx - 1;
		while (x > 0 && !t->tab_stop[x]) x--;
		t->cx = x < 0 ? 0 : x;
		if (t->cx == 0) break;
	}
}

void term_set_cursor(Term *t, int col, int row)
{
	t->wrap_pending = false;
	t->cx = clampi(col, 0, TERM_COLS - 1);
	t->cy = clampi(row, 0, TERM_ROWS - 1);
}

void term_move_cursor(Term *t, int dcol, int drow)
{
	t->wrap_pending = false;
	t->cx = clampi(t->cx + dcol, 0, TERM_COLS - 1);
	/* Vertical movement is confined to the scroll region, as in xterm. */
	t->cy = clampi(t->cy + drow, t->top, t->bot);
}

/* --- scrolling ---------------------------------------------------------- */

void term_scroll_up(Term *t, int top, int bot, int n)
{
	int i;

	if (top < 0) top = 0;
	if (bot > TERM_ROWS - 1) bot = TERM_ROWS - 1;
	if (top > bot || n <= 0) return;
	if (n > bot - top + 1) n = bot - top + 1;

	for (i = 0; i < n; i++) {
		/* Only a scroll of the whole screen feeds history. A scroll inside a
		 * region (vim, less) throws those lines away instead of polluting
		 * the scrollback with the screen's top rows. */
		if (top == 0 && bot == TERM_ROWS - 1 && !t->alt_active)
			sb_push(t, t->grid[top]);

		if (bot > top)
			memmove(&t->grid[top][0], &t->grid[top + 1][0],
			        (size_t)(bot - top) * TERM_COLS * sizeof(TermCell));

		blank_row(t, t->grid[bot]);
	}
}

void term_scroll_down(Term *t, int top, int bot, int n)
{
	int i;

	if (top < 0) top = 0;
	if (bot > TERM_ROWS - 1) bot = TERM_ROWS - 1;
	if (top > bot || n <= 0) return;
	if (n > bot - top + 1) n = bot - top + 1;

	for (i = 0; i < n; i++) {
		if (bot > top)
			memmove(&t->grid[top + 1][0], &t->grid[top][0],
			        (size_t)(bot - top) * TERM_COLS * sizeof(TermCell));

		blank_row(t, t->grid[top]);
	}
}

void term_set_scroll_region(Term *t, int top, int bot)
{
	if (top < 0) top = 0;
	if (bot > TERM_ROWS - 1) bot = TERM_ROWS - 1;
	if (top >= bot) {
		top = 0;
		bot = TERM_ROWS - 1;
	}

	t->top = top;
	t->bot = bot;

	/* DECSTBM homes the cursor (origin mode is not implemented, so home is
	 * the absolute top-left rather than the region's top-left). */
	t->wrap_pending = false;
	t->cx = 0;
	t->cy = 0;
}

/* --- erasing and editing ------------------------------------------------ */

void term_erase_display(Term *t, int mode)
{
	int y;

	t->wrap_pending = false;

	switch (mode) {
	case 0: /* cursor to end of screen */
		erase_cells(t, t->cy, t->cx, TERM_COLS - 1);
		for (y = t->cy + 1; y < TERM_ROWS; y++) blank_row(t, t->grid[y]);
		break;
	case 1: /* start of screen to cursor */
		for (y = 0; y < t->cy; y++) blank_row(t, t->grid[y]);
		erase_cells(t, t->cy, 0, t->cx);
		break;
	case 2: /* whole screen -- the cursor does not move */
		for (y = 0; y < TERM_ROWS; y++) blank_row(t, t->grid[y]);
		break;
	case 3: /* scrollback only */
		term_sb_clear(t);
		break;
	default:
		break;
	}
}

void term_erase_line(Term *t, int mode)
{
	t->wrap_pending = false;

	switch (mode) {
	case 0:
		erase_cells(t, t->cy, t->cx, TERM_COLS - 1);
		break;
	case 1:
		erase_cells(t, t->cy, 0, t->cx);
		break;
	case 2:
		erase_cells(t, t->cy, 0, TERM_COLS - 1);
		break;
	default:
		break;
	}
}

void term_erase_chars(Term *t, int n)
{
	if (n <= 0) return;
	t->wrap_pending = false;
	erase_cells(t, t->cy, t->cx, t->cx + n - 1);
}

void term_insert_lines(Term *t, int n)
{
	if (t->cy < t->top || t->cy > t->bot || n <= 0) return;
	if (n > t->bot - t->cy + 1) n = t->bot - t->cy + 1;

	if (t->bot > t->cy)
		memmove(&t->grid[t->cy + n][0], &t->grid[t->cy][0],
		        (size_t)(t->bot - t->cy + 1 - n) * TERM_COLS * sizeof(TermCell));

	for (int y = t->cy; y < t->cy + n; y++) blank_row(t, t->grid[y]);

	/* DEC STD 070: IL and DL move the cursor to the left margin. */
	t->cx = 0;
	t->wrap_pending = false;
}

void term_delete_lines(Term *t, int n)
{
	if (t->cy < t->top || t->cy > t->bot || n <= 0) return;
	if (n > t->bot - t->cy + 1) n = t->bot - t->cy + 1;

	if (t->bot > t->cy)
		memmove(&t->grid[t->cy][0], &t->grid[t->cy + n][0],
		        (size_t)(t->bot - t->cy + 1 - n) * TERM_COLS * sizeof(TermCell));

	for (int y = t->bot - n + 1; y <= t->bot; y++) blank_row(t, t->grid[y]);

	t->cx = 0;
	t->wrap_pending = false;
}

void term_insert_chars(Term *t, int n)
{
	TermCell c;
	int x;

	if (n <= 0) return;
	if (n > TERM_COLS - t->cx) n = TERM_COLS - t->cx;
	if (n <= 0) return;

	memmove(&t->grid[t->cy][t->cx + n], &t->grid[t->cy][t->cx],
	        (size_t)(TERM_COLS - t->cx - n) * sizeof(TermCell));

	cell_blank(t, &c);
	for (x = t->cx; x < t->cx + n; x++) t->grid[t->cy][x] = c;

	t->wrap_pending = false;
}

void term_delete_chars(Term *t, int n)
{
	TermCell c;
	int x;

	if (n <= 0) return;
	if (n > TERM_COLS - t->cx) n = TERM_COLS - t->cx;
	if (n <= 0) return;

	memmove(&t->grid[t->cy][t->cx], &t->grid[t->cy][t->cx + n],
	        (size_t)(TERM_COLS - t->cx - n) * sizeof(TermCell));

	cell_blank(t, &c);
	for (x = TERM_COLS - n; x < TERM_COLS; x++) t->grid[t->cy][x] = c;

	t->wrap_pending = false;
}

/* --- alternate screen and saved cursor ---------------------------------- */

void term_set_alt_screen(Term *t, bool on)
{
	if (on == t->alt_active) return;

	if (on) {
		memcpy(t->alt, t->grid, sizeof(t->grid));
		t->alt_cx = t->cx;
		t->alt_cy = t->cy;
		t->alt_fg = t->cur_fg;
		t->alt_bg = t->cur_bg;
		t->alt_attr = t->cur_attr;

		t->alt_active = true;
		t->cx = 0;
		t->cy = 0;
		t->top = 0;
		t->bot = TERM_ROWS - 1;
		t->wrap_pending = false;
		term_erase_display(t, 2);
	} else {
		memcpy(t->grid, t->alt, sizeof(t->grid));
		t->cx = t->alt_cx;
		t->cy = t->alt_cy;
		t->cur_fg = t->alt_fg;
		t->cur_bg = t->alt_bg;
		t->cur_attr = t->alt_attr;

		t->alt_active = false;
		t->top = 0;
		t->bot = TERM_ROWS - 1;
		t->wrap_pending = false;
	}
}

void term_set_insert_mode(Term *t, bool on)
{
	t->insert_mode = on;
}

void term_save_cursor(Term *t)
{
	t->saved_cx = t->cx;
	t->saved_cy = t->cy;
	t->saved_fg = t->cur_fg;
	t->saved_bg = t->cur_bg;
	t->saved_attr = t->cur_attr;
	t->wrap_pending = false;
}

void term_restore_cursor(Term *t)
{
	t->cx = clampi(t->saved_cx, 0, TERM_COLS - 1);
	t->cy = clampi(t->saved_cy, 0, TERM_ROWS - 1);
	t->cur_fg = t->saved_fg;
	t->cur_bg = t->saved_bg;
	t->cur_attr = t->saved_attr;
	t->wrap_pending = false;
}

/* --- tab stops ---------------------------------------------------------- */

void term_set_tab_stop(Term *t, int col)
{
	if (col >= 0 && col < TERM_COLS) t->tab_stop[col] = true;
}

void term_clear_tab_stops(Term *t, int mode)
{
	int x;

	if (mode == 0) {
		if (t->cx >= 0 && t->cx < TERM_COLS) t->tab_stop[t->cx] = false;
	} else if (mode == 3) {
		for (x = 0; x < TERM_COLS; x++) t->tab_stop[x] = false;
	}
}

/* --- rendition ---------------------------------------------------------- */

void term_sgr_reset(Term *t)
{
	t->cur_fg = TERM_COLOR_DEFAULT;
	t->cur_bg = TERM_COLOR_DEFAULT;
	t->cur_attr = 0;
}

void term_sgr_set_fg(Term *t, int color)
{
	t->cur_fg = (int8_t)color;
}

void term_sgr_set_bg(Term *t, int color)
{
	t->cur_bg = (int8_t)color;
}

void term_sgr_set_attr(Term *t, uint8_t attr, bool on)
{
	if (on)
		t->cur_attr |= attr;
	else
		t->cur_attr &= (uint8_t)~attr;
}

/* --- rendering support -------------------------------------------------- */

const TermCell *term_view_line(const Term *t, int offset, int row)
{
	int total, index;

	if (row < 0 || row >= TERM_ROWS) return NULL;

	/* The alternate screen keeps no history, so a scrollback view of it
	 * would be meaningless. */
	if (t->alt_active) return offset == 0 ? t->grid[row] : NULL;

	if (offset < 0) offset = 0;
	if (offset > t->sb_count) offset = t->sb_count;

	total = t->sb_count + TERM_ROWS;
	index = total - TERM_ROWS - offset + row;

	if (index < 0 || index >= total) return NULL;
	if (index < t->sb_count) return term_sb_line(t, index);
	return t->grid[index - t->sb_count];
}
