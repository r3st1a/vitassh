/*
 * Screens, overlays and the application state machine.
 *
 * Three screens: the host list, the terminal, and a message screen that carries
 * errors. On top of the terminal there are three overlays -- the connect
 * spinner, the host key prompt and the control-key panel -- which is where the
 * buttons a DualShock does not have (Ctrl-C, Esc, F-keys) live.
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "ime.h"
#include "input.h"
#include "keyboard.h"
#include "log.h"
#include "net.h"
#include "paths.h"
#include "render.h"
#include "ssh.h"
#include "term.h"
#include "touch.h"
#include "vault.h"
#include "vt.h"

typedef enum {
	SCREEN_HOSTS,
	SCREEN_TERMINAL,
	SCREEN_MESSAGE,
} Screen;

typedef enum {
	IME_FOR_NONE,
	IME_FOR_PASSWORD,
	IME_FOR_HOST_FIELD,
	/*
	 * The password row of the host form. Deliberately not IME_FOR_PASSWORD:
	 * that one hands what was typed to the running connection, which would
	 * both send the server a password it did not ask for and lose what the
	 * user meant to save.
	 */
	IME_FOR_FORM_PASSWORD,
} ImePurpose;

/* --- control-key panel -------------------------------------------------- */

typedef struct {
	const char *label;
	const char *seq;
} KeypadItem;

/*
 * xterm's default encodings, which is what the PTY advertises (TERM=xterm), so
 * the remote's terminfo will decode these correctly.
 */
static const KeypadItem keypad_items[] = {
	{"Ctrl-C", "\x03"},    {"Ctrl-D", "\x04"},    {"Ctrl-Z", "\x1a"},
	{"Ctrl-L", "\x0c"},    {"Ctrl-A", "\x01"},    {"Ctrl-E", "\x05"},
	{"Ctrl-K", "\x0b"},    {"Ctrl-U", "\x15"},    {"Ctrl-R", "\x12"},
	{"Ctrl-W", "\x17"},    {"Esc", "\x1b"},       {"Tab", "\t"},
	{"PgUp", "\x1b[5~"},   {"PgDn", "\x1b[6~"},   {"Home", "\x1b[H"},
	{"End", "\x1b[F"},     {"Ins", "\x1b[2~"},    {"Del", "\x1b[3~"},
	{"F1", "\x1bOP"},      {"F2", "\x1bOQ"},      {"F3", "\x1bOR"},
	{"F4", "\x1bOS"},      {"F5", "\x1b[15~"},    {"F6", "\x1b[17~"},
	{"F7", "\x1b[18~"},    {"F8", "\x1b[19~"},    {"F9", "\x1b[20~"},
	{"F10", "\x1b[21~"},   {"F11", "\x1b[23~"},   {"F12", "\x1b[24~"},
};

#define KEYPAD_COLS 6
#define KEYPAD_ROWS 5
#define KEYPAD_COUNT ((int)(sizeof(keypad_items) / sizeof(keypad_items[0])))
#define KEYPAD_CLOSE KEYPAD_COUNT /* one extra slot below the grid */

/* --- host list geometry ------------------------------------------------- */

#define HOST_LIST_Y 120
#define HOST_ROW_H 24
#define HOST_ROWS CFG_MAX_HOSTS /* the cap fits: see the assert below */
#define HOST_ROW_LAST (HOST_LIST_Y + HOST_ROWS * HOST_ROW_H)

/* The list has no scroll offset, so it must not reach the status bar. */
_Static_assert(HOST_ROW_LAST <= STATUS_Y, "host list overlaps the status bar");

/* --- host form geometry ------------------------------------------------- */

#define FORM_X 80
#define FORM_Y 40
#define FORM_W (SCREEN_W - 2 * FORM_X)
#define FORM_H 440
#define FORM_ROW_H 48
#define FORM_LABEL_X (FORM_X + 32)
#define FORM_VALUE_X (FORM_X + 260)
#define FORM_LIST_Y (FORM_Y + 64)

enum {
	FORM_ROW_NAME,
	FORM_ROW_HOST,
	FORM_ROW_PORT,
	FORM_ROW_USER,
	FORM_ROW_PASSWORD,
	FORM_ROW_SAVE,
	FORM_ROW_DELETE,
	FORM_ROW_COUNT,
};

/*
 * What the password row will do when the form is committed. PW_KEEP is zero
 * because form_begin() clears the form with memset and the ordinary case for
 * an untouched row is to leave the store alone -- including when the store is
 * unavailable, which must not stop a host from being renamed.
 */
enum {
	PW_KEEP = 0, /* leave whatever is saved for this key alone */
	PW_SET,      /* save what was typed */
	PW_CLEAR,    /* forget what is saved */
};

/*
 * The add/edit form. The fields are held as strings rather than as a HostEntry
 * so the user can clear one and get the default back; the conversion happens
 * once, in form_commit(), through config_entry_make().
 *
 * The password is held the same way, but it is never scrubbed by
 * config_field_clean(): a password is not a field of a line-based file, so a
 * '|' or a control byte in it is the user's business.
 */
typedef struct {
	bool open;
	bool is_new;
	bool delete_armed;
	int row;     /* highlighted row */
	int editing; /* row whose keyboard is up, or -1 */
	char error[64];
	char name[CFG_NAME_CAP];
	char host[CFG_HOST_CAP];
	char port[8];
	char user[CFG_USER_CAP];
	char password[VAULT_PASS_CAP];
	int pw_action;
	bool pw_prefilled; /* the keyboard was opened holding the saved password */
} HostForm;

/* --- control panel geometry --------------------------------------------- */

#define KP_X 20
#define KP_Y 52
#define KP_W (SCREEN_W - 2 * KP_X)
#define KP_H (SCREEN_H - KP_Y - 40)
#define KP_PAD 20
#define KP_GAP 8
#define KP_BTN_W ((KP_W - 2 * KP_PAD - (KEYPAD_COLS - 1) * KP_GAP) / KEYPAD_COLS)
#define KP_BTN_H 64
#define KP_GRID_X (KP_X + KP_PAD)
#define KP_GRID_Y (KP_Y + KP_PAD)
#define KP_CLOSE_H 48

/* --- state -------------------------------------------------------------- */

