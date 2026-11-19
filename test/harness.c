/*
 * Host-side tests for the device-independent core (term, vt, config, vault).
 *
 *   gcc -fsanitize=address,undefined -Isrc src/term.c src/vt.c src/config.c \
 *       src/fileio.c src/vault.c test/vault_platform_stub.c \
 *       test/harness.c -o build/hosttest
 *
 * The terminal is where the subtle bugs live, and none of them need a Vita to
 * find, so this is the main line of defence before anything reaches hardware.
 *
 * The vault is here for a different reason: its file format and its rules for
 * what counts as a valid file are the parts that cannot be checked by looking
 * at the device, and the only way to check them is to feed it damaged files.
 * The crypto underneath is a toy on the host (test/vault_platform_stub.c) --
 * what is under test is everything above it.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h> /* _mkdir/_rmdir, for the "cannot move it aside" case */
#define make_dir(p) _mkdir(p)
#define drop_dir(p) _rmdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0777)
#define drop_dir(p) rmdir(p)
#endif

#include "config.h"
#include "fileio.h"
#include "term.h"
#include "vault.h"
#include "vault_platform.h"
#include "vault_platform_stub.h"
#include "vt.h"

static int checks;
static int failures;

#define CHECK(cond)                                                           \
	do {                                                                      \
		checks++;                                                             \
		if (!(cond)) {                                                        \
			failures++;                                                       \
			printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
		}                                                                     \
	} while (0)

