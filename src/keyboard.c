#include "keyboard.h"

#include <string.h>

#include "clock.h"
#include "render.h"

/*
 * Keys carry a label, an action and a width in tenths of a unit. A row is laid
 * out across the full screen width, so the same table both draws and hit-tests.
 */
enum {
	A_CHAR,
	A_SHIFT,
	A_CTRL,
	A_ALT,
	A_ESC,
	A_TAB,
	A_ENTER,
	A_DEL,
	A_LEFT,
	A_UP,
	A_DOWN,
	A_RIGHT,
	A_PAGE_NUM,
	A_PAGE_SYM,
	A_PAGE_ALPHA,
	A_HIDE,
};

#define U(n) ((uint8_t)(n))

typedef struct {
	const char *label;
	uint8_t action;
	uint8_t units;
	char ch;
} KeyDef;

typedef struct {
	const KeyDef *keys;
	int count;
} RowDef;

/* --- function bar ------------------------------------------------------- */

static const KeyDef fn_keys[] = {
	{"Esc", A_ESC, U(10), 0},   {"Ctrl", A_CTRL, U(10), 0},
	{"Alt", A_ALT, U(10), 0},   {"Tab", A_TAB, U(10), 0},
	{"<", A_LEFT, U(10), 0},    {"^", A_UP, U(10), 0},
	{"v", A_DOWN, U(10), 0},    {">", A_RIGHT, U(10), 0},
	{"/", A_CHAR, U(10), '/'},  {"Hide", A_HIDE, U(10), 0},
};
#define FN_COUNT ((int)(sizeof(fn_keys) / sizeof(fn_keys[0])))

/* --- letters ------------------------------------------------------------ */

static const KeyDef alpha0[] = {
	{"q", A_CHAR, U(10), 'q'}, {"w", A_CHAR, U(10), 'w'},
	{"e", A_CHAR, U(10), 'e'}, {"r", A_CHAR, U(10), 'r'},
	{"t", A_CHAR, U(10), 't'}, {"y", A_CHAR, U(10), 'y'},
	{"u", A_CHAR, U(10), 'u'}, {"i", A_CHAR, U(10), 'i'},
	{"o", A_CHAR, U(10), 'o'}, {"p", A_CHAR, U(10), 'p'},
};

static const KeyDef alpha1[] = {
	{"a", A_CHAR, U(10), 'a'}, {"s", A_CHAR, U(10), 's'},
	{"d", A_CHAR, U(10), 'd'}, {"f", A_CHAR, U(10), 'f'},
	{"g", A_CHAR, U(10), 'g'}, {"h", A_CHAR, U(10), 'h'},
	{"j", A_CHAR, U(10), 'j'}, {"k", A_CHAR, U(10), 'k'},
	{"l", A_CHAR, U(10), 'l'},
};

static const KeyDef alpha2[] = {
	{"Shift", A_SHIFT, U(15), 0},
	{"z", A_CHAR, U(10), 'z'}, {"x", A_CHAR, U(10), 'x'},
	{"c", A_CHAR, U(10), 'c'}, {"v", A_CHAR, U(10), 'v'},
	{"b", A_CHAR, U(10), 'b'}, {"n", A_CHAR, U(10), 'n'},
	{"m", A_CHAR, U(10), 'm'},
	{"Del", A_DEL, U(15), 0},
};

static const KeyDef alpha3[] = {
	{"123", A_PAGE_NUM, U(15), 0},
	{",", A_CHAR, U(10), ','},
	{"Space", A_CHAR, U(45), ' '},
	{".", A_CHAR, U(10), '.'},
	{"Enter", A_ENTER, U(20), 0},
};

/* --- numbers and symbols ------------------------------------------------ */

