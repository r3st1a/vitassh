/*
 * Saved passwords, encrypted at rest.
 *
 * The file format is fixed size with no length fields, so a truncated or padded
 * file cannot be reinterpreted as a different set of slots:
 *
 *   0     magic "VSSHVLT1"
 *   8     salt, 16 bytes
 *   24    GCM nonce, 12 bytes
 *   36    ciphertext, 3616 bytes
 *   3652  GCM tag, 16 bytes
 *
 * The 36 header bytes are authenticated as additional data, so flipping a byte
 * anywhere in the file is detected. The plaintext is 16 slots of 226 bytes:
 *
 *   host[64] | user[32] | pass[128] | port, big-endian 16 bit
 *
 * Slots are matched on (host, user, port) and never on the display name, so
 * renaming a host keeps its password. Fields are compared and stored as text,
 * including the port, so no struct layout reaches the disk.
 */
#ifndef VITASSH_VAULT_H
#define VITASSH_VAULT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define VAULT_MAX_SLOTS CFG_MAX_HOSTS
#define VAULT_PASS_CAP 128

typedef struct {
	char host[CFG_HOST_CAP];
	char user[CFG_USER_CAP];
	uint16_t port;
	char pass[VAULT_PASS_CAP];
} VaultSlot;

typedef struct {
	/*
	 * False when the device secret could not be read, the startup self-test
	 * failed, or a damaged file could not be moved aside. A vault that is not
	 * ready never reads and never writes: the alternative is falling back to a
	 * key that will not match the file next time, which loses the passwords
	 * anyway but without saying so.
	 */
	bool ready;
	bool dirty;
	VaultSlot slot[VAULT_MAX_SLOTS];
} Vault;

/*
 * What became of the file. Only the caller can turn this into a log line --
 * this file has no business knowing about log.c -- so readiness is reported
 * separately, on the vault itself: the first three values leave it usable.
 */
typedef enum {
	VAULT_OK = 0,   /* usable: loaded, or nothing saved yet */
	VAULT_RECOVERED,/* usable: an unreadable file was moved aside */
	VAULT_FOREIGN,  /* usable: the tag failed, so probably another console */
	VAULT_ERR_DEVICE,  /* no device secret: never write, never guess one */
	VAULT_ERR_SELFTEST,/* the crypto primitives failed their known answers */
	VAULT_ERR_STUCK    /* damaged file left in place, so the vault is read-only */
} VaultStatus;

/* A fixed phrase for the log. Never NULL. */
const char *vault_status_text(VaultStatus st);

/*
 * Reads the vault, creating nothing, and clears v first. The status is for the
 * log; usability is v->ready, which is false only for the VAULT_ERR_* values.
 * No file yet is not an error -- it is the first run. A damaged file is moved
 * to "<path>.bad" and treated as empty; if it cannot be moved, the vault stays
 * read-only so the damaged file is never overwritten.
 */
VaultStatus vault_load(Vault *v, const char *path);

/* Writes the vault out atomically. Returns 0 on success. */
int vault_save(Vault *v, const char *path);

/*
 * Copies the password for h into out. Returns 0 on a hit, -1 otherwise --
 * including when out is too small, since half a password is not one.
 */
int vault_lookup(const Vault *v, const HostEntry *h, char *out, size_t cap);

/*
 * Returns 1 when h has a saved password, 0 otherwise. This is what a caller
 * that only wants to know whether one exists should use: looking one up into a
 * scratch buffer just to throw it away puts a plaintext password somewhere it
 * does not need to be.
 */
int vault_has(const Vault *v, const HostEntry *h);

/*
 * Stores the password for h, replacing an existing entry for the same host,
 * user and port. Returns 0 on success and -1 if the vault is not ready, the
 * vault is full, or a field does not fit -- an over-long field is refused
 * rather than truncated, because truncating would make two different hosts
 * share one password.
 */
int vault_set(Vault *v, const HostEntry *h, const char *pass);

/* Forgets the password for h. Does nothing when there is none. */
void vault_remove(Vault *v, const HostEntry *h);

/* Zeroes the whole vault, passwords included. Call before letting it go. */
void vault_clear(Vault *v);

#endif /* VITASSH_VAULT_H */
