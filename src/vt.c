#include "vt.h"

#include <stdio.h>
#include <string.h>

/*
 * DEC special graphics (ESC ( 0), folded down to the closest ASCII shapes.
 * curses programs draw their borders with these; without the mapping an
 * ASCII-only font would show the raw letters (q, x, l...) instead of lines.
 */
static const char dec_graphics[0x7F - 0x60] = {
	'*', '#', ' ', ' ', ' ', ' ', 'o', '#', ' ', ' ', /* 0x60-0x69 */
	'+', '+', '+', '+', '+', '-', '-', '-', '-', '-', /* 0x6a-0x73 */
	'+', '+', '+', '+', '|', '<', '>', 'n', '!', 'L', '*' /* 0x74-0x7e */
};

/* --- parameter bookkeeping ---------------------------------------------- */

static void vt_reset_params(VtParser *p)
{
	int i;

	for (i = 0; i < VT_MAX_PARAMS; i++) p->params[i] = -1;
	p->nparams = 0;
	p->param = 0;
	p->have_param = 0;
	p->private_marker = 0;
	p->intermediate = 0;
}

static void push_param(VtParser *p)
{
	if (p->nparams < VT_MAX_PARAMS)
		p->params[p->nparams++] = p->have_param ? p->param : -1;
	p->param = 0;
	p->have_param = 0;
}

/* A parameter that was omitted (or an empty slot) reads as `def`. */
static int param_def(const VtParser *p, int index, int def)
{
	int v;

	if (index >= p->nparams) return def;
	v = p->params[index];
	return v < 0 ? def : v;
}

/* For commands where an omitted or zero parameter means one. */
static int param_one(const VtParser *p, int index)
{
	int v = param_def(p, index, 0);

	return v <= 0 ? 1 : v;
}

/* --- replies to the remote ---------------------------------------------- */

static void vt_emit(VtParser *p, const char *s, size_t n)
{
	if (p->out_len + n > VT_OUTPUT_CAP) {
		p->out_overflow = true;
		return;
	}
	memcpy(p->out + p->out_len, s, n);
	p->out_len += n;
}

static void emit_cursor_report(VtParser *p, const Term *t)
{
	char buf[24];
	int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dR", t->cy + 1, t->cx + 1);

	if (n > 0 && (size_t)n < sizeof(buf)) vt_emit(p, buf, (size_t)n);
}

/* --- colour ------------------------------------------------------------- */

static int color256_to_palette(int index)
{
	static const int levels[6] = {0, 95, 135, 175, 215, 255};
	int r, g, b;

	if (index < 0) return TERM_COLOR_DEFAULT;
	if (index < 16) return index;

	if (index < 232) {
		int c = index - 16;
		r = levels[(c / 36) % 6];
		g = levels[(c / 6) % 6];
		b = levels[c % 6];
	} else {
		r = g = b = 8 + (index - 232) * 10;
	}
	return term_nearest_color(r, g, b);
}

static void sgr_dispatch(VtParser *p, Term *t)
{
	int i;

	if (p->nparams == 0) {
		term_sgr_reset(t);
		return;
	}

	for (i = 0; i < p->nparams; i++) {
		int v = p->params[i];

		if (v < 0) v = 0;

		switch (v) {
		case 0:
			term_sgr_reset(t);
			break;
		case 1:
			term_sgr_set_attr(t, TERM_ATTR_BOLD, true);
			break;
		case 2:
			term_sgr_set_attr(t, TERM_ATTR_DIM, true);
			break;
		case 4:
			term_sgr_set_attr(t, TERM_ATTR_UNDERLINE, true);
			break;
		case 7:
			term_sgr_set_attr(t, TERM_ATTR_REVERSE, true);
			break;
		case 8:
			term_sgr_set_attr(t, TERM_ATTR_INVISIBLE, true);
			break;
		case 21:
		case 22:
			term_sgr_set_attr(t, TERM_ATTR_BOLD, false);
			term_sgr_set_attr(t, TERM_ATTR_DIM, false);
			break;
		case 24:
			term_sgr_set_attr(t, TERM_ATTR_UNDERLINE, false);
			break;
		case 27:
			term_sgr_set_attr(t, TERM_ATTR_REVERSE, false);
			break;
		case 28:
			term_sgr_set_attr(t, TERM_ATTR_INVISIBLE, false);
			break;
		case 39:
			term_sgr_set_fg(t, TERM_COLOR_DEFAULT);
			break;
		case 49:
			term_sgr_set_bg(t, TERM_COLOR_DEFAULT);
			break;
		case 38:
		case 48: {
			bool is_fg = (v == 38);
			int mode = param_def(p, i + 1, -1);

			if (mode == 5) {
				int color = color256_to_palette(param_def(p, i + 2, -1));
				if (is_fg)
					term_sgr_set_fg(t, color);
				else
					term_sgr_set_bg(t, color);
				i += 2;
			} else if (mode == 2) {
				int r = param_def(p, i + 2, 0);
				int g = param_def(p, i + 3, 0);
				int b = param_def(p, i + 4, 0);
				int color = term_nearest_color(r, g, b);
				if (is_fg)
					term_sgr_set_fg(t, color);
				else
					term_sgr_set_bg(t, color);
				i += 4;
			}
			break;
		}
		default:
			if (v >= 30 && v <= 37)
				term_sgr_set_fg(t, v - 30);
			else if (v >= 40 && v <= 47)
				term_sgr_set_bg(t, v - 40);
			else if (v >= 90 && v <= 97)
				term_sgr_set_fg(t, v - 90 + 8);
			else if (v >= 100 && v <= 107)
				term_sgr_set_bg(t, v - 100 + 8);
			/* 3 italic, 5 blink, 9 strike, 53 overline: no glyph for it. */
			break;
		}
	}
}

