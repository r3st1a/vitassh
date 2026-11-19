/*
 * Saved passwords, encrypted at rest. See src/vault.h for the file format.
 *
 * Everything here is device-independent: the crypto primitives arrive through
 * vault_platform.h and the path arrives from the caller, so this file -- where
 * the slot layout and the acceptance rules live -- builds and runs under the
 * host test harness.
 */
#include "vault.h"

#include <stdio.h>
#include <string.h>

#include "fileio.h"
#include "vault_platform.h"

#define VAULT_MAGIC "VSSHVLT1"
#define VAULT_MAGIC_LEN 8

/* Slot fields, laid out end to end with no padding and no alignment. */
#define VAULT_SLOT_HOST_OFF 0
#define VAULT_SLOT_USER_OFF (VAULT_SLOT_HOST_OFF + CFG_HOST_CAP)
#define VAULT_SLOT_PASS_OFF (VAULT_SLOT_USER_OFF + CFG_USER_CAP)
#define VAULT_SLOT_PORT_OFF (VAULT_SLOT_PASS_OFF + VAULT_PASS_CAP)
#define VAULT_SLOT_SIZE (VAULT_SLOT_PORT_OFF + 2)
#define VAULT_PLAIN_LEN (VAULT_MAX_SLOTS * VAULT_SLOT_SIZE)

#define VAULT_OFF_MAGIC 0
#define VAULT_OFF_SALT (VAULT_OFF_MAGIC + VAULT_MAGIC_LEN)
#define VAULT_OFF_IV (VAULT_OFF_SALT + VAULT_SALT_LEN)
#define VAULT_OFF_DATA (VAULT_OFF_IV + VAULT_IV_LEN)
#define VAULT_OFF_TAG (VAULT_OFF_DATA + VAULT_PLAIN_LEN)
#define VAULT_FILE_LEN (VAULT_OFF_TAG + VAULT_TAG_LEN)

/*
 * A secrets.dat on the card is written and read by builds that may not be the
 * same one. Changing a field cap changes what the bytes mean, so these fire
 * deliberately: the answer is a new magic version and a decision about the old
 * file, not a silent reinterpretation of files already on someone's console.
 */
_Static_assert(VAULT_SLOT_SIZE == 226, "slot layout changed: bump the magic");
_Static_assert(VAULT_OFF_DATA == 36, "header layout changed: bump the magic");
_Static_assert(VAULT_FILE_LEN == 3668, "file layout changed: bump the magic");

