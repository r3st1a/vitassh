#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fileio.h"

#define CFG_TEMPLATE                                                          \
	"# VitaSSH hosts -- one per line, fields separated by '|'\n"                \
	"#\n"                                                                      \
	"#   name|host|port|user\n"                                                \
	"#\n"                                                                      \
	"# port defaults to 22 and user to root, so both may be left empty.\n"      \
	"# Blank lines and lines starting with '#' are ignored.\n"                  \
	"#\n"                                                                      \
	"# home|192.168.1.10|22|root\n"

static char *trim(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t') s++;

	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
	                   end[-1] == '\n'))
		end--;
	*end = '\0';
	return s;
}

/*
 * trim() only steps past the leading blanks; callers want the text moved down
 * too, which is what this wraps.
 */
static void trim_in_place(char *s)
{
	char *value = trim(s);

	if (value != s) memmove(s, value, strlen(value) + 1);
}

/*
 * Copies the next '|'-separated field into dst. Unlike strtok this keeps empty
 * fields, so "a||c" is three fields rather than two. Returns 0 once the line is
 * exhausted.
 */
static int next_field(char **cursor, char *dst, size_t cap)
{
	char *s = *cursor;
	char *bar;
	size_t n;

	if (s == NULL) return 0;

	bar = strchr(s, '|');
	if (bar != NULL) {
		n = (size_t)(bar - s);
		*cursor = bar + 1;
	} else {
		n = strlen(s);
		*cursor = NULL;
	}

	if (n > cap - 1) n = cap - 1;
	memcpy(dst, s, n);
	dst[n] = '\0';

	trim_in_place(dst);
	return 1;
}

static void copy_bounded(char *dst, size_t cap, const char *src)
{
	size_t n = strlen(src);

	if (n > cap - 1) n = cap - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

void config_field_clean(char *s)
{
	char *dst = s;

	if (s == NULL) return;

	for (; *s != '\0'; s++) {
		unsigned char c = (unsigned char)*s;

		/* '|' would shift every later field; a control byte would end the
		 * line early and turn the rest into a bogus entry on the next read. */
		if (c == '|' || c < 0x20 || c == 0x7F) continue;
		*dst++ = *s;
	}
	*dst = '\0';
}

int config_entry_make(const char *name, const char *host, const char *port,
                      const char *user, HostEntry *out)
{
	char n[CFG_NAME_CAP];
	char h[CFG_HOST_CAP];
	char p[16];
	char u[CFG_USER_CAP];

	if (out == NULL) return -1;

	copy_bounded(n, sizeof(n), name != NULL ? name : "");
	copy_bounded(h, sizeof(h), host != NULL ? host : "");
	copy_bounded(p, sizeof(p), port != NULL ? port : "");
	copy_bounded(u, sizeof(u), user != NULL ? user : "");

	trim_in_place(n);
	trim_in_place(h);
	trim_in_place(p);
	trim_in_place(u);

	if (h[0] == '\0') return -1;

	memset(out, 0, sizeof(*out));
	copy_bounded(out->host, sizeof(out->host), h);
	copy_bounded(out->name, sizeof(out->name), n[0] != '\0' ? n : h);
	copy_bounded(out->user, sizeof(out->user), u[0] != '\0' ? u : "root");

	if (p[0] != '\0') {
		int value = atoi(p);

		out->port = (value > 0 && value <= 65535) ? value : 22;
	} else {
		out->port = 22;
	}
	return 0;
}

int config_parse_line(const char *line, HostEntry *out)
{
	char buf[CFG_LINE_CAP];
	char name[CFG_NAME_CAP];
	char host[CFG_HOST_CAP];
	char port[16];
	char user[CFG_USER_CAP];
	char *cursor;
	const char *start;

	if (line == NULL || out == NULL) return -1;

	copy_bounded(buf, sizeof(buf), line);
	start = trim(buf);
	if (*start == '\0' || *start == '#') return 1;

	cursor = (char *)start;
	name[0] = host[0] = port[0] = user[0] = '\0';

	if (!next_field(&cursor, name, sizeof(name))) return -1;
	if (!next_field(&cursor, host, sizeof(host))) host[0] = '\0';
	if (!next_field(&cursor, port, sizeof(port))) port[0] = '\0';
	if (!next_field(&cursor, user, sizeof(user))) user[0] = '\0';

	return config_entry_make(name, host, port, user, out);
}

/* Creates the file with the commented template so there is something to edit. */
static void write_template(const char *path)
{
	FILE *f = fopen(path, "wb");

	if (f == NULL) return;
	fputs(CFG_TEMPLATE, f);
	fclose(f);
}

int config_load(HostList *list, const char *path)
{
	FILE *f;
	char line[CFG_LINE_CAP];

	if (list == NULL || path == NULL) return -1;
	list->count = 0;

	f = fopen(path, "rb");
	if (f == NULL) {
		write_template(path);
		return 0;
	}

	while (fgets(line, sizeof(line), f) != NULL) {
		HostEntry entry;

		if (config_parse_line(line, &entry) != 0) continue;
		if (list->count >= CFG_MAX_HOSTS) break;
		list->item[list->count++] = entry;
	}

	fclose(f);
	return list->count;
}

int config_save(const HostList *list, const char *path)
{
	char tmp[CFG_HOST_CAP * 2];
	FILE *f;
	int i;

	if (list == NULL || path == NULL) return -1;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return -1;

	f = fopen(tmp, "wb");
	if (f == NULL) return -1;

	fputs(CFG_TEMPLATE, f);
	for (i = 0; i < list->count; i++) {
		const HostEntry *e = &list->item[i];

		if (fprintf(f, "%s|%s|%d|%s\n", e->name, e->host, e->port, e->user) < 0) {
			fclose(f);
			remove(tmp);
			return -1;
		}
	}

	if (fclose(f) != 0) {
		remove(tmp);
		return -1;
	}

	return file_replace(tmp, path);
}