/* --- CSI ---------------------------------------------------------------- */

static void csi_private_modes(VtParser *p, Term *t, bool set)
{
	int i;

	for (i = 0; i < p->nparams; i++) {
		int v = p->params[i];

		if (v < 0) v = 0;

		switch (v) {
		case 1:
			p->app_cursor_keys = set;
			break;
		case 7:
			t->autowrap = set;
			break;
		case 25:
			t->cursor_visible = set;
			break;
		case 47:
		case 1047:
		case 1049:
			term_set_alt_screen(t, set);
			break;
		case 1048:
			if (set)
				term_save_cursor(t);
			else
				term_restore_cursor(t);
			break;
		case 2004:
			p->bracketed_paste = set;
			break;
		default:
			/* Mouse reporting, origin mode, blink and the rest are accepted
			 * and ignored: refusing them would only make programs stall. */
			break;
		}
	}
}

static void csi_dispatch(VtParser *p, Term *t, uint8_t final)
{
	/* Secondary DA and keypad-mode sets carry a '>' or '=' marker and have no
	 * effect on the grid. */
	if (p->private_marker == '>' || p->private_marker == '=') return;
	if (p->intermediate == ' ') return; /* DECSCUSR cursor style */

	switch (final) {
	case 'A':
		term_move_cursor(t, 0, -param_one(p, 0));
		break;
	case 'B':
		term_move_cursor(t, 0, param_one(p, 0));
		break;
	case 'C':
		term_move_cursor(t, param_one(p, 0), 0);
		break;
	case 'D':
		term_move_cursor(t, -param_one(p, 0), 0);
		break;
	case 'E':
		term_move_cursor(t, 0, param_one(p, 0));
		term_cr(t);
		break;
	case 'F':
		term_move_cursor(t, 0, -param_one(p, 0));
		term_cr(t);
		break;
	case 'G':
		term_set_cursor(t, param_one(p, 0) - 1, t->cy);
		break;
	case 'd':
		term_set_cursor(t, t->cx, param_one(p, 0) - 1);
		break;
	case 'H':
	case 'f':
		term_set_cursor(t, param_one(p, 1) - 1, param_one(p, 0) - 1);
		break;
	case 'I':
		term_tab(t, param_one(p, 0));
		break;
	case 'Z':
		term_back_tab(t, param_one(p, 0));
		break;
	case 'J':
		term_erase_display(t, param_def(p, 0, 0));
		break;
	case 'K':
		term_erase_line(t, param_def(p, 0, 0));
		break;
	case 'L':
		term_insert_lines(t, param_one(p, 0));
		break;
	case 'M':
		term_delete_lines(t, param_one(p, 0));
		break;
	case 'P':
		term_delete_chars(t, param_one(p, 0));
		break;
	case '@':
		term_insert_chars(t, param_one(p, 0));
		break;
	case 'X':
		term_erase_chars(t, param_one(p, 0));
		break;
	case 'S':
		term_scroll_up(t, t->top, t->bot, param_one(p, 0));
		break;
	case 'T':
		term_scroll_down(t, t->top, t->bot, param_one(p, 0));
		break;
	case 'c':
		if (p->private_marker == 0) vt_emit(p, "\x1b[?1;2c", 7); /* VT100 with AVO */
		break;
	case 'g':
		term_clear_tab_stops(t, param_def(p, 0, 0));
		break;
	case 'h':
	case 'l':
		if (p->private_marker == '?') {
			csi_private_modes(p, t, final == 'h');
		} else {
			int i;
			for (i = 0; i < p->nparams; i++) {
				if (p->params[i] == 4) term_set_insert_mode(t, final == 'h');
			}
		}
		break;
	case 'm':
		sgr_dispatch(p, t);
		break;
	case 'n':
		if (p->private_marker == 0) {
			int v = param_def(p, 0, 0);
			if (v == 5)
				vt_emit(p, "\x1b[0n", 4); /* ready */
			else if (v == 6)
				emit_cursor_report(p, t);
		}
		break;
	case 'r':
		term_set_scroll_region(t, param_one(p, 0) - 1, param_one(p, 1) - 1);
		break;
	case 's':
		term_save_cursor(t);
		break;
	case 'u':
		term_restore_cursor(t);
		break;
	default:
		break;
	}
}