static void copy_cap(char *dst, size_t cap, const char *src)
{
	size_t n = strlen(src);

	if (n > cap - 1) n = cap - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

/*
 * The identity of an entry, as far as the vault is concerned: the display name
 * is not part of it, so renaming a host keeps its password.
 */
static int key_matches(const VaultSlot *s, const char *host, const char *user,
                       int port)
{
	if (port < 1 || port > 65535) return 0;

	return s->port == (uint16_t)port && strcmp(s->host, host) == 0 &&
	       strcmp(s->user, user) == 0;
}

static int entry_matches(const VaultSlot *s, const HostEntry *h)
{
	return key_matches(s, h->host, h->user, h->port);
}

static void pack(const Vault *v, uint8_t *plain)
{
	int i;

	memset(plain, 0, VAULT_PLAIN_LEN);

	for (i = 0; i < VAULT_MAX_SLOTS; i++) {
		const VaultSlot *s = &v->slot[i];
		uint8_t *p = plain + (size_t)i * VAULT_SLOT_SIZE;

		if (s->host[0] == '\0') continue; /* stays zeroed: an empty slot */

		memcpy(p + VAULT_SLOT_HOST_OFF, s->host, strlen(s->host) + 1);
		memcpy(p + VAULT_SLOT_USER_OFF, s->user, strlen(s->user) + 1);
		memcpy(p + VAULT_SLOT_PASS_OFF, s->pass, strlen(s->pass) + 1);
		p[VAULT_SLOT_PORT_OFF] = (uint8_t)(s->port >> 8);
		p[VAULT_SLOT_PORT_OFF + 1] = (uint8_t)(s->port & 0xFF);
	}
}

/*
 * Loads a decrypted slot table into v. Returns 0 if the whole table is sound.
 *
 * The tag only proves the bytes came from us, not that they are a table this
 * build can read -- an older version, or a bug, could have written something
 * else with a valid tag. So each string is required to be terminated within its
 * own field before anything reads it as one, and one bad slot rejects the file
 * rather than being skipped: silently dropping an entry would look exactly like
 * a forgotten password.
 */
static int unpack(Vault *v, const uint8_t *plain)
{
	int i;

	for (i = 0; i < VAULT_MAX_SLOTS; i++) {
		const uint8_t *p = plain + (size_t)i * VAULT_SLOT_SIZE;
		const uint8_t *host = p + VAULT_SLOT_HOST_OFF;
		const uint8_t *user = p + VAULT_SLOT_USER_OFF;
		const uint8_t *pass = p + VAULT_SLOT_PASS_OFF;
		VaultSlot *s = &v->slot[i];
		uint16_t port;

		if (((const char *)host)[0] == '\0') continue; /* an empty slot */

		if (memchr(host, 0, CFG_HOST_CAP) == NULL) return -1;
		if (memchr(user, 0, CFG_USER_CAP) == NULL) return -1;
		if (memchr(pass, 0, VAULT_PASS_CAP) == NULL) return -1;

		port = (uint16_t)((p[VAULT_SLOT_PORT_OFF] << 8) |
		                  p[VAULT_SLOT_PORT_OFF + 1]);
		if (port == 0) return -1;

		copy_cap(s->host, sizeof(s->host), (const char *)host);
		copy_cap(s->user, sizeof(s->user), (const char *)user);
		copy_cap(s->pass, sizeof(s->pass), (const char *)pass);
		s->port = port;
	}
	return 0;
}

static int status_usable(VaultStatus st)
{
	return st == VAULT_OK || st == VAULT_RECOVERED || st == VAULT_FOREIGN;
}

/*
 * Moves a file we cannot read out of the way so a fresh one can be written. The
 * old one is kept as .bad: it is the user's only copy of those passwords, and
 * nothing here is in a position to know it is worthless.
 *
 * One generation only, so repeated failures cannot fill the card.
 */
static VaultStatus quarantine(const char *path, VaultStatus why)
{
	char bad[256];

	if (snprintf(bad, sizeof(bad), "%s.bad", path) >= (int)sizeof(bad))
		return VAULT_ERR_STUCK;

	remove(bad);

	/*
	 * Failing here leaves the damaged file exactly where it is, and the vault
	 * read-only: overwriting it is the one outcome that loses data for good.
	 */
	if (rename(path, bad) != 0) return VAULT_ERR_STUCK;

	return why;
}

/* 0 when the file is exactly the expected size, -1 otherwise. */
static int read_exact(FILE *f, uint8_t *buf, size_t len)
{
	if (fread(buf, 1, len, f) != len) return -1;
	if (fgetc(f) != EOF) return -1; /* longer than the format allows */
	return 0;
}

VaultStatus vault_load(Vault *v, const char *path)
{
	uint8_t secret[VAULT_SECRET_LEN] = {0};
	uint8_t key[VAULT_KEY_LEN] = {0};
	uint8_t plain[VAULT_PLAIN_LEN] = {0};
	uint8_t file[VAULT_FILE_LEN];
	FILE *f = NULL;
	VaultStatus st;

	if (v == NULL || path == NULL) return VAULT_ERR_DEVICE;
	vault_clear(v);

	if (vault_selftest() != 0) {
		st = VAULT_ERR_SELFTEST;
		goto done;
	}
	if (vault_device_secret(secret) != 0) {
		st = VAULT_ERR_DEVICE;
		goto done;
	}

	f = fopen(path, "rb");
	if (f == NULL) {
		st = VAULT_OK; /* nothing saved yet */
		goto done;
	}

	if (read_exact(f, file, sizeof(file)) != 0) {
		/*
		 * An I/O error may pass, so the file is left alone; a wrong size
		 * will not, so it is moved aside and the vault starts empty.
		 *
		 * The handle has to be closed first: renaming a file that is still
		 * open fails on Windows, and the vault would then refuse to work
		 * over a file it could have moved out of the way.
		 */
		int io = ferror(f);

		fclose(f);
		f = NULL;
		st = io ? VAULT_ERR_STUCK : quarantine(path, VAULT_RECOVERED);
		goto done;
	}
	fclose(f);
	f = NULL;

	if (memcmp(file + VAULT_OFF_MAGIC, VAULT_MAGIC, VAULT_MAGIC_LEN) != 0) {
		st = quarantine(path, VAULT_RECOVERED);
		goto done;
	}

	if (vault_kdf(secret, sizeof(secret), file + VAULT_OFF_SALT, key) != 0 ||
	    vault_aead_decrypt(key, file + VAULT_OFF_IV, file, VAULT_OFF_DATA,
	                       file + VAULT_OFF_DATA, VAULT_PLAIN_LEN,
	                       file + VAULT_OFF_TAG, plain) != 0) {
		st = quarantine(path, VAULT_FOREIGN);
		goto done;
	}

	if (unpack(v, plain) != 0) {
		vault_clear(v);
		st = quarantine(path, VAULT_RECOVERED);
		goto done;
	}

	st = VAULT_OK;

done:
	if (f != NULL) fclose(f);
	v->ready = status_usable(st) != 0;
	vault_wipe(secret, sizeof(secret));
	vault_wipe(key, sizeof(key));
	vault_wipe(plain, sizeof(plain));
	return st;
}

const char *vault_status_text(VaultStatus st)
{
	switch (st) {
	case VAULT_OK:
		return "ready";
	case VAULT_RECOVERED:
		return "unreadable file moved to secrets.dat.bad";
	case VAULT_FOREIGN:
		return "integrity check failed (another console?) -- moved to "
		       "secrets.dat.bad";
	case VAULT_ERR_DEVICE:
		return "no device id, passwords disabled";
	case VAULT_ERR_SELFTEST:
		return "crypto self-test failed, passwords disabled";
	case VAULT_ERR_STUCK:
		return "damaged file could not be moved aside, passwords disabled";
	}
	return "unknown";
}

int vault_save(Vault *v, const char *path)
{
	uint8_t secret[VAULT_SECRET_LEN] = {0};
	uint8_t key[VAULT_KEY_LEN] = {0};
	uint8_t plain[VAULT_PLAIN_LEN] = {0};
	uint8_t rnd[VAULT_SALT_LEN + VAULT_IV_LEN] = {0};
	uint8_t file[VAULT_FILE_LEN] = {0};
	char tmp[256];
	FILE *f;
	int rc = -1;

	if (v == NULL || path == NULL) return -1;
	if (!v->ready) return -1;
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return -1;

	/*
	 * A fresh salt and nonce every time. Reusing a GCM nonce under the same
	 * key is not a weakness, it is a break, so this does not fall back to a
	 * constant when the generator fails -- it abandons the save.
	 */
	if (vault_device_secret(secret) != 0) goto done;
	if (vault_random(rnd, sizeof(rnd)) != 0) goto done;
	if (vault_kdf(secret, sizeof(secret), rnd, key) != 0) goto done;

	memcpy(file + VAULT_OFF_MAGIC, VAULT_MAGIC, VAULT_MAGIC_LEN);
	memcpy(file + VAULT_OFF_SALT, rnd, VAULT_SALT_LEN);
	memcpy(file + VAULT_OFF_IV, rnd + VAULT_SALT_LEN, VAULT_IV_LEN);

	pack(v, plain);

	/* The header is authenticated too, so every byte of the file is covered. */
	if (vault_aead_encrypt(key, file + VAULT_OFF_IV, file, VAULT_OFF_DATA,
	                       plain, VAULT_PLAIN_LEN, file + VAULT_OFF_DATA,
	                       file + VAULT_OFF_TAG) != 0)
		goto done;

	f = fopen(tmp, "wb");
	if (f == NULL) goto done;

	if (fwrite(file, 1, sizeof(file), f) != sizeof(file)) {
		fclose(f);
		remove(tmp);
		goto done;
	}
	if (fclose(f) != 0) {
		remove(tmp);
		goto done;
	}

	/*
	 * On failure the temporary is left on disk: it holds the only complete
	 * copy of the data, and the next save overwrites it anyway.
	 */
	if (file_replace(tmp, path) != 0) goto done;

	v->dirty = false;
	rc = 0;

done:
	vault_wipe(secret, sizeof(secret));
	vault_wipe(key, sizeof(key));
	vault_wipe(plain, sizeof(plain));
	return rc;
}

static const VaultSlot *find_slot(const Vault *v, const HostEntry *h)
{
	int i;

	if (v == NULL || h == NULL || !v->ready) return NULL;

	for (i = 0; i < VAULT_MAX_SLOTS; i++) {
		const VaultSlot *s = &v->slot[i];

		if (s->host[0] == '\0') continue;
		if (entry_matches(s, h)) return s;
	}
	return NULL;
}

int vault_has(const Vault *v, const HostEntry *h)
{
	return find_slot(v, h) != NULL ? 1 : 0;
}

int vault_lookup(const Vault *v, const HostEntry *h, char *out, size_t cap)
{
	const VaultSlot *s = find_slot(v, h);

	if (s == NULL || out == NULL || cap == 0) return -1;

	/* Callers pass a VAULT_PASS_CAP buffer; a truncated password is worse
	 * than none, so anything shorter than the whole thing is a miss rather
	 * than a near-miss. */
	if (strlen(s->pass) > cap - 1) return -1;

	memcpy(out, s->pass, strlen(s->pass) + 1);
	return 0;
}

int vault_set(Vault *v, const HostEntry *h, const char *pass)
{
	int i;
	int free_slot = -1;

	if (v == NULL || h == NULL || pass == NULL) return -1;
	if (!v->ready) return -1;
	if (h->port < 1 || h->port > 65535) return -1;

	/*
	 * Refuse anything that does not fit rather than truncating it: two hosts
	 * whose names differ only past the limit would become one entry and share
	 * one password.
	 */
	if (h->host[0] == '\0') return -1;
	if (strlen(h->host) >= CFG_HOST_CAP) return -1;
	if (strlen(h->user) >= CFG_USER_CAP) return -1;
	if (strlen(pass) >= VAULT_PASS_CAP) return -1;

	for (i = 0; i < VAULT_MAX_SLOTS; i++) {
		VaultSlot *s = &v->slot[i];

		if (s->host[0] == '\0') {
			if (free_slot < 0) free_slot = i;
			continue;
		}
		if (key_matches(s, h->host, h->user, h->port)) break;
	}

	if (i == VAULT_MAX_SLOTS) {
		if (free_slot < 0) return -1; /* all 16 slots are taken */
		i = free_slot;
	}

	vault_wipe(&v->slot[i], sizeof(v->slot[i]));
	memset(&v->slot[i], 0, sizeof(v->slot[i]));

	copy_cap(v->slot[i].host, sizeof(v->slot[i].host), h->host);
	copy_cap(v->slot[i].user, sizeof(v->slot[i].user), h->user);
	copy_cap(v->slot[i].pass, sizeof(v->slot[i].pass), pass);
	v->slot[i].port = (uint16_t)h->port;
	v->dirty = true;
	return 0;
}

void vault_remove(Vault *v, const HostEntry *h)
{
	int i;

	if (v == NULL || h == NULL) return;

	for (i = 0; i < VAULT_MAX_SLOTS; i++) {
		if (v->slot[i].host[0] == '\0') continue;
		if (!entry_matches(&v->slot[i], h)) continue;

		vault_wipe(&v->slot[i], sizeof(v->slot[i]));
		memset(&v->slot[i], 0, sizeof(v->slot[i]));
		v->dirty = true;
		return;
	}
}

void vault_clear(Vault *v)
{
	if (v == NULL) return;

	vault_wipe(v, sizeof(*v));
	memset(v, 0, sizeof(*v));
}