typedef struct {
	Screen screen;

	Term term; /* ~285 KB, so the app struct is static */
	VtParser vt;
	HostList hosts;
	Vault vault; /* saved passwords; never holds plaintext at rest */
	HostForm form;
	Ssh ssh;
	Input in;
	Touch touch;
	Kb kb;

	int selected;
	int scroll; /* scrollback offset; 0 is the live screen */

	bool keypad_open;
	int keypad_index;

	bool hostkey_prompt;
	bool quitting;

	/*
	 * Whether the saved password has already been tried on this connection.
	 * A rejected password sends the state machine back to asking, and a
	 * second silent attempt would keep the keyboard away for all three
	 * tries -- so the answer goes to the user the second time it is asked.
	 */
	bool saved_tried;

	ImePurpose ime_purpose;

	char peer[96]; /* "user@host" for the status bar */

	char message_title[64];
	char message_body[256];
} App;

static App app;

/* --- forward declarations ----------------------------------------------- */

/* The form is driven from handle_ime_result(), hosts_input() and draw_hosts(),
 * which sit on either side of its definition. */
static void form_begin(bool is_new);
static void form_input(const Input *in);
static void form_field_set(int row, const char *text);
static void copy_utf8(char *dst, size_t cap, const char *src);

/* --- small helpers ------------------------------------------------------ */

static void draw_centered(int y, int scale, uint32_t color, const char *text)
{
	render_text((SCREEN_W - render_text_width(scale, text)) / 2, y, scale, color, text);
}

/* Greedy word wrap; the message screen is the only caller and it is all 1x. */
static void draw_wrapped(int x, int y, uint32_t color, const char *text, int width)
{
	char line[120];
	const char *p = text;
	int maxc = width / 8;
	size_t n = 0;

	if (maxc > (int)sizeof(line) - 1) maxc = (int)sizeof(line) - 1;

	while (*p != '\0') {
		const char *word = p;
		size_t wl = 0;

		while (word[wl] != '\0' && word[wl] != ' ') wl++;

		if (n > 0 && n + 1 + wl > (size_t)maxc) {
			line[n] = '\0';
			render_text(x, y, 1, color, line);
			y += 10;
			n = 0;
		}

		if (n > 0 && n + 1 < sizeof(line)) line[n++] = ' ';

		if (wl > sizeof(line) - 1 - n) wl = sizeof(line) - 1 - n;
		memcpy(line + n, word, wl);
		n += wl;

		p = word + wl;
		while (*p == ' ') p++;
	}

	if (n > 0) {
		line[n] = '\0';
		render_text(x, y, 1, color, line);
	}
}

static void draw_status(const char *info, const char *right, const char *hints)
{
	render_fill(0, STATUS_Y, SCREEN_W, STATUS_H, RENDER_COLOR_PANEL);

	render_text(6, STATUS_Y + 3, 1, RENDER_COLOR_DIM, info);

	if (right != NULL && *right != '\0') {
		int w = render_text_width(1, right);

		render_text(SCREEN_W - 6 - w, STATUS_Y + 3, 1, RENDER_COLOR_WARN, right);
	}

	if (hints != NULL && *hints != '\0')
		render_text(6, STATUS_Y + 19, 1, RENDER_COLOR_DIM, hints);
}

/* --- transitions -------------------------------------------------------- */

/*
 * Wipes what was typed into the password row. App is static and lives for the
 * whole session, so anything left here stays in memory until the process ends;
 * every path that closes the form has to come through this.
 */
static void form_forget_password(void)
{
	memset(app.form.password, 0, sizeof(app.form.password));
	app.form.pw_action = PW_KEEP;
	app.form.pw_prefilled = false;
}

static void load_hosts(void)
{
	int n = config_load(&app.hosts, VITASSH_HOSTS);

	if (n < 0) {
		log_printf("ui: could not read %s", VITASSH_HOSTS);
		app.hosts.count = 0;
	}

	if (app.selected >= app.hosts.count)
		app.selected = app.hosts.count - 1;
	if (app.selected < 0) app.selected = 0;

	log_printf("ui: %d host(s)", app.hosts.count);
}

static void show_message(const char *title, const char *body)
{
	snprintf(app.message_title, sizeof(app.message_title), "%s",
	         title != NULL ? title : "");
	snprintf(app.message_body, sizeof(app.message_body), "%s",
	         body != NULL ? body : "");

	app.keypad_open = false;
	app.hostkey_prompt = false;
	app.form.open = false;
	app.form.editing = -1;
	kb_show(&app.kb, false);
	form_forget_password();
	app.scroll = 0;
	app.screen = SCREEN_MESSAGE;

	log_printf("ui: message \"%s\" -- %s", app.message_title, app.message_body);
}

static void back_to_hosts(void)
{
	ssh_stop(&app.ssh);
	app.keypad_open = false;
	app.hostkey_prompt = false;
	app.form.open = false;
	app.form.editing = -1;
	kb_show(&app.kb, false);
	form_forget_password();
	app.scroll = 0;
	app.screen = SCREEN_HOSTS;
	load_hosts();
}

static void start_connection(void)
{
	const HostEntry *h = &app.hosts.item[app.selected];
	int rc;

	term_reset(&app.term);
	vt_init(&app.vt);
	app.scroll = 0;
	app.keypad_open = false;
	app.hostkey_prompt = false;
	kb_show(&app.kb, false);
	app.saved_tried = false;
	snprintf(app.peer, sizeof(app.peer), "%s@%s:%d", h->user, h->host, h->port);

	rc = ssh_start(&app.ssh, h);
	if (rc != NET_OK) {
		show_message("Cannot connect", app.ssh.message);
		return;
	}

	app.screen = SCREEN_TERMINAL;
}

/* --- IME ---------------------------------------------------------------- */

static void open_password_dialog(void)
{
	char title[128];

	snprintf(title, sizeof(title), "Password for %s@%s", app.ssh.host.user,
	         app.ssh.host.host);

	/* The system dialog owns the screen and the buttons while it is up. */
	kb_show(&app.kb, false);

	if (ime_open(title, "", 128, IME_KIND_TEXT, true) == 0)
		app.ime_purpose = IME_FOR_PASSWORD;
	else
		log_printf("ui: could not open the password dialog");
}