static void csi_byte(VtParser *p, Term *t, uint8_t b)
{
	if (b >= '0' && b <= '9') {
		p->param = p->param * 10 + (b - '0');
		if (p->param > 65535) p->param = 65535;
		p->have_param = 1;
		return;
	}
	if (b == ';') {
		push_param(p);
		return;
	}
	if (b == '?' || b == '<' || b == '>' || b == '=') {
		if (p->nparams == 0 && !p->have_param && p->private_marker == 0)
			p->private_marker = b;
		return;
	}
	if (b >= 0x20 && b <= 0x2F) {
		p->intermediate = b;
		return;
	}
	if (b >= 0x40 && b <= 0x7E) {
		if (p->have_param || p->nparams > 0) push_param(p);
		csi_dispatch(p, t, b);
		p->state = VT_GROUND;
		return;
	}
	if (b == 0x1B) {
		p->state = VT_ESC;
		return;
	}
	if (b < 0x20) {
		/* C0 controls still execute inside a CSI sequence. */
		if (b == 0x0A || b == 0x0B || b == 0x0C)
			term_lf(t);
		else if (b == 0x0D)
			term_cr(t);
		else if (b == 0x08)
			term_bs(t);
	}
	/* DEL and anything unrecognised: dropped. */
}

/* --- ESC ---------------------------------------------------------------- */

static void esc_dispatch(VtParser *p, Term *t, uint8_t final)
{
	switch (final) {
	case '7':
		term_save_cursor(t);
		break;
	case '8':
		term_restore_cursor(t);
		break;
	case 'D':
		term_lf(t);
		break;
	case 'E':
		term_cr(t);
		term_lf(t);
		break;
	case 'M':
		term_ri(t);
		break;
	case 'H':
		term_set_tab_stop(t, t->cx);
		break;
	case 'Z':
		vt_emit(p, "\x1b[?1;2c", 7); /* DECID */
		break;
	case 'c':
		term_reset(t);
		p->g0_graphics = false;
		p->app_cursor_keys = false;
		p->title_len = 0;
		p->title[0] = '\0';
		break;
	default:
		/* ESC = / ESC > (keypad), ESC \ (ST) and the rest are no-ops. */
		break;
	}
}

static void esc_byte(VtParser *p, Term *t, uint8_t b)
{
	switch (b) {
	case '[':
		vt_reset_params(p);
		p->state = VT_CSI;
		return;
	case ']':
		p->title_len = 0;
		p->state = VT_OSC;
		return;
	case 'P': /* DCS */
	case '^': /* PM */
	case '_': /* APC */
		p->state = VT_STRING;
		return;
	case '(':
	case ')':
	case '*':
	case '+':
		p->intermediate = b;
		p->state = VT_ESC_CHARSET;
		return;
	case '#':
		p->state = VT_ESC_HASH;
		return;
	default:
		esc_dispatch(p, t, b);
		p->state = VT_GROUND;
		return;
	}
}

/* DECALN: fill the screen with E, the alignment test pattern. */
static void decaln(Term *t)
{
	int y, x;

	for (y = 0; y < TERM_ROWS; y++) {
		for (x = 0; x < TERM_COLS; x++) {
			t->grid[y][x].ch = 'E';
			t->grid[y][x].fg = TERM_COLOR_DEFAULT;
			t->grid[y][x].bg = TERM_COLOR_DEFAULT;
			t->grid[y][x].attr = 0;
		}
	}
}

/* --- OSC ---------------------------------------------------------------- */