static const KeyDef num0[] = {
	{"1", A_CHAR, U(10), '1'}, {"2", A_CHAR, U(10), '2'},
	{"3", A_CHAR, U(10), '3'}, {"4", A_CHAR, U(10), '4'},
	{"5", A_CHAR, U(10), '5'}, {"6", A_CHAR, U(10), '6'},
	{"7", A_CHAR, U(10), '7'}, {"8", A_CHAR, U(10), '8'},
	{"9", A_CHAR, U(10), '9'}, {"0", A_CHAR, U(10), '0'},
};

static const KeyDef num1[] = {
	{"-", A_CHAR, U(10), '-'}, {"/", A_CHAR, U(10), '/'},
	{":", A_CHAR, U(10), ':'}, {";", A_CHAR, U(10), ';'},
	{"(", A_CHAR, U(10), '('}, {")", A_CHAR, U(10), ')'},
	{"$", A_CHAR, U(10), '$'}, {"&", A_CHAR, U(10), '&'},
	{"@", A_CHAR, U(10), '@'}, {"\"", A_CHAR, U(10), '"'},
};

static const KeyDef num2[] = {
	{"=<", A_PAGE_SYM, U(15), 0},
	{".", A_CHAR, U(10), '.'}, {",", A_CHAR, U(10), ','},
	{"?", A_CHAR, U(10), '?'}, {"!", A_CHAR, U(10), '!'},
	{"'", A_CHAR, U(10), '\''}, {"*", A_CHAR, U(10), '*'},
	{"%", A_CHAR, U(10), '%'},
	{"Del", A_DEL, U(15), 0},
};

static const KeyDef num3[] = {
	{"ABC", A_PAGE_ALPHA, U(15), 0},
	{",", A_CHAR, U(10), ','},
	{"Space", A_CHAR, U(45), ' '},
	{".", A_CHAR, U(10), '.'},
	{"Enter", A_ENTER, U(20), 0},
};

static const KeyDef sym0[] = {
	{"~", A_CHAR, U(10), '~'}, {"`", A_CHAR, U(10), '`'},
	{"|", A_CHAR, U(10), '|'}, {"\\", A_CHAR, U(10), '\\'},
	{"^", A_CHAR, U(10), '^'}, {"_", A_CHAR, U(10), '_'},
	{"=", A_CHAR, U(10), '='}, {"+", A_CHAR, U(10), '+'},
	{"<", A_CHAR, U(10), '<'}, {">", A_CHAR, U(10), '>'},
};

static const KeyDef sym1[] = {
	{"[", A_CHAR, U(10), '['}, {"]", A_CHAR, U(10), ']'},
	{"{", A_CHAR, U(10), '{'}, {"}", A_CHAR, U(10), '}'},
	{"#", A_CHAR, U(10), '#'}, {"%", A_CHAR, U(10), '%'},
	{"*", A_CHAR, U(10), '*'}, {"!", A_CHAR, U(10), '!'},
	{"?", A_CHAR, U(10), '?'}, {"&", A_CHAR, U(10), '&'},
};

static const KeyDef sym2[] = {
	{"123", A_PAGE_NUM, U(15), 0},
	{"'", A_CHAR, U(10), '\''}, {";", A_CHAR, U(10), ';'},
	{":", A_CHAR, U(10), ':'}, {"(", A_CHAR, U(10), '('},
	{")", A_CHAR, U(10), ')'}, {"@", A_CHAR, U(10), '@'},
	{"$", A_CHAR, U(10), '$'},
	{"Del", A_DEL, U(15), 0},
};

static const KeyDef sym3[] = {
	{"ABC", A_PAGE_ALPHA, U(15), 0},
	{",", A_CHAR, U(10), ','},
	{"Space", A_CHAR, U(45), ' '},
	{".", A_CHAR, U(10), '.'},
	{"Enter", A_ENTER, U(20), 0},
};

#define ROW(a) { a, (int)(sizeof(a) / sizeof((a)[0])) }

static const RowDef pages[3][4] = {
	{ ROW(alpha0), ROW(alpha1), ROW(alpha2), ROW(alpha3) },
	{ ROW(num0), ROW(num1), ROW(num2), ROW(num3) },
	{ ROW(sym0), ROW(sym1), ROW(sym2), ROW(sym3) },
};