static void handle_ime_result(ImeStatus status)
{
	if (app.ime_purpose == IME_FOR_PASSWORD) {
		if (status == IME_ACCEPTED) {
			ssh_provide_password(&app.ssh, ime_text());
		} else {
			log_printf("ui: password entry cancelled");
			show_message("Cancelled",
			             "No password was entered, so the connection was dropped.");
		}
	} else if (app.ime_purpose == IME_FOR_HOST_FIELD) {
		int row = app.form.editing;

		/*
		 * Copied here and nowhere else: ime_text() points at a buffer that the
		 * next ime_open() overwrites, and this is the only frame it is valid on.
		 */
		if (status == IME_ACCEPTED && app.form.open && row >= FORM_ROW_NAME &&
		    row <= FORM_ROW_USER)
			form_field_set(row, ime_text());

		/* Cleared on cancel too, so the next X opens a keyboard again. */
		if (app.form.open) app.form.editing = -1;
	} else if (app.ime_purpose == IME_FOR_FORM_PASSWORD) {
		if (status == IME_ACCEPTED && app.form.open) {
			const char *text = ime_text();

			if (text[0] == '\0' && app.form.pw_prefilled) {
				/* Opened holding the saved password and came back
				 * empty: that is how one is forgotten. */
				app.form.pw_action = PW_CLEAR;
				memset(app.form.password, 0, sizeof(app.form.password));
			} else if (strlen(text) >= VAULT_PASS_CAP) {
				/*
				 * Refused rather than cut short. A silently truncated
				 * password would be saved, then rejected by the server
				 * with nothing on screen to say why.
				 */
				snprintf(app.form.error, sizeof(app.form.error),
				         "Too long by %d bytes.",
				         (int)(strlen(text) - (VAULT_PASS_CAP - 1)));
			} else if (text[0] != '\0') {
				copy_utf8(app.form.password, sizeof(app.form.password),
				          text);
				app.form.pw_action = PW_SET;
			}
			/*
			 * A blank field that was not prefilled changes nothing: an
			 * empty password is not something to store by accident, and
			 * there is no saved one to forget.
			 */
		}

		/* A cancel leaves pw_action alone, so O cannot throw away a saved
		 * password on the way out of the keyboard. */
		app.form.pw_prefilled = false;
		if (app.form.open) app.form.editing = -1;
	}

	app.ime_purpose = IME_FOR_NONE;
}

/* --- ssh plumbing ------------------------------------------------------- */

static void drain_vt_replies(void)
{
	char buf[VT_OUTPUT_CAP];
	size_t n;

	while ((n = vt_take_output(&app.vt, buf, sizeof(buf))) > 0)
		ssh_send(&app.ssh, buf, n);
}

static void tick_terminal(void)
{
	SshEvent ev;

	if (app.ssh.state == SSH_IDLE) return;

	ev = ssh_tick(&app.ssh, &app.term, &app.vt);
	drain_vt_replies();

	switch (ev) {
	case SSH_EVENT_NEED_PASSWORD: {
		char pw[VAULT_PASS_CAP];

		/*
		 * A saved password is offered once, without asking. If the server
		 * refuses it the machine asks again -- and that second ask has to
		 * reach the user, or an out-of-date password would be retried
		 * until the connection dropped and the keyboard would never
		 * appear, which looks exactly like the app being stuck.
		 */
		if (!app.saved_tried &&
		    vault_lookup(&app.vault, &app.ssh.host, pw, sizeof(pw)) == 0) {
			log_printf("ui: trying the saved password for %s", app.peer);
			ssh_provide_password(&app.ssh, pw);
		} else {
			open_password_dialog();
		}
		app.saved_tried = true;

		memset(pw, 0, sizeof(pw));
		break;
	}
	case SSH_EVENT_NEED_HOSTKEY:
		app.hostkey_prompt = true;
		break;
	case SSH_EVENT_CLOSED:
		show_message("Connection closed", app.ssh.message);
		break;
	case SSH_EVENT_FAILED:
		show_message("Connection failed", app.ssh.message);
		break;
	default:
		break;
	}
}

/* --- per-screen input --------------------------------------------------- */

static void send_arrow(unsigned bit)
{
	char seq[3];

	seq[0] = 0x1B;
	seq[1] = app.vt.app_cursor_keys ? 'O' : '[';
	seq[2] = (bit == BTN_UP) ? 'A' : (bit == BTN_DOWN) ? 'B'
	       : (bit == BTN_RIGHT) ? 'C' : 'D';

	ssh_send(&app.ssh, seq, sizeof(seq));
}

static void hosts_input(const Input *in)
{
	if (app.hosts.count > 0) {
		if ((in->repeated & BTN_UP) && app.selected > 0) app.selected--;
		if ((in->repeated & BTN_DOWN) && app.selected + 1 < app.hosts.count)
			app.selected++;
	}

	if (in->pressed & BTN_TRIANGLE) {
		form_begin(true);
		return;
	}

	if (in->pressed & BTN_SQUARE) {
		if (app.hosts.count > 0) form_begin(false);
		return;
	}

	if (in->pressed & BTN_CROSS) {
		if (app.hosts.count > 0) start_connection();
		return;
	}

	if (in->pressed & BTN_SELECT) {
		int keep = app.selected;

		load_hosts();
		app.selected = keep;
		if (app.selected >= app.hosts.count) app.selected = app.hosts.count - 1;
		if (app.selected < 0) app.selected = 0;
		return;
	}

	if (in->pressed & BTN_CIRCLE) app.quitting = true;
}

/* --- host form ---------------------------------------------------------- */

/*
 * Bounded copy that will not cut a UTF-8 sequence in half. The field caps count
 * bytes while the keyboard counts UTF-16 units, so a name of 31 Chinese
 * characters arrives as 93 bytes.
 */
static void copy_utf8(char *dst, size_t cap, const char *src)
{
	size_t n;

	if (cap == 0) return;

	n = strlen(src);
	if (n > cap - 1) {
		n = cap - 1;
		while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
	}

	memcpy(dst, src, n);
	dst[n] = '\0';
}

static void form_begin(bool is_new)
{
	memset(&app.form, 0, sizeof(app.form));
	app.form.is_new = is_new;
	app.form.editing = -1;

	if (is_new && app.hosts.count >= CFG_MAX_HOSTS) {
		char body[128];

		snprintf(body, sizeof(body),
		         "VitaSSH keeps at most %d hosts. Delete one before adding "
		         "another.",
		         CFG_MAX_HOSTS);
		show_message("Host list full", body);
		return;
	}

	if (!is_new) {
		const HostEntry *h = &app.hosts.item[app.selected];

		snprintf(app.form.name, sizeof(app.form.name), "%s", h->name);
		snprintf(app.form.host, sizeof(app.form.host), "%s", h->host);
		snprintf(app.form.port, sizeof(app.form.port), "%d", h->port);
		snprintf(app.form.user, sizeof(app.form.user), "%s", h->user);
	}

	app.form.open = true;
	log_printf("ui: host form open (%s)", is_new ? "new" : "edit");
}

