/*
 * Host list persistence. Like term.c and vt.c this carries no psp2 header, so
 * the parser can be unit tested on the host.
 */
#ifndef VITASSH_CONFIG_H
#define VITASSH_CONFIG_H

#include <stddef.h>

#define CFG_MAX_HOSTS 16
#define CFG_NAME_CAP 32
#define CFG_HOST_CAP 64
#define CFG_USER_CAP 32

/*
 * Room for one "name|host|port|user" line: three separators, a five-digit port
 * and the terminator on top of the field caps. The parser and the reader both
 * size their line buffers with this -- at CFG_HOST_CAP * 2 (128) the longest
 * line config_save() can produce does not fit, and a full-length entry written
 * by the app cannot be read back.
 */
#define CFG_LINE_CAP (CFG_NAME_CAP + CFG_HOST_CAP + CFG_USER_CAP + 24)

/* Where the app looks on the device. Unused on the host, which passes its own
 * paths so tests never touch real storage. */
#define CFG_DEFAULT_PATH "ux0:data/vitassh/hosts.txt"

typedef struct {
	char name[CFG_NAME_CAP];
	char host[CFG_HOST_CAP];
	int port;
	char user[CFG_USER_CAP];
} HostEntry;

typedef struct {
	HostEntry item[CFG_MAX_HOSTS];
	int count;
} HostList;

/*
 * Parses one "name|host|port|user" line. Port and user may be left empty and
 * fall back to 22 and root; an empty name falls back to the host.
 * Returns 0 for an entry, 1 for a blank or comment line, -1 for a bad one.
 */
int config_parse_line(const char *line, HostEntry *out);

/*
 * Builds an entry from four already-split fields, applying the same defaults the
 * parser does: empty port -> 22, empty user -> root, empty name -> host. Fields
 * are trimmed. This is what the in-app editor commits through, so an entry typed
 * on the device and one read from the file cannot disagree. Returns 0, or -1 if
 * the host is empty.
 */
int config_entry_make(const char *name, const char *host, const char *port,
                      const char *user, HostEntry *out);

/*
 * Drops '|' and control bytes from a field in place, so text typed on the device
 * cannot shift fields or split a line when it is written back out.
 */
void config_field_clean(char *s);

/*
 * Returns how many hosts were loaded, or -1 if the file exists but could not be
 * read. A missing file is not an error: it is created with a commented template
 * and 0 is returned, so the user has something to edit.
 */
int config_load(HostList *list, const char *path);

/* Writes the list out atomically (temp file, then rename). Returns 0 on ok. */
int config_save(const HostList *list, const char *path);

#endif /* VITASSH_CONFIG_H */