#define KB_ROWS 4

/* --- geometry ----------------------------------------------------------- */

#define KB_MARGIN 8
#define KB_GAP 5
#define KB_TOP_PAD 6
#define KB_FN_H 34
#define KB_ROW_H 52
#define KB_ROW_GAP 5

#define KB_REPEAT_DELAY_MS 350
#define KB_REPEAT_RATE_MS 55

static int fn_top(void)
{
	return KB_Y + KB_TOP_PAD;
}

static int row_top(int row)
{
	return KB_Y + KB_TOP_PAD + KB_FN_H + KB_ROW_GAP +
	       row * (KB_ROW_H + KB_ROW_GAP);
}

static void key_rect(const KeyDef *keys, int count, int index, int y, int h,
                     int *x, int *w)
{
	int avail = SCREEN_W - 2 * KB_MARGIN - (count - 1) * KB_GAP;
	int total = 0;
	int before = 0;
	int i;

	for (i = 0; i < count; i++) total += keys[i].units;
	for (i = 0; i < index; i++) before += keys[i].units;

	*x = KB_MARGIN + before * avail / total + index * KB_GAP;
	*w = keys[index].units * avail / total;
}

/* --- state -------------------------------------------------------------- */

static void clear_active(Kb *kb)
{
	kb->active_fn = -1;
	kb->active_row = -1;
	kb->active_col = -1;
}

static void clear_held(Kb *kb)
{
	kb->held_fn = -1;
	kb->held_row = -1;
	kb->held_col = -1;
	kb->held_action = -1;
}

void kb_init(Kb *kb)
{
	memset(kb, 0, sizeof(*kb));
	kb->page = KB_PAGE_ALPHA;
	clear_active(kb);
	clear_held(kb);
}

void kb_show(Kb *kb, bool show)
{
	kb->visible = show;
	kb->shift = false;
	kb->shift_lock = false;
	kb->ctrl = false;
	kb->alt = false;
	clear_active(kb);
	clear_held(kb);
}

bool kb_visible(const Kb *kb)
{
	return kb->visible;
}

/* --- drawing ------------------------------------------------------------ */

static void draw_key(int x, int y, int w, int h, const char *label, bool pressed,
                     bool latched)
{
	uint32_t bg = (pressed || latched) ? RENDER_COLOR_SELECT : 0xFF282828u;
	uint32_t fg = (pressed || latched) ? RENDER_COLOR_WHITE : 0xFFC8C8C8u;
	int lw = render_text_width(2, label);

	render_fill(x, y, w, h, bg);
	render_frame(x, y, w, h, pressed ? RENDER_COLOR_WHITE : 0xFF383838u,
	             pressed ? 2 : 1);
	render_text(x + (w - lw) / 2, y + (h - 16) / 2, 2, fg, label);
}

static bool is_latched(const Kb *kb, const KeyDef *key)
{
	switch (key->action) {
	case A_CTRL:
		return kb->ctrl;
	case A_ALT:
		return kb->alt;
	case A_SHIFT:
		return kb->shift || kb->shift_lock;
	default:
		return false;
	}
}