static void form_field_set(int row, const char *text)
{
	switch (row) {
	case FORM_ROW_NAME:
		copy_utf8(app.form.name, sizeof(app.form.name), text);
		config_field_clean(app.form.name);
		break;
	case FORM_ROW_HOST:
		copy_utf8(app.form.host, sizeof(app.form.host), text);
		config_field_clean(app.form.host);
		break;
	case FORM_ROW_PORT: {
		size_t o = 0;
		size_t i;

		for (i = 0; text[i] != '\0' && o < sizeof(app.form.port) - 1; i++) {
			if (text[i] >= '0' && text[i] <= '9')
				app.form.port[o++] = text[i];
		}
		app.form.port[o] = '\0';
		break;
	}
	case FORM_ROW_USER:
		copy_utf8(app.form.user, sizeof(app.form.user), text);
		config_field_clean(app.form.user);
		break;
	default:
		return;
	}

	app.form.error[0] = '\0';
}

/*
 * ascii_to_utf16() in ime.c maps bytes to code points, so a field that already
 * holds UTF-8 would come back doubled. Anything that is offered back to the
 * keyboard has to pass this first.
 */
static bool is_ascii(const char *s)
{
	for (; *s != '\0'; s++)
		if ((unsigned char)*s >= 0x80) return false;
	return true;
}

static void form_edit_row(int row)
{
	static const struct {
		const char *title;
		ImeKind kind;
		int max_len;
	} spec[FORM_ROW_COUNT] = {
		{ "Host name", IME_KIND_TEXT,   CFG_NAME_CAP - 1 },
		{ "Host or IP", IME_KIND_HOST,  CFG_HOST_CAP - 1 },
		{ "Port",      IME_KIND_NUMBER, 5 },
		{ "User name", IME_KIND_TEXT,   CFG_USER_CAP - 1 },
		{ "Password",  IME_KIND_TEXT,   VAULT_PASS_CAP - 1 },
		{ NULL,        IME_KIND_TEXT,   0 },
		{ NULL,        IME_KIND_TEXT,   0 },
	};
	char saved[VAULT_PASS_CAP] = {0};
	char initial[VAULT_PASS_CAP] = {0};
	const char *field;

	if (row < FORM_ROW_NAME || row > FORM_ROW_PASSWORD) return;

	app.form.pw_prefilled = false;

	if (row == FORM_ROW_PASSWORD) {
		HostEntry key;

		/*
		 * Offer the saved password back, but only when it is entirely
		 * ASCII, for the same reason the text fields are blanked. An empty
		 * result then means "forget it", which is the only way to clear a
		 * saved password without deleting the host.
		 */
		if (app.vault.ready &&
		    config_entry_make(app.form.name, app.form.host, app.form.port,
		                      app.form.user, &key) == 0 &&
		    vault_lookup(&app.vault, &key, saved, sizeof(saved)) == 0 &&
		    is_ascii(saved)) {
			snprintf(initial, sizeof(initial), "%s", saved);
			app.form.pw_prefilled = true;
		}

		/* ime_open() has taken its own copy by the time it returns. */
		memset(saved, 0, sizeof(saved));

		if (ime_open(spec[row].title, initial, spec[row].max_len,
		             spec[row].kind, true) != 0) {
			log_printf("ui: could not open the keyboard for the host form");
			return;
		}

		app.form.editing = row;
		app.ime_purpose = IME_FOR_FORM_PASSWORD;
		app.form.error[0] = '\0';
		return;
	}

	switch (row) {
	case FORM_ROW_NAME: field = app.form.name; break;
	case FORM_ROW_HOST: field = app.form.host; break;
	case FORM_ROW_PORT: field = app.form.port; break;
	default:            field = app.form.user; break;
	}

	snprintf(initial, sizeof(initial), "%s", is_ascii(field) ? field : "");

	if (ime_open(spec[row].title, initial, spec[row].max_len, spec[row].kind,
	             false) != 0) {
		log_printf("ui: could not open the keyboard for the host form");
		return;
	}

	app.form.editing = row;
	app.ime_purpose = IME_FOR_HOST_FIELD;
	app.form.error[0] = '\0';
}

/* Turns the form's fields into an entry, or reports why they cannot be. */
static int form_entry(HostEntry *out)
{
	if (config_entry_make(app.form.name, app.form.host, app.form.port,
	                      app.form.user, out) != 0) {
		snprintf(app.form.error, sizeof(app.form.error), "%s",
		         "A host name or address is required.");
		return -1;
	}

	/*
	 * The parser reads a leading '#' as a comment, so this name would take
	 * the whole host with it on the next load.
	 */
	if (out->name[0] == '#') {
		snprintf(app.form.error, sizeof(app.form.error), "%s",
		         "The name cannot start with '#'.");
		return -1;
	}
	return 0;
}

/*
 * Whether any host in the list answers to this key. Two hosts may share host,
 * user and port and differ only in name, so this has to be asked of the whole
 * list before a password is dropped as orphaned.
 */
static bool key_in_use(const HostList *list, const HostEntry *key)
{
	int i;

	for (i = 0; i < list->count; i++) {
		const HostEntry *h = &list->item[i];

		if (h->port == key->port && strcmp(h->host, key->host) == 0 &&
		    strcmp(h->user, key->user) == 0)
			return true;
	}
	return false;
}