#define CHECK_INT(got, want)                                                  \
	do {                                                                      \
		long g_ = (long)(got), w_ = (long)(want);                             \
		checks++;                                                             \
		if (g_ != w_) {                                                       \
			failures++;                                                       \
			printf("  FAIL %s:%d  %s == %ld, want %ld\n", __FILE__, __LINE__, \
			       #got, g_, w_);                                             \
		}                                                                     \
	} while (0)

#define CHECK_STR(got, want)                                                  \
	do {                                                                      \
		const char *g_ = (got), *w_ = (want);                                 \
		checks++;                                                             \
		if (strcmp(g_, w_) != 0) {                                            \
			failures++;                                                       \
			printf("  FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, g_, \
			       w_);                                                       \
		}                                                                     \
	} while (0)

/* ~285 KB, so it lives in BSS rather than on the stack. */
static Term term;
static VtParser parser;

static void reset(void)
{
	term_init(&term);
	vt_init(&parser);
}

static void feed(const char *s)
{
	vt_feed_str(&parser, &term, s);
}

/* Screen row as ASCII, trailing blanks trimmed. */
static void row_text(const Term *t, int row, char *out)
{
	int x;

	for (x = 0; x < TERM_COLS; x++) {
		uint32_t c = t->grid[row][x].ch;

		out[x] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
	}
	out[TERM_COLS] = '\0';

	for (x = TERM_COLS - 1; x >= 0 && out[x] == ' '; x--) out[x] = '\0';
}

/* Cursor row as ASCII, including the blank cells before it. */
static void dump_screen(const Term *t)
{
	char line[TERM_COLS + 1];
	int y;

	for (y = 0; y < TERM_ROWS; y++) {
		row_text(t, y, line);
		if (line[0] == '\0') continue;
		printf("    %2d |%s|\n", y, line);
	}
}

static void take_reply(char *dst, size_t cap)
{
	size_t n = vt_take_output(&parser, dst, cap - 1);

	dst[n] = '\0';
}

/* --- config ------------------------------------------------------------- */

static void test_config_line(void)
{
	HostEntry e;

	CHECK_INT(config_parse_line("home|192.168.1.10|22|root", &e), 0);
	CHECK_STR(e.name, "home");
	CHECK_STR(e.host, "192.168.1.10");
	CHECK_INT(e.port, 22);
	CHECK_STR(e.user, "root");

	/* Port and user are optional. */
	CHECK_INT(config_parse_line("box|10.0.0.5", &e), 0);
	CHECK_STR(e.name, "box");
	CHECK_STR(e.host, "10.0.0.5");
	CHECK_INT(e.port, 22);
	CHECK_STR(e.user, "root");

	/* An empty name falls back to the host. */
	CHECK_INT(config_parse_line("|10.0.0.5||", &e), 0);
	CHECK_STR(e.name, "10.0.0.5");
	CHECK_STR(e.user, "root");

	/* Surrounding whitespace and CRLF are stripped. */
	CHECK_INT(config_parse_line("  spare | 10.0.0.9 | 2222 | bob \r\n", &e), 0);
	CHECK_STR(e.name, "spare");
	CHECK_STR(e.host, "10.0.0.9");
	CHECK_INT(e.port, 2222);
	CHECK_STR(e.user, "bob");

	/* Skipped lines. */
	CHECK_INT(config_parse_line("", &e), 1);
	CHECK_INT(config_parse_line("   \n", &e), 1);
	CHECK_INT(config_parse_line("# a comment", &e), 1);

	/* Rejected lines. */
	CHECK_INT(config_parse_line("onlyaname", &e), -1);
	CHECK_INT(config_parse_line("name|", &e), -1);

	/* A nonsense port falls back rather than failing the entry. */
	CHECK_INT(config_parse_line("h|1.2.3.4|notaport|u", &e), 0);
	CHECK_INT(e.port, 22);
	CHECK_INT(config_parse_line("h|1.2.3.4|99999|u", &e), 0);
	CHECK_INT(e.port, 22);
}

static void test_config_roundtrip(void)
{
	static HostList list, back;
	const char *path = "build/harness_hosts.txt";
	FILE *f;
	char buf[256];

	/* A missing file is created from the template and reads back empty. */
	remove(path);
	CHECK_INT(config_load(&list, path), 0);
	CHECK_INT(list.count, 0);
	f = fopen(path, "rb");
	CHECK(f != NULL);
	if (f != NULL) {
		CHECK(fgets(buf, sizeof(buf), f) != NULL);
		CHECK(buf[0] == '#');
		fclose(f);
	}

	list.count = 3;
	CHECK_INT(config_parse_line("alpha|10.0.0.1|22|root", &list.item[0]), 0);
	CHECK_INT(config_parse_line("beta|10.0.0.2|2200|bob", &list.item[1]), 0);
	CHECK_INT(config_parse_line("gamma|example.com", &list.item[2]), 0);

	CHECK_INT(config_save(&list, path), 0);
	CHECK_INT(config_load(&back, path), 3);
	CHECK_STR(back.item[0].name, "alpha");
	CHECK_STR(back.item[1].host, "10.0.0.2");
	CHECK_INT(back.item[1].port, 2200);
	CHECK_STR(back.item[1].user, "bob");
	CHECK_STR(back.item[2].host, "example.com");
	CHECK_INT(back.item[2].port, 22);

	remove(path);
	remove("build/harness_hosts.txt.tmp");
}

/*
 * config_entry_make() is what the in-app editor commits through, so its defaults
 * have to be the parser's defaults -- otherwise a host typed on the device and
 * the same host written by hand would end up different.
 */
static void test_config_make(void)
{
	HostEntry e;

	CHECK_INT(config_entry_make("home", "10.0.0.1", "22", "root", &e), 0);
	CHECK_STR(e.name, "home");
	CHECK_STR(e.host, "10.0.0.1");
	CHECK_INT(e.port, 22);
	CHECK_STR(e.user, "root");

	/* Empty port, user and name take their fallbacks. */
	CHECK_INT(config_entry_make("", "10.0.0.2", "", "", &e), 0);
	CHECK_STR(e.name, "10.0.0.2");
	CHECK_INT(e.port, 22);
	CHECK_STR(e.user, "root");

	/* Fields are trimmed, as they are on the file path. */
	CHECK_INT(config_entry_make("  spare ", " 10.0.0.9 ", " 2222 ", " bob ", &e),
	          0);
	CHECK_STR(e.name, "spare");
	CHECK_STR(e.host, "10.0.0.9");
	CHECK_INT(e.port, 2222);
	CHECK_STR(e.user, "bob");

	/* Out-of-range ports fall back rather than failing the entry. */
	CHECK_INT(config_entry_make("h", "1.2.3.4", "0", "u", &e), 0);
	CHECK_INT(e.port, 22);
	CHECK_INT(config_entry_make("h", "1.2.3.4", "65536", "u", &e), 0);
	CHECK_INT(e.port, 22);
	CHECK_INT(config_entry_make("h", "1.2.3.4", "65535", "u", &e), 0);
	CHECK_INT(e.port, 65535);

	/* The host is the one field that cannot be left out. */
	CHECK_INT(config_entry_make("name", "", "22", "root", &e), -1);
	CHECK_INT(config_entry_make("name", "   ", "22", "root", &e), -1);

	/* NULL is treated as empty rather than dereferenced. */
	CHECK_INT(config_entry_make(NULL, "1.2.3.4", NULL, NULL, &e), 0);
	CHECK_INT(e.port, 22);
	CHECK_STR(e.user, "root");
}

static void test_config_field_clean(void)
{
	char buf[64];
	static const char *const utf8 = "\xe4\xb8\xad\xe6\x96\x87";

	/* A '|' would shift every later field on the next read. */
	snprintf(buf, sizeof(buf), "%s", "a|b");
	config_field_clean(buf);
	CHECK_STR(buf, "ab");

	/* Control bytes would end the line early. */
	snprintf(buf, sizeof(buf), "%s", "a\nb\rc\td");
	config_field_clean(buf);
	CHECK_STR(buf, "abcd");

	snprintf(buf, sizeof(buf), "%s", "a\x7fz");
	config_field_clean(buf);
	CHECK_STR(buf, "az");

	/* Ordinary text is untouched. */
	snprintf(buf, sizeof(buf), "%s", "host.example.com");
	config_field_clean(buf);
	CHECK_STR(buf, "host.example.com");

	/* High bytes are UTF-8, not control characters, and must survive. */
	snprintf(buf, sizeof(buf), "%s", utf8);
	config_field_clean(buf);
	CHECK_STR(buf, utf8);
}

/*
 * A line at the field caps is CFG_NAME_CAP + CFG_HOST_CAP + CFG_USER_CAP bytes
 * plus separators -- 133 of them, against a 128-byte line buffer if it is sized
 * CFG_HOST_CAP * 2. Verified against that buffer: fgets() split the line, the
 * host came back silently truncated and the tail was discarded. (Had the split
 * landed on a '|', the tail would have parsed as a second, bogus entry.)
 */
static void test_config_long_fields(void)
{
	static HostList list, back;
	const char *path = "build/harness_long.txt";
	char name[CFG_NAME_CAP];
	char host[CFG_HOST_CAP];
	char user[CFG_USER_CAP];
	int i;

	for (i = 0; i < CFG_NAME_CAP - 1; i++) name[i] = (char)('a' + i % 26);
	name[CFG_NAME_CAP - 1] = '\0';

	for (i = 0; i < CFG_HOST_CAP - 1; i++) host[i] = (char)('a' + i % 26);
	host[CFG_HOST_CAP - 1] = '\0';

	for (i = 0; i < CFG_USER_CAP - 1; i++) user[i] = (char)('a' + i % 26);
	user[CFG_USER_CAP - 1] = '\0';

	list.count = 1;
	CHECK_INT(config_entry_make(name, host, "65535", user, &list.item[0]), 0);

	CHECK_INT(config_save(&list, path), 0);
	CHECK_INT(config_load(&back, path), 1);
	CHECK_STR(back.item[0].name, name);
	CHECK_STR(back.item[0].host, host);
	CHECK_INT(back.item[0].port, 65535);
	CHECK_STR(back.item[0].user, user);

	remove(path);
	remove("build/harness_long.txt.tmp");
}

static void test_config_cap(void)
{
	static HostList list;
	const char *path = "build/harness_cap.txt";
	char last[16];
	FILE *f;
	int i;

	/* The reader stops at the cap even when the file holds more. */
	f = fopen(path, "wb");
	CHECK(f != NULL);
	if (f != NULL) {
		for (i = 0; i < CFG_MAX_HOSTS + 1; i++)
			fprintf(f, "h%d|10.0.0.%d|22|root\n", i, i + 1);
		fclose(f);

		snprintf(last, sizeof(last), "h%d", CFG_MAX_HOSTS - 1);
		CHECK_INT(config_load(&list, path), CFG_MAX_HOSTS);
		CHECK_INT(list.count, CFG_MAX_HOSTS);
		CHECK_STR(list.item[0].name, "h0");
		CHECK_STR(list.item[CFG_MAX_HOSTS - 1].name, last);
	}

	remove(path);
}

static void test_config_save_failure(void)
{
	static HostList list, back;
	const char *path = "build/harness_empty.txt";

	list.count = 1;
	CHECK_INT(config_parse_line("h|1.2.3.4|22|root", &list.item[0]), 0);

	/* A directory that does not exist: the write has to report the failure. */
	CHECK_INT(config_save(&list, "build/no_such_dir/hosts.txt"), -1);

	/* An empty list still writes the header, so a reload finds zero hosts. */
	list.count = 0;
	CHECK_INT(config_save(&list, path), 0);
	CHECK_INT(config_load(&back, path), 0);
	CHECK_INT(back.count, 0);

	remove(path);
	remove("build/harness_empty.txt.tmp");
}

/* --- vault: saved passwords --------------------------------------------- */

/*
 * The file size is spelled out here rather than taken from vault.h on purpose:
 * it is part of what these tests are asserting. If the layout changes, this
 * number changes with it in a diff someone has to look at, because it decides
 * whether passwords saved by the previous version can still be read.
 */
#define VAULT_PATH "build/harness_vault.dat"
#define VAULT_BAD VAULT_PATH ".bad"
#define VAULT_LEN 3668
#define VAULT_HDR_LEN 36
#define VAULT_BODY_LEN 3616

_Static_assert(VAULT_LEN == 8 + 16 + 12 + VAULT_BODY_LEN + 16,
               "the numbers the vault tests use do not add up");

static int file_size(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;

	if (f == NULL) return -1;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return -1;
	}
	n = ftell(f);
	fclose(f);
	return (int)n;
}

static int read_file(const char *path, uint8_t *buf, size_t cap)
{
	FILE *f = fopen(path, "rb");
	size_t n;

	if (f == NULL) return -1;
	n = fread(buf, 1, cap, f);
	fclose(f);
	return (int)n;
}

static int spit(const char *path, const uint8_t *buf, size_t len)
{
	FILE *f = fopen(path, "wb");

	if (f == NULL) return -1;
	if (fwrite(buf, 1, len, f) != len) {
		fclose(f);
		return -1;
	}
	return fclose(f) == 0 ? 0 : -1;
}

static HostEntry mk(const char *name, const char *host, const char *port,
                    const char *user)
{
	HostEntry h;

	memset(&h, 0, sizeof(h));
	config_entry_make(name, host, port, user, &h); /* h stays zeroed on failure */
	return h;
}

static int used_slots(const Vault *v)
{
	int i, n = 0;

	for (i = 0; i < VAULT_MAX_SLOTS; i++)
		if (v->slot[i].host[0] != '\0') n++;
	return n;
}

/*
 * Whatever the loader accepted has to be a slot this build could have stored:
 * every string terminated inside its own field, every port in range. memchr
 * rather than strlen, because the bug this is looking for is a missing
 * terminator and strlen is the function that would run off the end finding it.
 */
static void check_sane(const Vault *v)
{
	int i;

	for (i = 0; i < VAULT_MAX_SLOTS; i++) {
		const VaultSlot *s = &v->slot[i];

		if (s->host[0] == '\0') continue;

		CHECK(memchr(s->host, 0, sizeof(s->host)) != NULL);
		CHECK(memchr(s->user, 0, sizeof(s->user)) != NULL);
		CHECK(memchr(s->pass, 0, sizeof(s->pass)) != NULL);
		CHECK(s->port >= 1 && s->port <= 65535);
	}
}

static VaultStatus load_raw(Vault *v, const uint8_t *buf, size_t len)
{
	if (spit(VAULT_PATH, buf, len) != 0) return VAULT_ERR_STUCK;
	return vault_load(v, VAULT_PATH);
}

/*
 * One damaged file, start to finish. Every case has to end in one of two
 * places: usable and empty, or unusable. What it must never do is hand back
 * something that was not written there -- a wrong password is worse than a
 * missing one, because a missing one gets retyped.
 */
static void damage_case(const char *what, Vault *v, const uint8_t *buf,
                        size_t len, VaultStatus want)
{
	int before = failures;

	CHECK_INT(load_raw(v, buf, len), want);
	CHECK(!v->ready || used_slots(v) == 0);
	check_sane(v);

	/* And it kept every byte of the file it refused to read. */
	CHECK_INT(file_size(VAULT_BAD), (int)len);

	if (failures != before) printf("  (damaged case: %s)\n", what);
}

static void test_vault_roundtrip(void)
{
	static Vault v, back;
	char out[VAULT_PASS_CAP];
	char longpass[VAULT_PASS_CAP];
	char longhost[CFG_HOST_CAP];
	char longuser[CFG_USER_CAP];
	HostEntry h1, h2, h3, h4, h5, h6, renamed, miss, full;

	vault_stub_reset();
	remove(VAULT_PATH);
	remove(VAULT_BAD);

	CHECK_INT(vault_load(&v, VAULT_PATH), VAULT_OK);
	CHECK_INT(v.ready, 1);
	CHECK_INT(used_slots(&v), 0);

	/* Four hosts whose keys differ in exactly one field each, so a match
	 * that ignores any one of host, user or port shows up as a wrong hit. */
	h1 = mk("home", "10.0.0.1", "22", "root");
	h2 = mk("home", "10.0.0.1", "2222", "root");
	h3 = mk("home", "10.0.0.1", "22", "admin");
	h4 = mk("other", "10.0.0.2", "22", "root");

	CHECK_INT(vault_lookup(&v, &h1, out, sizeof(out)), -1);

	CHECK_INT(vault_set(&v, &h1, "p1"), 0);
	CHECK_INT(vault_set(&v, &h2, "p2"), 0);
	CHECK_INT(vault_set(&v, &h3, "p3"), 0);
	CHECK_INT(vault_set(&v, &h4, "p4"), 0);
	CHECK_INT(v.dirty, 1);
	CHECK_INT(used_slots(&v), 4);

	/* Full-length fields, one byte under each cap, plus the top port. */
	memset(longhost, 'h', CFG_HOST_CAP - 1);
	longhost[CFG_HOST_CAP - 1] = '\0';
	memset(longuser, 'u', CFG_USER_CAP - 1);
	longuser[CFG_USER_CAP - 1] = '\0';
	memset(longpass, 'p', VAULT_PASS_CAP - 1);
	longpass[VAULT_PASS_CAP - 1] = '\0';
	h5 = mk("long", longhost, "65535", longuser);
	CHECK_INT(vault_set(&v, &h5, longpass), 0);

	/* The vault is not a line-based format, so a password is not scrubbed
	 * the way a host field is: whatever the on-screen keyboard can produce
	 * has to survive, separators and all. */
	h6 = mk("odd", "10.0.0.9", "22", "root");
	CHECK_INT(vault_set(&v, &h6, "a|b\tc\xC3\xA9"), 0);

	CHECK_INT(vault_save(&v, VAULT_PATH), 0);
	CHECK_INT(v.dirty, 0);
	CHECK_INT(file_size(VAULT_PATH), VAULT_LEN);

	CHECK_INT(vault_load(&back, VAULT_PATH), VAULT_OK);
	CHECK_INT(back.ready, 1);
	CHECK_INT(used_slots(&back), 6);

	CHECK_INT(vault_lookup(&back, &h1, out, sizeof(out)), 0);
	CHECK_STR(out, "p1");
	CHECK_INT(vault_lookup(&back, &h2, out, sizeof(out)), 0);
	CHECK_STR(out, "p2");
	CHECK_INT(vault_lookup(&back, &h3, out, sizeof(out)), 0);
	CHECK_STR(out, "p3");
	CHECK_INT(vault_lookup(&back, &h4, out, sizeof(out)), 0);
	CHECK_STR(out, "p4");
	CHECK_INT(vault_lookup(&back, &h5, out, sizeof(out)), 0);
	CHECK_STR(out, longpass);
	CHECK_INT(vault_lookup(&back, &h6, out, sizeof(out)), 0);
	CHECK_STR(out, "a|b\tc\xC3\xA9");

	/* The name is not part of the key, so renaming a host keeps its
	 * password -- the thing a user would least expect to lose. */
	renamed = mk("a different label", "10.0.0.1", "22", "root");
	CHECK_INT(vault_lookup(&back, &renamed, out, sizeof(out)), 0);
	CHECK_STR(out, "p1");

	/* ...and every near miss is a miss. */
	miss = mk("home", "10.0.0.1", "2200", "root");
	CHECK_INT(vault_lookup(&back, &miss, out, sizeof(out)), -1);
	miss = mk("home", "10.0.0.1", "22", "roo");
	CHECK_INT(vault_lookup(&back, &miss, out, sizeof(out)), -1);
	miss = mk("home", "10.0.0.11", "22", "root");
	CHECK_INT(vault_lookup(&back, &miss, out, sizeof(out)), -1);
	miss = mk("home", "10.0.0.1", "22", "ROOT");
	CHECK_INT(vault_lookup(&back, &miss, out, sizeof(out)), -1);
	miss = mk("home", "10.0.0.1", "23", "root");
	CHECK_INT(vault_lookup(&back, &miss, out, sizeof(out)), -1);

	/* Replaced in place, not appended. */
	full = mk("home", "10.0.0.1", "22", "root");
	CHECK_INT(vault_has(&back, &full), 1);
	CHECK_INT(vault_set(&back, &full, "second"), 0);
	CHECK_INT(used_slots(&back), 6);
	CHECK_INT(vault_lookup(&back, &full, out, sizeof(out)), 0);
	CHECK_STR(out, "second");

	/* Removing is by key, and removing what is not there is not an error. */
	vault_remove(&back, &h2);
	CHECK_INT(used_slots(&back), 5);
	CHECK_INT(vault_has(&back, &h2), 0);
	CHECK_INT(vault_has(&back, &h1), 1);
	CHECK_INT(vault_lookup(&back, &h2, out, sizeof(out)), -1);
	vault_remove(&back, &h2);
	CHECK_INT(used_slots(&back), 5);
	CHECK_INT(back.dirty, 1);

	remove(VAULT_PATH);
}

static void test_vault_limits(void)
{
	static Vault v;
	HostEntry h;
	char big[VAULT_PASS_CAP + 8];
	char out[VAULT_PASS_CAP];

	vault_stub_reset();
	remove(VAULT_PATH);
	remove(VAULT_BAD);
	CHECK_INT(vault_load(&v, VAULT_PATH), VAULT_OK);

	h = mk("h", "10.0.0.1", "22", "root");

	/* One byte over the field is refused, not cut down to size: two
	 * passwords that differ only past the limit would become one. */
	memset(big, 'p', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	CHECK_INT(vault_set(&v, &h, big), -1);
	CHECK_INT(used_slots(&v), 0);
	CHECK_INT(vault_lookup(&v, &h, out, sizeof(out)), -1);

	/* The longest password that does fit, and a lookup buffer one byte
	 * short of it: half a password is not one, so that is a miss. */
	memset(big, 'p', VAULT_PASS_CAP - 1);
	big[VAULT_PASS_CAP - 1] = '\0';
	CHECK_INT(vault_set(&v, &h, big), 0);
	CHECK_INT(vault_lookup(&v, &h, out, sizeof(out)), 0);
	CHECK_STR(out, big);
	CHECK_INT(vault_lookup(&v, &h, out, VAULT_PASS_CAP - 1), -1);

	/* An empty password is a password. */
	CHECK_INT(vault_set(&v, &h, ""), 0);
	CHECK_INT(vault_lookup(&v, &h, out, sizeof(out)), 0);
	CHECK_STR(out, "");

	/* Keys that cannot be formed. */
	h.port = 0;
	CHECK_INT(vault_set(&v, &h, "x"), -1);
	h.port = 70000;
	CHECK_INT(vault_set(&v, &h, "x"), -1);
	h.port = 22;
	h.host[0] = '\0';
	CHECK_INT(vault_set(&v, &h, "x"), -1);
	CHECK_INT(vault_lookup(&v, &h, out, sizeof(out)), -1);

	remove(VAULT_PATH);
}

static void test_vault_capacity(void)
{
	static Vault v;
	HostEntry e, first, extra;
	char host[32];
	char out[VAULT_PASS_CAP];
	int i;

	vault_stub_reset();
	remove(VAULT_PATH);
	remove(VAULT_BAD);
	CHECK_INT(vault_load(&v, VAULT_PATH), VAULT_OK);

	for (i = 0; i < VAULT_MAX_SLOTS; i++) {
		snprintf(host, sizeof(host), "10.0.0.%d", i + 1);
		e = mk("h", host, "22", "root");
		CHECK_INT(vault_set(&v, &e, "p"), 0);
	}
	CHECK_INT(used_slots(&v), VAULT_MAX_SLOTS);

	/* There is no seventeenth slot, and nothing old is evicted to make
	 * room for one -- losing a password silently is the failure mode this
	 * whole design is arranged to avoid. */
	extra = mk("h", "10.0.0.99", "22", "root");
	CHECK_INT(vault_set(&v, &extra, "p"), -1);
	CHECK_INT(vault_lookup(&v, &extra, out, sizeof(out)), -1);

	/* Updating an entry that is already there needs no free slot. */
	first = mk("h", "10.0.0.1", "22", "root");
	CHECK_INT(vault_set(&v, &first, "new"), 0);
	CHECK_INT(used_slots(&v), VAULT_MAX_SLOTS);
	CHECK_INT(vault_lookup(&v, &first, out, sizeof(out)), 0);
	CHECK_STR(out, "new");

	/* Removing one makes room again, and the file stays the same size
	 * whether it is full or not -- it is a fixed-size table. */
	vault_remove(&v, &first);
	CHECK_INT(used_slots(&v), VAULT_MAX_SLOTS - 1);
	CHECK_INT(vault_set(&v, &extra, "p"), 0);
	CHECK_INT(used_slots(&v), VAULT_MAX_SLOTS);
	CHECK_INT(vault_save(&v, VAULT_PATH), 0);
	CHECK_INT(file_size(VAULT_PATH), VAULT_LEN);

	remove(VAULT_PATH);
}

static void test_vault_staging(void)
{
	static Vault live, staged;
	HostEntry h = mk("h", "10.0.0.1", "22", "root");
	char out[VAULT_PASS_CAP];

	vault_stub_reset();
	remove(VAULT_PATH);
	remove(VAULT_BAD);

	CHECK_INT(vault_load(&live, VAULT_PATH), VAULT_OK);
	CHECK_INT(vault_set(&live, &h, "live"), 0);
	CHECK_INT(vault_save(&live, VAULT_PATH), 0);
	CHECK_INT(live.dirty, 0);

	/* The host editor stages its changes on a copy, so a save that fails
	 * cannot leave the live vault claiming to hold what the file does not. */
	staged = live;
	CHECK_INT(vault_set(&staged, &h, "staged"), 0);
	CHECK_INT(vault_save(&staged, "build/no_such_dir/secrets.dat"), -1);
	CHECK_INT(live.dirty, 0);
	CHECK_INT(vault_lookup(&live, &h, out, sizeof(out)), 0);
	CHECK_STR(out, "live");

	/* On success the caller adopts the copy, which is clean again. */
	CHECK_INT(vault_save(&staged, VAULT_PATH), 0);
	CHECK_INT(staged.dirty, 0);
	live = staged;
	CHECK_INT(vault_lookup(&live, &h, out, sizeof(out)), 0);
	CHECK_STR(out, "staged");

	remove(VAULT_PATH);
}

static void test_vault_damage(void)
{
	static Vault v, back;
	static uint8_t good[VAULT_LEN];
	static uint8_t buf[VAULT_LEN + 8];
	HostEntry h = mk("h", "10.0.0.1", "22", "root");
	char out[VAULT_PASS_CAP];

	vault_stub_reset();
	remove(VAULT_PATH);
	remove(VAULT_BAD);

	CHECK_INT(vault_load(&v, VAULT_PATH), VAULT_OK);
	CHECK_INT(vault_set(&v, &h, "secret"), 0);
	CHECK_INT(vault_save(&v, VAULT_PATH), 0);
	CHECK_INT(read_file(VAULT_PATH, good, sizeof(good)), VAULT_LEN);

	/* The baseline, so a case that passes for the wrong reason is obvious. */
	CHECK_INT(load_raw(&v, good, VAULT_LEN), VAULT_OK);
	CHECK_INT(vault_lookup(&v, &h, out, sizeof(out)), 0);
	CHECK_STR(out, "secret");

	/* Length. The format has no length field, so a file of the wrong size
	 * is not a shorter or longer table, it is not a table. */
	damage_case("one byte short", &v, good, VAULT_LEN - 1, VAULT_RECOVERED);
	damage_case("forty bytes short", &v, good, VAULT_LEN - 40, VAULT_RECOVERED);
	damage_case("empty file", &v, good, 0, VAULT_RECOVERED);
	memcpy(buf, good, VAULT_LEN);
	buf[VAULT_LEN] = 0xEE;
	damage_case("one byte long", &v, buf, VAULT_LEN + 1, VAULT_RECOVERED);

	/* Nothing that starts out wrong. */
	memset(buf, 0x00, VAULT_LEN);
	damage_case("all zeroes", &v, buf, VAULT_LEN, VAULT_RECOVERED);
	memset(buf, 0xFF, VAULT_LEN);
	damage_case("all ones", &v, buf, VAULT_LEN, VAULT_RECOVERED);

	/* The magic is the only part read before the key is derived. */
	memcpy(buf, good, VAULT_LEN);
	buf[0] ^= 1;
	damage_case("magic flipped", &v, buf, VAULT_LEN, VAULT_RECOVERED);

	/* Salt and nonce feed the key and the cipher, so a flip there shows up
	 * as a tag failure rather than as a malformed file. Both ends of each,
	 * because that is where an off-by-one in the layout would land. */
	memcpy(buf, good, VAULT_LEN);
	buf[8] ^= 1;
	damage_case("salt first byte flipped", &v, buf, VAULT_LEN, VAULT_FOREIGN);
	memcpy(buf, good, VAULT_LEN);
	buf[23] ^= 0x80;
	damage_case("salt last byte flipped", &v, buf, VAULT_LEN, VAULT_FOREIGN);
	memcpy(buf, good, VAULT_LEN);
	buf[24] ^= 1;
	damage_case("nonce first byte flipped", &v, buf, VAULT_LEN, VAULT_FOREIGN);
	memcpy(buf, good, VAULT_LEN);
	buf[35] ^= 1;
	damage_case("nonce last byte flipped", &v, buf, VAULT_LEN, VAULT_FOREIGN);

	/* Ciphertext, at the first byte, somewhere in the middle and the last. */
	memcpy(buf, good, VAULT_LEN);
	buf[36] ^= 1;
	damage_case("ciphertext start flipped", &v, buf, VAULT_LEN, VAULT_FOREIGN);
	memcpy(buf, good, VAULT_LEN);
	buf[1826] ^= 1;
	damage_case("ciphertext middle flipped", &v, buf, VAULT_LEN, VAULT_FOREIGN);
	memcpy(buf, good, VAULT_LEN);
	buf[3651] ^= 1;
	damage_case("ciphertext end flipped", &v, buf, VAULT_LEN, VAULT_FOREIGN);

	/* And the tag itself, which is what the check reads. */
	memcpy(buf, good, VAULT_LEN);
	buf[3652] ^= 1;
	damage_case("tag start flipped", &v, buf, VAULT_LEN, VAULT_FOREIGN);
	memcpy(buf, good, VAULT_LEN);
	buf[3667] ^= 1;
	damage_case("tag end flipped", &v, buf, VAULT_LEN, VAULT_FOREIGN);

	/* The same file on a different console: intact, correctly sized, and
	 * still not ours. It must not be mistaken for damage we could rewrite
	 * over, and it must not produce a slot. */
	{
		static const uint8_t other[VAULT_SECRET_LEN] = {
			0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A,
			0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A,
		};

		vault_stub_set_secret(other);
		damage_case("another console's file", &v, good, VAULT_LEN,
		            VAULT_FOREIGN);
		vault_stub_reset();
	}

	/* And the case the tag cannot catch: a genuine file, correctly signed,
	 * holding a field that runs to the end of its slot with no terminator.
	 * The offsets are spelled out here because the point of this case is to
	 * disagree with src/vault.c about the layout if it ever changes. */
	{
		static uint8_t plain[VAULT_BODY_LEN];
		uint8_t key[VAULT_KEY_LEN];
		uint8_t secret[VAULT_SECRET_LEN];

		memset(plain, 'A', sizeof(plain));
		memset(buf, 0, VAULT_LEN);
		memcpy(buf, "VSSHVLT1", 8);
		memset(buf + 8, 0x5A, VAULT_SALT_LEN);
		memset(buf + 24, 0x3C, VAULT_IV_LEN);

		CHECK_INT(vault_device_secret(secret), 0);
		CHECK_INT(vault_kdf(secret, sizeof(secret), buf + 8, key), 0);
		CHECK_INT(vault_aead_encrypt(key, buf + 24, buf, VAULT_HDR_LEN,
		                             plain, sizeof(plain),
		                             buf + VAULT_HDR_LEN,
		                             buf + VAULT_HDR_LEN + VAULT_BODY_LEN),
		          0);

		damage_case("signature right, field unterminated", &v, buf,
		            VAULT_LEN, VAULT_RECOVERED);
	}

	/* After all of that the vault is still usable, and what it writes now
	 * is the only thing it will read back. */
	CHECK_INT(vault_set(&v, &h, "again"), 0);
	CHECK_INT(vault_save(&v, VAULT_PATH), 0);
	CHECK_INT(vault_load(&back, VAULT_PATH), VAULT_OK);
	CHECK_INT(vault_lookup(&back, &h, out, sizeof(out)), 0);
	CHECK_STR(out, "again");

	remove(VAULT_PATH);
	remove(VAULT_BAD);
}

static void test_vault_stuck(void)
{
	static Vault v;
	static uint8_t buf[VAULT_LEN];
	HostEntry h = mk("h", "10.0.0.1", "22", "root");
	char out[VAULT_PASS_CAP];

	vault_stub_reset();
	remove(VAULT_PATH);
	remove(VAULT_BAD);

	CHECK_INT(vault_load(&v, VAULT_PATH), VAULT_OK);
	CHECK_INT(vault_set(&v, &h, "x"), 0);
	CHECK_INT(vault_save(&v, VAULT_PATH), 0);

	CHECK_INT(read_file(VAULT_PATH, buf, sizeof(buf)), VAULT_LEN);
	buf[0] ^= 1;
	CHECK_INT(spit(VAULT_PATH, buf, sizeof(buf)), 0);

	/* Damage it, and block the way out. Overwriting the damaged file is the
	 * one outcome that loses those passwords for good, so the vault has to
	 * go read-only rather than take it. */
	CHECK_INT(make_dir(VAULT_BAD), 0);
	CHECK_INT(vault_load(&v, VAULT_PATH), VAULT_ERR_STUCK);
	CHECK_INT(v.ready, 0);
	CHECK_INT(file_size(VAULT_PATH), VAULT_LEN); /* still there, untouched */
	CHECK_INT(vault_set(&v, &h, "y"), -1);
	CHECK_INT(vault_save(&v, VAULT_PATH), -1);
	CHECK_INT(vault_lookup(&v, &h, out, sizeof(out)), -1);
	CHECK_INT(drop_dir(VAULT_BAD), 0);

	/* Unblock it and the next load recovers on its own. */
	CHECK_INT(vault_load(&v, VAULT_PATH), VAULT_RECOVERED);
	CHECK_INT(v.ready, 1);
	CHECK_INT(used_slots(&v), 0);
	CHECK_INT(vault_set(&v, &h, "y"), 0);
	CHECK_INT(vault_save(&v, VAULT_PATH), 0);
	CHECK_INT(vault_lookup(&v, &h, out, sizeof(out)), 0);
	CHECK_STR(out, "y");

	remove(VAULT_PATH);
	remove(VAULT_BAD);
}

static void test_vault_save_failure(void)
{
	static Vault v, back;
	static uint8_t a[VAULT_LEN], b[VAULT_LEN];
	HostEntry h1 = mk("h", "10.0.0.1", "22", "root");
	HostEntry h2 = mk("h", "10.0.0.2", "22", "root");
	char out[VAULT_PASS_CAP];

	vault_stub_reset();
	remove(VAULT_PATH);
	remove(VAULT_BAD);

	CHECK_INT(vault_load(&v, VAULT_PATH), VAULT_OK);
	CHECK_INT(vault_set(&v, &h1, "one"), 0);
	CHECK_INT(vault_save(&v, VAULT_PATH), 0);

	/* A write that cannot be completed leaves the previous file alone and
	 * the vault dirty, so the caller can report it and try again. */
	CHECK_INT(vault_set(&v, &h2, "two"), 0);
	CHECK_INT(vault_save(&v, "build/no_such_dir/secrets.dat"), -1);
	CHECK_INT(v.dirty, 1);

	CHECK_INT(vault_load(&back, VAULT_PATH), VAULT_OK);
	CHECK_INT(vault_lookup(&back, &h1, out, sizeof(out)), 0);
	CHECK_STR(out, "one");
	CHECK_INT(vault_lookup(&back, &h2, out, sizeof(out)), -1);

	/* A fresh nonce every time, so the same vault written twice is never
	 * the same bytes. Identical files would mean a reused nonce, which
	 * under GCM is not a weakness but a break. */
	CHECK_INT(vault_save(&back, "build/harness_vault_a.dat"), 0);
	CHECK_INT(vault_save(&back, "build/harness_vault_b.dat"), 0);
	CHECK_INT(read_file("build/harness_vault_a.dat", a, sizeof(a)), VAULT_LEN);
	CHECK_INT(read_file("build/harness_vault_b.dat", b, sizeof(b)), VAULT_LEN);
	CHECK(memcmp(a, b, VAULT_LEN) != 0);
	remove("build/harness_vault_a.dat");
	remove("build/harness_vault_b.dat");

	/* A generator that fails abandons the save instead of falling back to a
	 * constant, so nothing is written at all. */
	remove(VAULT_PATH);
	vault_stub_fail_random(1);
	CHECK_INT(vault_save(&back, VAULT_PATH), -1);
	vault_stub_fail_random(0);
	CHECK_INT(file_size(VAULT_PATH), -1);

	remove(VAULT_PATH);
	remove(VAULT_PATH ".tmp");
}

static void test_vault_cleared(void)
{
	static Vault v;
	HostEntry h = mk("h", "10.0.0.1", "22", "root");
	char out[VAULT_PASS_CAP];

	vault_stub_reset();
	remove(VAULT_PATH);
	remove(VAULT_BAD);
	CHECK_INT(vault_load(&v, VAULT_PATH), VAULT_OK);
	CHECK_INT(vault_set(&v, &h, "x"), 0);

	/* A vault that is not ready is the one shape that must never be written
	 * from: readiness is what says the key on this console still matches
	 * the file. */
	vault_clear(&v);
	CHECK_INT(v.ready, 0);
	CHECK_INT(v.dirty, 0);
	CHECK_INT(used_slots(&v), 0);
	CHECK_INT(vault_lookup(&v, &h, out, sizeof(out)), -1);
	CHECK_INT(vault_set(&v, &h, "y"), -1);
	CHECK_INT(vault_save(&v, VAULT_PATH), -1);

	remove(VAULT_PATH);
}

static void test_file_replace(void)
{
	static uint8_t buf[64];
	const char *a = "build/harness_fr_a.txt";
	const char *b = "build/harness_fr_b.txt";

	CHECK_INT(spit(a, (const uint8_t *)"old", 3), 0);
	CHECK_INT(spit(b, (const uint8_t *)"newer", 5), 0);

	CHECK_INT(file_replace(b, a), 0);
	CHECK_INT(file_size(a), 5);
	CHECK_INT(file_size(b), -1); /* moved, not copied */
	CHECK_INT(read_file(a, buf, sizeof(buf)), 5);
	CHECK_INT(memcmp(buf, "newer", 5), 0);

	/* A replacement that is not there must not cost the file it would have
	 * replaced: this is the case that decides whether a save can destroy a
	 * good file for nothing. */
	CHECK_INT(file_replace("build/no_such_dir/x", a), -1);
	CHECK_INT(file_size(a), 5);

	/* Replacing a file that does not exist yet is the ordinary first save. */
	CHECK_INT(file_replace(a, "build/harness_fr_c.txt"), 0);
	CHECK_INT(file_size(a), -1);
	CHECK_INT(file_size("build/harness_fr_c.txt"), 5);

	remove("build/harness_fr_c.txt");
}

/* --- term basics -------------------------------------------------------- */

static void test_basic_writing(void)
{
	char row[TERM_COLS + 1];

	reset();
	feed("abc");
	CHECK_INT(term.cx, 3);
	CHECK_INT(term.cy, 0);
	row_text(&term, 0, row);
	CHECK_STR(row, "abc");

	feed("\r\nsecond");
	CHECK_INT(term.cy, 1);
	CHECK_INT(term.cx, 6);
	row_text(&term, 1, row);
	CHECK_STR(row, "second");

	/* Backspace moves but does not erase; the next char overwrites. */
	feed("\b\bXY");
	row_text(&term, 1, row);
	CHECK_STR(row, "secoXY");

	/* A bare LF keeps the column, as on a real VT. */
	reset();
	feed("ab\ncd");
	CHECK_INT(term.cy, 1);
	CHECK_INT(term.cx, 4);
	row_text(&term, 1, row);
	CHECK_STR(row, "  cd");
}

static void test_tabs(void)
{
	reset();
	feed("a\tb");
	CHECK_INT(term.cx, 9);
	CHECK_INT(term.grid[0][8].ch, 'b');

	/* CBT goes back to the previous stop, which from column 9 is column 8. */
	feed("\x1b[Z");
	CHECK_INT(term.cx, 8);

	/* A custom stop at column 2, then tab from column 0. */
	reset();
	feed("\x1b[3G\x1bH\x1b[1Ga\tb");
	CHECK_INT(term.cx, 3);
	CHECK_INT(term.grid[0][2].ch, 'b');

	/* TBC 3 clears them all, so a tab runs to the right margin. */
	reset();
	feed("\x1b[3g\ta");
	CHECK_INT(term.cx, TERM_COLS - 1);
}

static void test_wrapping(void)
{
	char row[TERM_COLS + 1];
	char full[TERM_COLS + 1];
	int i;

	reset();
	for (i = 0; i < TERM_COLS; i++) feed("x");

	/* The wrap is deferred: the last char sits in the final column and the
	 * cursor has not moved to the next line yet. */
	CHECK_INT(term.wrap_pending, 1);
	CHECK_INT(term.cy, 0);
	CHECK_INT(term.cx, TERM_COLS - 1);

	feed("y");
	CHECK_INT(term.cy, 1);
	CHECK_INT(term.cx, 1);
	row_text(&term, 1, row);
	CHECK_STR(row, "y");

	/* With autowrap off the extra characters overwrite the last column. */
	reset();
	feed("\x1b[?7l");
	for (i = 0; i < TERM_COLS + 5; i++) feed("z");
	CHECK_INT(term.cy, 0);
	CHECK_INT(term.cx, TERM_COLS - 1);
	row_text(&term, 0, row);
	for (i = 0; i < TERM_COLS; i++) full[i] = 'z';
	full[TERM_COLS] = '\0';
	CHECK_STR(row, full);
}

/* --- cursor addressing -------------------------------------------------- */

static void test_cursor_addressing(void)
{
	reset();
	feed("\x1b[5;10H");
	CHECK_INT(term.cy, 4);
	CHECK_INT(term.cx, 9);

	feed("\x1b[2A");
	CHECK_INT(term.cy, 2);
	feed("\x1b[3B");
	CHECK_INT(term.cy, 5);
	feed("\x1b[4C");
	CHECK_INT(term.cx, 13);
	feed("\x1b[6D");
	CHECK_INT(term.cx, 7);

	feed("\x1b[3;20f"); /* HVP is CUP's twin */
	CHECK_INT(term.cy, 2);
	CHECK_INT(term.cx, 19);

	feed("\x1b[7G");
	CHECK_INT(term.cx, 6);
	feed("\x1b[9d");
	CHECK_INT(term.cy, 8);

	/* CNL / CPL also home the column. */
	feed("\x1b[2E");
	CHECK_INT(term.cy, 10);
	CHECK_INT(term.cx, 0);
	feed("\x1b[4F");
	CHECK_INT(term.cy, 6);
	CHECK_INT(term.cx, 0);

	/* A move past the edge clamps instead of wrapping. */
	feed("\x1b[999;999H");
	CHECK_INT(term.cy, TERM_ROWS - 1);
	CHECK_INT(term.cx, TERM_COLS - 1);
}

static void test_erase(void)
{
	char row[TERM_COLS + 1];

	reset();
	feed("hello world");
	feed("\x1b[6G\x1b[K"); /* EL 0: cursor to end */
	row_text(&term, 0, row);
	CHECK_STR(row, "hello");

	reset();
	feed("hello world\x1b[1;6H\x1b[1K"); /* EL 1: start to cursor */
	row_text(&term, 0, row);
	CHECK_STR(row, "      world");

	reset();
	feed("hello\x1b[2K");
	row_text(&term, 0, row);
	CHECK_STR(row, "");

	/* ED 2 clears everything but leaves the cursor where it was. */
	reset();
	feed("one\r\ntwo\r\nthree\x1b[2;2H\x1b[2J");
	CHECK_INT(term.cy, 1);
	CHECK_INT(term.cx, 1);
	row_text(&term, 0, row);
	CHECK_STR(row, "");
	row_text(&term, 2, row);
	CHECK_STR(row, "");

	/* ED 0 wipes from the cursor down, ED 1 from the top down to it. */
	reset();
	feed("aaa\r\nbbb\r\nccc\x1b[2;1H\x1b[0J");
	row_text(&term, 0, row);
	CHECK_STR(row, "aaa");
	row_text(&term, 1, row);
	CHECK_STR(row, "");
	row_text(&term, 2, row);
	CHECK_STR(row, "");

	/* ECH blanks part of a line without moving anything. */
	reset();
	feed("abcdef\x1b[1;3H\x1b[2X");
	row_text(&term, 0, row);
	CHECK_STR(row, "ab  ef");
	CHECK_INT(term.cx, 2);
}

static void test_line_editing(void)
{
	char row[TERM_COLS + 1];

	/* ICH pushes right, DCH pulls left. */
	reset();
	feed("abcdef\x1b[1;3H\x1b[2@");
	row_text(&term, 0, row);
	CHECK_STR(row, "ab  cdef");

	reset();
	feed("abcdef\x1b[1;3H\x1b[2P");
	row_text(&term, 0, row);
	CHECK_STR(row, "abef");

	/* DL pulls following lines up (the classic `dd`). */
	reset();
	feed("one\r\ntwo\r\nthree\x1b[2;1H\x1b[M");
	row_text(&term, 0, row);
	CHECK_STR(row, "one");
	row_text(&term, 1, row);
	CHECK_STR(row, "three");
	row_text(&term, 2, row);
	CHECK_STR(row, "");

	/* IL opens a gap. */
	reset();
	feed("one\r\ntwo\x1b[1;1H\x1b[L");
	row_text(&term, 0, row);
	CHECK_STR(row, "");
	row_text(&term, 1, row);
	CHECK_STR(row, "one");
}

static void test_insert_mode(void)
{
	char row[TERM_COLS + 1];

	reset();
	feed("abcdef\x1b[1;3H");
	feed("\x1b[4h"); /* IRM on */
	feed("XY");
	row_text(&term, 0, row);
	CHECK_STR(row, "abXYcdef");

	feed("\x1b[4l"); /* and off again */
	feed("\x1b[1;1H");
	feed("Z");
	row_text(&term, 0, row);
	CHECK_STR(row, "ZbXYcdef");
}

/* --- rendition ---------------------------------------------------------- */

static void test_sgr(void)
{
	reset();
	feed("\x1b[31mR");
	CHECK_INT(term.grid[0][0].fg, 1);
	CHECK_INT(term.grid[0][0].bg, TERM_COLOR_DEFAULT);

	/* Bold promotes a normal colour to its bright twin. */
	reset();
	feed("\x1b[1;31mR");
	CHECK_INT(term.grid[0][0].fg, 9);

	/* Dim demotes it back. */
	reset();
	feed("\x1b[2;91mR");
	CHECK_INT(term.grid[0][0].fg, 1);

	/* Reverse swaps the pair at write time. */
	reset();
	feed("\x1b[7;31;44mR");
	CHECK_INT(term.grid[0][0].fg, 4);
	CHECK_INT(term.grid[0][0].bg, 1);

	/* Invisible paints the foreground with the background. */
	reset();
	feed("\x1b[8;31;44mR");
	CHECK_INT(term.grid[0][0].fg, 4);

	/* Underline is the one attribute a bitmap font can still draw. */
	reset();
	feed("\x1b[4;31mR");
	CHECK_INT(term.grid[0][0].attr & TERM_ATTR_UNDERLINE, TERM_ATTR_UNDERLINE);
	CHECK_INT(term.grid[0][0].fg, 1);

	/* SGR 0 and an empty parameter both reset. */
	reset();
	feed("\x1b[1;4;31m\x1b[mR");
	CHECK_INT(term.grid[0][0].fg, TERM_COLOR_DEFAULT);
	CHECK_INT(term.grid[0][0].attr, 0);

	reset();
	feed("\x1b[31m\x1b[0mR");
	CHECK_INT(term.grid[0][0].fg, TERM_COLOR_DEFAULT);

	/* Default-coloured cells stay default so the theme can supply them. */
	reset();
	feed("\x1b[39;49mR");
	CHECK_INT(term.grid[0][0].fg, TERM_COLOR_DEFAULT);
	CHECK_INT(term.grid[0][0].bg, TERM_COLOR_DEFAULT);

	/* Erasing keeps the background, which is what makes a coloured clear
	 * fill the screen. */
	reset();
	feed("\x1b[44m\x1b[2J");
	CHECK_INT(term.grid[10][10].bg, 4);
	CHECK_INT(term.grid[10][10].ch, ' ');

	/* 256-colour and truecolour requests are snapped to the palette. */
	reset();
	feed("\x1b[38;5;196mR"); /* pure red in the 256 cube */
	CHECK_INT(term.grid[0][0].fg, 1); /* 9 is *light* red, so dark red wins */
	reset();
	feed("\x1b[38;5;232mR"); /* near-black */
	CHECK_INT(term.grid[0][0].fg, 0);
	reset();
	feed("\x1b[48;2;255;255;255mR");
	CHECK_INT(term.grid[0][0].bg, 15);
	/* An extended colour must not swallow the parameters after it. */
	reset();
	feed("\x1b[38;5;196;1mR");
	CHECK_INT(term.grid[0][0].fg, 9);
}

/* --- scrolling and regions ---------------------------------------------- */

static void test_scrolling(void)
{
	char row[TERM_COLS + 1];
	int i;

	/* Filling past the last row scrolls and feeds the scrollback. */
	reset();
	for (i = 0; i < TERM_ROWS + 3; i++) {
		char line[24];

		snprintf(line, sizeof(line), "L%d\r\n", i);
		feed(line);
	}
	CHECK_INT(term_sb_count(&term), 4);
	row_text(&term, 0, row);
	CHECK_STR(row, "L4");

	/* Index 0 of the scrollback is the oldest retained line. */
	{
		char sb[TERM_COLS + 1];
		int x;

		for (x = 0; x < TERM_COLS; x++) {
			uint32_t c = term_sb_line(&term, 0)[x].ch;

			sb[x] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
		}
		sb[TERM_COLS] = '\0';
		CHECK(strncmp(sb, "L0", 2) == 0);
	}

	/* term_view_line walks back through history one screenful at a time. */
	CHECK(term_view_line(&term, 0, 0) != NULL);
	CHECK_INT(term_view_line(&term, 0, 0)->ch, 'L');
	CHECK(term_view_line(&term, 1, 0) == term_sb_line(&term, 3));
	CHECK(term_view_line(&term, TERM_SB_LINES + 100, TERM_ROWS - 1) != NULL);

	/* ED 3 drops the history without touching the screen. */
	feed("\x1b[3J");
	CHECK_INT(term_sb_count(&term), 0);
	row_text(&term, 0, row);
	CHECK_STR(row, "L4");
}

static void test_scroll_region(void)
{
	char row[TERM_COLS + 1];
	int i;

	reset();
	for (i = 0; i < 10; i++) {
		char line[24];

		snprintf(line, sizeof(line), "line%d\r\n", i);
		feed(line);
	}

	/* A region scroll must not push anything into history. */
	CHECK_INT(term_sb_count(&term), 0);
	feed("\x1b[5;8r"); /* rows 4..7 */

	/* DECSTBM homes the cursor. */
	CHECK_INT(term.cy, 0);
	CHECK_INT(term.cx, 0);

	feed("\x1b[8;1H"); /* bottom of the region */
	CHECK_INT(term.cy, 7);
	feed("\n");

	row_text(&term, 0, row);
	CHECK_STR(row, "line0"); /* above the region: untouched */
	row_text(&term, 3, row);
	CHECK_STR(row, "line3");
	row_text(&term, 4, row);
	CHECK_STR(row, "line5"); /* region scrolled up by one */
	row_text(&term, 7, row);
	CHECK_STR(row, "");
	CHECK_INT(term_sb_count(&term), 0);

	/* Vertical moves are confined to the region... */
	feed("\x1b[20B");
	CHECK_INT(term.cy, 7);

	/* ...but a direct address is not. */
	feed("\x1b[1;1H");
	CHECK_INT(term.cy, 0);

	/* RI at the top of the region scrolls the other way. */
	feed("\x1b[5;1H\x1bM");
	row_text(&term, 4, row);
	CHECK_STR(row, "");
	row_text(&term, 5, row);
	CHECK_STR(row, "line5");

	/* Resetting the region restores the full screen. */
	feed("\x1b[r");
	CHECK_INT(term.top, 0);
	CHECK_INT(term.bot, TERM_ROWS - 1);
}

/* --- alternate screen --------------------------------------------------- */

static void test_alt_screen(void)
{
	char row[TERM_COLS + 1];

	reset();
	feed("shell prompt");
	feed("\x1b[?1049h");
	CHECK_INT(term.alt_active, 1);
	row_text(&term, 0, row);
	CHECK_STR(row, ""); /* the alternate screen starts blank */

	feed("\x1b[2;3Hvim-ish");
	row_text(&term, 1, row);
	CHECK_STR(row, "  vim-ish");

	feed("\x1b[?1049l");
	CHECK_INT(term.alt_active, 0);
	row_text(&term, 0, row);
	CHECK_STR(row, "shell prompt");
	CHECK_INT(term.cx, 12);
	CHECK_INT(term.cy, 0);

	/* Rows that scroll on the alternate screen stay out of history. */
	reset();
	feed("\x1b[?1049h");
	{
		int i;

		for (i = 0; i < TERM_ROWS + 5; i++) feed("junk\r\n");
	}
	CHECK_INT(term_sb_count(&term), 0);
	feed("\x1b[?1049l");
	CHECK_INT(term_sb_count(&term), 0);

	/* term_view_line refuses to scroll back on the alternate screen. */
	feed("\x1b[?1049h");
	CHECK(term_view_line(&term, 1, 0) == NULL);
	feed("\x1b[?1049l");
}

static void test_save_restore(void)
{
	reset();
	feed("\x1b[10;20H\x1b[31m\x1b" "7"); /* DECSC */
	feed("\x1b[1;1H\x1b[0m");
	feed("\x1b" "8"); /* DECRC */
	CHECK_INT(term.cy, 9);
	CHECK_INT(term.cx, 19);
	CHECK_INT(term.cur_fg, 1);

	/* CSI s / CSI u are the same slot. */
	feed("\x1b[3;4H\x1b[s\x1b[1;1H\x1b[u");
	CHECK_INT(term.cy, 2);
	CHECK_INT(term.cx, 3);
}

/* --- reports, charsets, titles ------------------------------------------ */

static void test_reports(void)
{
	char reply[VT_OUTPUT_CAP + 1];

	reset();
	feed("\x1b[4;7H\x1b[6n");
	take_reply(reply, sizeof(reply));
	CHECK_STR(reply, "\x1b[4;7R");

	feed("\x1b[5n");
	take_reply(reply, sizeof(reply));
	CHECK_STR(reply, "\x1b[0n");

	feed("\x1b[c");
	take_reply(reply, sizeof(reply));
	CHECK_STR(reply, "\x1b[?1;2c");

	/* A private DA or a keypad-mode set is not answered. */
	feed("\x1b[>c\x1b=");
	take_reply(reply, sizeof(reply));
	CHECK_STR(reply, "");

	/* The buffer is drained, so a second read yields nothing. */
	feed("\x1b[6n");
	take_reply(reply, sizeof(reply));
	CHECK_STR(reply, "\x1b[4;7R"); /* still parked at 4;7 from above */
	take_reply(reply, sizeof(reply));
	CHECK_STR(reply, "");
}

static void test_charsets_and_titles(void)
{
	char row[TERM_COLS + 1];

	/* DEC special graphics: curses draws boxes with these letters. */
	reset();
	feed("\x1b(0q\x1b(Bq");
	CHECK_INT(term.grid[0][0].ch, '-');
	CHECK_INT(term.grid[0][1].ch, 'q');

	reset();
	feed("\x1b(0x\x1b(B");
	CHECK_INT(term.grid[0][0].ch, '|');

	/* OSC 0 and 2 set the title, other OSC codes are dropped. */
	reset();
	feed("\x1b]0;my title\x07X");
	CHECK_STR(parser.title, "my title");
	CHECK_INT(term.grid[0][0].ch, 'X');

	reset();
	feed("\x1b]2;other\x1b\\Y");
	CHECK_STR(parser.title, "other");
	CHECK_INT(term.grid[0][0].ch, 'Y');

	reset();
	feed("\x1b]4;1;rgb:00/00/00\x07Z");
	CHECK_STR(parser.title, "");
	CHECK_INT(term.grid[0][0].ch, 'Z');

	/* DCS payloads are consumed, not printed. */
	reset();
	feed("\x1bP1$r0m\x1b\\ok");
	row_text(&term, 0, row);
	CHECK_STR(row, "ok");
}

static void test_utf8(void)
{
	char row[TERM_COLS + 1];

	/* Multibyte input is decoded and consumed as one cell, so the columns
	 * after it stay aligned even though the font cannot draw it. */
	reset();
	feed("a\xe4\xb8\xadz");
	CHECK_INT(term.cx, 3);
	row_text(&term, 0, row);
	CHECK_STR(row, "a?z");
	CHECK_INT(term.grid[0][1].ch, '?');

	/* A truncated sequence does not desynchronise the next line. */
	reset();
	feed("a\xe4\xb8");
	feed("\r\nb");
	row_text(&term, 1, row);
	CHECK_STR(row, "b");

	/* A stray continuation byte is dropped rather than printed. */
	reset();
	feed("\x80\x81" "ab");
	row_text(&term, 0, row);
	CHECK_STR(row, "ab");
}

static void test_reset(void)
{
	reset();
	feed("stuff\x1b[31m\x1b[?1049h\x1b(0");
	feed("\x1b" "c"); /* RIS */
	CHECK_INT(term.alt_active, 0);
	CHECK_INT(term.cur_fg, TERM_COLOR_DEFAULT);
	CHECK_INT(term.top, 0);
	CHECK_INT(term.bot, TERM_ROWS - 1);
	CHECK_INT(term.grid[0][0].ch, ' ');
	CHECK_INT(parser.g0_graphics, 0);
}

static void test_ignored_sequences(void)
{
	char row[TERM_COLS + 1];

	/* Mouse reporting and the rest must be swallowed without stalling and
	 * without printing their arguments. */
	reset();
	feed("\x1b[?1000h\x1b[?1006h\x1b[?25l\x1b[?25h\x1b[?2004h");
	feed("ok");
	row_text(&term, 0, row);
	CHECK_STR(row, "ok");
	CHECK_INT(term.cursor_visible, 1);
	CHECK_INT(parser.bracketed_paste, 1);

	reset();
	feed("\x1b[2 q"); /* DECSCUSR, has an intermediate */
	feed("ok");
	row_text(&term, 0, row);
	CHECK_STR(row, "ok");

	/* An unknown final byte is dropped, not echoed. */
	reset();
	feed("\x1b[99zok");
	row_text(&term, 0, row);
	CHECK_STR(row, "ok");

	/* CSI inside a CSI restarts rather than leaking bytes to the grid. */
	reset();
	feed("\x1b[1\x1b[2;3Hok");
	CHECK_INT(term.cy, 1);
	CHECK_INT(term.cx, 4);
	row_text(&term, 1, row);
	CHECK_STR(row, "  ok");
}

/* --- corpus: real output shapes ----------------------------------------- */

static void show_corpus(void)
{
	char row[TERM_COLS + 1];

	printf("\n  -- ls --color --\n");
	reset();
	feed("\x1b[0m\x1b[01;34mDocuments\x1b[0m  \x1b[01;34mDownloads\x1b[0m  "
	     "\x1b[01;32mrun.sh\x1b[0m  notes.txt\r\n");
	dump_screen(&term);
	CHECK_INT(term.grid[0][0].fg, 12);
	CHECK_INT(term.grid[0][12].fg, 12); /* inside Downloads */

	printf("\n  -- top --\n");
	reset();
	feed("\x1b[?1049h\x1b[H\x1b[2J");
	feed("\x1b[7mtop - 12:00:00 up 1 day\x1b[0m\r\n");
	feed("Tasks: \x1b[1m120 total\x1b[0m, 2 running\r\n");
	feed("\x1b[1;1H\x1b[7m");
	dump_screen(&term);

	printf("\n  -- vim --\n");
	reset();
	feed("\x1b[?1049h\x1b[2J\x1b[1;1H");
	feed("\x1b(0lqqqqqqqqqqqqqqk\x1b(B\r\n");
	feed("\x1b(0x\x1b(B hello world  \x1b(0x\x1b(B\r\n");
	feed("\x1b(0mqqqqqqqqqqqqqqj\x1b(B\r\n");
	feed("\x1b[24;1H\x1b[7m-- INSERT --\x1b[0m");
	feed("\x1b[2;3H");
	dump_screen(&term);
	row_text(&term, 0, row);
	CHECK_STR(row, "+--------------+"); /* DEC corners folded to '+' */
	CHECK_INT(term.grid[1][0].ch, '|');
	CHECK_INT(term.grid[2][0].ch, '+');

	/* Tearing down the alternate screen brings the shell back. */
	feed("\x1b[?1049l");
	CHECK_INT(term.alt_active, 0);
}

int main(void)
{
	printf("vitassh host tests\n");

	printf(" config\n");
	test_config_line();
	test_config_roundtrip();
	test_config_make();
	test_config_field_clean();
	test_config_long_fields();
	test_config_cap();
	test_config_save_failure();

	printf(" vault\n");
	test_vault_roundtrip();
	test_vault_limits();
	test_vault_capacity();
	test_vault_staging();
	test_vault_damage();
	test_vault_stuck();
	test_vault_save_failure();
	test_vault_cleared();
	test_file_replace();

	printf(" term\n");
	test_basic_writing();
	test_tabs();
	test_wrapping();
	test_cursor_addressing();
	test_erase();
	test_line_editing();
	test_insert_mode();

	printf(" rendition\n");
	test_sgr();

	printf(" scrolling\n");
	test_scrolling();
	test_scroll_region();

	printf(" screens\n");
	test_alt_screen();
	test_save_restore();

	printf(" parser\n");
	test_reports();
	test_charsets_and_titles();
	test_utf8();
	test_reset();
	test_ignored_sequences();

	printf(" corpus\n");
	show_corpus();

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