void kb_draw(const Kb *kb)
{
	const RowDef *rows = pages[kb->page];
	int i, r, c;

	render_fill(0, KB_Y, SCREEN_W, KB_H, RENDER_COLOR_PANEL);
	render_fill(0, KB_Y, SCREEN_W, 2, RENDER_COLOR_PANEL_EDGE);

	for (i = 0; i < FN_COUNT; i++) {
		int x, w;
		const KeyDef *key = &fn_keys[i];

		key_rect(fn_keys, FN_COUNT, i, fn_top(), KB_FN_H, &x, &w);
		draw_key(x, fn_top(), w, KB_FN_H, key->label,
		         kb->active_fn == i, is_latched(kb, key));
	}

	for (r = 0; r < KB_ROWS; r++) {
		const RowDef *row = &rows[r];
		int y = row_top(r);

		for (c = 0; c < row->count; c++) {
			const KeyDef *key = &row->keys[c];
			char upper[2];
			const char *label = key->label;
			int x, w;

			/*
			 * Shift affects the letter page only; the symbol pages are
			 * already one character per key, and a shifted digit is just
			 * another symbol key there.
			 */
			if (key->action == A_CHAR && kb->page == KB_PAGE_ALPHA &&
			    (kb->shift || kb->shift_lock) && key->ch >= 'a' &&
			    key->ch <= 'z') {
				upper[0] = (char)(key->ch - 'a' + 'A');
				upper[1] = '\0';
				label = upper;
			}
			if (key->action == A_SHIFT && kb->shift_lock) label = "Caps";

			key_rect(row->keys, row->count, c, y, KB_ROW_H, &x, &w);
			draw_key(x, y, w, KB_ROW_H, label,
			         kb->active_row == r && kb->active_col == c,
			         is_latched(kb, key));
		}
	}
}

/* --- emitting ----------------------------------------------------------- */

static int put(char *out, int n, int cap, char c)
{
	if (n < cap) out[n] = c;
	return n < cap ? n + 1 : n;
}

static void clear_modifiers(Kb *kb)
{
	kb->ctrl = false;
	kb->alt = false;
	if (!kb->shift_lock) kb->shift = false;
}

static int emit_char(Kb *kb, char ch, char *out, int cap)
{
	unsigned char c = (unsigned char)ch;
	int n = 0;

	if (kb->page == KB_PAGE_ALPHA && (kb->shift || kb->shift_lock) &&
	    c >= 'a' && c <= 'z')
		c = (unsigned char)(c - 'a' + 'A');

	if (kb->ctrl) {
		int code = -1;

		if (c >= 'a' && c <= 'z')
			code = c - 'a' + 1;
		else if (c >= 'A' && c <= 'Z')
			code = c - 'A' + 1;
		else {
			switch (c) {
			case '@': code = 0x00; break;
			case '[': code = 0x1b; break;
			case '\\': code = 0x1c; break;
			case ']': code = 0x1d; break;
			case '^': code = 0x1e; break;
			case '_': code = 0x1f; break;
			case '?': code = 0x7f; break;
			default: break;
			}
		}

		/* Ctrl with something that has no control form sends the key
		 * itself, which is what xterm does. */
		if (code >= 0) c = (unsigned char)code;
	}

	if (kb->alt) n = put(out, n, cap, '\x1b');
	n = put(out, n, cap, (char)c);

	clear_modifiers(kb);
	return n;
}

static int emit_action(Kb *kb, int action, bool cursor_keys, char *out, int cap)
{
	char seq[3];
	int n = 0;

	switch (action) {
	case A_SHIFT:
		if (kb->shift_lock) {
			kb->shift_lock = false;
			kb->shift = false;
		} else if (kb->shift) {
			/* Tapping an armed Shift locks it. */
			kb->shift_lock = true;
		} else {
			kb->shift = true;
		}
		return 0;
	case A_CTRL:
		kb->ctrl = !kb->ctrl;
		return 0;
	case A_ALT:
		kb->alt = !kb->alt;
		return 0;
	case A_PAGE_NUM:
		kb->page = KB_PAGE_NUM;
		clear_active(kb);
		return 0;
	case A_PAGE_SYM:
		kb->page = KB_PAGE_SYM;
		clear_active(kb);
		return 0;
	case A_PAGE_ALPHA:
		kb->page = KB_PAGE_ALPHA;
		clear_active(kb);
		return 0;
	case A_HIDE:
		return -1;
	default:
		break;
	}

	switch (action) {
	case A_ESC:
		n = put(out, n, cap, '\x1b');
		break;
	case A_TAB:
		n = put(out, n, cap, '\t');
		break;
	case A_ENTER:
		n = put(out, n, cap, '\r');
		break;
	case A_DEL:
		n = put(out, n, cap, '\x7f');
		break;
	case A_LEFT:
	case A_UP:
	case A_DOWN:
	case A_RIGHT:
		seq[0] = '\x1b';
		seq[1] = cursor_keys ? 'O' : '[';
		seq[2] = action == A_UP ? 'A' : action == A_DOWN ? 'B'
		       : action == A_RIGHT ? 'C' : 'D';
		n = put(out, n, cap, seq[0]);
		n = put(out, n, cap, seq[1]);
		n = put(out, n, cap, seq[2]);
		break;
	default:
		break;
	}

	clear_modifiers(kb);
	return n;
}