/* Writes the edited list out. `deleting` drops the selected host instead. */
static void form_commit(bool deleting)
{
	static HostList staged;    /* ~2 KB, and for the same reason App is static */
	static Vault staged_vault; /* ~4 KB, for the same reason */
	HostEntry entry;
	HostEntry old_key;
	bool have_old = false;
	bool vault_touched = false;
	int selected;

	memset(&entry, 0, sizeof(entry));
	memset(&old_key, 0, sizeof(old_key));

	staged = app.hosts;
	staged_vault = app.vault;

	if (deleting) {
		int i;

		if (app.form.is_new || staged.count <= 0) return;

		/*
		 * Read the key before the shift. Afterwards that slot holds the
		 * next host, and the password would be looked for under the wrong
		 * address.
		 */
		old_key = staged.item[app.selected];
		have_old = true;

		for (i = app.selected; i + 1 < staged.count; i++)
			staged.item[i] = staged.item[i + 1];
		staged.count--;
		selected = app.selected;
	} else {
		if (form_entry(&entry) != 0) return;

		if (!app.form.is_new) {
			old_key = staged.item[app.selected];
			have_old = true;
		}

		if (app.form.is_new) {
			if (staged.count >= CFG_MAX_HOSTS) {
				snprintf(app.form.error, sizeof(app.form.error),
				         "The list already holds %d hosts.", CFG_MAX_HOSTS);
				return;
			}
			staged.item[staged.count++] = entry;
			selected = staged.count - 1;
		} else {
			staged.item[app.selected] = entry;
			selected = app.selected;
		}
	}

	/*
	 * PW_KEEP never comes near the store, so a store that cannot be used
	 * does not also stop hosts from being added, renamed or reordered.
	 */
	if (app.form.pw_action != PW_KEEP && !staged_vault.ready) {
		snprintf(app.form.error, sizeof(app.form.error), "%s",
		         "The saved passwords are unavailable.");
		return;
	}

	/*
	 * A key that no host answers to any more takes its password with it.
	 * This covers a deleted host and one whose address or user was edited;
	 * leaving either behind would let orphans fill the sixteen slots.
	 */
	if (have_old && !key_in_use(&staged, &old_key) &&
	    vault_has(&staged_vault, &old_key)) {
		vault_remove(&staged_vault, &old_key);
		vault_touched = true;
	}

	if (!deleting && app.form.pw_action == PW_SET) {
		if (vault_set(&staged_vault, &entry, app.form.password) != 0) {
			/* Either a field does not fit or all sixteen slots are
			 * taken; to the user those are the same answer. */
			snprintf(app.form.error, sizeof(app.form.error), "%s",
			         "No room for another saved password.");
			return;
		}
		vault_touched = true;
	} else if (!deleting && app.form.pw_action == PW_CLEAR) {
		vault_remove(&staged_vault, &entry);
		vault_touched = true;
	}

	/*
	 * The vault goes first. If it cannot be written, hosts.txt is left
	 * exactly as it was, and there is no state where a host is listed with a
	 * password that was never stored.
	 */
	if (vault_touched && vault_save(&staged_vault, VITASSH_SECRETS) != 0) {
		snprintf(app.form.error, sizeof(app.form.error), "%s",
		         "The saved passwords could not be written.");
		return;
	}

	/*
	 * Only the staged copies have been touched, so a failed write leaves the
	 * session exactly as it was -- there is nothing to roll back. The two
	 * files cannot be committed together; that much is unavoidable.
	 */
	if (config_save(&staged, VITASSH_HOSTS) != 0) {
		char body[160];

		/*
		 * The vault is adopted even so. It is on the card now, and
		 * keeping the old one in memory would let the next save quietly
		 * write the passwords back to what they used to be.
		 */
		if (vault_touched) {
			app.vault = staged_vault;
			log_printf("ui: %s written but %s was not", VITASSH_SECRETS,
			           VITASSH_HOSTS);
		}

		/*
		 * The two files cannot be committed together, so when only one of
		 * them made it the message has to say which.
		 */
		if (vault_touched)
			snprintf(body, sizeof(body),
			         "%s could not be written. The saved passwords were.",
			         VITASSH_HOSTS);
		else
			snprintf(body, sizeof(body),
			         "%s could not be written. Nothing was changed.",
			         VITASSH_HOSTS);
		app.form.open = false;
		app.form.editing = -1;
		show_message("Could not save", body);
		return;
	}

	if (vault_touched) app.vault = staged_vault;
	app.hosts = staged;
	app.selected = selected;
	if (app.selected >= app.hosts.count) app.selected = app.hosts.count - 1;
	if (app.selected < 0) app.selected = 0;

	app.form.open = false;
	app.form.editing = -1;
	form_forget_password();
	log_printf("ui: host form %s, %d host(s)", deleting ? "deleted" : "saved",
	           app.hosts.count);
}

static void form_input(const Input *in)
{
	unsigned dirs = in->pressed & (BTN_UP | BTN_DOWN);
	int last = app.form.is_new ? FORM_ROW_SAVE : FORM_ROW_DELETE;

	if (in->pressed & BTN_CIRCLE) {
		/* The first O backs out of a pending delete, the second closes. */
		if (app.form.delete_armed) {
			app.form.delete_armed = false;
		} else {
			app.form.open = false;
			form_forget_password();
			log_printf("ui: host form cancelled");
		}
		return;
	}

	if (dirs != 0) {
		if (dirs & BTN_UP) app.form.row--;
		if (dirs & BTN_DOWN) app.form.row++;

		if (app.form.row < FORM_ROW_NAME) app.form.row = FORM_ROW_NAME;
		if (app.form.row > last) app.form.row = last;

		app.form.delete_armed = false;
		app.form.error[0] = '\0';
		return;
	}

	if (in->pressed & BTN_CROSS) {
		switch (app.form.row) {
		case FORM_ROW_NAME:
		case FORM_ROW_HOST:
		case FORM_ROW_PORT:
		case FORM_ROW_USER:
		case FORM_ROW_PASSWORD:
			form_edit_row(app.form.row);
			break;
		case FORM_ROW_SAVE:
			form_commit(false);
			break;
		case FORM_ROW_DELETE:
			if (app.form.is_new) break;
			if (app.form.delete_armed) {
				form_commit(true);
			} else {
				app.form.delete_armed = true;
				app.form.error[0] = '\0';
			}
			break;
		default:
			break;
		}
	}
}

static void keypad_input(const Input *in)
{
	unsigned dirs = in->pressed & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT);

	if (in->pressed & (BTN_SELECT | BTN_CIRCLE)) {
		app.keypad_open = false;
		return;
	}

	if (dirs != 0) {
		int idx = app.keypad_index;
		int col, row;

		if (idx == KEYPAD_CLOSE) {
			if (dirs & BTN_UP) app.keypad_index = KEYPAD_COUNT - KEYPAD_COLS;
			return;
		}

		col = idx % KEYPAD_COLS;
		row = idx / KEYPAD_COLS;

		if (dirs & BTN_LEFT) col--;
		if (dirs & BTN_RIGHT) col++;
		if (dirs & BTN_UP) row--;
		if (dirs & BTN_DOWN) row++;

		if (dirs & BTN_DOWN && row >= KEYPAD_ROWS) {
			/* Stepping off the bottom of the grid lands on Close. */
			if (row == KEYPAD_ROWS && col < KEYPAD_COLS) {
				app.keypad_index = KEYPAD_CLOSE;
				return;
			}
			row = KEYPAD_ROWS - 1;
		}

		if (col < 0) col = 0;
		if (col >= KEYPAD_COLS) col = KEYPAD_COLS - 1;
		if (row < 0) row = 0;
		if (row >= KEYPAD_ROWS) row = KEYPAD_ROWS - 1;

		app.keypad_index = row * KEYPAD_COLS + col;
		return;
	}

	if (in->pressed & BTN_CROSS) {
		const KeypadItem *item;

		if (app.keypad_index == KEYPAD_CLOSE) {
			app.keypad_open = false;
			return;
		}

		item = &keypad_items[app.keypad_index];
		log_printf("ui: keypad %s", item->label);
		ssh_send(&app.ssh, item->seq, strlen(item->seq));
		/* Stay open: control keys tend to be used in bursts. */
	}
}