static void osc_finish(VtParser *p)
{
	p->title[p->title_len] = '\0';

	if (p->title_len > 2 && (p->title[0] == '0' || p->title[0] == '2') &&
	    p->title[1] == ';') {
		memmove(p->title, p->title + 2, p->title_len - 2 + 1);
		p->title_len -= 2;
	} else {
		/* Palette, clipboard and the rest: parsed and dropped. */
		p->title_len = 0;
		p->title[0] = '\0';
	}
}

static void osc_byte(VtParser *p, uint8_t b)
{
	if (b == 0x07) { /* BEL terminates */
		osc_finish(p);
		p->state = VT_GROUND;
		return;
	}
	if (b == 0x1B) {
		p->state = VT_OSC_ESC;
		return;
	}
	if (p->title_len < VT_TITLE_CAP - 1) p->title[p->title_len++] = (char)b;
}

/* --- ground ------------------------------------------------------------- */

static void vt_put_graphic(VtParser *p, Term *t, uint32_t ch)
{
	if (p->g0_graphics && ch >= 0x60 && ch <= 0x7E)
		ch = (uint32_t)(uint8_t)dec_graphics[ch - 0x60];
	term_putc(t, ch);
}

static void ground_byte(VtParser *p, Term *t, uint8_t b)
{
	if (b == 0x1B) { /* ESC -- checked first, it is not just another C0 */
		p->state = VT_ESC;
		return;
	}

	if (b < 0x20) {
		switch (b) {
		case 0x07: /* BEL: no bell to ring */
			break;
		case 0x08:
			term_bs(t);
			break;
		case 0x09:
			term_tab(t, 1);
			break;
		case 0x0A:
		case 0x0B:
		case 0x0C:
			term_lf(t);
			break;
		case 0x0D:
			term_cr(t);
			break;
		default:
			/* SO/SI and the rest: G1 is not implemented. */
			break;
		}
		return;
	}

	if (b == 0x7F) return; /* DEL */

	if (b < 0x80) {
		vt_put_graphic(p, t, b);
		return;
	}

	if (p->utf8_need > 0 && (b & 0xC0) == 0x80) {
		p->utf8_cp = (p->utf8_cp << 6) | (uint32_t)(b & 0x3F);
		p->utf8_need--;
		if (p->utf8_need == 0) {
			/* The embedded font covers ASCII only. Everything above it is
			 * consumed so the stream stays in sync, and shown as a
			 * placeholder rather than as mojibake. */
			term_putc(t, p->utf8_cp <= 0x7E ? p->utf8_cp : '?');
		}
		return;
	}

	/* A fresh lead byte. */
	p->utf8_need = 0;
	if ((b & 0xE0) == 0xC0) {
		p->utf8_need = 1;
		p->utf8_cp = b & 0x1Fu;
	} else if ((b & 0xF0) == 0xE0) {
		p->utf8_need = 2;
		p->utf8_cp = b & 0x0Fu;
	} else if ((b & 0xF8) == 0xF0) {
		p->utf8_need = 3;
		p->utf8_cp = b & 0x07u;
	}
	/* Anything else is an invalid lead byte and is dropped. */
}

/* --- entry points ------------------------------------------------------- */

void vt_init(VtParser *p)
{
	memset(p, 0, sizeof(*p));
	p->state = VT_GROUND;
	vt_reset_params(p);
}

void vt_feed(VtParser *p, Term *t, const uint8_t *data, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		uint8_t b = data[i];

		switch (p->state) {
		case VT_GROUND:
			ground_byte(p, t, b);
			break;
		case VT_ESC:
			esc_byte(p, t, b);
			break;
		case VT_ESC_CHARSET:
			if (p->intermediate == '(') p->g0_graphics = (b == '0');
			p->state = VT_GROUND;
			break;
		case VT_ESC_HASH:
			if (b == '8') decaln(t);
			p->state = VT_GROUND;
			break;
		case VT_CSI:
			csi_byte(p, t, b);
			break;
		case VT_OSC:
			osc_byte(p, b);
			break;
		case VT_OSC_ESC:
			osc_finish(p);
			p->state = VT_GROUND;
			break;
		case VT_STRING:
			if (b == 0x1B) p->state = VT_STRING_ESC;
			break;
		case VT_STRING_ESC:
			p->state = VT_GROUND;
			break;
		default:
			p->state = VT_GROUND;
			break;
		}
	}
}

void vt_feed_str(VtParser *p, Term *t, const char *s)
{
	vt_feed(p, t, (const uint8_t *)s, strlen(s));
}

size_t vt_take_output(VtParser *p, char *dst, size_t cap)
{
	size_t n = p->out_len < cap ? p->out_len : cap;

	if (n > 0) memcpy(dst, p->out, n);
	p->out_len = 0;
	p->out_overflow = false;
	return n;
}