/* The emit above takes the key's character as an argument; this wrapper keeps
 * the switch readable. */
static int apply_key(Kb *kb, const KeyDef *key, bool cursor_keys, char *out,
                     int cap)
{
	if (key->action == A_CHAR) return emit_char(kb, key->ch, out, cap);
	return emit_action(kb, key->action, cursor_keys, out, cap);
}

static bool repeats(int action)
{
	return action == A_DEL || action == A_LEFT || action == A_UP ||
	       action == A_DOWN || action == A_RIGHT;
}

/* --- input -------------------------------------------------------------- */

static int hit_fn(int x, int y)
{
	int i;

	if (y < fn_top() || y >= fn_top() + KB_FN_H) return -1;

	for (i = 0; i < FN_COUNT; i++) {
		int kx, kw;

		key_rect(fn_keys, FN_COUNT, i, fn_top(), KB_FN_H, &kx, &kw);
		if (x >= kx && x < kx + kw) return i;
	}
	return -1;
}

static bool hit_main(const Kb *kb, int x, int y, int *row, int *col)
{
	const RowDef *rows = pages[kb->page];
	int r, c;

	for (r = 0; r < KB_ROWS; r++) {
		const RowDef *def = &rows[r];
		int top = row_top(r);

		if (y < top || y >= top + KB_ROW_H) continue;

		for (c = 0; c < def->count; c++) {
			int kx, kw;

			key_rect(def->keys, def->count, c, top, KB_ROW_H, &kx, &kw);
			if (x >= kx && x < kx + kw) {
				*row = r;
				*col = c;
				return true;
			}
		}
	}
	return false;
}

int kb_input(Kb *kb, const Touch *touch, bool cursor_keys, char *out, int cap)
{
	uint64_t now;

	if (!kb->visible) return 0;

	now = now_ms();

	if (touch->pressed) {
		int fn = hit_fn(touch->x, touch->y);
		int row = -1, col = -1;
		const KeyDef *key;
		int rc;

		if (fn >= 0) {
			key = &fn_keys[fn];
			kb->active_fn = fn;
			kb->active_row = -1;
			kb->held_fn = fn;
			kb->held_row = -1;
		} else if (hit_main(kb, touch->x, touch->y, &row, &col)) {
			key = &pages[kb->page][row].keys[col];
			kb->active_fn = -1;
			kb->active_row = row;
			kb->active_col = col;
			kb->held_fn = -1;
			kb->held_row = row;
			kb->held_col = col;
		} else {
			clear_active(kb);
			clear_held(kb);
			return 0;
		}

		kb->held_action = key->action;
		kb->repeat_at = now + KB_REPEAT_DELAY_MS;

		rc = apply_key(kb, key, cursor_keys, out, cap);
		return rc;
	}

	if (touch->down) {
		if (repeats(kb->held_action) && now >= kb->repeat_at) {
			kb->repeat_at = now + KB_REPEAT_RATE_MS;
			return emit_action(kb, kb->held_action, cursor_keys, out, cap);
		}
		return 0;
	}

	/* Released: the next frame is a fresh press. */
	clear_active(kb);
	clear_held(kb);
	return 0;
}