static void terminal_input(const Input *in)
{
	const int page = TERM_ROWS - 2;
	int sb = term_sb_count(&app.term);

	/*
	 * The touch keyboard belongs to the live view. Entering scrollback puts it
	 * away so the whole screen is the history; tapping the terminal brings it
	 * back.
	 */
	if (app.scroll > 0 && kb_visible(&app.kb)) kb_show(&app.kb, false);

	if (app.scroll == 0) {
		if (kb_visible(&app.kb)) {
			char out[8];
			int n = kb_input(&app.kb, &app.touch, app.vt.app_cursor_keys,
			                 out, sizeof(out));

			if (n < 0)
				kb_show(&app.kb, false);
			else if (n > 0)
				ssh_send(&app.ssh, out, (size_t)n);
		} else if (app.touch.pressed) {
			kb_show(&app.kb, true);
			app.keypad_open = false;
		}
	}

	if (app.scroll > 0) {
		/* Scrollback: the d-pad drives the view, not the remote. */
		if (in->repeated & BTN_UP) app.scroll++;
		if (in->repeated & BTN_DOWN) app.scroll--;
		if (in->long_press & BTN_UP) app.scroll = sb;
		if (in->long_press & BTN_DOWN) app.scroll = 0;
		if (in->pressed & BTN_L) app.scroll += page;
		if (in->pressed & BTN_R) app.scroll -= page;

		if (app.scroll > sb) app.scroll = sb;
		if (app.scroll < 0) app.scroll = 0;

		if (in->pressed & BTN_CROSS) app.scroll = 0;
		if (in->pressed & BTN_SELECT) {
			kb_show(&app.kb, false);
			app.keypad_open = true;
			app.keypad_index = 0;
		}
		if (in->pressed & BTN_CIRCLE) back_to_hosts();
		return;
	}

	if ((in->repeated & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT)) != 0) {
		/* Send each held direction rather than picking one; holding two is
		 * rare and the remote can sort out the order. */
		if (in->repeated & BTN_UP) send_arrow(BTN_UP);
		if (in->repeated & BTN_DOWN) send_arrow(BTN_DOWN);
		if (in->repeated & BTN_LEFT) send_arrow(BTN_LEFT);
		if (in->repeated & BTN_RIGHT) send_arrow(BTN_RIGHT);
		return;
	}

	if (in->pressed & BTN_L) {
		kb_show(&app.kb, false);
		app.scroll = page;
		if (app.scroll > sb) app.scroll = sb;
		return;
	}

	if (in->pressed & BTN_CROSS) ssh_send(&app.ssh, "\r", 1);
	if (in->pressed & BTN_TRIANGLE) ssh_send(&app.ssh, "\t", 1);
	if (in->pressed & BTN_SQUARE) ssh_send(&app.ssh, "\x7f", 1);
	if (in->long_press & BTN_SQUARE) ssh_send(&app.ssh, "\x15", 1); /* Ctrl-U */
	if (in->pressed & BTN_START) {
		kb_show(&app.kb, !kb_visible(&app.kb));
		app.keypad_open = false;
	}
	if (in->pressed & BTN_SELECT) {
		kb_show(&app.kb, false);
		app.keypad_open = true;
		app.keypad_index = 0;
	}
	if (in->pressed & BTN_CIRCLE) back_to_hosts();
}

static void handle_input(void)
{
	if (app.screen == SCREEN_HOSTS) {
		/* The form swallows everything, SELECT included: reloading the file
		 * underneath it would move app.selected out from under the edit. */
		if (app.form.open) {
			form_input(&app.in);
			return;
		}

		hosts_input(&app.in);
		return;
	}

	if (app.screen == SCREEN_MESSAGE) {
		if (app.in.pressed & (BTN_CROSS | BTN_CIRCLE)) back_to_hosts();
		return;
	}

	if (app.hostkey_prompt) {
		if (app.in.pressed & BTN_L) {
			app.hostkey_prompt = false;
			log_printf("ui: host key accepted");
			ssh_accept_hostkey(&app.ssh, true);
		} else if (app.in.pressed & BTN_R) {
			app.hostkey_prompt = false;
			log_printf("ui: host key rejected");
			ssh_accept_hostkey(&app.ssh, false);
		}
		return;
	}

	if (app.keypad_open) {
		keypad_input(&app.in);
		return;
	}

	if (!ssh_running(&app.ssh)) {
		/* Still connecting, or waiting on the user: the only useful button is
		 * an escape hatch back to the host list. */
		if (app.in.pressed & BTN_CIRCLE) back_to_hosts();
		return;
	}

	terminal_input(&app.in);
}

/* --- overlays ----------------------------------------------------------- */

static void draw_connecting(void)
{
	char line[128];

	render_fill(180, 200, 600, 120, RENDER_COLOR_PANEL);
	render_frame(180, 200, 600, 120, RENDER_COLOR_PANEL_EDGE, 2);

	draw_centered(220, 2, RENDER_COLOR_WHITE, ssh_state_name(app.ssh.state));

	snprintf(line, sizeof(line), "%s@%s:%d", app.ssh.host.user, app.ssh.host.host,
	         app.ssh.host.port);
	draw_centered(252, 1, RENDER_COLOR_DIM, line);
	draw_centered(280, 1, RENDER_COLOR_DIM, "O  cancel and go back");
}

static void draw_hostkey_prompt(void)
{
	char line[128];
	size_t len = strlen(app.ssh.fingerprint);

	render_fill(120, 176, 720, 192, RENDER_COLOR_PANEL);
	render_frame(120, 176, 720, 192, RENDER_COLOR_WARN, 2);

	draw_centered(196, 2, RENDER_COLOR_WARN, "Unknown host key");

	snprintf(line, sizeof(line), "%s:%d", app.ssh.host.host, app.ssh.host.port);
	draw_centered(224, 1, RENDER_COLOR_WHITE, line);
	draw_centered(244, 1, RENDER_COLOR_DIM, "SHA-256 fingerprint of the offered key:");

	if (len == 64) {
		/* Two rows of 32 hex digits, which is what fits at 2x. */
		char half[33];

		memcpy(half, app.ssh.fingerprint, 32);
		half[32] = '\0';
		draw_centered(272, 2, RENDER_COLOR_WHITE, half);

		memcpy(half, app.ssh.fingerprint + 32, 32);
		half[32] = '\0';
		draw_centered(296, 2, RENDER_COLOR_WHITE, half);
	} else {
		draw_centered(288, 1, RENDER_COLOR_WHITE, app.ssh.fingerprint);
	}

	draw_centered(340, 1, RENDER_COLOR_DIM,
	              "Compare it with the server before trusting it.");
	draw_centered(356, 1, RENDER_COLOR_WARN,
	              "L  accept and connect        R  abort");
}

static void draw_keypad(void)
{
	int i;

	render_fill(KP_X, KP_Y, KP_W, KP_H, RENDER_COLOR_PANEL);
	render_frame(KP_X, KP_Y, KP_W, KP_H, RENDER_COLOR_PANEL_EDGE, 2);
	draw_centered(KP_Y + 8, 1, RENDER_COLOR_DIM, "control keys");

	for (i = 0; i < KEYPAD_COUNT; i++) {
		const KeypadItem *item = &keypad_items[i];
		bool sel = (i == app.keypad_index);
		int x = KP_GRID_X + (i % KEYPAD_COLS) * (KP_BTN_W + KP_GAP);
		int y = KP_GRID_Y + (i / KEYPAD_COLS) * (KP_BTN_H + KP_GAP);
		int lw = render_text_width(2, item->label);

		render_fill(x, y, KP_BTN_W, KP_BTN_H,
		            sel ? RENDER_COLOR_SELECT : 0xFF282828u);
		if (sel) render_frame(x, y, KP_BTN_W, KP_BTN_H, RENDER_COLOR_WHITE, 2);

		render_text(x + (KP_BTN_W - lw) / 2, y + (KP_BTN_H - 16) / 2, 2,
		            sel ? RENDER_COLOR_WHITE : RENDER_COLOR_DIM, item->label);
	}

	{
		bool sel = (app.keypad_index == KEYPAD_CLOSE);
		int x = KP_GRID_X;
		int y = KP_GRID_Y + KEYPAD_ROWS * (KP_BTN_H + KP_GAP);
		int lw = render_text_width(2, "Close");

		render_fill(x, y, KEYPAD_COLS * KP_BTN_W + (KEYPAD_COLS - 1) * KP_GAP,
		            KP_CLOSE_H, sel ? RENDER_COLOR_SELECT : 0xFF282828u);
		if (sel) {
			render_frame(x, y, KEYPAD_COLS * KP_BTN_W + (KEYPAD_COLS - 1) * KP_GAP,
			             KP_CLOSE_H, RENDER_COLOR_WHITE, 2);
		}

		render_text(x + (KEYPAD_COLS * KP_BTN_W + (KEYPAD_COLS - 1) * KP_GAP - lw) / 2,
		            y + (KP_CLOSE_H - 16) / 2, 2,
		            sel ? RENDER_COLOR_WHITE : RENDER_COLOR_DIM, "Close");
	}
}

/*
 * The password row's right-hand text, recomputed every frame. The key it is
 * about is whatever the other four rows currently say, so caching it when the
 * form opens would go stale the moment the user edits the address.
 */
static const char *form_password_state(void)
{
	HostEntry key;

	if (!app.vault.ready) return "(store unavailable)";
	if (app.form.pw_action == PW_SET) return "(will be saved)";
	if (app.form.pw_action == PW_CLEAR) return "(will be forgotten)";

	if (config_entry_make(app.form.name, app.form.host, app.form.port,
	                      app.form.user, &key) != 0)
		return "(no host yet)";

	return vault_has(&app.vault, &key) ? "(saved)" : "(not saved)";
}

/*
 * The add/edit panel, laid out like the control-key panel so the two overlays
 * read the same way: full-width strips with the cursor's one filled in.
 */
static void draw_form(void)
{
	static const char *const labels[FORM_ROW_COUNT] = {
		"Name", "Host", "Port", "User", "Password", "Save", "Delete",
	};
	static const char *const hints[FORM_ROW_COUNT] = {
		"(default: the host)",
		"(required)",
		"(default: 22)",
		"(default: root)",
		"(not saved)", /* unreachable: the password row always has a value */
		NULL,
		NULL,
	};
	const char *values[FORM_ROW_COUNT] = {
		app.form.name, app.form.host, app.form.port, app.form.user,
		form_password_state(), NULL, NULL,
	};
	int i;

	_Static_assert(sizeof(labels) / sizeof(labels[0]) == FORM_ROW_COUNT,
	               "the form's labels are out of step with its rows");
	_Static_assert(sizeof(hints) / sizeof(hints[0]) == FORM_ROW_COUNT,
	               "the form's hints are out of step with its rows");
	_Static_assert(sizeof(values) / sizeof(values[0]) == FORM_ROW_COUNT,
	               "the form's values are out of step with its rows");

	render_fill(FORM_X, FORM_Y, FORM_W, FORM_H, RENDER_COLOR_PANEL);
	render_frame(FORM_X, FORM_Y, FORM_W, FORM_H, RENDER_COLOR_PANEL_EDGE, 2);
	draw_centered(FORM_Y + 14, 1, RENDER_COLOR_DIM,
	              app.form.is_new ? "new host" : "edit host");

	for (i = 0; i < FORM_ROW_COUNT; i++) {
		bool sel = (i == app.form.row);
		int y = FORM_LIST_Y + i * FORM_ROW_H;

		if (sel)
			render_fill(FORM_X + 16, y - 8, FORM_W - 32, FORM_ROW_H - 8,
			            RENDER_COLOR_SELECT);

		if (i < FORM_ROW_SAVE) {
			render_text(FORM_LABEL_X, y, 1,
			            sel ? RENDER_COLOR_WHITE : RENDER_COLOR_DIM,
			            labels[i]);

			if (values[i][0] != '\0') {
				render_text(FORM_VALUE_X, y, 1,
				            sel ? RENDER_COLOR_WHITE : RENDER_COLOR_DIM,
				            values[i]);
			} else {
				render_text(FORM_VALUE_X, y, 1, RENDER_COLOR_DIM, hints[i]);
			}
			continue;
		}

		{
			const char *text = labels[i];
			uint32_t fg;
			int w;

			if (i == FORM_ROW_SAVE) {
				fg = RENDER_COLOR_OK;
			} else if (app.form.is_new) {
				fg = RENDER_COLOR_DIM; /* nothing selected to delete */
			} else if (app.form.delete_armed) {
				text = "Delete -- press X again";
				fg = RENDER_COLOR_WARN;
			} else {
				fg = RENDER_COLOR_ERROR;
			}

			w = render_text_width(2, text);
			render_text(FORM_X + (FORM_W - w) / 2, y + 6, 2,
			            sel ? RENDER_COLOR_WHITE : fg, text);
		}
	}

	if (app.form.error[0] != '\0')
		render_text(FORM_LABEL_X, FORM_Y + FORM_H - 30, 1, RENDER_COLOR_ERROR,
		            app.form.error);
}

/* --- screens ------------------------------------------------------------ */

static void draw_hosts(void)
{
	char line[160];
	int i;

	render_text(32, 28, 2, RENDER_COLOR_WHITE, "VitaSSH");
	render_text(32, 54, 1, RENDER_COLOR_DIM, "SSH client for the PS Vita");
	render_fill(24, 84, SCREEN_W - 48, 2, RENDER_COLOR_PANEL_EDGE);

	if (app.hosts.count == 0) {
		render_text(32, 108, 1, RENDER_COLOR_WARN, "No hosts configured yet.");
		render_text(32, 136, 1, RENDER_COLOR_WHITE,
		            "Press ^ (triangle) to add one.");
		render_text(32, 164, 1, RENDER_COLOR_DIM,
		            "Hosts are also kept in " VITASSH_HOSTS ",");
		render_text(32, 180, 1, RENDER_COLOR_DIM,
		            "one per line as name|host|port|user.");
		render_text(32, 196, 1, RENDER_COLOR_DIM,
		            "Press SELECT to reload that file after editing it.");
	} else {
		for (i = 0; i < app.hosts.count && i < HOST_ROWS; i++) {
			const HostEntry *h = &app.hosts.item[i];
			int y = HOST_LIST_Y + i * HOST_ROW_H;
			bool sel = (i == app.selected);

			if (sel)
				render_fill(24, y - 4, SCREEN_W - 48, HOST_ROW_H - 2,
				            RENDER_COLOR_SELECT);

			render_text(40, y, 1, sel ? RENDER_COLOR_WHITE : RENDER_COLOR_DIM,
			            h->name);

			snprintf(line, sizeof(line), "%s@%s:%d", h->user, h->host, h->port);
			render_text(320, y, 1, RENDER_COLOR_WHITE, line);
		}
	}

	if (app.form.open) {
		draw_status(app.form.is_new ? "new host" : "edit host", NULL,
		            app.form.delete_armed
		                ? "X again to delete   O cancel"
		                : "UP/DOWN field   X edit/save   O cancel");
		draw_form();
		return;
	}

	draw_status(app.hosts.count > 0 ? app.hosts.item[app.selected].name
	                                : "host list",
	            NULL,
	            "UP/DOWN choose   X connect   ^ new   [] edit   O quit   "
	            "SELECT reload");
}

static void draw_terminal(void)
{
	char right[64];
	bool kb = kb_visible(&app.kb);
	int y_offset = 0;

	/*
	 * With the keyboard up, only the rows above it can be seen. Slide the grid
	 * so the cursor sits in the middle of that strip: a shell at the bottom
	 * stays bottom-aligned (the grid ends), while a full-screen program such
	 * as vim keeps the cursor on screen instead of hiding it under the keys.
	 */
	if (kb && app.scroll == 0) {
		const int visible = KB_Y / CELL_H;
		int top = app.term.cy - visible / 2;

		if (top < 0) top = 0;
		if (top > TERM_ROWS - visible) top = TERM_ROWS - visible;
		y_offset = -top * CELL_H;
	}

	render_terminal(&app.term, app.scroll,
	                app.scroll == 0 && ssh_running(&app.ssh), y_offset);

	if (app.scroll > 0)
		snprintf(right, sizeof(right), "SCROLL +%d", app.scroll);
	else if (ssh_running(&app.ssh))
		snprintf(right, sizeof(right), "live");
	else
		snprintf(right, sizeof(right), "%s", ssh_state_name(app.ssh.state));

	draw_status(app.peer, right,
	            app.scroll > 0
	                ? "UP/DOWN scroll   L/R page   X live   O menu   SELECT keys"
	                : "X enter  [] bksp  ^ tab  START/tap keys  SELECT keys  L scrollback");

	if (app.hostkey_prompt)
		draw_hostkey_prompt();
	else if (app.keypad_open)
		draw_keypad();
	else if (!ssh_running(&app.ssh))
		draw_connecting();

	if (kb && !app.hostkey_prompt && !app.keypad_open)
		kb_draw(&app.kb);
}

static void draw_message(void)
{
	render_text(32, 40, 2, RENDER_COLOR_WARN, app.message_title);
	draw_wrapped(32, 92, RENDER_COLOR_WHITE, app.message_body, SCREEN_W - 64);

	draw_status("disconnected", NULL, "X or O   back to the host list");
}

/* --- frame -------------------------------------------------------------- */

void ui_init(void)
{
	VaultStatus vs;

	memset(&app, 0, sizeof(app));

	kb_init(&app.kb);
	term_init(&app.term);
	vt_init(&app.vt);
	ssh_init(&app.ssh);
	app.screen = SCREEN_HOSTS;
	app.ime_purpose = IME_FOR_NONE;
	app.form.editing = -1; /* 0 would be a valid row, so clear it explicitly */

	load_hosts();

	/*
	 * After the hosts, so that a vault which cannot be read is logged against a
	 * session that is already up: passwords are an optional convenience, and a
	 * console that cannot offer them still has to be usable.
	 */
	vs = vault_load(&app.vault, VITASSH_SECRETS);
	if (vs != VAULT_OK)
		log_printf("ui: saved passwords: %s", vault_status_text(vs));
}

void ui_shutdown(void)
{
	/*
	 * The vault holds its passwords in the clear while the console is on -- it
	 * has to, to be able to type one into a login prompt -- so they are zeroed
	 * on the way out. This guards against what happens to this memory after the
	 * program stops, not against anything already running on the machine.
	 */
	vault_clear(&app.vault);
	form_forget_password();
}

bool ui_frame(void)
{
	ImeStatus ime;
	bool consumed_ime = false;

	input_poll(&app.in);
	touch_poll(&app.touch);

	ime = ime_poll();
	if (ime == IME_ACCEPTED || ime == IME_CANCELLED) {
		if (app.ime_purpose != IME_FOR_NONE) {
			handle_ime_result(ime);
			consumed_ime = true;
		}
	}

	/*
	 * While the keyboard is up the system owns the buttons, and on the frame it
	 * closes the confirming press is still in this frame's edges -- swallowing
	 * that one frame keeps the Enter from also reaching the shell.
	 */
	if (ime != IME_ACTIVE && !consumed_ime) handle_input();

	if (app.screen == SCREEN_TERMINAL) tick_terminal();

	render_begin();
	switch (app.screen) {
	case SCREEN_HOSTS:
		draw_hosts();
		break;
	case SCREEN_TERMINAL:
		draw_terminal();
		break;
	case SCREEN_MESSAGE:
		draw_message();
		break;
	}
	render_end();

	/* Must come after render_end(): the dialog layer needs the scene closed. */
	render_dialog_update();

	return !app.quitting;
}
